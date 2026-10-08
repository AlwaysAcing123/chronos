// Durable model deployments and their decisions (migration 005, ADR 0018).
//
// Lock order, shared with every account mutation: the account row, then
// the deployment row, then a decision row. Operations that do not involve
// an account (create, transitions, recording a decision) lock only the
// deployment row, so no two transactions wait on each other in a cycle.

#include "chronos/postgresql_model_deployments.hpp"
#include "chronos/postgresql_paper_order_repository.hpp"

#include "postgres_support.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos {

std::string_view deployment_state_name(DeploymentState state) noexcept {
    switch (state) {
        case DeploymentState::Stopped: return "stopped";
        case DeploymentState::Running: return "running";
        case DeploymentState::Stopping: return "stopping";
        case DeploymentState::Faulted: return "faulted";
    }
    return "stopped";
}

std::optional<DeploymentState> deployment_state_from_name(std::string_view name) noexcept {
    for (DeploymentState state : {
             DeploymentState::Stopped, DeploymentState::Running, DeploymentState::Stopping, DeploymentState::Faulted,
         }) {
        if (deployment_state_name(state) == name) {
            return state;
        }
    }
    return std::nullopt;
}

std::string_view decision_state_name(DecisionState state) noexcept {
    switch (state) {
        case DecisionState::Held: return "held";
        case DecisionState::Pending: return "pending";
        case DecisionState::Recorded: return "recorded";
        case DecisionState::Abandoned: return "abandoned";
    }
    return "held";
}

namespace {

using namespace postgres_support;

// Rendered as RFC 3339 UTC with nine fractional digits, like every API time.
constexpr std::string_view kTimestampFormat =
    "to_char(%s AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US') || '000Z'";

std::string rendered(std::string_view column, std::string_view alias) {
    std::string format(kTimestampFormat);
    const auto at = format.find("%s");
    format.replace(at, 2, column);
    return format + " AS " + std::string(alias);
}

const std::string& deployment_select() {
    static const std::string select = std::string(R"SQL(
        SELECT
            d.deployment_id, d.creation_sequence, d.account_id, d.request_sha256,
            d.model_reference, d.experiment_id, d.model_version_id,
            d.parameters_sha256, d.model_name, d.training_cutoff_basis, d.instrument,
            d.threshold_ppb, d.target_quantity_raw, d.decision_interval_ns,
            d.latency_ns, d.max_signal_age_ns, d.max_orders_per_run,
            d.max_consecutive_rejections, d.training_cutoff_ns, d.stream_id,
            d.state, d.state_reason, )SQL") +
        rendered("d.state_changed_at", "state_changed_at") + ", " + rendered("d.created_at", "created_at") +
        R"SQL(,
            d.run_generation, d.next_decision_sequence, d.watermark_receive_ns,
            d.watermark_ingest_sequence, d.orders_this_run,
            d.consecutive_rejections, d.version,
            a.starting_cash_raw, a.maximum_order_quantity_raw,
            a.maximum_order_notional_raw, a.maximum_position_quantity_raw,
            a.maximum_quote_age_ns, a.fee_per_fill_raw,
            (
                SELECT p.decision_sequence
                FROM model_deployment_decisions AS p
                WHERE p.deployment_id = d.deployment_id AND p.state = 'pending'
            ) AS pending_decision
        FROM model_deployments AS d
        JOIN paper_accounts AS a ON a.account_id = d.account_id
    )SQL";
    return select;
}

// The decision columns, each prefixed with `table` (such as "d.").
std::string decision_columns(std::string_view table) {
    std::string columns;
    for (std::string_view column : {
             "deployment_id", "decision_sequence", "run_generation", "stream_id", "instrument",
             "observation_ingest_sequence", "observation_event_ns", "observation_receive_ns",
             "decided_at_market_ns", "inputs_raw", "predicted_class", "probabilities",
             "confidence_ppb", "position_before_raw", "target_raw", "policy_reason",
             "client_order_id", "side", "quantity_raw", "not_before_receive_ns",
             "expires_at_market_ns", "state", "resolution", "execution_ingest_sequence",
             "execution_receive_ns", "resolved_at_market_ns",
         }) {
        columns += std::string(table) + std::string(column) + ", ";
    }
    return columns + rendered(std::string(table) + "created_at", "created_at");
}

const std::string& decision_select() {
    static const std::string select =
        "SELECT " + decision_columns("") + " FROM model_deployment_decisions";
    return select;
}

// Decision pages also sum each decision's booked fills: through its order
// (the unique partial index on account and client order ID) to the order's
// fills (the unique index on order and fill index), so a page costs one
// bounded query. A held, pending, or abandoned decision has no order; a
// recorded one whose order booked nothing sums to no fill.
const std::string& decision_page_select() {
    static const std::string select = "SELECT " + decision_columns("d.") + R"SQL(,
            fills.fill_count, fills.fill_quantity_raw, fills.fill_notional_raw,
            fills.fill_fee_raw, fills.first_filled_at_ns, fills.last_filled_at_ns,
            fills.single_price_raw
        FROM model_deployment_decisions AS d
        JOIN model_deployments AS m ON m.deployment_id = d.deployment_id
        LEFT JOIN LATERAL (
            SELECT
                count(f.fill_record_id) AS fill_count,
                sum(f.quantity_raw) AS fill_quantity_raw,
                sum(f.notional_raw) AS fill_notional_raw,
                sum(f.fee_raw) AS fill_fee_raw,
                min(f.filled_at_ns) AS first_filled_at_ns,
                max(f.filled_at_ns) AS last_filled_at_ns,
                CASE WHEN count(f.fill_record_id) = 1 THEN min(f.price_raw) END AS single_price_raw
            FROM paper_order_records AS r
            JOIN paper_fills AS f ON f.order_record_id = r.order_record_id
            WHERE r.account_id = m.account_id
              AND r.client_order_id = d.client_order_id
              AND r.client_order_id <> ''
        ) AS fills ON true
    )SQL";
    return select;
}

std::int64_t int64_of(const PostgresResult& result, int row, std::string_view column) {
    return parse_integer<std::int64_t>(result.value(row, column), column);
}

std::uint64_t uint64_of(const PostgresResult& result, int row, std::string_view column) {
    return parse_integer<std::uint64_t>(result.value(row, column), column);
}

std::int32_t int32_of(const PostgresResult& result, int row, std::string_view column) {
    return parse_integer<std::int32_t>(result.value(row, column), column);
}

// A text[] of values that need no quoting: digits, signs, dots, exponents.
std::string array_literal(const std::vector<std::string>& values) {
    std::string literal = "{";
    for (std::size_t index = 0; index < values.size(); ++index) {
        for (char character : values[index]) {
            const bool plain = (character >= '0' && character <= '9') || character == '-' || character == '+' ||
                character == '.' || character == 'e' || character == 'E';
            if (!plain) {
                throw std::invalid_argument("an array value must be a plain number");
            }
        }
        if (values[index].empty()) {
            throw std::invalid_argument("an array value must not be empty");
        }
        literal += (index == 0 ? "" : ",") + values[index];
    }
    return literal + "}";
}

std::vector<std::string> array_values(const std::string& literal, std::string_view column) {
    if (literal.size() < 2 || literal.front() != '{' || literal.back() != '}') {
        throw std::runtime_error("invalid array in persisted field: " + std::string(column));
    }
    std::vector<std::string> values;
    std::string current;
    for (std::size_t index = 1; index + 1 < literal.size(); ++index) {
        if (literal[index] == ',') {
            values.push_back(current);
            current.clear();
        } else if (literal[index] == '"') {
            throw std::runtime_error("unexpected quoted array value in persisted field: " + std::string(column));
        } else {
            current += literal[index];
        }
    }
    values.push_back(current);
    return values;
}

ModelDeploymentRecord parse_deployment(const PostgresResult& result, int row) {
    const std::string state_name = result.value(row, "state");
    const auto state = deployment_state_from_name(state_name);
    if (!state) {
        throw std::runtime_error("invalid deployment state: " + state_name);
    }
    ModelDeploymentRecord record;
    ModelDeploymentConfig& config = record.config;
    config.deployment_id = result.value(row, "deployment_id");
    config.account_id = result.value(row, "account_id");
    config.starting_cash = Money::from_raw(int64_of(result, row, "starting_cash_raw"));
    const std::string reference = result.value(row, "model_reference");
    if (reference == "experiment") {
        config.model = ExperimentModel{
            .experiment_id = result.value(row, "experiment_id"),
            .name = result.value(row, "model_name"),
        };
    } else if (reference == "registered") {
        config.model = RegisteredModel{.model_version_id = result.value(row, "model_version_id")};
    } else {
        throw std::runtime_error("invalid deployment model reference: " + reference);
    }
    config.family = result.value(row, "model_name");
    config.parameters_sha256 = result.value(row, "parameters_sha256");
    config.training_cutoff_basis = result.value(row, "training_cutoff_basis");
    config.instrument = InstrumentId(result.value(row, "instrument"));
    config.training_cutoff = Timestamp::from_nanoseconds(int64_of(result, row, "training_cutoff_ns"));
    config.threshold_ppb = uint64_of(result, row, "threshold_ppb");
    config.target_quantity = Quantity::from_raw(int64_of(result, row, "target_quantity_raw"));
    config.decision_interval_ns = int64_of(result, row, "decision_interval_ns");
    config.latency_ns = int64_of(result, row, "latency_ns");
    config.max_signal_age_ns = int64_of(result, row, "max_signal_age_ns");
    config.max_orders_per_run = int32_of(result, row, "max_orders_per_run");
    config.max_consecutive_rejections = int32_of(result, row, "max_consecutive_rejections");
    config.stream = MarketDataStreamId(result.value(row, "stream_id"));
    config.risk_limits = PreTradeRiskLimits{
        .maximum_order_quantity = Quantity::from_raw(int64_of(result, row, "maximum_order_quantity_raw")),
        .maximum_order_notional = Money::from_raw(int64_of(result, row, "maximum_order_notional_raw")),
        .maximum_position_quantity = Quantity::from_raw(int64_of(result, row, "maximum_position_quantity_raw")),
        .maximum_quote_age_nanoseconds = int64_of(result, row, "maximum_quote_age_ns"),
        .fee_per_fill = Money::from_raw(int64_of(result, row, "fee_per_fill_raw")),
    };
    config.request_sha256 = result.value(row, "request_sha256");
    record.creation_sequence = int64_of(result, row, "creation_sequence");
    record.state = *state;
    record.state_reason = result.value(row, "state_reason");
    record.state_changed_at = result.value(row, "state_changed_at");
    record.created_at = result.value(row, "created_at");
    record.run_generation = int64_of(result, row, "run_generation");
    record.next_decision_sequence = int64_of(result, row, "next_decision_sequence");
    if (const auto receive = optional_int64(result, row, "watermark_receive_ns")) {
        record.watermark_receive = Timestamp::from_nanoseconds(*receive);
    }
    record.watermark_ingest_sequence = optional_uint64(result, row, "watermark_ingest_sequence");
    record.orders_this_run = int32_of(result, row, "orders_this_run");
    record.consecutive_rejections = int32_of(result, row, "consecutive_rejections");
    record.version = int64_of(result, row, "version");
    record.pending_decision = optional_int64(result, row, "pending_decision");
    return record;
}

std::optional<DecisionState> decision_state_from_name(std::string_view name) {
    for (DecisionState state : {DecisionState::Held, DecisionState::Pending, DecisionState::Recorded,
                                DecisionState::Abandoned}) {
        if (decision_state_name(state) == name) {
            return state;
        }
    }
    return std::nullopt;
}

DeploymentDecisionRecord parse_decision(const PostgresResult& result, int row) {
    DeploymentDecisionRecord record;
    NewDeploymentDecision& decision = record.decision;
    decision.deployment_id = result.value(row, "deployment_id");
    record.decision_sequence = int64_of(result, row, "decision_sequence");
    decision.run_generation = int64_of(result, row, "run_generation");
    decision.stream = MarketDataStreamId(result.value(row, "stream_id"));
    decision.instrument = InstrumentId(result.value(row, "instrument"));
    decision.observation_ingest_sequence = uint64_of(result, row, "observation_ingest_sequence");
    decision.observation_event = Timestamp::from_nanoseconds(int64_of(result, row, "observation_event_ns"));
    decision.observation_receive = Timestamp::from_nanoseconds(int64_of(result, row, "observation_receive_ns"));
    decision.decided_at_market = Timestamp::from_nanoseconds(int64_of(result, row, "decided_at_market_ns"));
    const auto inputs = array_values(result.value(row, "inputs_raw"), "inputs_raw");
    if (inputs.size() != kModelInputCount) {
        throw std::runtime_error("a decision must have five inputs");
    }
    for (std::size_t index = 0; index < kModelInputCount; ++index) {
        decision.inputs[index] = parse_integer<std::int64_t>(inputs[index], "inputs_raw");
    }
    decision.predicted_class = result.value(row, "predicted_class");
    const auto probabilities = array_values(result.value(row, "probabilities"), "probabilities");
    if (probabilities.size() != 3) {
        throw std::runtime_error("a decision must have three probabilities");
    }
    std::copy(probabilities.begin(), probabilities.end(), decision.probabilities.begin());
    decision.confidence_ppb = uint64_of(result, row, "confidence_ppb");
    decision.position_before = Quantity::from_raw(int64_of(result, row, "position_before_raw"));
    if (const auto target = optional_int64(result, row, "target_raw")) {
        decision.target = Quantity::from_raw(*target);
    }
    decision.policy_reason = result.value(row, "policy_reason");
    record.client_order_id = result.optional_value(row, "client_order_id");
    if (const auto side = result.optional_value(row, "side")) {
        decision.side = *side == "buy" ? Side::Buy : Side::Sell;
    }
    if (const auto quantity = optional_int64(result, row, "quantity_raw")) {
        decision.quantity = Quantity::from_raw(*quantity);
    }
    if (const auto at = optional_int64(result, row, "not_before_receive_ns")) {
        record.not_before_receive = Timestamp::from_nanoseconds(*at);
    }
    if (const auto at = optional_int64(result, row, "expires_at_market_ns")) {
        record.expires_at_market = Timestamp::from_nanoseconds(*at);
    }
    const std::string state_name = result.value(row, "state");
    const auto state = decision_state_from_name(state_name);
    if (!state) {
        throw std::runtime_error("invalid decision state: " + state_name);
    }
    record.state = *state;
    record.resolution = result.optional_value(row, "resolution");
    record.execution_ingest_sequence = optional_uint64(result, row, "execution_ingest_sequence");
    if (const auto at = optional_int64(result, row, "execution_receive_ns")) {
        record.execution_receive = Timestamp::from_nanoseconds(*at);
    }
    if (const auto at = optional_int64(result, row, "resolved_at_market_ns")) {
        record.resolved_at_market = Timestamp::from_nanoseconds(*at);
    }
    record.created_at = result.value(row, "created_at");
    return record;
}

// A database without migration 008 has no model reference columns: say
// which migration to apply rather than only that a statement failed.
void require_deployment_status(const PostgresResult& result, ExecStatusType expected, std::string_view operation) {
    if (result.status() != expected && result.sql_state() == "42703") {
        throw std::runtime_error(
            std::string(operation) + " failed: the database has no registered model deployments; apply "
            "migrations/008_registered_model_deployments.sql"
        );
    }
    require_status(result, expected, operation);
}

std::optional<ModelDeploymentRecord> select_deployment(PGconn* connection, std::string_view deployment_id) {
    PostgresParameters parameters;
    parameters.add(deployment_id);
    const auto result = execute(connection, deployment_select() + " WHERE d.deployment_id = $1", parameters);
    require_deployment_status(result, PGRES_TUPLES_OK, "read deployment");
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return parse_deployment(result, 0);
}

// The deployment's row lock, the first lock of a deployment write.
std::optional<std::string> lock_deployment_row(PGconn* connection, std::string_view deployment_id) {
    PostgresParameters parameters;
    parameters.add(deployment_id);
    const auto result = execute(
        connection,
        "SELECT state FROM model_deployments WHERE deployment_id = $1 FOR UPDATE",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "lock deployment");
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return result.value(0, "state");
}

DeploymentConflict conflict_of(
    DeploymentConflictCode code,
    std::string message,
    std::optional<ModelDeploymentRecord> deployment = std::nullopt
) {
    return DeploymentConflict{.code = code, .message = std::move(message), .deployment = std::move(deployment)};
}

std::string client_order_id_for(std::string_view deployment_id, std::int64_t sequence) {
    std::string digits = std::to_string(sequence);
    if (digits.size() < 12) {
        digits.insert(0, 12 - digits.size(), '0');
    }
    return std::string(deployment_id) + "-" + digits;
}

std::int64_t checked_add(std::int64_t left, std::int64_t right) {
    std::int64_t sum = 0;
    if (__builtin_add_overflow(left, right, &sum)) {
        throw std::overflow_error("a deployment time leaves the timestamp range");
    }
    return sum;
}

}  // namespace

namespace postgres_support {

void resolve_deployment_order(
    PGconn* connection,
    std::string_view deployment_id,
    std::int64_t decision_sequence,
    const DeploymentOrderResolution& resolution
) {
    PostgresParameters parameters;
    parameters.add(deployment_id);
    parameters.add(decision_sequence);
    parameters.add(resolution.outcome);
    parameters.add(resolution.execution_ingest_sequence);
    parameters.add(resolution.execution_receive.nanoseconds());
    parameters.add(resolution.resolved_at_market.nanoseconds());
    const auto decided = execute(
        connection,
        R"SQL(
            UPDATE model_deployment_decisions
            SET state = 'recorded',
                resolution = $3,
                execution_ingest_sequence = $4,
                execution_receive_ns = $5,
                resolved_at_market_ns = $6,
                resolved_at = clock_timestamp()
            WHERE deployment_id = $1 AND decision_sequence = $2 AND state = 'pending'
        )SQL",
        parameters
    );
    require_status(decided, PGRES_COMMAND_OK, "resolve deployment decision");
    // The invariant's core: a decision leaves pending at most once, and only
    // in the transaction that records its one order.
    if (decided.affected_rows() != 1) {
        throw std::logic_error("a deployment order needs exactly one pending decision");
    }
    PostgresParameters counters;
    counters.add(deployment_id);
    counters.add(resolution.rejected);
    const auto counted = execute(
        connection,
        R"SQL(
            UPDATE model_deployments
            SET orders_this_run = orders_this_run + 1,
                consecutive_rejections = CASE WHEN $2::boolean THEN consecutive_rejections + 1 ELSE 0 END,
                version = version + 1
            WHERE deployment_id = $1
        )SQL",
        counters
    );
    require_status(counted, PGRES_COMMAND_OK, "count deployment order");
}

}  // namespace postgres_support

// ---------------------------------------------------------------------------
// Under an account lock

std::optional<std::string> PaperAccountLock::owning_deployment() {
    if (state_ == nullptr || !state_->transaction) {
        throw std::logic_error("the account lock was already released");
    }
    PostgresParameters parameters;
    parameters.add(state_->account_id);
    const auto result = execute(
        state_->connection,
        "SELECT deployment_id FROM model_deployments WHERE account_id = $1",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "read account owner");
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return result.value(0, "deployment_id");
}

std::optional<LockedDeploymentDecision> PaperAccountLock::lock_deployment_decision(
    std::string_view deployment_id,
    std::int64_t decision_sequence
) {
    if (state_ == nullptr || !state_->transaction) {
        throw std::logic_error("the account lock was already released");
    }
    PostgresParameters owner;
    owner.add(deployment_id);
    owner.add(state_->account_id);
    const auto locked = execute(
        state_->connection,
        "SELECT state FROM model_deployments WHERE deployment_id = $1 AND account_id = $2 FOR UPDATE",
        owner
    );
    require_status(locked, PGRES_TUPLES_OK, "lock deployment");
    if (locked.row_count() == 0) {
        return std::nullopt;
    }
    PostgresParameters key;
    key.add(deployment_id);
    key.add(decision_sequence);
    const auto decision = execute(
        state_->connection,
        decision_select() + " WHERE deployment_id = $1 AND decision_sequence = $2 FOR UPDATE",
        key
    );
    require_status(decision, PGRES_TUPLES_OK, "lock deployment decision");
    if (decision.row_count() == 0) {
        return std::nullopt;
    }
    auto deployment = select_deployment(state_->connection, deployment_id);
    if (!deployment) {
        return std::nullopt;
    }
    return LockedDeploymentDecision{.deployment = std::move(*deployment), .decision = parse_decision(decision, 0)};
}

void PaperAccountLock::commit_deployment_abandonment(
    std::string_view deployment_id,
    std::int64_t decision_sequence,
    std::string_view reason,
    Timestamp market_time
) {
    if (state_ == nullptr || !state_->transaction) {
        throw std::logic_error("the account lock was already released");
    }
    struct Release {
        PaperAccountLock::State& state;
        ~Release() { state.transaction.reset(); }
    } release{*state_};
    PostgresParameters parameters;
    parameters.add(deployment_id);
    parameters.add(decision_sequence);
    parameters.add(reason);
    parameters.add(market_time.nanoseconds());
    const auto result = execute(
        state_->connection,
        R"SQL(
            UPDATE model_deployment_decisions
            SET state = 'abandoned',
                resolution = $3,
                resolved_at_market_ns = $4,
                resolved_at = clock_timestamp()
            WHERE deployment_id = $1 AND decision_sequence = $2 AND state = 'pending'
        )SQL",
        parameters
    );
    require_status(result, PGRES_COMMAND_OK, "abandon deployment decision");
    state_->transaction->commit();
}

// ---------------------------------------------------------------------------
// Repository operations, each in its own transaction

std::variant<CreatedDeployment, DeploymentConflict> PostgresPaperOrderRepository::create_deployment(
    const ModelDeploymentConfig& config
) {
    impl_->ensure_connected();
    PGconn* connection = impl_->connection;
    const auto existing = [&]() -> std::variant<CreatedDeployment, DeploymentConflict> {
        auto record = select_deployment(connection, config.deployment_id);
        if (!record) {
            return conflict_of(DeploymentConflictCode::DeploymentExists, "the deployment could not be created");
        }
        if (record->config.request_sha256 != config.request_sha256) {
            return conflict_of(
                DeploymentConflictCode::DeploymentExists,
                "a different deployment already has this ID",
                std::move(record)
            );
        }
        return CreatedDeployment{.deployment = std::move(*record), .created = false};
    };
    {
        PostgresTransaction transaction(connection);
        if (lock_deployment_row(connection, config.deployment_id)) {
            auto found = existing();
            transaction.commit();
            return found;
        }
        // A new account, owned from the start: it never exists as a manual
        // account, even for an instant.
        PostgresParameters account;
        account.add(config.account_id);
        account.add(config.starting_cash.raw());
        account.add(config.starting_cash.raw());
        account.add(config.risk_limits.maximum_order_quantity.raw());
        account.add(config.risk_limits.maximum_order_notional.raw());
        account.add(config.risk_limits.maximum_position_quantity.raw());
        account.add(config.risk_limits.maximum_quote_age_nanoseconds);
        account.add(config.risk_limits.fee_per_fill.raw());
        const auto inserted_account = execute(
            connection,
            R"SQL(
                INSERT INTO paper_accounts (
                    account_id, starting_cash_raw, cash_raw, maximum_order_quantity_raw,
                    maximum_order_notional_raw, maximum_position_quantity_raw,
                    maximum_quote_age_ns, fee_per_fill_raw, is_healthy, market_stream_id
                ) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, true, NULL)
                ON CONFLICT (account_id) DO NOTHING
                RETURNING version
            )SQL",
            account
        );
        require_status(inserted_account, PGRES_TUPLES_OK, "create deployment account");
        if (inserted_account.row_count() == 0) {
            // A concurrent create of this deployment may have committed the
            // account while this insert waited for it; this statement's
            // fresh snapshot sees that deployment if so.
            if (select_deployment(connection, config.deployment_id)) {
                return existing();
            }
            return conflict_of(DeploymentConflictCode::AccountExists, "a paper account already has this ID");
        }
        PostgresParameters deployment;
        deployment.add(config.deployment_id);
        deployment.add(config.account_id);
        deployment.add(config.request_sha256);
        const auto* experiment = std::get_if<ExperimentModel>(&config.model);
        const auto* registered = std::get_if<RegisteredModel>(&config.model);
        deployment.add(experiment != nullptr ? "experiment" : "registered");
        deployment.add(experiment != nullptr ? std::optional(experiment->experiment_id) : std::nullopt);
        deployment.add(registered != nullptr ? std::optional(registered->model_version_id) : std::nullopt);
        deployment.add(config.parameters_sha256);
        deployment.add(config.family);
        deployment.add(config.training_cutoff_basis);
        deployment.add(config.instrument.value());
        deployment.add(config.threshold_ppb);
        deployment.add(config.target_quantity.raw());
        deployment.add(config.decision_interval_ns);
        deployment.add(config.latency_ns);
        deployment.add(config.max_signal_age_ns);
        deployment.add(static_cast<std::int64_t>(config.max_orders_per_run));
        deployment.add(static_cast<std::int64_t>(config.max_consecutive_rejections));
        deployment.add(config.training_cutoff.nanoseconds());
        deployment.add(config.stream.value());
        deployment.add(kDeploymentModelFeatureSet);
        deployment.add(kDeploymentPolicy);
        const auto inserted = execute(
            connection,
            R"SQL(
                INSERT INTO model_deployments (
                    deployment_id, account_id, request_sha256, model_reference,
                    experiment_id, model_version_id, parameters_sha256, model_name,
                    training_cutoff_basis, instrument, threshold_ppb,
                    target_quantity_raw, decision_interval_ns, latency_ns,
                    max_signal_age_ns, max_orders_per_run, max_consecutive_rejections,
                    training_cutoff_ns, stream_id, model_feature_set, policy,
                    state, state_reason
                ) VALUES (
                    $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14,
                    $15, $16, $17, $18, $19, $20, $21, 'stopped', 'created'
                )
                ON CONFLICT (deployment_id) DO NOTHING
                RETURNING deployment_id
            )SQL",
            deployment
        );
        require_deployment_status(inserted, PGRES_TUPLES_OK, "create deployment");
        if (inserted.row_count() == 1) {
            transaction.commit();
            auto record = select_deployment(connection, config.deployment_id);
            if (!record) {
                throw std::runtime_error("a created deployment could not be read back");
            }
            return CreatedDeployment{.deployment = std::move(*record), .created = true};
        }
        // A concurrent create of the same ID won; this one's account is
        // rolled back with the transaction.
    }
    return existing();
}

std::optional<ModelDeploymentRecord> PostgresPaperOrderRepository::find_deployment(std::string_view deployment_id) {
    impl_->ensure_connected();
    return select_deployment(impl_->connection, deployment_id);
}

DeploymentPage PostgresPaperOrderRepository::list_deployments(
    std::size_t limit,
    std::optional<std::int64_t> before_sequence
) {
    if (limit == 0 || limit > 200) {
        throw std::invalid_argument("a deployment page holds 1 to 200 deployments");
    }
    impl_->ensure_connected();
    PostgresParameters parameters;
    parameters.add_optional(before_sequence);
    parameters.add(static_cast<std::uint64_t>(limit + 1));
    const auto result = execute(
        impl_->connection,
        deployment_select() +
            " WHERE ($1::bigint IS NULL OR d.creation_sequence < $1::bigint)"
            " ORDER BY d.creation_sequence DESC LIMIT $2",
        parameters
    );
    require_deployment_status(result, PGRES_TUPLES_OK, "list deployments");
    DeploymentPage page;
    for (int row = 0; row < result.row_count() && page.deployments.size() < limit; ++row) {
        page.deployments.push_back(parse_deployment(result, row));
    }
    if (static_cast<std::size_t>(result.row_count()) > limit) {
        page.next_before_sequence = page.deployments.back().creation_sequence;
    }
    return page;
}

std::variant<ModelDeploymentRecord, DeploymentConflict> PostgresPaperOrderRepository::transition_deployment(
    std::string_view deployment_id,
    const std::vector<DeploymentState>& from,
    DeploymentState to,
    std::string_view reason,
    bool begin_run
) {
    if (reason.empty()) {
        throw std::invalid_argument("a transition needs a reason");
    }
    impl_->ensure_connected();
    PGconn* connection = impl_->connection;
    PostgresTransaction transaction(connection);
    const auto current = lock_deployment_row(connection, deployment_id);
    if (!current) {
        return conflict_of(DeploymentConflictCode::DeploymentNotFound, "no such deployment");
    }
    const auto state = deployment_state_from_name(*current);
    if (!state || std::find(from.begin(), from.end(), *state) == from.end()) {
        auto record = select_deployment(connection, deployment_id);
        transaction.commit();
        return conflict_of(
            DeploymentConflictCode::InvalidTransition,
            "the deployment is " + *current,
            std::move(record)
        );
    }
    if (begin_run) {
        auto record = select_deployment(connection, deployment_id);
        if (record && record->pending_decision) {
            transaction.commit();
            return conflict_of(
                DeploymentConflictCode::InvalidTransition,
                "a decision is still unresolved",
                std::move(record)
            );
        }
    }
    PostgresParameters parameters;
    parameters.add(deployment_id);
    parameters.add(deployment_state_name(to));
    parameters.add(reason);
    parameters.add(begin_run);
    const auto updated = execute(
        connection,
        R"SQL(
            UPDATE model_deployments
            SET state = $2,
                state_reason = $3,
                state_changed_at = clock_timestamp(),
                run_generation = run_generation + CASE WHEN $4::boolean THEN 1 ELSE 0 END,
                orders_this_run = CASE WHEN $4::boolean THEN 0 ELSE orders_this_run END,
                consecutive_rejections = CASE WHEN $4::boolean THEN 0 ELSE consecutive_rejections END,
                version = version + 1
            WHERE deployment_id = $1
        )SQL",
        parameters
    );
    require_status(updated, PGRES_COMMAND_OK, "transition deployment");
    auto record = select_deployment(connection, deployment_id);
    transaction.commit();
    if (!record) {
        throw std::runtime_error("a transitioned deployment could not be read back");
    }
    return std::move(*record);
}

std::variant<DeploymentDecisionRecord, DeploymentConflict> PostgresPaperOrderRepository::record_decision(
    const NewDeploymentDecision& decision
) {
    if (decision.side.has_value() != decision.quantity.has_value()) {
        throw std::invalid_argument("an order decision needs its side and quantity");
    }
    impl_->ensure_connected();
    PGconn* connection = impl_->connection;
    PostgresTransaction transaction(connection);
    if (!lock_deployment_row(connection, decision.deployment_id)) {
        return conflict_of(DeploymentConflictCode::DeploymentNotFound, "no such deployment");
    }
    auto deployment = select_deployment(connection, decision.deployment_id);
    if (!deployment) {
        return conflict_of(DeploymentConflictCode::DeploymentNotFound, "no such deployment");
    }
    const auto refuse = [&](std::string message) -> std::variant<DeploymentDecisionRecord, DeploymentConflict> {
        auto record = std::move(*deployment);
        transaction.commit();
        return conflict_of(DeploymentConflictCode::DecisionRefused, std::move(message), std::move(record));
    };
    if (deployment->state != DeploymentState::Running) {
        return refuse("the deployment is " + std::string(deployment_state_name(deployment->state)));
    }
    if (deployment->run_generation != decision.run_generation) {
        return refuse("the decision belongs to an earlier run");
    }
    if (deployment->pending_decision) {
        return refuse("an earlier decision is still unresolved");
    }
    if (decision.instrument != deployment->config.instrument || decision.stream != deployment->config.stream) {
        return refuse("the observation is not of the deployment's instrument and stream");
    }
    if (deployment->watermark_receive) {
        const bool later = decision.observation_receive > *deployment->watermark_receive ||
            (decision.observation_receive == *deployment->watermark_receive &&
             decision.observation_ingest_sequence > *deployment->watermark_ingest_sequence);
        if (!later) {
            return refuse("the observation is not after the latest decided one");
        }
    }
    // The decision and the watermark record the observation's sequence:
    // only the stream's current writer may. After the deployment row, so
    // the lock order is deployment, then stream.
    if (auto stale = impl_->fence.refusal(
            connection,
            decision.stream.value(),
            decision.observation_ingest_sequence,
            decision.observation_ingest_sequence,
            true
        )) {
        return refuse(std::move(*stale));
    }
    const std::int64_t sequence = deployment->next_decision_sequence;
    DeploymentDecisionRecord record{
        .decision = decision,
        .decision_sequence = sequence,
        .state = decision.side ? DecisionState::Pending : DecisionState::Held,
        .client_order_id = std::nullopt,
        .not_before_receive = std::nullopt,
        .expires_at_market = std::nullopt,
        .resolution = std::nullopt,
        .execution_ingest_sequence = std::nullopt,
        .execution_receive = std::nullopt,
        .resolved_at_market = std::nullopt,
        .created_at = {},
    };
    if (decision.side) {
        record.client_order_id = client_order_id_for(decision.deployment_id, sequence);
        record.not_before_receive = Timestamp::from_nanoseconds(
            checked_add(decision.observation_receive.nanoseconds(), deployment->config.latency_ns)
        );
        record.expires_at_market = Timestamp::from_nanoseconds(
            checked_add(decision.observation_receive.nanoseconds(), deployment->config.max_signal_age_ns)
        );
    }
    std::vector<std::string> inputs;
    for (std::int64_t value : decision.inputs) {
        inputs.push_back(std::to_string(value));
    }
    PostgresParameters parameters;
    parameters.add(decision.deployment_id);
    parameters.add(sequence);
    parameters.add(decision.run_generation);
    parameters.add(decision.stream.value());
    parameters.add(decision.instrument.value());
    parameters.add(decision.observation_ingest_sequence);
    parameters.add(decision.observation_event.nanoseconds());
    parameters.add(decision.observation_receive.nanoseconds());
    parameters.add(decision.decided_at_market.nanoseconds());
    parameters.add(array_literal(inputs));
    parameters.add(decision.predicted_class);
    parameters.add(array_literal({decision.probabilities.begin(), decision.probabilities.end()}));
    parameters.add(decision.confidence_ppb);
    parameters.add(decision.position_before.raw());
    parameters.add_optional(decision.target ? std::optional(decision.target->raw()) : std::nullopt);
    parameters.add(decision.policy_reason);
    parameters.add(record.client_order_id);
    parameters.add(decision.side ? std::optional<std::string>(*decision.side == Side::Buy ? "buy" : "sell") : std::nullopt);
    parameters.add_optional(decision.quantity ? std::optional(decision.quantity->raw()) : std::nullopt);
    parameters.add_optional(
        record.not_before_receive ? std::optional(record.not_before_receive->nanoseconds()) : std::nullopt
    );
    parameters.add_optional(
        record.expires_at_market ? std::optional(record.expires_at_market->nanoseconds()) : std::nullopt
    );
    parameters.add(decision_state_name(record.state));
    const auto inserted = execute(
        connection,
        R"SQL(
            INSERT INTO model_deployment_decisions (
                deployment_id, decision_sequence, run_generation, stream_id, instrument,
                observation_ingest_sequence, observation_event_ns, observation_receive_ns,
                decided_at_market_ns, inputs_raw, predicted_class, probabilities,
                confidence_ppb, position_before_raw, target_raw, policy_reason,
                client_order_id, side, quantity_raw, not_before_receive_ns,
                expires_at_market_ns, state
            ) VALUES (
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $10::text[], $11, $12::text[], $13,
                $14, $15, $16, $17, $18, $19, $20, $21, $22
            )
        )SQL",
        parameters
    );
    require_status(inserted, PGRES_COMMAND_OK, "record deployment decision");
    PostgresParameters watermark;
    watermark.add(decision.deployment_id);
    watermark.add(sequence + 1);
    watermark.add(decision.observation_receive.nanoseconds());
    watermark.add(decision.observation_ingest_sequence);
    const auto advanced = execute(
        connection,
        R"SQL(
            UPDATE model_deployments
            SET next_decision_sequence = $2,
                watermark_receive_ns = $3,
                watermark_ingest_sequence = $4,
                version = version + 1
            WHERE deployment_id = $1
        )SQL",
        watermark
    );
    require_status(advanced, PGRES_COMMAND_OK, "advance deployment watermark");
    transaction.commit();
    return record;
}

DecisionPage PostgresPaperOrderRepository::list_decisions(
    std::string_view deployment_id,
    std::size_t limit,
    std::optional<std::int64_t> before_sequence
) {
    if (limit == 0 || limit > 200) {
        throw std::invalid_argument("a decision page holds 1 to 200 decisions");
    }
    impl_->ensure_connected();
    PostgresParameters parameters;
    parameters.add(deployment_id);
    parameters.add_optional(before_sequence);
    parameters.add(static_cast<std::uint64_t>(limit + 1));
    const auto result = execute(
        impl_->connection,
        decision_page_select() +
            " WHERE d.deployment_id = $1 AND ($2::bigint IS NULL OR d.decision_sequence < $2::bigint)"
            " ORDER BY d.decision_sequence DESC LIMIT $3",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "list deployment decisions");
    DecisionPage page;
    for (int row = 0; row < result.row_count() && page.decisions.size() < limit; ++row) {
        DeploymentDecisionRecord decision = parse_decision(result, row);
        if (int64_of(result, row, "fill_count") > 0) {
            const auto price = optional_int64(result, row, "single_price_raw");
            decision.fill = DecisionFill{
                .count = int64_of(result, row, "fill_count"),
                .quantity = Quantity::from_raw(int64_of(result, row, "fill_quantity_raw")),
                .notional = Money::from_raw(int64_of(result, row, "fill_notional_raw")),
                .fees = Money::from_raw(int64_of(result, row, "fill_fee_raw")),
                .first_filled_at = Timestamp::from_nanoseconds(int64_of(result, row, "first_filled_at_ns")),
                .last_filled_at = Timestamp::from_nanoseconds(int64_of(result, row, "last_filled_at_ns")),
                .price = price ? std::optional(Price::from_raw(*price)) : std::nullopt,
            };
        }
        page.decisions.push_back(std::move(decision));
    }
    if (static_cast<std::size_t>(result.row_count()) > limit) {
        page.next_before_sequence = page.decisions.back().decision_sequence;
    }
    return page;
}

std::vector<DeploymentDecisionRecord> PostgresPaperOrderRepository::pending_decisions() {
    impl_->ensure_connected();
    const auto result = execute(
        impl_->connection,
        decision_select() + " WHERE state = 'pending' ORDER BY deployment_id, decision_sequence"
    );
    require_status(result, PGRES_TUPLES_OK, "list pending deployment decisions");
    std::vector<DeploymentDecisionRecord> decisions;
    for (int row = 0; row < result.row_count(); ++row) {
        decisions.push_back(parse_decision(result, row));
    }
    return decisions;
}

std::optional<std::string> PostgresPaperOrderRepository::account_owner(std::string_view account_id) {
    impl_->ensure_connected();
    PostgresParameters parameters;
    parameters.add(account_id);
    const auto result = execute(
        impl_->connection,
        "SELECT deployment_id FROM model_deployments WHERE account_id = $1",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "read account owner");
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return result.value(0, "deployment_id");
}

}  // namespace chronos
