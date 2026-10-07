// Model deployments end to end through the server's wiring (ADR 0018): the
// feed, the market-event gate, the hub with the matcher before it, the
// service's trusted path, and PostgreSQL, with a scripted inference
// provider in place of the Python worker. Covers the chronology rules, the
// policy, ownership, Stop and the kill switch, restart recovery, and the
// failures the brief requires: inference, persistence, staleness, halts,
// duplicate controls, uncertain submissions, and shutdown.
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/paper_trading/deployment_http.hpp"
#include "chronos/paper_trading/deployment_runtime.hpp"
#include "chronos/paper_trading/paper_trading_http_contract.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "chronos/paper_trading/working_order_matcher.hpp"
#include "market_data_test_support.hpp"
#include "deployment_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace market_data = chronos::market_data;
using chronos::DecisionState;
using chronos::DeploymentState;
using chronos::InstrumentId;
using chronos::MarketEvent;
using chronos::MarketStatusEvent;
using chronos::Money;
using chronos::PostgresPaperOrderRepository;
using chronos::Quantity;
using chronos::Side;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradingStatus;
using chronos::paper_trading::DeploymentCreateRequest;
using chronos::paper_trading::DeploymentCreated;
using chronos::paper_trading::DeploymentError;
using chronos::paper_trading::DeploymentErrorCode;
using chronos::paper_trading::DeploymentHub;
using chronos::paper_trading::DeploymentManager;
using chronos::paper_trading::DeploymentRunCounters;
using chronos::paper_trading::DeploymentRuntimeOptions;
using chronos::paper_trading::DeploymentStatus;
using chronos::paper_trading::InferenceFailure;
using chronos::paper_trading::InferenceFailureCode;
using chronos::paper_trading::InferencePrediction;
using chronos::paper_trading::InferenceProvider;
using chronos::paper_trading::InferenceRequest;
using chronos::paper_trading::InferenceSession;
using chronos::paper_trading::ModelIdentity;
using chronos::paper_trading::ModelSelection;
using chronos::paper_trading::PaperAccountView;
using chronos::paper_trading::PaperTradingError;
using chronos::paper_trading::PaperTradingErrorCode;
using chronos::paper_trading::PaperTradingService;
using chronos::paper_trading::WorkingOrderMatcher;
using chronos::paper_trading::WorkingOrderMatcherOptions;
using chronos::test::Answer;
using chronos::test::FakeModel;
using chronos::test::FakeProvider;
using chronos::test::predicted;
using chronos::strategy::PredictedClass;
using chronos::test::kMarketOpenNanoseconds;
using chronos::test::PostgresTestConnection;
using chronos::test::QuoteLevels;
using chronos::test::terminate_backends;
using chronos::test::wait_until;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;

std::string connection_string;

constexpr std::int64_t kMillisecond = 1'000'000;
// Training ends 10 ms after the open.
constexpr std::int64_t kCutoffOffset = 10 * kMillisecond;

Money money(std::string_view value) { return Money::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

// ---------------------------------------------------------------------------
// The server's wiring on a scratch schema

class ScratchSchema final : public chronos::test::ScratchSchema {
public:
    ScratchSchema() : chronos::test::ScratchSchema(connection_string) {
        PostgresPaperOrderRepository(connection()).apply_migration(chronos::test::all_migrations_sql());
    }
};

DeploymentRuntimeOptions runtime_options() {
    return DeploymentRuntimeOptions{
        .stream = chronos::MarketDataStreamId(std::string(chronos::test::kTestStream)),
        .tradeable_instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
        .account_risk_limits = chronos::test::paper_risk_limits(),
        .model_load_timeout = std::chrono::milliseconds(1'000),
        .inference_timeout = std::chrono::milliseconds(200),
        .idle_wake = std::chrono::milliseconds(5),
    };
}

struct Rig {
    Rig(
        const ScratchSchema& schema,
        std::shared_ptr<FakeModel> model_,
        std::string service_application = "deployment-service",
        bool enabled = true,
        DeploymentManager::Logger log = {}
    )
        : model(std::move(model_)),
          market(chronos::test::market_state()),
          notifier(clock, market_data::MarketDataNotifier::Options{}),
          matcher(
              std::make_unique<PostgresPaperOrderRepository>(schema.connection()),
              WorkingOrderMatcherOptions{
                  .first_retry_delay = std::chrono::milliseconds(5),
                  .maximum_retry_delay = std::chrono::milliseconds(20),
              },
              [] {},
              [](std::string_view) {}
          ),
          hub(&matcher),
          feed(*market, notifier, 0, &hub, &gate),
          service(
              std::make_unique<PostgresPaperOrderRepository>(
                  with_application_name(schema.connection(), service_application)
              ),
              *market,
              chronos::test::paper_service_config(),
              {},
              &gate
          ),
          manager(
              service,
              hub,
              enabled ? std::unique_ptr<InferenceProvider>(std::make_unique<FakeProvider>(model)) : nullptr,
              with_source(runtime_options()),
              std::move(log)
          ) {
        service.set_trading_status_view(&hub);
    }

    // Publishes an AAPL quote received `offset` after the open.
    void quote(std::int64_t offset, std::string_view ask = "101", std::string instrument = "AAPL") {
        quote_levels(offset, QuoteLevels{.ask_price = ask}, std::move(instrument));
    }

    void quote_levels(std::int64_t offset, QuoteLevels levels, std::string instrument = "AAPL") {
        publish(MarketEvent(chronos::test::quote(next_sequence, offset, std::move(instrument), levels)));
    }

    // The source the status reports, as the server reads its own.
    DeploymentRuntimeOptions with_source(DeploymentRuntimeOptions options) {
        options.source_status = [this] {
            if (status_read_hook) {
                status_read_hook();
            }
            market_data::MarketDataSourceStatus status;
            status.state = source_state.load();
            return status;
        };
        return options;
    }

    void status(std::int64_t offset, TradingStatus value) {
        const std::int64_t received = kMarketOpenNanoseconds + offset;
        publish(MarketEvent(MarketStatusEvent{
            .header = chronos::MarketEventHeader{
                .instrument = InstrumentId("AAPL"),
                .stream = chronos::MarketDataStreamId(std::string(chronos::test::kTestStream)),
                .venue = std::nullopt,
                .event_timestamp = Timestamp::from_nanoseconds(received - 250),
                .receive_timestamp = Timestamp::from_nanoseconds(received),
                .ingest_sequence = next_sequence,
                .venue_sequence = std::nullopt,
                .quality = chronos::MarketDataQuality{}.with(chronos::MarketDataQualityFlag::Replay),
            },
            .status = value,
            .reason = "test",
        }));
    }

    void publish(MarketEvent event) {
        const auto rejected = feed.apply(chronos::test::validated(std::move(event)));
        if (rejected) {
            throw chronos::test::TestFailure("feed rejected an event: " + *rejected);
        }
        ++next_sequence;
    }

    DeploymentStatus status_of(std::string_view id = "dep-1") {
        auto found = manager.get(id);
        if (const auto* failed = std::get_if<DeploymentError>(&found)) {
            throw chronos::test::TestFailure("get failed: " + failed->message);
        }
        return std::get<DeploymentStatus>(std::move(found));
    }

    DeploymentRunCounters counters(std::string_view id = "dep-1") {
        auto status = status_of(id);
        REQUIRE(status.run.has_value());
        return *status.run;
    }

    template <typename Condition>
    void wait_for(Condition condition, std::string_view description) {
        wait_until(condition, description);
    }

    PaperAccountView account(std::string_view id = "model-1") {
        auto found = service.get_account(id);
        REQUIRE(std::holds_alternative<PaperAccountView>(found));
        return std::get<PaperAccountView>(std::move(found));
    }

    std::shared_ptr<FakeModel> model;
    market_data::SteadyClock clock;
    std::unique_ptr<chronos::MarketState> market;
    market_data::MarketDataNotifier notifier;
    market_data::MarketEventGate gate;
    WorkingOrderMatcher matcher;
    DeploymentHub hub;
    market_data::MarketStateFeed feed;
    PaperTradingService service;
    // Delivering unless a test says otherwise.
    std::atomic<market_data::MarketDataSourceState> source_state{market_data::MarketDataSourceState::Running};
    std::function<void()> status_read_hook;
    DeploymentManager manager;
    std::uint64_t next_sequence{0};
};

DeploymentCreateRequest create_request(std::string id = "dep-1", std::string account = "model-1") {
    return DeploymentCreateRequest{
        .deployment_id = std::move(id),
        .account_id = std::move(account),
        .starting_cash = money("10000"),
        .experiment_id = std::string(64, 'e'),
        .model = "logistic",
        .instrument = "AAPL",
        .threshold_ppb = 600'000'000,
        .target_quantity = quantity("1"),
        .decision_interval_ns = kMillisecond,
        .latency_ns = 2 * kMillisecond,
        .max_signal_age_ns = 100 * kMillisecond,
        .max_orders_per_run = 10,
        .max_consecutive_rejections = 3,
    };
}

DeploymentStatus created(Rig& rig, DeploymentCreateRequest request = create_request()) {
    auto made = rig.manager.create(request);
    if (const auto* failed = std::get_if<DeploymentError>(&made)) {
        throw chronos::test::TestFailure("create failed: " + failed->message);
    }
    return std::get<DeploymentCreated>(std::move(made)).status;
}

DeploymentStatus started(Rig& rig, std::string_view id = "dep-1") {
    auto running = rig.manager.start(id);
    if (const auto* failed = std::get_if<DeploymentError>(&running)) {
        throw chronos::test::TestFailure("start failed: " + failed->message);
    }
    return std::get<DeploymentStatus>(std::move(running));
}

std::vector<chronos::DeploymentDecisionRecord> decisions(Rig& rig, std::string_view id = "dep-1") {
    auto page = rig.manager.decisions(id, 200, std::nullopt);
    REQUIRE(std::holds_alternative<chronos::DecisionPage>(page));
    return std::get<chronos::DecisionPage>(std::move(page)).decisions;
}

std::string scalar(const ScratchSchema& schema, std::string_view sql) {
    PostgresTestConnection inspector(schema.connection());
    return inspector.scalar(sql);
}

std::uint64_t skipped(const DeploymentRunCounters& counters, const std::string& reason) {
    const auto found = counters.skipped.find(reason);
    return found == counters.skipped.end() ? 0 : found->second;
}

std::uint64_t waited(const DeploymentRunCounters& counters, const std::string& reason) {
    const auto found = counters.waits.find(reason);
    return found == counters.waits.end() ? 0 : found->second;
}

// ---------------------------------------------------------------------------

void test_nothing_trades_before_the_cutoff_and_orders_wait_for_the_latency() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    REQUIRE(started(rig).deployment.run_generation == 1);

    rig.quote(kCutoffOffset - kMillisecond);
    rig.wait_for([&] { return skipped(rig.counters(), "before_training_cutoff") == 1; }, "a candidate before the cutoff");
    REQUIRE(rig.model->requests.load() == 0);

    // The signal: an up prediction on the quote at the cutoff.
    rig.quote(kCutoffOffset, "101");
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "an order decision");
    // A later quote, but sooner than the latency: not eligible.
    rig.quote(kCutoffOffset + kMillisecond, "102");
    rig.wait_for([&] { return waited(rig.counters(), "awaiting_latency") > 0; }, "a wait for the latency");
    REQUIRE(rig.counters().orders_recorded == 0);
    // At the latency: the order executes against this quote, not the
    // signal's.
    const std::uint64_t execution_sequence = rig.next_sequence;
    rig.quote(kCutoffOffset + 2 * kMillisecond, "103");
    rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "a recorded order");

    const auto made = decisions(rig);
    REQUIRE(made.size() == 1);
    const auto& decision = made.front();
    REQUIRE(decision.state == DecisionState::Recorded);
    REQUIRE(decision.resolution == std::optional<std::string>("filled"));
    REQUIRE(decision.decision.observation_ingest_sequence == 1);
    REQUIRE(decision.execution_ingest_sequence == std::optional<std::uint64_t>(execution_sequence));
    REQUIRE(decision.execution_receive->nanoseconds() - decision.decision.observation_receive.nanoseconds() >=
        2 * kMillisecond);
    REQUIRE(decision.client_order_id == std::optional<std::string>("dep-1-000000000001"));
    const PaperAccountView account = rig.account();
    REQUIRE(account.state.quantity(InstrumentId("AAPL")) == quantity("1"));
    // 10000 - 103 - 0.01: the execution quote's ask and the fee.
    REQUIRE(account.state.cash == money("9896.99"));
    REQUIRE(scalar(schema, "SELECT source_kind || ':' || source_id FROM paper_order_records") == "model:dep-1");
}

void test_the_policy_never_exceeds_the_target_or_goes_short() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    started(rig);
    std::int64_t at = kCutoffOffset;
    std::int64_t last_observation_ns = 0;
    const auto step = [&](std::string_view ask = "101") {
        last_observation_ns = kMarketOpenNanoseconds + at;
        rig.quote(at, ask);
        at += 3 * kMillisecond;
    };
    // An execution quote can itself produce a hold. A counter increasing
    // does not prove the newest quote was consumed before the fake model
    // changes; wait for that exact durable observation instead.
    const auto wait_for_latest_decision = [&] {
        rig.wait_for([&] {
            const auto watermark = rig.status_of().deployment.watermark_receive;
            return watermark && watermark->nanoseconds() == last_observation_ns;
        }, "the decision on the latest quoted observation");
    };
    step();
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
    step();
    rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "the buy");
    // More up predictions hold at the target.
    for (int index = 0; index < 3; ++index) {
        step();
        wait_for_latest_decision();
        REQUIRE(decisions(rig).front().decision.policy_reason == "already_at_target");
    }
    REQUIRE(rig.account().state.quantity(InstrumentId("AAPL")) == quantity("1"));
    // Down sells to flat, and further downs hold flat.
    rig.model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Down); });
    step();
    rig.wait_for([&] { return rig.counters().order_decisions == 2; }, "the sell decision");
    step();
    rig.wait_for([&] { return rig.counters().orders_recorded == 2; }, "the sell");
    step();
    wait_for_latest_decision();
    REQUIRE(decisions(rig).front().decision.policy_reason == "already_at_target");
    REQUIRE(rig.account().state.quantity(InstrumentId("AAPL")) == quantity("0"));
    // A confident flat prediction and an unconfident up one both hold.
    rig.model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Up, 500'000'000); });
    step();
    wait_for_latest_decision();
    REQUIRE(decisions(rig).front().decision.policy_reason == "hold_below_threshold");
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "2");
}

void test_manual_requests_cannot_trade_an_owned_account() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    rig.quote(kCutoffOffset);
    // Claiming the model source, even the deployment's own ID, is no
    // authority on the manual path.
    for (const auto kind : {chronos::OrderSourceKind::Manual, chronos::OrderSourceKind::Model}) {
        auto request = chronos::test::order_request("model-1", "manual-1", Side::Buy, "1",
            chronos::OrderType::Market, std::nullopt, kind);
        request.source.source_id = "dep-1";
        auto refused = rig.service.submit_order(request);
        REQUIRE(std::holds_alternative<PaperTradingError>(refused));
        REQUIRE(std::get<PaperTradingError>(refused).code == PaperTradingErrorCode::AccountOwnedByDeployment);
    }
    auto cancelled = rig.service.cancel_order(chronos::paper_trading::CancelPaperOrderRequest{
        .account_id = "model-1",
        .client_order_id = "dep-1-000000000001",
        .cancel_request_id = "cancel-1",
    });
    REQUIRE(std::get<PaperTradingError>(cancelled).code == PaperTradingErrorCode::AccountOwnedByDeployment);
    // Other accounts trade as before, concurrently with a running
    // deployment.
    auto manual = rig.service.create_account({.account_id = "manual", .starting_cash = money("1000")});
    REQUIRE(std::holds_alternative<PaperAccountView>(manual));
    started(rig);
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    REQUIRE(std::holds_alternative<chronos::paper_trading::RecordedPaperOrder>(
        rig.service.submit_order(chronos::test::order_request("manual", "manual-1", Side::Buy, "1"))
    ));
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the deployment's decision");
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records WHERE account_id = 'model-1'") == "0");
}

void test_stop_keeps_the_position_and_abandons_the_pending_decision() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "the buy");
    // Drain the post-fill candidate while the model still says Up. Waiting
    // for the fill alone does not prove the mailbox is empty; changing the
    // prediction then can turn that older quote into an unintended sell.
    rig.quote(kCutoffOffset + 6 * kMillisecond);
    rig.wait_for([&] {
        return rig.status_of().deployment.watermark_receive == Timestamp::from_nanoseconds(
            kMarketOpenNanoseconds + kCutoffOffset + 6 * kMillisecond
        );
    }, "a post-fill hold to consume the last quote");
    rig.model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Down); });
    rig.quote(kCutoffOffset + 9 * kMillisecond);
    rig.wait_for([&] { return rig.counters().order_decisions == 2; }, "a pending sell");

    // Stop before any quote makes the sell eligible.
    auto stopped = rig.manager.stop("dep-1");
    REQUIRE(std::holds_alternative<DeploymentStatus>(stopped));
    const auto& status = std::get<DeploymentStatus>(stopped);
    REQUIRE(status.deployment.state == DeploymentState::Stopped);
    REQUIRE(status.deployment.state_reason == "stop_requested");
    REQUIRE(!status.active);
    REQUIRE(rig.model->open_sessions.load() == 0);
    const auto sell = decisions(rig).front();
    REQUIRE(sell.state == DecisionState::Abandoned);
    REQUIRE(sell.resolution == std::optional<std::string>("stop_requested"));
    // Nothing is liquidated, and later quotes trade nothing.
    rig.quote(kCutoffOffset + 12 * kMillisecond);
    REQUIRE(rig.account().state.quantity(InstrumentId("AAPL")) == quantity("1"));
    // A second stop changes nothing.
    auto again = rig.manager.stop("dep-1");
    REQUIRE(std::get<DeploymentStatus>(again).deployment.version == status.deployment.version);
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "1");
}

void test_the_kill_switch_stops_everything_and_is_idempotent() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig, create_request("dep-1", "model-1"));
    created(rig, create_request("dep-2", "model-2"));
    created(rig, create_request("dep-3", "model-3"));
    started(rig, "dep-1");
    started(rig, "dep-2");
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters("dep-1").order_decisions == 1 && rig.counters("dep-2").order_decisions == 1; },
        "both decisions");
    auto killed = rig.manager.kill_switch();
    REQUIRE(std::holds_alternative<std::vector<DeploymentStatus>>(killed));
    REQUIRE(std::get<std::vector<DeploymentStatus>>(killed).size() == 2);
    for (const char* id : {"dep-1", "dep-2"}) {
        const auto status = rig.status_of(id);
        REQUIRE(status.deployment.state == DeploymentState::Stopped);
        REQUIRE(status.deployment.state_reason == "kill_switch");
        REQUIRE(!status.active);
        REQUIRE(decisions(rig, id).front().resolution == std::optional<std::string>("kill_switch"));
    }
    REQUIRE(rig.status_of("dep-3").deployment.state_reason == "created");
    REQUIRE(std::get<std::vector<DeploymentStatus>>(rig.manager.kill_switch()).empty());
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "0");
    REQUIRE(rig.model->open_sessions.load() == 0);
}

void test_duplicate_creates_starts_and_stops_are_idempotent() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    auto again = rig.manager.create(create_request());
    REQUIRE(!std::get<DeploymentCreated>(again).created);
    DeploymentCreateRequest different = create_request();
    different.threshold_ppb = 700'000'000;
    REQUIRE(std::get<DeploymentError>(rig.manager.create(different)).code == DeploymentErrorCode::Conflict);
    REQUIRE(started(rig).deployment.run_generation == 1);
    REQUIRE(started(rig).deployment.run_generation == 1);
    REQUIRE(rig.model->open_sessions.load() == 1);
    static_cast<void>(rig.manager.stop("dep-1"));
    static_cast<void>(rig.manager.stop("dep-1"));
    REQUIRE(started(rig).deployment.run_generation == 2);
    // Invalid requests are refused before anything is verified or written.
    DeploymentCreateRequest big = create_request("dep-9", "model-9");
    big.target_quantity = quantity("2");
    REQUIRE(std::get<DeploymentError>(rig.manager.create(big)).field == "target_quantity");
    DeploymentCreateRequest slow = create_request("dep-9", "model-9");
    slow.max_signal_age_ns = slow.latency_ns;
    REQUIRE(std::get<DeploymentError>(rig.manager.create(slow)).field == "max_signal_age_ms");
}

void test_the_control_route_name_is_not_a_deployment_id() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    const auto made = rig.manager.create(create_request("kill-switch", "reserved-account"));
    REQUIRE(std::holds_alternative<DeploymentError>(made));
    REQUIRE(std::get<DeploymentError>(made).field == "deployment_id");
    REQUIRE(scalar(schema, "SELECT count(*) FROM model_deployments") == "0");
}

void test_active_workers_are_bounded_and_stopping_releases_a_slot() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    const auto capacity = runtime_options().maximum_active_deployments;
    for (std::size_t index = 0; index <= capacity; ++index) {
        created(rig, create_request("bounded-" + std::to_string(index), "bounded-account-" + std::to_string(index)));
        if (index < capacity) {
            started(rig, "bounded-" + std::to_string(index));
        }
    }
    REQUIRE(rig.model->open_sessions.load() == static_cast<int>(capacity));
    // A duplicate Start consumes no slot.
    REQUIRE(started(rig, "bounded-0").deployment.run_generation == 1);
    const auto extra_id = "bounded-" + std::to_string(capacity);
    const auto refused = rig.manager.start(extra_id);
    REQUIRE(std::holds_alternative<DeploymentError>(refused));
    REQUIRE(std::get<DeploymentError>(refused).reason == "deployment_capacity_reached");
    REQUIRE(rig.status_of(extra_id).deployment.run_generation == 0);
    REQUIRE(rig.model->open_sessions.load() == static_cast<int>(capacity));
    static_cast<void>(rig.manager.stop("bounded-0"));
    REQUIRE(started(rig, extra_id).deployment.run_generation == 1);
    REQUIRE(rig.model->open_sessions.load() == static_cast<int>(capacity));
}

void test_expiry_uses_the_execution_quotes_clock_not_an_earlier_status_read() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "a pending signal");
    class AdvancingStatus final : public chronos::paper_trading::TradingStatusView {
    public:
        explicit AdvancingStatus(chronos::MarketState& market) : market_(market) {}
        bool is_trading(const InstrumentId&) const override {
            // The live source advances its receive clock independently of
            // the event gate; force that change between the two reads.
            REQUIRE(!market_.advance_time(Timestamp::from_nanoseconds(
                kMarketOpenNanoseconds + kCutoffOffset + 101 * kMillisecond
            )));
            return true;
        }
    private:
        chronos::MarketState& market_;
    } status(*rig.market);
    rig.service.set_trading_status_view(&status);
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] {
        const auto rows = decisions(rig);
        return !rows.empty() && rows.front().state != DecisionState::Pending;
    }, "the signal to resolve");
    const auto resolved = decisions(rig).front();
    REQUIRE(resolved.state == DecisionState::Abandoned);
    REQUIRE(resolved.resolution == std::optional<std::string>("signal_expired"));
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "0");
    // Restore the long-lived view before the rig's shutdown.
    rig.service.set_trading_status_view(&rig.hub);
}

void test_a_bad_model_fails_closed() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    Rig rig(schema, model);
    model->describe_failure = InferenceFailure{.code = InferenceFailureCode::ModelRejected, .message = "unevaluable"};
    const auto refused = std::get<DeploymentError>(rig.manager.create(create_request()));
    REQUIRE(refused.code == DeploymentErrorCode::ModelUnavailable);
    REQUIRE(scalar(schema, "SELECT count(*) FROM model_deployments") == "0");
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_accounts") == "0");
    model->describe_failure.reset();
    created(rig);
    // The model changed after the deployment was created.
    ModelIdentity changed = model->identity;
    changed.parameters_sha256 = std::string(64, '0');
    model->served_identity = changed;
    const auto mismatch = std::get<DeploymentError>(rig.manager.start("dep-1"));
    REQUIRE(mismatch.reason == "model_identity_mismatch");
    REQUIRE(rig.status_of().deployment.state == DeploymentState::Faulted);
    REQUIRE(rig.status_of().deployment.state_reason == "model_identity_mismatch");
    REQUIRE(model->open_sessions.load() == 0);
    model->served_identity.reset();
    model->open_failure = InferenceFailure{.code = InferenceFailureCode::Timeout, .message = "slow"};
    REQUIRE(std::get<DeploymentError>(rig.manager.start("dep-1")).reason == "inference_timeout");
    model->open_failure.reset();
    // A faulted deployment starts again only when asked.
    REQUIRE(started(rig).deployment.state == DeploymentState::Running);
}

void test_inference_failures_fault_the_deployment() {
    for (const auto code : {InferenceFailureCode::Timeout, InferenceFailureCode::WorkerExited,
                            InferenceFailureCode::ProtocolError}) {
        const ScratchSchema schema;
        auto model = std::make_shared<FakeModel>();
        Rig rig(schema, model);
        created(rig);
        started(rig);
        model->answer_with([code](const InferenceRequest&) {
            return InferenceFailure{.code = code, .message = "scripted"};
        });
        rig.quote(kCutoffOffset);
        rig.wait_for([&] { return rig.status_of().deployment.state == DeploymentState::Faulted; }, "a fault");
        const auto status = rig.status_of();
        REQUIRE(status.deployment.state_reason == std::string(inference_failure_name(code)));
        rig.wait_for([&] { return !rig.status_of().active; }, "the runner to end");
        REQUIRE(model->open_sessions.load() == 0);
        REQUIRE(decisions(rig).empty());
        REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "0");
    }
}

void test_a_restart_never_resumes_and_never_reuses_an_observation() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    std::int64_t interrupted_sequence = 0;
    {
        Rig rig(schema, model);
        created(rig);
        started(rig);
        rig.quote(kCutoffOffset);
        rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
        rig.quote(kCutoffOffset + 3 * kMillisecond);
        rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "the buy");
        // The execution quote may still be queued after its buy commits.
        // Its prediction stays Up; only the following signal turns Down.
        rig.model->answer_with([](const InferenceRequest& request) {
            return predicted(request.ingest_sequence == 1 ? PredictedClass::Up : PredictedClass::Down);
        });
        rig.quote(kCutoffOffset + 6 * kMillisecond);
        rig.wait_for([&] { return rig.counters().order_decisions == 2; }, "a pending sell");
        const auto pending = decisions(rig).front();
        REQUIRE(pending.state == DecisionState::Pending);
        interrupted_sequence = pending.decision_sequence;
    }
    // The rig's shutdown stopped it cleanly. A crash leaves it running with
    // its decision pending, as here.
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar("SELECT state_reason FROM model_deployments") == "server_stopped");
    inspector.run_script(
        "UPDATE model_deployments SET state = 'running', state_reason = 'started';"
        "UPDATE model_deployment_decisions SET state = 'pending', resolution = NULL, resolved_at_market_ns = NULL,"
        " resolved_at = NULL WHERE decision_sequence = " + std::to_string(interrupted_sequence) + ";"
    );
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Up); });
    Rig restarted(schema, model);
    restarted.manager.recover();
    auto status = restarted.status_of();
    REQUIRE(status.deployment.state == DeploymentState::Stopped);
    REQUIRE(status.deployment.state_reason == "server_restarted");
    REQUIRE(!status.active);
    REQUIRE(decisions(restarted).front().resolution == std::optional<std::string>("server_restarted"));
    // Nothing trades until it is started.
    const int requests = model->requests.load();
    restarted.quote(kCutoffOffset - kMillisecond, "101", "SPY");
    REQUIRE(model->requests.load() == requests);
    REQUIRE(!restarted.status_of().active);
    // The replay starts over from the same recording: the same
    // observations, with the same sequences, are none of them decided
    // again.
    Rig replayed(schema, model);
    replayed.manager.recover();
    started(replayed);
    for (int index = 0; index < 3; ++index) {
        replayed.quote(kCutoffOffset + index * 3 * kMillisecond);
        replayed.wait_for([&] { return replayed.counters().candidates == static_cast<std::uint64_t>(index + 1); },
            "the replayed observation");
    }
    REQUIRE(skipped(replayed.counters(), "not_after_watermark") == 3);
    REQUIRE(model->requests.load() == requests);
    replayed.model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Down); });
    replayed.quote(kCutoffOffset + 9 * kMillisecond);
    replayed.wait_for([&] { return replayed.counters().order_decisions == 1; }, "a new decision");
    const auto latest = decisions(replayed).front();
    REQUIRE(latest.decision_sequence == interrupted_sequence + 1);
    const std::string digits = std::to_string(interrupted_sequence + 1);
    REQUIRE(latest.client_order_id == std::optional<std::string>("dep-1-" + std::string(12 - digits.size(), '0') + digits));
    REQUIRE(latest.decision.run_generation == 2);
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "1");
}

void test_a_stale_or_halted_market_expires_the_signal() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
    // Halted: no order, whatever the quotes.
    rig.status(kCutoffOffset + kMillisecond, TradingStatus::Halted);
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return waited(rig.counters(), "instrument_not_trading") > 0; }, "a wait for the halt");
    REQUIRE(rig.counters().orders_recorded == 0);
    // The market clock passes the signal's maximum age on another
    // instrument's quote: the decision is abandoned, not filled late.
    rig.quote(kCutoffOffset + 200 * kMillisecond, "101", "SPY");
    rig.wait_for([&] { return rig.counters().abandoned == 1; }, "the expired decision");
    REQUIRE(decisions(rig).front().resolution == std::optional<std::string>("signal_expired"));
    REQUIRE(rig.status_of().deployment.state == DeploymentState::Running);
    // While halted, the instrument's rows have no inputs.
    rig.quote(kCutoffOffset + 201 * kMillisecond);
    rig.wait_for([&] { return skipped(rig.counters(), "inputs_unavailable:trading_halted") == 1; }, "a halted row");
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "0");
}

// The service's connection is lost while a submission waits for a lock:
// the outcome is uncertain, the decision settles it, and the order is
// recorded exactly once when PostgreSQL is back.
void test_an_uncertain_submission_is_reconciled_from_the_decision() {
    const ScratchSchema schema;
    const std::string application = chronos::test::unique_test_id("deployment-uncertain");
    Rig rig(schema, std::make_shared<FakeModel>(), application);
    created(rig);
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
    PostgresTestConnection inspector(schema.connection());
    PostgresTestConnection holder(schema.connection());
    holder.run_script("BEGIN; SELECT 1 FROM model_deployments WHERE deployment_id = 'dep-1' FOR UPDATE");
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    wait_until_blocked_on_lock(inspector, application);
    terminate_backends(inspector, application);
    holder.run_script("ROLLBACK");
    rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "the reconciled order");
    REQUIRE(rig.counters().last_error.has_value());
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "1");
    REQUIRE(decisions(rig).front().state == DecisionState::Recorded);
}

// Stop arrives while the submission waits for the deployment's row: one of
// them wins, and either way a decision has at most one order.
void test_stop_during_a_submission_keeps_the_invariant() {
    const ScratchSchema schema;
    const std::string application = chronos::test::unique_test_id("deployment-stop-race");
    Rig rig(schema, std::make_shared<FakeModel>(), application);
    created(rig);
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
    PostgresTestConnection inspector(schema.connection());
    PostgresTestConnection holder(schema.connection());
    holder.run_script("BEGIN; SELECT 1 FROM model_deployments WHERE deployment_id = 'dep-1' FOR UPDATE");
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    wait_until_blocked_on_lock(inspector, application);
    std::thread stopper([&] { static_cast<void>(rig.manager.stop("dep-1")); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    holder.run_script("ROLLBACK");
    stopper.join();
    REQUIRE(rig.status_of().deployment.state == DeploymentState::Stopped);
    const auto decision = decisions(rig).front();
    const std::string orders = scalar(schema, "SELECT count(*) FROM paper_order_records");
    if (decision.state == DecisionState::Recorded) {
        REQUIRE(orders == "1");
    } else {
        REQUIRE(decision.state == DecisionState::Abandoned);
        REQUIRE(orders == "0");
    }
    REQUIRE(scalar(schema, "SELECT count(*) FROM model_deployment_decisions WHERE state = 'pending'") == "0");
}

// The database refuses the resolution after the order's rows: nothing is
// recorded, and the deployment faults rather than retrying blindly.
void test_a_failed_commit_faults_without_an_order() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the buy decision");
    PostgresTestConnection inspector(schema.connection());
    inspector.run_script(R"SQL(
        CREATE FUNCTION fail_resolution() RETURNS trigger LANGUAGE plpgsql AS $$
        BEGIN
            RAISE EXCEPTION 'injected resolution failure';
        END
        $$;
        CREATE TRIGGER fail_resolution BEFORE UPDATE ON model_deployment_decisions
            FOR EACH ROW WHEN (NEW.state = 'recorded') EXECUTE FUNCTION fail_resolution();
    )SQL");
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return rig.status_of().deployment.state == DeploymentState::Faulted; }, "the fault");
    REQUIRE(rig.status_of().deployment.state_reason == "runtime_error");
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "0");
    REQUIRE(decisions(rig).front().state == DecisionState::Abandoned);
    REQUIRE(decisions(rig).front().resolution == std::optional<std::string>("faulted"));
}

// A decision cannot be recorded while PostgreSQL is gone; nothing is
// pending, and the next candidate decides once it is back.
void test_a_lost_connection_before_a_decision_records_nothing() {
    const ScratchSchema schema;
    const std::string application = chronos::test::unique_test_id("deployment-before");
    auto model = std::make_shared<FakeModel>();
    std::atomic<bool> unavailable{false};
    Rig rig(schema, model, application, true, [&](std::string_view message) {
        if (message.find("the account could not be read") != std::string_view::npos ||
            message.find("a decision could not be recorded") != std::string_view::npos) {
            unavailable.store(true);
        }
    });
    created(rig);
    started(rig);
    PostgresTestConnection inspector(schema.connection());
    // The failpoint: inference itself ends the service's connection.
    std::atomic<bool> armed{true};
    model->answer_with([&](const InferenceRequest&) {
        if (armed.exchange(false)) {
            terminate_backends(inspector, application);
        }
        return predicted(PredictedClass::Up);
    });
    rig.quote(kCutoffOffset);
    // A status poll reconnects the same service connection; do not let the
    // test repair the failpoint before the runner has observed its failure.
    wait_until([&] { return unavailable.load(); }, "the runner to observe the lost connection");
    // The test's own reads share the lost connection, so one may fail too.
    const auto lost = [&] {
        try {
            const auto counters = rig.counters();
            return skipped(counters, "persistence_unavailable") + skipped(counters, "account_unavailable") == 1;
        } catch (const chronos::test::TestFailure&) {
            return false;
        }
    };
    rig.wait_for(lost, "the lost decision");
    REQUIRE(decisions(rig).empty());
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the next decision");
}

void test_limits_fault_the_deployment() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    DeploymentCreateRequest request = create_request();
    request.max_consecutive_rejections = 2;
    created(rig, request);
    started(rig);
    std::int64_t at = kCutoffOffset;
    // Asks the account cannot afford: every order is rejected by risk.
    for (int index = 0; index < 2; ++index) {
        rig.quote(at, "20000");
        rig.wait_for([&] { return rig.counters().order_decisions == static_cast<std::uint64_t>(index + 1); }, "a decision");
        rig.quote(at + 3 * kMillisecond, "20000");
        if (index == 0) {
            rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "the first rejection");
        }
        at += 6 * kMillisecond;
    }
    rig.wait_for([&] { return rig.status_of().deployment.state == DeploymentState::Faulted; }, "the limit");
    REQUIRE(rig.status_of().deployment.state_reason == "rejection_limit_reached");
    REQUIRE(rig.status_of().deployment.consecutive_rejections == 2);
    REQUIRE(decisions(rig).front().resolution->starts_with("risk_rejected:"));
}

void test_shutdown_stops_every_runner_and_worker() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    {
        Rig rig(schema, model);
        created(rig, create_request("dep-1", "model-1"));
        created(rig, create_request("dep-2", "model-2"));
        started(rig, "dep-1");
        started(rig, "dep-2");
        REQUIRE(model->open_sessions.load() == 2);
        rig.manager.shutdown();
        REQUIRE(model->open_sessions.load() == 0);
        REQUIRE(rig.status_of("dep-1").deployment.state_reason == "server_stopped");
        REQUIRE(rig.status_of("dep-2").deployment.state == DeploymentState::Stopped);
    }
    REQUIRE(model->open_sessions.load() == 0);
}


// A decision committed although its runner saw a failure: the database
// holds it pending and refuses the next one. The runner reads it back,
// adopts it, and records its one order, rather than stalling.
void test_a_decision_recorded_without_the_runner_knowing_is_adopted() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    Rig rig(schema, model);
    created(rig);
    started(rig);
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Flat); });
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().holds == 1; }, "a hold");
    PostgresPaperOrderRepository elsewhere(schema.connection());
    const auto uncertain = elsewhere.record_decision(chronos::NewDeploymentDecision{
        .deployment_id = "dep-1",
        .run_generation = 1,
        .stream = chronos::MarketDataStreamId(std::string(chronos::test::kTestStream)),
        .instrument = InstrumentId("AAPL"),
        .observation_ingest_sequence = 0,
        .observation_event = Timestamp::from_nanoseconds(kMarketOpenNanoseconds + kCutoffOffset + kMillisecond - 250),
        .observation_receive = Timestamp::from_nanoseconds(kMarketOpenNanoseconds + kCutoffOffset + kMillisecond),
        .decided_at_market = Timestamp::from_nanoseconds(kMarketOpenNanoseconds + kCutoffOffset + kMillisecond),
        .inputs = {200'000'000, 0, 0, 500'000'000, 500'000'000},
        .predicted_class = "up",
        .probabilities = {"0.05", "0.05", "0.9"},
        .confidence_ppb = 900'000'000,
        .position_before = quantity("0"),
        .target = quantity("1"),
        .policy_reason = "order_scheduled",
        .side = Side::Buy,
        .quantity = quantity("1"),
    });
    REQUIRE(std::holds_alternative<chronos::DeploymentDecisionRecord>(uncertain));
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Up); });
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return skipped(rig.counters(), "decision_refused") == 1; }, "the refused decision");
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "the adopted decision");
    rig.quote(kCutoffOffset + 6 * kMillisecond);
    rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "its order");
    const auto adopted = decisions(rig).front();
    REQUIRE(adopted.decision_sequence == 2);
    REQUIRE(adopted.state == DecisionState::Recorded);
    REQUIRE(scalar(schema, "SELECT count(*) FROM paper_order_records") == "1");
}

// ---------------------------------------------------------------------------
// The HTTP contract

using chronos::paper_trading::HttpRequest;
using chronos::paper_trading::HttpResponse;

struct Called {
    int status;
    nlohmann::json body;
};

Called call(Rig& rig, std::string method, std::string path, std::string body = {}, std::string query = {},
            std::string content_type = "application/json") {
    std::vector<std::string_view> parts;
    std::string_view rest = path;
    rest.remove_prefix(1);
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        parts.push_back(rest.substr(0, slash));
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    const HttpRequest request{
        .method = std::move(method),
        .path = path,
        .query = std::move(query),
        .content_type = body.empty() ? std::string{} : std::move(content_type),
        .body = std::move(body),
    };
    const HttpResponse response = chronos::paper_trading::deployment_route(
        rig.manager, request, parts, [](std::string_view) {}
    );
    return Called{.status = response.status, .body = nlohmann::json::parse(response.body)};
}

std::string create_body(std::string id = "dep-1", std::string account = "model-1", std::string target = "1") {
    return nlohmann::json{
        {"deployment_id", id},
        {"account_id", account},
        {"starting_cash", "10000"},
        {"experiment_id", std::string(64, 'e')},
        {"model", "logistic"},
        {"instrument", "AAPL"},
        {"threshold", "0.6"},
        {"target_quantity", target},
        {"decision_interval_ms", "1"},
        {"latency_ms", "2"},
        {"max_signal_age_ms", "100"},
        {"max_orders_per_run", "10"},
        {"max_consecutive_rejections", "3"},
    }.dump();
}

void test_the_http_routes_follow_the_contract() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    auto made = call(rig, "POST", "/api/v1/deployments", create_body());
    REQUIRE(made.status == 201);
    REQUIRE(made.body["created"] == true);
    const auto& deployment = made.body["deployment"];
    REQUIRE(deployment["state"] == "stopped");
    REQUIRE(deployment["state_reason"] == "created");
    REQUIRE(deployment["active"] == false);
    REQUIRE(deployment["model"]["parameters_sha256"] == std::string(64, 'f'));
    REQUIRE(deployment["model"]["training_cutoff"] == "2026-09-25T13:30:00.010000000Z");
    REQUIRE(deployment["policy"]["threshold"] == "0.6");
    REQUIRE(deployment["policy"]["target_quantity"] == "1");
    REQUIRE(deployment["timing"]["latency_ms"] == "2");
    REQUIRE(deployment["watermark"].is_null());
    REQUIRE(call(rig, "POST", "/api/v1/deployments", create_body()).status == 200);
    // Strict requests: wrong media type, unknown, duplicate, missing, or
    // non-string fields are malformed; domain rules are 422.
    REQUIRE(call(rig, "POST", "/api/v1/deployments", create_body("dep-2", "model-2"), {}, "text/plain").status == 415);
    auto extra = nlohmann::json::parse(create_body("dep-2", "model-2"));
    extra["leverage"] = "10";
    REQUIRE(call(rig, "POST", "/api/v1/deployments", extra.dump()).body["error"]["field"] == "leverage");
    std::string duplicated = create_body("dep-2", "model-2");
    duplicated.insert(1, "\"model\":\"majority\",");
    REQUIRE(call(rig, "POST", "/api/v1/deployments", duplicated).body["error"]["field"] == "model");
    auto number = nlohmann::json::parse(create_body("dep-2", "model-2"));
    number["latency_ms"] = 2;
    REQUIRE(call(rig, "POST", "/api/v1/deployments", number.dump()).status == 400);
    const auto too_big = call(rig, "POST", "/api/v1/deployments", create_body("dep-2", "model-2", "2"));
    REQUIRE(too_big.status == 422);
    REQUIRE(too_big.body["error"]["field"] == "target_quantity");
    auto reused = call(rig, "POST", "/api/v1/deployments", create_body("dep-2", "model-1"));
    REQUIRE(reused.status == 409);
    REQUIRE(reused.body["error"]["code"] == "account_exists");
    REQUIRE(call(rig, "POST", "/api/v1/deployments", create_body("dep-2", "model-2")).status == 201);

    // Newest first, with keyset cursors.
    auto first = call(rig, "GET", "/api/v1/deployments", {}, "limit=1");
    REQUIRE(first.status == 200);
    REQUIRE(first.body["enabled"] == true);
    REQUIRE(first.body["deployments"][0]["deployment_id"] == "dep-2");
    const std::string cursor = first.body["next_cursor"];
    auto second = call(rig, "GET", "/api/v1/deployments", {}, "limit=1&cursor=" + cursor);
    REQUIRE(second.body["deployments"][0]["deployment_id"] == "dep-1");
    REQUIRE(second.body["next_cursor"].is_null());
    REQUIRE(call(rig, "GET", "/api/v1/deployments", {}, "cursor=x1.1").body["error"]["code"] == "invalid_cursor");
    REQUIRE(call(rig, "GET", "/api/v1/deployments", {}, "limit=0").body["error"]["code"] == "invalid_page_limit");

    // Start, decisions, Stop, and the kill switch take an empty object.
    REQUIRE(call(rig, "POST", "/api/v1/deployments/dep-1/start", R"({"force":"1"})").status == 400);
    REQUIRE(call(rig, "GET", "/api/v1/deployments/dep-1/start").status == 405);
    auto running = call(rig, "POST", "/api/v1/deployments/dep-1/start", "{}");
    REQUIRE(running.status == 200);
    REQUIRE(running.body["deployment"]["state"] == "running");
    REQUIRE(running.body["deployment"]["active"] == true);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "a decision");
    auto made_decisions = call(rig, "GET", "/api/v1/deployments/dep-1/decisions");
    REQUIRE(made_decisions.status == 200);
    const auto& decision = made_decisions.body["decisions"][0];
    REQUIRE(decision["state"] == "pending");
    REQUIRE(decision["order"]["client_order_id"] == "dep-1-000000000001");
    REQUIRE(decision["order"]["not_before_received_at"] == "2026-09-25T13:30:00.012000000Z");
    REQUIRE(decision["prediction"]["confidence"] == "0.9");
    REQUIRE(decision["inputs"].size() == 5);

    // The owned account says so, and refuses manual orders with 409.
    const auto owned = nlohmann::json::parse(
        chronos::paper_trading::render_account(rig.service.get_account("model-1"), 200).body
    );
    REQUIRE(owned["account"]["owner"]["kind"] == "model_deployment");
    REQUIRE(owned["account"]["owner"]["deployment_id"] == "dep-1");
    REQUIRE(std::get<PaperAccountView>(rig.service.create_account({.account_id = "manual", .starting_cash = money("1")}))
                .owning_deployment == std::nullopt);
    const auto manual = nlohmann::json::parse(
        chronos::paper_trading::render_account(rig.service.get_account("manual"), 200).body
    );
    REQUIRE(manual["account"]["owner"].is_null());
    const auto refused = chronos::paper_trading::render_order_submission(rig.service.submit_order(
        chronos::test::order_request("model-1", "manual-1", Side::Buy, "1")
    ));
    REQUIRE(refused.status == 409);
    REQUIRE(nlohmann::json::parse(refused.body)["error"]["code"] == "account_owned_by_deployment");

    auto stopped = call(rig, "POST", "/api/v1/deployments/dep-1/stop", "{}");
    REQUIRE(stopped.body["deployment"]["state"] == "stopped");
    REQUIRE(stopped.body["deployment"]["run"]["order_decisions"] == "1");
    REQUIRE(call(rig, "GET", "/api/v1/deployments/dep-1/decisions").body["decisions"][0]["resolution"] == "stop_requested");
    auto killed = call(rig, "POST", "/api/v1/deployments/kill-switch", "{}");
    REQUIRE(killed.status == 200);
    REQUIRE(killed.body["stopped"].empty());
    REQUIRE(call(rig, "GET", "/api/v1/deployments/nobody").status == 404);
    REQUIRE(call(rig, "GET", "/api/v1/deployments/dep-1/orders").status == 404);
}

void test_decision_cursors_are_bound_to_their_deployment() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Flat); });
    Rig rig(schema, model);
    REQUIRE(call(rig, "POST", "/api/v1/deployments", create_body("dep.1", "model-1")).status == 201);
    REQUIRE(call(rig, "POST", "/api/v1/deployments", create_body("dep-2", "model-2")).status == 201);
    started(rig, "dep.1");
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters("dep.1").holds == 1; }, "the first held decision");
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return rig.counters("dep.1").holds == 2; }, "the second held decision");
    const auto first = call(rig, "GET", "/api/v1/deployments/dep.1/decisions", {}, "limit=1");
    const std::string cursor = first.body["next_cursor"];
    REQUIRE(first.body["decisions"][0]["decision_sequence"] == "2");
    const auto next = call(rig, "GET", "/api/v1/deployments/dep.1/decisions", {}, "limit=1&cursor=" + cursor);
    REQUIRE(next.status == 200);
    REQUIRE(next.body["decisions"][0]["decision_sequence"] == "1");
    REQUIRE(next.body["next_cursor"].is_null());
    const auto wrong = call(rig, "GET", "/api/v1/deployments/dep-2/decisions", {}, "cursor=" + cursor);
    REQUIRE(wrong.status == 400);
    REQUIRE(wrong.body["error"]["code"] == "invalid_cursor");
}

void test_page_queries_refuse_empty_parameters() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    for (const std::string path : {"/api/v1/deployments", "/api/v1/deployments/dep-1/decisions"}) {
        for (const std::string query : {"limit=1&", "&limit=1", "limit=1&&cursor=d1.1"}) {
            const auto rejected = call(rig, "GET", path, {}, query);
            REQUIRE(rejected.status == 400);
            REQUIRE(rejected.body["error"]["code"] == "malformed_request");
        }
    }
}

void test_without_the_opt_in_nothing_can_be_created_or_started() {
    const ScratchSchema schema;
    {
        Rig enabled(schema, std::make_shared<FakeModel>());
        created(enabled);
    }
    Rig rig(schema, std::make_shared<FakeModel>(), "deployment-service", false);
    REQUIRE(!rig.manager.enabled());
    auto refused = call(rig, "POST", "/api/v1/deployments", create_body("dep-2", "model-2"));
    REQUIRE(refused.status == 409);
    REQUIRE(refused.body["error"]["code"] == "deployments_disabled");
    REQUIRE(call(rig, "POST", "/api/v1/deployments/dep-1/start", "{}").body["error"]["code"] == "deployments_disabled");
    auto listed = call(rig, "GET", "/api/v1/deployments");
    REQUIRE(listed.body["enabled"] == false);
    REQUIRE(listed.body["deployments"].size() == 1);
    // Stop and the kill switch always work.
    REQUIRE(call(rig, "POST", "/api/v1/deployments/dep-1/stop", "{}").status == 200);
    REQUIRE(call(rig, "POST", "/api/v1/deployments/kill-switch", "{}").status == 200);
}


// ---------------------------------------------------------------------------
// What a deployment is doing, and how long it took (ADR 0019)

using chronos::paper_trading::DeploymentCondition;

DeploymentCondition condition_of(Rig& rig) { return rig.status_of().condition; }

Timestamp after_open(std::int64_t offset) { return Timestamp::from_nanoseconds(kMarketOpenNanoseconds + offset); }

// An order's execution quote can still be queued after the order commits,
// and then it is a candidate like any other. An interval longer than the
// latency skips it whichever way the runner and the feed interleave, so
// every decision below is on a quote the test chose.
constexpr std::int64_t kSpacedInterval = 5 * kMillisecond;

DeploymentCreateRequest spaced_request() {
    DeploymentCreateRequest request = create_request();
    request.decision_interval_ns = kSpacedInterval;
    return request;
}

void test_the_condition_follows_the_run_and_falls_back_to_the_lifecycle() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    {
        Rig rig(schema, model);
        created(rig, spaced_request());
        started(rig);
        // No quote yet: the instrument has no top of book.
        const auto before = condition_of(rig);
        REQUIRE(before.code == "quote_unavailable");
        REQUIRE(before.reason == std::optional<std::string>("no_top_of_book"));

        rig.quote(kCutoffOffset - kMillisecond);
        rig.wait_for([&] { return condition_of(rig).code == "before_training_cutoff"; }, "the cutoff wait");
        const auto cutoff = condition_of(rig);
        REQUIRE(cutoff.training_cutoff == std::optional(after_open(kCutoffOffset)));
        REQUIRE(cutoff.observation_receive == std::optional(after_open(kCutoffOffset - kMillisecond)));
        REQUIRE(cutoff.observed_at_market == std::optional(after_open(kCutoffOffset - kMillisecond)));
        REQUIRE(cutoff.observed_at_wall.has_value());

        rig.quote(kCutoffOffset);
        rig.wait_for([&] {
            const auto found = condition_of(rig);
            return found.code == "order_pending" && found.reason == std::optional<std::string>("awaiting_latency");
        }, "the latency wait");
        const auto pending = condition_of(rig);
        REQUIRE(pending.decision_sequence == std::optional<std::int64_t>(1));
        REQUIRE(pending.not_before_receive == std::optional(after_open(kCutoffOffset + 2 * kMillisecond)));
        REQUIRE(pending.expires_at == std::optional(after_open(kCutoffOffset + 100 * kMillisecond)));

        rig.quote(kCutoffOffset + 3 * kMillisecond);
        rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "the order");
        REQUIRE(condition_of(rig).code != "order_pending");

        model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Flat); });
        const std::int64_t held_at = kCutoffOffset + kSpacedInterval;
        rig.quote(held_at);
        rig.wait_for([&] { return rig.counters().holds == 1; }, "a hold");
        const auto watching = condition_of(rig);
        REQUIRE(watching.code == "watching");
        REQUIRE(watching.reason == std::optional<std::string>("decided_hold"));
        // Half the interval after the latest decision: the next eligible
        // receive time is exact.
        rig.quote(held_at + kSpacedInterval / 2);
        rig.wait_for([&] { return condition_of(rig).code == "waiting_for_decision_interval"; }, "the interval wait");
        REQUIRE(condition_of(rig).next_eligible_receive == std::optional(after_open(held_at + kSpacedInterval)));

        // Present market facts outrank the latest outcome.
        rig.source_state = market_data::MarketDataSourceState::Paused;
        REQUIRE(condition_of(rig).code == "source_paused");
        rig.source_state = market_data::MarketDataSourceState::Completed;
        REQUIRE(condition_of(rig).code == "source_completed");
        rig.source_state = market_data::MarketDataSourceState::Running;
        rig.quote(kCutoffOffset + 2'000 * kMillisecond, "101", "SPY");
        const auto stale = condition_of(rig);
        REQUIRE(stale.code == "quote_stale");
        REQUIRE(rig.status_of().market->quote.age_ns ==
            std::optional<std::int64_t>(kCutoffOffset + 2'000 * kMillisecond - held_at - kSpacedInterval / 2));
        rig.status(kCutoffOffset + 2'001 * kMillisecond, TradingStatus::Halted);
        REQUIRE(condition_of(rig).code == "instrument_not_trading");

        // And the lifecycle outranks everything.
        static_cast<void>(rig.manager.stop("dep-1"));
        const auto stopped = condition_of(rig);
        REQUIRE(stopped.code == "stopped");
        REQUIRE(stopped.reason == std::optional<std::string>("stop_requested"));
        // The ended run's latest outcomes stay readable in this process.
        REQUIRE(rig.status_of().activity.has_value());
    }
    // After a restart, only durable facts remain.
    Rig restarted(schema, model);
    restarted.manager.recover();
    const auto status = restarted.status_of();
    REQUIRE(!status.activity.has_value());
    REQUIRE(!status.run.has_value());
    REQUIRE(status.condition.code == "stopped");
    REQUIRE(status.condition.reason == std::optional<std::string>("stop_requested"));
    REQUIRE(decisions(restarted).size() == 2);
}

void test_quote_age_and_market_time_share_one_snapshot() {
    const ScratchSchema schema;
    Rig rig(schema, std::make_shared<FakeModel>());
    created(rig);
    rig.quote(kCutoffOffset);
    // Advance between the initial market-status read and the quote read.
    // No runner or scheduling race is needed to reproduce the inconsistency.
    const Timestamp later = Timestamp::from_nanoseconds(kMarketOpenNanoseconds + kCutoffOffset + kMillisecond);
    rig.status_read_hook = [&] { REQUIRE(!rig.market->advance_time(later)); };
    const auto status = rig.status_of();
    rig.status_read_hook = {};
    REQUIRE(status.market->market_time == std::optional(later));
    REQUIRE(status.market->quote.age_ns == std::optional(kMillisecond));
}

std::string duration_of(const nlohmann::json& timing, const char* name) {
    const auto& duration = timing.at(name);
    REQUIRE(duration.at("clock") == "market");
    return duration.contains("nanoseconds") ? "ns:" + duration.at("nanoseconds").get<std::string>()
                                            : "unavailable:" + duration.at("unavailable").get<std::string>();
}

void test_decision_timing_and_fills_are_exact_and_name_their_clock() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    Rig rig(schema, model);
    created(rig, spaced_request());
    started(rig);
    std::int64_t at = kCutoffOffset;
    // Each decision on one quote, executed on another 3 ms later; the next
    // decision is a full interval on.
    const auto decide_then_execute = [&](std::uint64_t decisions_made, QuoteLevels execution) {
        rig.quote(at);
        rig.wait_for([&] { return rig.counters().order_decisions == decisions_made; }, "an order decision");
        rig.quote_levels(at + 3 * kMillisecond, execution);
        rig.wait_for([&] { return rig.counters().orders_recorded == decisions_made; }, "its order");
        at += kSpacedInterval;
    };
    decide_then_execute(1, QuoteLevels{.ask_price = "101"});
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Down); });
    // Half a share of bid: the sell fills half and cancels the rest.
    decide_then_execute(2, QuoteLevels{.bid_price = "99", .bid_quantity = "0.5"});
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Up); });
    // An ask the account cannot afford: recorded, rejected, nothing booked.
    decide_then_execute(3, QuoteLevels{.ask_price = "20000"});
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Flat); });
    rig.quote(at);
    rig.wait_for([&] { return rig.counters().holds == 1; }, "a hold");

    const auto page = call(rig, "GET", "/api/v1/deployments/dep-1/decisions");
    REQUIRE(page.status == 200);
    const auto& rows = page.body.at("decisions");
    REQUIRE(rows.size() == 4);
    const auto& hold = rows[0];
    const auto& rejected = rows[1];
    const auto& partial = rows[2];
    const auto& filled = rows[3];

    REQUIRE(duration_of(filled.at("timing"), "signal_to_decision") == "ns:0");
    REQUIRE(duration_of(filled.at("timing"), "execution_quote_offset") == "ns:3000000");
    REQUIRE(duration_of(filled.at("timing"), "signal_to_resolution") == "ns:3000000");
    REQUIRE(duration_of(filled.at("timing"), "signal_to_fill") == "ns:3000000");
    REQUIRE(filled.at("fill") == nlohmann::json({
        {"count", "1"}, {"quantity", "1"}, {"notional", "101"}, {"fees", "0.01"}, {"price", "101"},
        {"first_filled_at", "2026-09-25T13:30:00.013000000Z"}, {"last_filled_at", "2026-09-25T13:30:00.013000000Z"},
    }));

    REQUIRE(partial.at("resolution") == "partially_filled_and_cancelled");
    REQUIRE(partial.at("fill").at("quantity") == "0.5");
    REQUIRE(partial.at("fill").at("price") == "99");
    REQUIRE(partial.at("fill").at("notional") == "49.5");
    REQUIRE(duration_of(partial.at("timing"), "signal_to_fill") == "ns:3000000");

    // Recorded is not filled.
    REQUIRE(rejected.at("state") == "recorded");
    REQUIRE(rejected.at("resolution").get<std::string>().starts_with("risk_rejected:"));
    REQUIRE(rejected.at("fill").is_null());
    REQUIRE(duration_of(rejected.at("timing"), "execution_quote_offset") == "ns:3000000");
    REQUIRE(duration_of(rejected.at("timing"), "signal_to_fill") == "unavailable:no_fill");

    REQUIRE(hold.at("state") == "held");
    REQUIRE(duration_of(hold.at("timing"), "signal_to_decision") == "ns:0");
    for (const char* name : {"execution_quote_offset", "signal_to_resolution", "signal_to_fill"}) {
        REQUIRE(duration_of(hold.at("timing"), name) == "unavailable:no_order");
    }

    // A pending decision, then one abandoned at an impossible time: the
    // durations say so instead of going negative or zero.
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Down); });
    at += kSpacedInterval;
    rig.quote(at);
    rig.wait_for([&] { return rig.counters().order_decisions == 4; }, "a pending sell");
    const auto pending = call(rig, "GET", "/api/v1/deployments/dep-1/decisions").body.at("decisions")[0];
    REQUIRE(pending.at("state") == "pending");
    for (const char* name : {"execution_quote_offset", "signal_to_resolution", "signal_to_fill"}) {
        REQUIRE(duration_of(pending.at("timing"), name) == "unavailable:not_resolved");
    }
    REQUIRE(rig.service.abandon_deployment_decision("dep-1", 5, "stopped", Timestamp::from_nanoseconds(0)));
    const auto abandoned = call(rig, "GET", "/api/v1/deployments/dep-1/decisions").body.at("decisions")[0];
    REQUIRE(abandoned.at("state") == "abandoned");
    REQUIRE(duration_of(abandoned.at("timing"), "signal_to_resolution") == "unavailable:inconsistent_clock");
    REQUIRE(duration_of(abandoned.at("timing"), "execution_quote_offset") == "unavailable:no_execution");
    REQUIRE(duration_of(abandoned.at("timing"), "signal_to_fill") == "unavailable:no_fill");
}

// Every row a status read could touch, in one fingerprint.
std::string durable_fingerprint(const ScratchSchema& schema) {
    return scalar(schema,
        "SELECT md5(string_agg(row_text, ',' ORDER BY row_text)) FROM ("
        "  SELECT model_deployments::text AS row_text FROM model_deployments"
        "  UNION ALL SELECT model_deployment_decisions::text FROM model_deployment_decisions"
        "  UNION ALL SELECT paper_accounts::text FROM paper_accounts"
        "  UNION ALL SELECT paper_order_records::text FROM paper_order_records"
        "  UNION ALL SELECT paper_fills::text FROM paper_fills) AS all_rows");
}

void test_status_reads_change_nothing() {
    const ScratchSchema schema;
    auto model = std::make_shared<FakeModel>();
    Rig rig(schema, model);
    created(rig, spaced_request());
    started(rig);
    rig.quote(kCutoffOffset);
    rig.wait_for([&] { return rig.counters().order_decisions == 1; }, "an order decision");
    rig.quote(kCutoffOffset + 3 * kMillisecond);
    rig.wait_for([&] { return rig.counters().orders_recorded == 1; }, "its order");
    model->answer_with([](const InferenceRequest&) { return predicted(PredictedClass::Flat); });
    rig.quote(kCutoffOffset + kSpacedInterval);
    rig.wait_for([&] { return rig.counters().holds == 1; }, "a hold");
    // Idle now: no pending order, and no new quote.
    const std::string before = durable_fingerprint(schema);
    for (int index = 0; index < 25; ++index) {
        REQUIRE(call(rig, "GET", "/api/v1/deployments/dep-1").status == 200);
        REQUIRE(call(rig, "GET", "/api/v1/deployments").status == 200);
        REQUIRE(call(rig, "GET", "/api/v1/deployments/dep-1/decisions").status == 200);
    }
    REQUIRE(durable_fingerprint(schema) == before);
    // The rendered resource carries the new facts.
    const auto deployment = call(rig, "GET", "/api/v1/deployments/dep-1").body.at("deployment");
    REQUIRE(deployment.at("condition").at("code") == "watching");
    REQUIRE(deployment.at("activity").at("last_candidate").at("code") == "decided_hold");
    REQUIRE(deployment.at("market").at("source_state") == "running");
    REQUIRE(deployment.at("market").at("quote").at("available") == true);
    REQUIRE(deployment.at("market").at("quote").at("maximum_age_ns") == "1000000000");
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr || std::string_view(configured_connection).empty()) {
        std::cout << "deployment runtime test skipped: CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {
            "nothing trades before the cutoff, and orders wait for the latency",
            test_nothing_trades_before_the_cutoff_and_orders_wait_for_the_latency,
        },
        {"the policy never exceeds the target or goes short", test_the_policy_never_exceeds_the_target_or_goes_short},
        {"manual requests cannot trade an owned account", test_manual_requests_cannot_trade_an_owned_account},
        {"stop keeps the position and abandons the pending decision", test_stop_keeps_the_position_and_abandons_the_pending_decision},
        {"the kill switch stops everything and is idempotent", test_the_kill_switch_stops_everything_and_is_idempotent},
        {"duplicate creates, starts, and stops are idempotent", test_duplicate_creates_starts_and_stops_are_idempotent},
        {"the control route name is not a deployment ID", test_the_control_route_name_is_not_a_deployment_id},
        {"decision cursors are bound to their deployment", test_decision_cursors_are_bound_to_their_deployment},
        {"page queries refuse empty parameters", test_page_queries_refuse_empty_parameters},
        {"active workers are bounded and stopping releases a slot", test_active_workers_are_bounded_and_stopping_releases_a_slot},
        {"expiry uses the execution quote's clock", test_expiry_uses_the_execution_quotes_clock_not_an_earlier_status_read},
        {"a bad model fails closed", test_a_bad_model_fails_closed},
        {"inference failures fault the deployment", test_inference_failures_fault_the_deployment},
        {"a restart never resumes and never reuses an observation", test_a_restart_never_resumes_and_never_reuses_an_observation},
        {"a stale or halted market expires the signal", test_a_stale_or_halted_market_expires_the_signal},
        {"an uncertain submission is reconciled from the decision", test_an_uncertain_submission_is_reconciled_from_the_decision},
        {"stop during a submission keeps the invariant", test_stop_during_a_submission_keeps_the_invariant},
        {"a failed commit faults without an order", test_a_failed_commit_faults_without_an_order},
        {"a lost connection before a decision records nothing", test_a_lost_connection_before_a_decision_records_nothing},
        {"limits fault the deployment", test_limits_fault_the_deployment},
        {"shutdown stops every runner and worker", test_shutdown_stops_every_runner_and_worker},
        {
            "a decision recorded without the runner knowing is adopted",
            test_a_decision_recorded_without_the_runner_knowing_is_adopted,
        },
        {"the HTTP routes follow the contract", test_the_http_routes_follow_the_contract},
        {
            "without the opt-in nothing can be created or started",
            test_without_the_opt_in_nothing_can_be_created_or_started,
        },
        {
            "the condition follows the run and falls back to the lifecycle",
            test_the_condition_follows_the_run_and_falls_back_to_the_lifecycle,
        },
        {
            "decision timing and fills are exact and name their clock",
            test_decision_timing_and_fills_are_exact_and_name_their_clock,
        },
        {"status reads change nothing", test_status_reads_change_nothing},
        {"quote age and market time share one snapshot", test_quote_age_and_market_time_share_one_snapshot},
    });
}
