#include "chronos/postgresql_paper_order_repository.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// The durable half of model deployments (migration 005, ADR 0018): the
// deployment and its owned account, its lifecycle, its decisions and
// watermark, and the one transaction that records a deployment's order
// together with its decision. Each test builds its database in a scratch
// schema of the shared test database.

namespace {

using chronos::BookLevel;
using chronos::CommitPaperOrderResult;
using chronos::CreatedDeployment;
using chronos::DecisionState;
using chronos::DeploymentConflict;
using chronos::DeploymentConflictCode;
using chronos::DeploymentDecisionRecord;
using chronos::DeploymentOrderResolution;
using chronos::DeploymentState;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::ModelDeploymentConfig;
using chronos::ModelDeploymentRecord;
using chronos::Money;
using chronos::NewDeploymentDecision;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperAccountLock;
using chronos::PaperOrderReceipt;
using chronos::PaperOrderRecord;
using chronos::PaperOrderResult;
using chronos::PaperPersistenceUnavailable;
using chronos::PersistentPaperOrderSession;
using chronos::PostgresPaperOrderRepository;
using chronos::PreTradeRiskLimits;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::WorkingOrderMatch;
using chronos::test::all_migrations_sql;
using chronos::test::PostgresTestConnection;
using chronos::test::terminate_backends;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;

std::string connection_string;

const InstrumentId kAapl("AAPL");
const MarketDataStreamId kStream("market-stream");

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

class ScratchSchema final : public chronos::test::ScratchSchema {
public:
    ScratchSchema() : chronos::test::ScratchSchema(connection_string) {
        PostgresPaperOrderRepository(connection()).apply_migration(all_migrations_sql());
    }
};

ModelDeploymentConfig config(std::string deployment_id, std::string account_id) {
    ModelDeploymentConfig config;
    config.deployment_id = std::move(deployment_id);
    config.account_id = std::move(account_id);
    config.starting_cash = money("1000");
    config.experiment_id = std::string(64, 'e');
    config.parameters_sha256 = std::string(64, 'f');
    config.model_name = "logistic";
    config.instrument = kAapl;
    config.training_cutoff = Timestamp::from_nanoseconds(100);
    config.threshold_ppb = 550'000'000;
    config.target_quantity = quantity("1");
    config.decision_interval_ns = 1'000;
    config.latency_ns = 10;
    config.max_signal_age_ns = 1'000;
    config.max_orders_per_run = 10;
    config.max_consecutive_rejections = 3;
    config.stream = kStream;
    config.risk_limits = PreTradeRiskLimits{
        .maximum_order_quantity = quantity("1"),
        .maximum_order_notional = money("500"),
        .maximum_position_quantity = quantity("1"),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = money("0.01"),
    };
    config.request_sha256 = std::string(64, 'a');
    return config;
}

ModelDeploymentRecord create(PostgresPaperOrderRepository& store, const ModelDeploymentConfig& wanted) {
    auto created = store.create_deployment(wanted);
    REQUIRE(std::holds_alternative<CreatedDeployment>(created));
    REQUIRE(std::get<CreatedDeployment>(created).created);
    return std::get<CreatedDeployment>(std::move(created)).deployment;
}

DeploymentConflict require_conflict(
    const auto& result,
    DeploymentConflictCode code
) {
    REQUIRE(std::holds_alternative<DeploymentConflict>(result));
    const auto& conflict = std::get<DeploymentConflict>(result);
    REQUIRE(conflict.code == code);
    REQUIRE(!conflict.message.empty());
    return conflict;
}

ModelDeploymentRecord start(PostgresPaperOrderRepository& store, std::string_view deployment_id) {
    auto started = store.transition_deployment(
        deployment_id,
        {DeploymentState::Stopped, DeploymentState::Faulted},
        DeploymentState::Running,
        "started",
        true
    );
    REQUIRE(std::holds_alternative<ModelDeploymentRecord>(started));
    return std::get<ModelDeploymentRecord>(std::move(started));
}

// A decision on observation `sequence`, received at `receive`: a buy of
// one share, or a hold when `side` is empty.
NewDeploymentDecision decision(
    std::string deployment_id,
    std::int64_t generation,
    std::uint64_t sequence,
    std::int64_t receive,
    std::optional<Side> side = Side::Buy
) {
    return NewDeploymentDecision{
        .deployment_id = std::move(deployment_id),
        .run_generation = generation,
        .stream = kStream,
        .instrument = kAapl,
        .observation_ingest_sequence = sequence,
        .observation_event = Timestamp::from_nanoseconds(receive - 1),
        .observation_receive = Timestamp::from_nanoseconds(receive),
        .decided_at_market = Timestamp::from_nanoseconds(receive),
        .inputs = {1, -2, 0, 500'000'000, 400'000'000},
        .predicted_class = side ? "up" : "flat",
        .probabilities = {"0.1", "0.2", "0.7"},
        .confidence_ppb = 700'000'000,
        .position_before = quantity("0"),
        .target = side ? std::optional(quantity("1")) : std::nullopt,
        .policy_reason = side ? "order_scheduled" : "hold_flat_prediction",
        .side = side,
        .quantity = side ? std::optional(quantity("1")) : std::nullopt,
    };
}

DeploymentDecisionRecord record(PostgresPaperOrderRepository& store, const NewDeploymentDecision& wanted) {
    auto recorded = store.record_decision(wanted);
    if (const auto* conflict = std::get_if<DeploymentConflict>(&recorded)) {
        throw chronos::test::TestFailure("decision refused: " + conflict->message);
    }
    return std::get<DeploymentDecisionRecord>(std::move(recorded));
}

TopOfBookEvent market(std::uint64_t sequence, std::int64_t receive) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = kAapl,
            .stream = kStream,
            .venue = chronos::VenueId("XNAS"),
            .event_timestamp = Timestamp::from_nanoseconds(receive - 1),
            .receive_timestamp = Timestamp::from_nanoseconds(receive),
            .ingest_sequence = sequence,
            .venue_sequence = sequence + 1'000,
            .quality = chronos::MarketDataQuality{}.with(chronos::MarketDataQualityFlag::Replay),
        },
        .bid = BookLevel{.price = price("99"), .quantity = quantity("5")},
        .ask = BookLevel{.price = price("101"), .quantity = quantity("5")},
    };
}

OrderIntent model_order(const ModelDeploymentRecord& deployment, const DeploymentDecisionRecord& pending, std::int64_t at) {
    return OrderIntent{
        .client_order_id = *pending.client_order_id,
        .account_id = deployment.config.account_id,
        .source = OrderSource{.kind = OrderSourceKind::Model, .source_id = deployment.config.deployment_id},
        .instrument = kAapl,
        .side = *pending.decision.side,
        .type = OrderType::Market,
        .quantity = *pending.decision.quantity,
        .limit_price = std::nullopt,
        .time_in_force = TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(at),
    };
}

enum class Check {
    // As the service's trusted path: only a decision still pending.
    Pending,
    // Commits whatever the decision's state, to show the database refuses.
    Bypassed,
};

// Submits the decision's order on `quote` and commits it with the decision.
CommitPaperOrderResult submit_decision(
    PostgresPaperOrderRepository& store,
    const ModelDeploymentRecord& deployment,
    const DeploymentDecisionRecord& pending,
    const TopOfBookEvent& quote,
    Check check = Check::Pending
) {
    std::optional<PaperAccountLock> locked = store.lock_account(deployment.config.account_id);
    REQUIRE(locked.has_value());
    REQUIRE(locked->owning_deployment() == deployment.config.deployment_id);
    const auto held = locked->lock_deployment_decision(deployment.config.deployment_id, pending.decision_sequence);
    REQUIRE(held.has_value());
    if (check == Check::Pending) {
        REQUIRE(held->decision.state == DecisionState::Pending);
    }
    const OrderIntent intent = model_order(deployment, pending, quote.header.receive_timestamp.nanoseconds());
    const PaperOrderResult result = locked->processor().submit(intent, quote);
    const WorkingOrderMatch earlier{.observation = quote, .fills = {}, .skipped = {}, .fault = std::nullopt};
    return locked->commit_deployment_submission(
        earlier,
        intent,
        quote,
        result,
        deployment.config.deployment_id,
        pending.decision_sequence,
        DeploymentOrderResolution{
            .outcome = std::holds_alternative<PaperOrderReceipt>(result) ? "filled" : "rejected",
            .rejected = !std::holds_alternative<PaperOrderReceipt>(result),
            .execution_ingest_sequence = quote.header.ingest_sequence,
            .execution_receive = quote.header.receive_timestamp,
            .resolved_at_market = quote.header.receive_timestamp,
        }
    );
}

DeploymentDecisionRecord only_decision(PostgresPaperOrderRepository& store, std::string_view deployment_id, std::int64_t sequence) {
    const auto page = store.list_decisions(deployment_id, 200, std::nullopt);
    for (const auto& found : page.decisions) {
        if (found.decision_sequence == sequence) {
            return found;
        }
    }
    throw chronos::test::TestFailure("no such decision");
}

std::size_t order_count(PostgresTestConnection& inspector, std::string_view account_id) {
    return std::stoul(inspector.scalar(
        "SELECT count(*) FROM paper_order_records WHERE account_id = $1",
        {std::string(account_id)}
    ));
}

void test_the_generic_commit_cannot_leave_a_pending_decision_with_an_order() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    const auto deployment = create(store, config("dep-1", "model-1"));
    start(store, "dep-1");
    const auto pending = record(store, decision("dep-1", 1, 1, 100));
    const auto quote = market(2, 110);
    auto locked = store.lock_account("model-1");
    REQUIRE(locked.has_value());
    // A generic writer can claim the correct source and guess the pending
    // ID, but it must not commit an order without resolving the decision.
    const auto intent = model_order(deployment, pending, 110);
    const auto result = locked->processor().submit(intent, quote);
    const WorkingOrderMatch earlier{.observation = quote, .fills = {}, .skipped = {}, .fault = std::nullopt};
    bool refused = false;
    try {
        static_cast<void>(locked->commit_submission(earlier, intent, quote, result));
    } catch (const std::exception&) {
        refused = true;
    }
    REQUIRE(refused);
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(order_count(inspector, "model-1") == 0);
    REQUIRE(only_decision(store, "dep-1", 1).state == DecisionState::Pending);
    REQUIRE(inspector.scalar("SELECT cash_raw FROM paper_accounts WHERE account_id = 'model-1'") ==
        std::to_string(money("1000").raw()));
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_fills") == "0");
}

// ---------------------------------------------------------------------------

void test_a_deployment_is_created_stopped_with_its_own_account() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    const ModelDeploymentRecord created = create(store, config("dep-1", "model-1"));
    REQUIRE(created.state == DeploymentState::Stopped);
    REQUIRE(created.state_reason == "created");
    REQUIRE(created.run_generation == 0);
    REQUIRE(created.next_decision_sequence == 1);
    REQUIRE(!created.watermark_receive);
    REQUIRE(!created.pending_decision);
    REQUIRE(created.config.starting_cash == money("1000"));
    REQUIRE(created.config.risk_limits.maximum_position_quantity == quantity("1"));
    REQUIRE(created.config.training_cutoff == Timestamp::from_nanoseconds(100));
    REQUIRE(created.created_at.ends_with("Z"));
    REQUIRE(store.account_owner("model-1") == std::optional<std::string>("dep-1"));
    const auto account = PersistentPaperOrderSession::load(store, "model-1");
    REQUIRE(account.has_value());
    REQUIRE(account->account().state().cash == money("1000"));
    REQUIRE(account->version() == 0);

    // The identical request again is the same deployment, not a new one.
    auto again = store.create_deployment(config("dep-1", "model-1"));
    REQUIRE(std::holds_alternative<CreatedDeployment>(again));
    REQUIRE(!std::get<CreatedDeployment>(again).created);
    REQUIRE(std::get<CreatedDeployment>(again).deployment.creation_sequence == created.creation_sequence);

    // A different request for the same ID is refused, with what exists.
    ModelDeploymentConfig different = config("dep-1", "model-1");
    different.request_sha256 = std::string(64, 'b');
    const auto exists = require_conflict(store.create_deployment(different), DeploymentConflictCode::DeploymentExists);
    REQUIRE(exists.deployment.has_value());
    REQUIRE(exists.deployment->config.request_sha256 == std::string(64, 'a'));
}

void test_a_deployment_never_takes_an_existing_account() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(std::holds_alternative<PersistentPaperOrderSession>(
        PersistentPaperOrderSession::create(store, "manual", money("1000"), config("x", "y").risk_limits)
    ));
    require_conflict(store.create_deployment(config("dep-1", "manual")), DeploymentConflictCode::AccountExists);
    REQUIRE(!store.account_owner("manual"));
    REQUIRE(!store.find_deployment("dep-1"));

    create(store, config("dep-2", "model-2"));
    ModelDeploymentConfig second = config("dep-3", "model-2");
    second.request_sha256 = std::string(64, 'c');
    require_conflict(store.create_deployment(second), DeploymentConflictCode::AccountExists);
    REQUIRE(!store.find_deployment("dep-3"));
    REQUIRE(inspector.scalar("SELECT count(*) FROM model_deployments") == "1");
}

void test_concurrent_identical_creates_make_one_deployment() {
    const ScratchSchema schema;
    std::vector<std::string> outcomes(4);
    std::vector<std::thread> creators;
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        creators.emplace_back([&schema, &outcomes, index] {
            try {
                PostgresPaperOrderRepository store(schema.connection());
                auto created = store.create_deployment(config("dep-1", "model-1"));
                outcomes[index] = std::holds_alternative<CreatedDeployment>(created)
                    ? (std::get<CreatedDeployment>(created).created ? "created" : "existing")
                    : std::get<DeploymentConflict>(created).message;
            } catch (const std::exception& error) {
                outcomes[index] = error.what();
            }
        });
    }
    for (auto& creator : creators) {
        creator.join();
    }
    std::size_t created = 0;
    for (const auto& outcome : outcomes) {
        if (outcome == "created") {
            ++created;
        } else if (outcome != "existing") {
            throw chronos::test::TestFailure("unexpected create outcome: " + outcome);
        }
    }
    REQUIRE(created == 1);
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar("SELECT count(*) FROM model_deployments") == "1");
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_accounts") == "1");
}

void test_transitions_follow_the_lifecycle() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    create(store, config("dep-1", "model-1"));
    const ModelDeploymentRecord running = start(store, "dep-1");
    REQUIRE(running.state == DeploymentState::Running);
    REQUIRE(running.state_reason == "started");
    REQUIRE(running.run_generation == 1);

    // A start of a running deployment is not a second run.
    const auto again = require_conflict(
        store.transition_deployment("dep-1", {DeploymentState::Stopped}, DeploymentState::Running, "started", true),
        DeploymentConflictCode::InvalidTransition
    );
    REQUIRE(again.deployment->run_generation == 1);

    auto stopping = store.transition_deployment(
        "dep-1", {DeploymentState::Running}, DeploymentState::Stopping, "stop_requested", false
    );
    REQUIRE(std::get<ModelDeploymentRecord>(stopping).run_generation == 1);
    auto stopped = store.transition_deployment(
        "dep-1", {DeploymentState::Stopping}, DeploymentState::Stopped, "stop_requested", false
    );
    REQUIRE(std::get<ModelDeploymentRecord>(stopped).state == DeploymentState::Stopped);
    REQUIRE(start(store, "dep-1").run_generation == 2);
    require_conflict(
        store.transition_deployment("nobody", {DeploymentState::Stopped}, DeploymentState::Running, "started", true),
        DeploymentConflictCode::DeploymentNotFound
    );
}

void test_decisions_advance_a_durable_watermark() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    create(store, config("dep-1", "model-1"));
    // Only a running deployment decides.
    require_conflict(store.record_decision(decision("dep-1", 1, 5, 200, std::nullopt)), DeploymentConflictCode::DecisionRefused);
    start(store, "dep-1");
    require_conflict(store.record_decision(decision("dep-1", 2, 5, 200, std::nullopt)), DeploymentConflictCode::DecisionRefused);

    const DeploymentDecisionRecord hold = record(store, decision("dep-1", 1, 5, 200, std::nullopt));
    REQUIRE(hold.decision_sequence == 1);
    REQUIRE(hold.state == DecisionState::Held);
    REQUIRE(!hold.client_order_id);
    // The same observation, or an earlier one, cannot be decided again.
    require_conflict(store.record_decision(decision("dep-1", 1, 5, 200, std::nullopt)), DeploymentConflictCode::DecisionRefused);
    require_conflict(store.record_decision(decision("dep-1", 1, 4, 200, std::nullopt)), DeploymentConflictCode::DecisionRefused);
    require_conflict(store.record_decision(decision("dep-1", 1, 9, 199, std::nullopt)), DeploymentConflictCode::DecisionRefused);

    const DeploymentDecisionRecord pending = record(store, decision("dep-1", 1, 6, 300));
    REQUIRE(pending.decision_sequence == 2);
    REQUIRE(pending.state == DecisionState::Pending);
    REQUIRE(pending.client_order_id == std::optional<std::string>("dep-1-000000000002"));
    REQUIRE(pending.not_before_receive == Timestamp::from_nanoseconds(310));
    REQUIRE(pending.expires_at_market == Timestamp::from_nanoseconds(1'300));
    // One unresolved decision at a time.
    require_conflict(store.record_decision(decision("dep-1", 1, 7, 400, std::nullopt)), DeploymentConflictCode::DecisionRefused);

    // A restart, or a replay that starts over, reads the same watermark.
    PostgresPaperOrderRepository restarted(schema.connection());
    const auto found = restarted.find_deployment("dep-1");
    REQUIRE(found->watermark_receive == Timestamp::from_nanoseconds(300));
    REQUIRE(found->watermark_ingest_sequence == std::optional<std::uint64_t>(6));
    REQUIRE(found->next_decision_sequence == 3);
    REQUIRE(found->pending_decision == std::optional<std::int64_t>(2));
    const auto pendings = restarted.pending_decisions();
    REQUIRE(pendings.size() == 1);
    REQUIRE(pendings[0].decision_sequence == 2);
    REQUIRE(pendings[0].decision.inputs == decision("dep-1", 1, 6, 300).inputs);
    REQUIRE(pendings[0].decision.probabilities == decision("dep-1", 1, 6, 300).probabilities);
    // A new run cannot begin over an unresolved decision.
    static_cast<void>(restarted.transition_deployment(
        "dep-1", {DeploymentState::Running}, DeploymentState::Stopped, "server_restarted", false
    ));
    require_conflict(
        restarted.transition_deployment("dep-1", {DeploymentState::Stopped}, DeploymentState::Running, "started", true),
        DeploymentConflictCode::InvalidTransition
    );
}

void test_a_deployment_order_and_its_decision_commit_together() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    create(store, config("dep-1", "model-1"));
    const ModelDeploymentRecord running = start(store, "dep-1");
    const DeploymentDecisionRecord pending = record(store, decision("dep-1", 1, 6, 300));

    const CommitPaperOrderResult committed = submit_decision(store, running, pending, market(7, 310));
    REQUIRE(std::get<std::int64_t>(committed) == 1);
    const DeploymentDecisionRecord resolved = only_decision(store, "dep-1", 1);
    REQUIRE(resolved.state == DecisionState::Recorded);
    REQUIRE(resolved.resolution == std::optional<std::string>("filled"));
    REQUIRE(resolved.execution_ingest_sequence == std::optional<std::uint64_t>(7));
    REQUIRE(resolved.execution_receive == Timestamp::from_nanoseconds(310));
    const auto deployment = store.find_deployment("dep-1");
    REQUIRE(deployment->orders_this_run == 1);
    REQUIRE(deployment->consecutive_rejections == 0);
    REQUIRE(!deployment->pending_decision);
    const auto order = store.find_order("model-1", *pending.client_order_id);
    REQUIRE(std::holds_alternative<PaperOrderRecord>(order));
    REQUIRE(std::get<PaperOrderRecord>(order).intent.source.kind == OrderSourceKind::Model);
    REQUIRE(PersistentPaperOrderSession::load(store, "model-1")->account().state().quantity(kAapl) == quantity("1"));

    // The decision has its one order: even bypassing the pending check,
    // submitting it again records nothing.
    bool refused = false;
    try {
        static_cast<void>(submit_decision(store, running, pending, market(8, 320), Check::Bypassed));
    } catch (const std::exception& error) {
        refused = std::string_view(error.what()).find("belongs to model deployment dep-1") != std::string_view::npos;
    }
    REQUIRE(refused);
    REQUIRE(order_count(inspector, "model-1") == 1);
    REQUIRE(only_decision(store, "dep-1", 1).execution_ingest_sequence == std::optional<std::uint64_t>(7));
}

void test_a_failure_before_the_commit_records_neither() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    create(store, config("dep-1", "model-1"));
    const ModelDeploymentRecord running = start(store, "dep-1");
    const DeploymentDecisionRecord pending = record(store, decision("dep-1", 1, 6, 300));
    // A failpoint between the order's rows and the decision's resolution.
    inspector.run_script(R"SQL(
        CREATE FUNCTION fail_resolution() RETURNS trigger LANGUAGE plpgsql AS $$
        BEGIN
            RAISE EXCEPTION 'injected resolution failure';
        END
        $$;
        CREATE TRIGGER fail_resolution BEFORE UPDATE ON model_deployment_decisions
            FOR EACH ROW WHEN (NEW.state = 'recorded') EXECUTE FUNCTION fail_resolution();
    )SQL");
    bool failed = false;
    try {
        static_cast<void>(submit_decision(store, running, pending, market(7, 310)));
    } catch (const std::exception& error) {
        failed = std::string_view(error.what()).find("injected resolution failure") != std::string_view::npos;
    }
    REQUIRE(failed);
    REQUIRE(order_count(inspector, "model-1") == 0);
    REQUIRE(PersistentPaperOrderSession::load(store, "model-1")->version() == 0);
    REQUIRE(only_decision(store, "dep-1", 1).state == DecisionState::Pending);
    REQUIRE(store.find_deployment("dep-1")->orders_this_run == 0);

    inspector.run_script("DROP TRIGGER fail_resolution ON model_deployment_decisions");
    REQUIRE(std::get<std::int64_t>(submit_decision(store, running, pending, market(8, 320))) == 1);
    REQUIRE(order_count(inspector, "model-1") == 1);
}

// The connection is lost with the order written but not committed: the
// outcome is uncertain to the server, and the decision settles it.
void test_a_lost_connection_leaves_the_decision_pending_without_an_order() {
    const ScratchSchema schema;
    const std::string application = chronos::test::unique_test_id("deployment-lost");
    PostgresPaperOrderRepository store(with_application_name(schema.connection(), application));
    PostgresTestConnection inspector(schema.connection());
    create(store, config("dep-1", "model-1"));
    const ModelDeploymentRecord running = start(store, "dep-1");
    const DeploymentDecisionRecord pending = record(store, decision("dep-1", 1, 6, 300));

    // Holding the deployment row makes the resolution wait, after the order
    // rows were written, until the connection is terminated.
    PostgresTestConnection holder(schema.connection());
    holder.run_script("BEGIN; SELECT 1 FROM model_deployments WHERE deployment_id = 'dep-1' FOR UPDATE");
    std::string outcome = "running";
    std::thread submitter([&] {
        try {
            std::optional<PaperAccountLock> locked = store.lock_account("model-1");
            const TopOfBookEvent quote = market(7, 310);
            const OrderIntent intent = model_order(running, pending, 310);
            const PaperOrderResult result = locked->processor().submit(intent, quote);
            static_cast<void>(locked->commit_deployment_submission(
                WorkingOrderMatch{.observation = quote, .fills = {}, .skipped = {}, .fault = std::nullopt},
                intent,
                quote,
                result,
                "dep-1",
                pending.decision_sequence,
                DeploymentOrderResolution{
                    .outcome = "filled",
                    .rejected = false,
                    .execution_ingest_sequence = 7,
                    .execution_receive = Timestamp::from_nanoseconds(310),
                    .resolved_at_market = Timestamp::from_nanoseconds(310),
                }
            ));
            outcome = "committed";
        } catch (const PaperPersistenceUnavailable&) {
            outcome = "unavailable";
        } catch (const std::exception& error) {
            outcome = error.what();
        }
    });
    wait_until_blocked_on_lock(inspector, application);
    terminate_backends(inspector, application);
    submitter.join();
    holder.run_script("ROLLBACK");
    REQUIRE(outcome == "unavailable");

    // Reconciliation reads the decision: pending means no order exists.
    PostgresPaperOrderRepository restarted(schema.connection());
    REQUIRE(only_decision(restarted, "dep-1", 1).state == DecisionState::Pending);
    REQUIRE(order_count(inspector, "model-1") == 0);
    REQUIRE(std::holds_alternative<chronos::PaperJournalReadMiss>(
        restarted.find_order("model-1", *pending.client_order_id)
    ));
}

void test_an_abandoned_decision_can_never_record_an_order() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    create(store, config("dep-1", "model-1"));
    const ModelDeploymentRecord running = start(store, "dep-1");
    const DeploymentDecisionRecord pending = record(store, decision("dep-1", 1, 6, 300));
    {
        std::optional<PaperAccountLock> locked = store.lock_account("model-1");
        REQUIRE(locked->lock_deployment_decision("dep-1", 1).has_value());
        locked->commit_deployment_abandonment("dep-1", 1, "signal_expired", Timestamp::from_nanoseconds(1'301));
    }
    const DeploymentDecisionRecord abandoned = only_decision(store, "dep-1", 1);
    REQUIRE(abandoned.state == DecisionState::Abandoned);
    REQUIRE(abandoned.resolution == std::optional<std::string>("signal_expired"));
    REQUIRE(abandoned.resolved_at_market == Timestamp::from_nanoseconds(1'301));
    REQUIRE(!abandoned.execution_ingest_sequence);
    // The trusted path checks the decision under the lock; bypassing that,
    // the database still refuses its order, with or without a resolution.
    bool refused = false;
    try {
        static_cast<void>(submit_decision(store, running, pending, market(7, 310), Check::Bypassed));
    } catch (const std::exception& error) {
        refused = std::string_view(error.what()).find("belongs to model deployment dep-1") != std::string_view::npos;
    }
    REQUIRE(refused);
    {
        std::optional<PaperAccountLock> locked = store.lock_account("model-1");
        const TopOfBookEvent quote = market(7, 310);
        const OrderIntent intent = model_order(running, pending, 310);
        const PaperOrderResult result = locked->processor().submit(intent, quote);
        bool guarded = false;
        try {
            static_cast<void>(locked->commit_submission(
                WorkingOrderMatch{.observation = quote, .fills = {}, .skipped = {}, .fault = std::nullopt},
                intent,
                quote,
                result
            ));
        } catch (const std::exception& error) {
            guarded = std::string_view(error.what()).find("belongs to model deployment dep-1") != std::string_view::npos;
        }
        REQUIRE(guarded);
    }
    REQUIRE(order_count(inspector, "model-1") == 0);
    // The deployment decides again.
    REQUIRE(record(store, decision("dep-1", 1, 8, 400)).state == DecisionState::Pending);
}

// The global lock order: a submission holds the account, then the
// deployment; a decision being recorded waits for the deployment and
// never holds anything the submission needs.
void test_a_submission_and_a_new_decision_serialize_without_deadlock() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    create(store, config("dep-1", "model-1"));
    const ModelDeploymentRecord running = start(store, "dep-1");
    const DeploymentDecisionRecord pending = record(store, decision("dep-1", 1, 6, 300));

    std::optional<PaperAccountLock> locked = store.lock_account("model-1");
    REQUIRE(locked->lock_deployment_decision("dep-1", 1).has_value());
    const std::string application = chronos::test::unique_test_id("deployment-decider");
    std::string outcome = "running";
    std::thread decider([&] {
        try {
            PostgresPaperOrderRepository deciding(with_application_name(schema.connection(), application));
            auto recorded = deciding.record_decision(decision("dep-1", 1, 9, 500));
            outcome = std::holds_alternative<DeploymentDecisionRecord>(recorded)
                ? "recorded"
                : std::get<DeploymentConflict>(recorded).message;
        } catch (const std::exception& error) {
            outcome = error.what();
        }
    });
    wait_until_blocked_on_lock(inspector, application);
    const TopOfBookEvent quote = market(7, 310);
    const OrderIntent intent = model_order(running, pending, 310);
    const PaperOrderResult result = locked->processor().submit(intent, quote);
    REQUIRE(std::holds_alternative<std::int64_t>(locked->commit_deployment_submission(
        WorkingOrderMatch{.observation = quote, .fills = {}, .skipped = {}, .fault = std::nullopt},
        intent,
        quote,
        result,
        "dep-1",
        1,
        DeploymentOrderResolution{
            .outcome = "filled",
            .rejected = false,
            .execution_ingest_sequence = 7,
            .execution_receive = Timestamp::from_nanoseconds(310),
            .resolved_at_market = Timestamp::from_nanoseconds(310),
        }
    )));
    decider.join();
    // It saw the resolved decision, so it could record the next.
    REQUIRE(outcome == "recorded");
    REQUIRE(store.find_deployment("dep-1")->pending_decision == std::optional<std::int64_t>(2));
}

void test_rejections_and_orders_are_counted_per_run() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    create(store, config("dep-1", "model-1"));
    ModelDeploymentRecord running = start(store, "dep-1");
    // A buy the account cannot afford is rejected by risk, and counted.
    DeploymentDecisionRecord first = record(store, decision("dep-1", 1, 6, 300));
    TopOfBookEvent expensive = market(7, 310);
    expensive.ask->price = price("600");
    REQUIRE(std::holds_alternative<std::int64_t>(submit_decision(store, running, first, expensive)));
    auto deployment = store.find_deployment("dep-1");
    REQUIRE(deployment->orders_this_run == 1);
    REQUIRE(deployment->consecutive_rejections == 1);
    REQUIRE(only_decision(store, "dep-1", 1).resolution == std::optional<std::string>("rejected"));
    DeploymentDecisionRecord second = record(store, decision("dep-1", 1, 8, 400));
    REQUIRE(std::holds_alternative<std::int64_t>(submit_decision(store, running, second, market(9, 410))));
    deployment = store.find_deployment("dep-1");
    REQUIRE(deployment->orders_this_run == 2);
    REQUIRE(deployment->consecutive_rejections == 0);
    // A new run starts its counts over; the watermark carries on.
    static_cast<void>(store.transition_deployment(
        "dep-1", {DeploymentState::Running}, DeploymentState::Stopped, "stop_requested", false
    ));
    running = start(store, "dep-1");
    REQUIRE(running.orders_this_run == 0);
    REQUIRE(running.run_generation == 2);
    REQUIRE(running.watermark_receive == Timestamp::from_nanoseconds(400));
}

void test_deployments_and_decisions_page_newest_first() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    for (int index = 1; index <= 5; ++index) {
        ModelDeploymentConfig wanted = config("dep-" + std::to_string(index), "model-" + std::to_string(index));
        create(store, wanted);
    }
    auto first = store.list_deployments(2, std::nullopt);
    REQUIRE(first.deployments.size() == 2);
    REQUIRE(first.deployments[0].config.deployment_id == "dep-5");
    REQUIRE(first.deployments[1].config.deployment_id == "dep-4");
    REQUIRE(first.next_before_sequence.has_value());
    // A deployment created between pages does not shift the next page.
    create(store, config("dep-6", "model-6"));
    auto second = store.list_deployments(2, first.next_before_sequence);
    REQUIRE(second.deployments[0].config.deployment_id == "dep-3");
    REQUIRE(second.deployments[1].config.deployment_id == "dep-2");
    auto third = store.list_deployments(2, second.next_before_sequence);
    REQUIRE(third.deployments.size() == 1);
    REQUIRE(!third.next_before_sequence);

    start(store, "dep-1");
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        record(store, decision("dep-1", 1, sequence, 200 + static_cast<std::int64_t>(sequence), std::nullopt));
    }
    auto decisions = store.list_decisions("dep-1", 2, std::nullopt);
    REQUIRE(decisions.decisions.size() == 2);
    REQUIRE(decisions.decisions[0].decision_sequence == 3);
    auto older = store.list_decisions("dep-1", 2, decisions.next_before_sequence);
    REQUIRE(older.decisions.size() == 1);
    REQUIRE(older.decisions[0].decision_sequence == 1);
    REQUIRE(!older.next_before_sequence);
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr || std::string_view(configured_connection).empty()) {
        std::cout << "PostgreSQL model deployment test skipped: CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {"the generic commit cannot leave a pending decision with an order", test_the_generic_commit_cannot_leave_a_pending_decision_with_an_order},
        {"a deployment is created stopped with its own account", test_a_deployment_is_created_stopped_with_its_own_account},
        {"a deployment never takes an existing account", test_a_deployment_never_takes_an_existing_account},
        {"concurrent identical creates make one deployment", test_concurrent_identical_creates_make_one_deployment},
        {"transitions follow the lifecycle", test_transitions_follow_the_lifecycle},
        {"decisions advance a durable watermark", test_decisions_advance_a_durable_watermark},
        {"a deployment order and its decision commit together", test_a_deployment_order_and_its_decision_commit_together},
        {"a failure before the commit records neither", test_a_failure_before_the_commit_records_neither},
        {
            "a lost connection leaves the decision pending without an order",
            test_a_lost_connection_leaves_the_decision_pending_without_an_order,
        },
        {"an abandoned decision can never record an order", test_an_abandoned_decision_can_never_record_an_order},
        {
            "a submission and a new decision serialize without deadlock",
            test_a_submission_and_a_new_decision_serialize_without_deadlock,
        },
        {"rejections and orders are counted per run", test_rejections_and_orders_are_counted_per_run},
        {"deployments and decisions page newest first", test_deployments_and_decisions_page_newest_first},
    });
}
