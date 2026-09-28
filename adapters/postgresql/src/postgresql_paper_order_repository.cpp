#include "chronos/postgresql_paper_order_repository.hpp"

#include <libpq-fe.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos {
namespace {

class PostgresResult final {
public:
    explicit PostgresResult(PGresult* result) : result_(result) {
        if (result_ == nullptr) {
            throw std::runtime_error("PostgreSQL returned no result object");
        }
    }

    ~PostgresResult() { PQclear(result_); }

    PostgresResult(const PostgresResult&) = delete;
    PostgresResult& operator=(const PostgresResult&) = delete;
    PostgresResult(PostgresResult&& other) noexcept
        : result_(std::exchange(other.result_, nullptr)) {}
    PostgresResult& operator=(PostgresResult&& other) noexcept {
        if (this != &other) {
            PQclear(result_);
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] ExecStatusType status() const {
        return PQresultStatus(result_);
    }

    [[nodiscard]] int row_count() const { return PQntuples(result_); }

    [[nodiscard]] bool is_null(int row, std::string_view column) const {
        return PQgetisnull(result_, row, column_index(column)) != 0;
    }

    [[nodiscard]] std::string value(
        int row,
        std::string_view column
    ) const {
        const int index = column_index(column);
        if (PQgetisnull(result_, row, index) != 0) {
            throw std::runtime_error(
                "unexpected NULL PostgreSQL column: " + std::string(column)
            );
        }
        return std::string(PQgetvalue(result_, row, index));
    }

    [[nodiscard]] std::optional<std::string> optional_value(
        int row,
        std::string_view column
    ) const {
        return is_null(row, column)
            ? std::nullopt
            : std::optional<std::string>(value(row, column));
    }

    [[nodiscard]] std::string error_message() const {
        const char* message = PQresultErrorMessage(result_);
        return message == nullptr ? "unknown PostgreSQL error" : message;
    }

    [[nodiscard]] std::string sql_state() const {
        const char* state = PQresultErrorField(result_, PG_DIAG_SQLSTATE);
        return state == nullptr ? "" : state;
    }

    [[nodiscard]] std::string constraint_name() const {
        const char* name = PQresultErrorField(
            result_,
            PG_DIAG_CONSTRAINT_NAME
        );
        return name == nullptr ? "" : name;
    }

private:
    [[nodiscard]] int column_index(std::string_view column) const {
        const std::string name(column);
        const int index = PQfnumber(result_, name.c_str());
        if (index < 0) {
            throw std::runtime_error(
                "PostgreSQL result is missing column: " + name
            );
        }
        return index;
    }

    PGresult* result_;
};

class PostgresParameters final {
public:
    void add(std::string value) {
        values_.push_back(std::move(value));
    }

    void add(std::string_view value) {
        values_.emplace_back(std::string(value));
    }

    void add(const char* value) { values_.emplace_back(std::string(value)); }

    void add(std::int64_t value) { add(std::to_string(value)); }
    void add(std::uint64_t value) { add(std::to_string(value)); }
    void add(std::uint32_t value) { add(std::to_string(value)); }
    void add(bool value) { add(value ? "true" : "false"); }

    void add(const std::optional<std::string>& value) {
        values_.push_back(value);
    }

    void add_optional(std::optional<std::int64_t> value) {
        values_.push_back(value ? std::optional<std::string>(
            std::to_string(*value)
        ) : std::nullopt);
    }

    void add_optional(std::optional<std::uint64_t> value) {
        values_.push_back(value ? std::optional<std::string>(
            std::to_string(*value)
        ) : std::nullopt);
    }

    [[nodiscard]] const std::vector<const char*>& pointers() {
        pointers_.clear();
        pointers_.reserve(values_.size());
        for (const auto& value : values_) {
            pointers_.push_back(value ? value->c_str() : nullptr);
        }
        return pointers_;
    }

    [[nodiscard]] int size() const {
        if (values_.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {
            throw std::overflow_error("too many PostgreSQL parameters");
        }
        return static_cast<int>(values_.size());
    }

private:
    std::vector<std::optional<std::string>> values_;
    std::vector<const char*> pointers_;
};

PostgresResult execute(PGconn* connection, std::string_view sql) {
    const std::string statement(sql);
    return PostgresResult(PQexec(connection, statement.c_str()));
}

PostgresResult execute(
    PGconn* connection,
    std::string_view sql,
    PostgresParameters& parameters
) {
    const std::string statement(sql);
    const auto& pointers = parameters.pointers();
    return PostgresResult(PQexecParams(
        connection,
        statement.c_str(),
        parameters.size(),
        nullptr,
        pointers.data(),
        nullptr,
        nullptr,
        0
    ));
}

void require_status(
    const PostgresResult& result,
    ExecStatusType expected,
    std::string_view operation
) {
    if (result.status() != expected) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + result.error_message()
        );
    }
}

class PostgresTransaction final {
public:
    explicit PostgresTransaction(PGconn* connection)
        : connection_(connection) {
        const auto result = execute(connection_, "BEGIN");
        require_status(result, PGRES_COMMAND_OK, "begin transaction");
    }

    ~PostgresTransaction() {
        if (!finished_) {
            const auto result = execute(connection_, "ROLLBACK");
            static_cast<void>(result);
        }
    }

    PostgresTransaction(const PostgresTransaction&) = delete;
    PostgresTransaction& operator=(const PostgresTransaction&) = delete;

    void commit() {
        const auto result = execute(connection_, "COMMIT");
        require_status(result, PGRES_COMMAND_OK, "commit transaction");
        finished_ = true;
    }

private:
    PGconn* connection_;
    bool finished_{false};
};

template <typename Integer>
Integer parse_integer(std::string_view value, std::string_view field) {
    Integer parsed{};
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed
    );
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        throw std::runtime_error(
            "invalid integer in persisted field: " + std::string(field)
        );
    }
    return parsed;
}

bool parse_boolean(std::string_view value, std::string_view field) {
    if (value == "t") {
        return true;
    }
    if (value == "f") {
        return false;
    }
    throw std::runtime_error(
        "invalid boolean in persisted field: " + std::string(field)
    );
}

std::optional<std::int64_t> optional_int64(
    const PostgresResult& result,
    int row,
    std::string_view field
) {
    const auto value = result.optional_value(row, field);
    return value ? std::optional<std::int64_t>(
        parse_integer<std::int64_t>(*value, field)
    ) : std::nullopt;
}

std::optional<std::uint64_t> optional_uint64(
    const PostgresResult& result,
    int row,
    std::string_view field
) {
    const auto value = result.optional_value(row, field);
    return value ? std::optional<std::uint64_t>(
        parse_integer<std::uint64_t>(*value, field)
    ) : std::nullopt;
}

std::string source_kind_text(OrderSourceKind kind) {
    switch (kind) {
        case OrderSourceKind::Manual:
            return "manual";
        case OrderSourceKind::Model:
            return "model";
    }
    return "unknown:" + std::to_string(static_cast<int>(kind));
}

std::string side_text(Side side) {
    switch (side) {
        case Side::Buy:
            return "buy";
        case Side::Sell:
            return "sell";
    }
    return "unknown:" + std::to_string(static_cast<int>(side));
}

std::string order_type_text(OrderType type) {
    switch (type) {
        case OrderType::Market:
            return "market";
        case OrderType::Limit:
            return "limit";
    }
    return "unknown:" + std::to_string(static_cast<int>(type));
}

std::string time_in_force_text(TimeInForce time_in_force) {
    if (time_in_force == TimeInForce::ImmediateOrCancel) {
        return "ioc";
    }
    return "unknown:" + std::to_string(static_cast<int>(time_in_force));
}

std::string execution_status_text(SimulatedExecutionStatus status) {
    switch (status) {
        case SimulatedExecutionStatus::Filled:
            return "filled";
        case SimulatedExecutionStatus::PartiallyFilledAndCancelled:
            return "partially_filled_and_cancelled";
        case SimulatedExecutionStatus::Cancelled:
            return "cancelled";
    }
    throw std::invalid_argument("cannot persist unknown execution status");
}

std::string cancellation_reason_text(SimulatedCancellationReason reason) {
    switch (reason) {
        case SimulatedCancellationReason::LimitNotMarketable:
            return "limit_not_marketable";
        case SimulatedCancellationReason::DisplayedLiquidityExhausted:
            return "displayed_liquidity_exhausted";
    }
    throw std::invalid_argument(
        "cannot persist unknown cancellation reason"
    );
}

std::string risk_code_text(PreTradeRiskErrorCode code) {
    switch (code) {
        case PreTradeRiskErrorCode::MissingClientOrderId:
            return "missing_client_order_id";
        case PreTradeRiskErrorCode::MissingAccountId:
            return "missing_account_id";
        case PreTradeRiskErrorCode::MissingSourceId:
            return "missing_source_id";
        case PreTradeRiskErrorCode::UnknownOrderSource:
            return "unknown_order_source";
        case PreTradeRiskErrorCode::AccountMismatch:
            return "account_mismatch";
        case PreTradeRiskErrorCode::InstrumentMismatch:
            return "instrument_mismatch";
        case PreTradeRiskErrorCode::UnknownSide:
            return "unknown_side";
        case PreTradeRiskErrorCode::UnknownOrderType:
            return "unknown_order_type";
        case PreTradeRiskErrorCode::UnsupportedTimeInForce:
            return "unsupported_time_in_force";
        case PreTradeRiskErrorCode::NonpositiveQuantity:
            return "nonpositive_quantity";
        case PreTradeRiskErrorCode::UnexpectedLimitPrice:
            return "unexpected_limit_price";
        case PreTradeRiskErrorCode::MissingLimitPrice:
            return "missing_limit_price";
        case PreTradeRiskErrorCode::NonpositiveLimitPrice:
            return "nonpositive_limit_price";
        case PreTradeRiskErrorCode::NegativeSubmissionTimestamp:
            return "negative_submission_timestamp";
        case PreTradeRiskErrorCode::SubmissionTimestampMovedBackward:
            return "submission_timestamp_moved_backward";
        case PreTradeRiskErrorCode::InvalidMarketState:
            return "invalid_market_state";
        case PreTradeRiskErrorCode::MarketInstrumentMismatch:
            return "market_instrument_mismatch";
        case PreTradeRiskErrorCode::SuspectMarketState:
            return "suspect_market_state";
        case PreTradeRiskErrorCode::MarketObservationFromFuture:
            return "market_observation_from_future";
        case PreTradeRiskErrorCode::StaleMarketState:
            return "stale_market_state";
        case PreTradeRiskErrorCode::MissingOppositeBookSide:
            return "missing_opposite_book_side";
        case PreTradeRiskErrorCode::OrderQuantityLimitExceeded:
            return "order_quantity_limit_exceeded";
        case PreTradeRiskErrorCode::NotionalRoundedToZero:
            return "notional_rounded_to_zero";
        case PreTradeRiskErrorCode::OrderNotionalLimitExceeded:
            return "order_notional_limit_exceeded";
        case PreTradeRiskErrorCode::PositionLimitExceeded:
            return "position_limit_exceeded";
        case PreTradeRiskErrorCode::InsufficientCash:
            return "insufficient_cash";
        case PreTradeRiskErrorCode::InsufficientPosition:
            return "insufficient_position";
        case PreTradeRiskErrorCode::InsufficientCashForFee:
            return "insufficient_cash_for_fee";
        case PreTradeRiskErrorCode::ArithmeticOverflow:
            return "arithmetic_overflow";
    }
    throw std::invalid_argument("cannot persist unknown risk error code");
}

std::string execution_error_code_text(SimulatedExecutionErrorCode code) {
    switch (code) {
        case SimulatedExecutionErrorCode::AccountMismatch:
            return "account_mismatch";
        case SimulatedExecutionErrorCode::InstrumentMismatch:
            return "instrument_mismatch";
        case SimulatedExecutionErrorCode::DuplicateClientOrderId:
            return "duplicate_client_order_id";
        case SimulatedExecutionErrorCode::MarketStreamMismatch:
            return "market_stream_mismatch";
        case SimulatedExecutionErrorCode::StaleMarketApproval:
            return "stale_market_approval";
        case SimulatedExecutionErrorCode::MarketSnapshotMismatch:
            return "market_snapshot_mismatch";
    }
    throw std::invalid_argument(
        "cannot persist unknown execution error code"
    );
}

std::string processing_error_code_text(PaperOrderProcessingErrorCode code) {
    switch (code) {
        case PaperOrderProcessingErrorCode::ProcessorFaulted:
            return "processor_faulted";
        case PaperOrderProcessingErrorCode::AccountingInvariantViolation:
            return "accounting_invariant_violation";
    }
    throw std::invalid_argument(
        "cannot persist unknown processing error code"
    );
}

void add_account_state_parameters(
    PostgresParameters& parameters,
    const PaperOrderProcessorRecoveryState& state
) {
    parameters.add(state.account.instrument.value());
    parameters.add(state.account.state.starting_cash.raw());
    parameters.add(state.account.state.cash.raw());
    parameters.add(state.account.state.position.raw());
    parameters.add(state.account.state.open_cost_basis.raw());
    parameters.add(state.account.state.gross_realized_pnl.raw());
    parameters.add(state.account.state.fees_paid.raw());
    parameters.add_optional(
        state.account.state.last_fill_timestamp
            ? std::optional<std::int64_t>(
                  state.account.state.last_fill_timestamp->nanoseconds()
              )
            : std::nullopt
    );
    parameters.add(state.risk_limits.maximum_order_quantity.raw());
    parameters.add(state.risk_limits.maximum_order_notional.raw());
    parameters.add(state.risk_limits.maximum_position_quantity.raw());
    parameters.add(state.risk_limits.maximum_quote_age_nanoseconds);
    parameters.add(state.risk_limits.fee_per_fill.raw());
    parameters.add(state.is_healthy);

    if (!state.execution.active_market) {
        for (int index = 0; index < 11; ++index) {
            parameters.add(std::optional<std::string>{});
        }
    } else {
        const auto& market = *state.execution.active_market;
        parameters.add(market.header.stream.value());
        parameters.add(
            market.header.venue
                ? std::optional<std::string>(
                      market.header.venue->value()
                  )
                : std::nullopt
        );
        parameters.add(market.header.event_timestamp.nanoseconds());
        parameters.add(market.header.receive_timestamp.nanoseconds());
        parameters.add(market.header.ingest_sequence);
        parameters.add_optional(market.header.venue_sequence);
        parameters.add(market.header.quality.raw());
        parameters.add_optional(
            market.bid
                ? std::optional<std::int64_t>(market.bid->price.raw())
                : std::nullopt
        );
        parameters.add_optional(
            market.bid
                ? std::optional<std::int64_t>(market.bid->quantity.raw())
                : std::nullopt
        );
        parameters.add_optional(
            market.ask
                ? std::optional<std::int64_t>(market.ask->price.raw())
                : std::nullopt
        );
        parameters.add_optional(
            market.ask
                ? std::optional<std::int64_t>(market.ask->quantity.raw())
                : std::nullopt
        );
    }
    parameters.add(state.execution.consumed_bid_quantity.raw());
    parameters.add(state.execution.consumed_ask_quantity.raw());
}

struct PersistedOutcomeFields {
    std::string kind;
    std::optional<std::string> code;
    std::optional<std::string> field;
    std::optional<std::string> message;
    const SimulatedExecutionReport* execution{nullptr};
    bool processed_by_execution{false};
    bool fill_applied{false};
    std::string outcome_event;
};

PersistedOutcomeFields outcome_fields(const PaperOrderResult& result) {
    return std::visit(
        [](const auto& outcome) -> PersistedOutcomeFields {
            using Outcome = std::decay_t<decltype(outcome)>;
            if constexpr (std::is_same_v<Outcome, PaperOrderReceipt>) {
                return PersistedOutcomeFields{
                    .kind = "receipt",
                    .code = std::nullopt,
                    .field = std::nullopt,
                    .message = std::nullopt,
                    .execution = &outcome.execution,
                    .processed_by_execution = true,
                    .fill_applied = outcome.accounting.has_value(),
                    .outcome_event = "order_completed",
                };
            } else if constexpr (
                std::is_same_v<Outcome, PreTradeRiskRejection>
            ) {
                return PersistedOutcomeFields{
                    .kind = "risk_rejection",
                    .code = risk_code_text(outcome.code),
                    .field = outcome.field,
                    .message = outcome.message,
                    .outcome_event = "risk_rejected",
                };
            } else if constexpr (
                std::is_same_v<Outcome, SimulatedExecutionError>
            ) {
                return PersistedOutcomeFields{
                    .kind = "execution_error",
                    .code = execution_error_code_text(outcome.code),
                    .field = outcome.field,
                    .message = outcome.message,
                    .outcome_event = "execution_rejected",
                };
            } else {
                return PersistedOutcomeFields{
                    .kind = "processing_error",
                    .code = processing_error_code_text(outcome.code),
                    .field = std::nullopt,
                    .message = outcome.message,
                    .execution = outcome.execution
                        ? &*outcome.execution
                        : nullptr,
                    .processed_by_execution =
                        outcome.execution.has_value(),
                    .fill_applied = false,
                    .outcome_event = "processor_faulted",
                };
            }
        },
        result
    );
}

void add_order_record_parameters(
    PostgresParameters& parameters,
    std::string_view account_id,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    const PaperOrderProcessorRecoveryState& state_after
) {
    const PersistedOutcomeFields outcome = outcome_fields(result);
    parameters.add(account_id);
    parameters.add(intent.client_order_id);
    parameters.add(intent.account_id);
    parameters.add(intent.instrument.value());
    parameters.add(source_kind_text(intent.source.kind));
    parameters.add(intent.source.source_id);
    parameters.add(side_text(intent.side));
    parameters.add(order_type_text(intent.type));
    parameters.add(intent.quantity.raw());
    parameters.add_optional(
        intent.limit_price
            ? std::optional<std::int64_t>(intent.limit_price->raw())
            : std::nullopt
    );
    parameters.add(time_in_force_text(intent.time_in_force));
    parameters.add(intent.submitted_at.nanoseconds());
    parameters.add(market.header.instrument.value());
    parameters.add(market.header.stream.value());
    parameters.add(
        market.header.venue
            ? std::optional<std::string>(market.header.venue->value())
            : std::nullopt
    );
    parameters.add(market.header.event_timestamp.nanoseconds());
    parameters.add(market.header.receive_timestamp.nanoseconds());
    parameters.add(market.header.ingest_sequence);
    parameters.add_optional(market.header.venue_sequence);
    parameters.add(market.header.quality.raw());
    parameters.add_optional(
        market.bid
            ? std::optional<std::int64_t>(market.bid->price.raw())
            : std::nullopt
    );
    parameters.add_optional(
        market.bid
            ? std::optional<std::int64_t>(market.bid->quantity.raw())
            : std::nullopt
    );
    parameters.add_optional(
        market.ask
            ? std::optional<std::int64_t>(market.ask->price.raw())
            : std::nullopt
    );
    parameters.add_optional(
        market.ask
            ? std::optional<std::int64_t>(market.ask->quantity.raw())
            : std::nullopt
    );
    parameters.add(outcome.kind);
    parameters.add(outcome.code);
    parameters.add(outcome.field);
    parameters.add(outcome.message);

    const SimulatedExecutionReport* execution = outcome.execution;
    parameters.add(execution
        ? std::optional<std::string>(
              execution_status_text(execution->status)
          )
        : std::nullopt);
    parameters.add_optional(execution
        ? std::optional<std::int64_t>(execution->unfilled_quantity.raw())
        : std::nullopt);
    parameters.add(execution && execution->cancellation_reason
        ? std::optional<std::string>(cancellation_reason_text(
              *execution->cancellation_reason
          ))
        : std::nullopt);
    parameters.add_optional(execution
        ? std::optional<std::uint64_t>(
              execution->market_ingest_sequence
          )
        : std::nullopt);

    const Fill* fill = execution && execution->fill
        ? &*execution->fill
        : nullptr;
    parameters.add(fill
        ? std::optional<std::string>(fill->fill_id)
        : std::nullopt);
    parameters.add(fill
        ? std::optional<std::string>(side_text(fill->side))
        : std::nullopt);
    parameters.add_optional(fill
        ? std::optional<std::int64_t>(fill->price.raw())
        : std::nullopt);
    parameters.add_optional(fill
        ? std::optional<std::int64_t>(fill->quantity.raw())
        : std::nullopt);
    parameters.add_optional(fill
        ? std::optional<std::int64_t>(fill->fee.raw())
        : std::nullopt);
    parameters.add_optional(fill
        ? std::optional<std::int64_t>(fill->timestamp.nanoseconds())
        : std::nullopt);

    const AccountState& account = state_after.account.state;
    parameters.add(account.cash.raw());
    parameters.add(account.position.raw());
    parameters.add(account.open_cost_basis.raw());
    parameters.add(account.gross_realized_pnl.raw());
    parameters.add(account.fees_paid.raw());
    parameters.add_optional(account.last_fill_timestamp
        ? std::optional<std::int64_t>(
              account.last_fill_timestamp->nanoseconds()
          )
        : std::nullopt);
    parameters.add(outcome.processed_by_execution);
}

std::vector<std::string> event_types_for(const PaperOrderResult& result) {
    const PersistedOutcomeFields outcome = outcome_fields(result);
    std::vector<std::string> events{
        "order_submitted",
        outcome.outcome_event,
    };
    if (outcome.fill_applied) {
        events.push_back("fill_applied");
    }
    if (outcome.kind == "receipt" || outcome.kind == "processing_error") {
        events.push_back("account_state_updated");
    }
    return events;
}

PaperPersistenceConflict conflict(
    PaperPersistenceConflictCode code,
    std::string message
) {
    return PaperPersistenceConflict{
        .code = code,
        .message = std::move(message),
    };
}

}  // namespace

struct PostgresPaperOrderRepository::Impl {
    explicit Impl(std::string connection_string)
        : connection(PQconnectdb(connection_string.c_str())) {
        if (connection == nullptr) {
            throw std::runtime_error(
                "could not allocate PostgreSQL connection"
            );
        }
        if (PQstatus(connection) != CONNECTION_OK) {
            const std::string message = PQerrorMessage(connection);
            PQfinish(connection);
            connection = nullptr;
            throw std::runtime_error(
                "could not connect to PostgreSQL: " + message
            );
        }
    }

    ~Impl() { PQfinish(connection); }

    PGconn* connection;
};

PostgresPaperOrderRepository::PostgresPaperOrderRepository(
    std::string connection_string
)
    : impl_(std::make_unique<Impl>(std::move(connection_string))) {}

PostgresPaperOrderRepository::~PostgresPaperOrderRepository() = default;

void PostgresPaperOrderRepository::apply_migration(std::string_view sql) {
    if (sql.empty()) {
        throw std::invalid_argument("migration SQL must not be empty");
    }
    const auto result = execute(impl_->connection, sql);
    if (result.status() != PGRES_COMMAND_OK) {
        const std::string message = result.error_message();
        const auto rollback = execute(impl_->connection, "ROLLBACK");
        static_cast<void>(rollback);
        throw std::runtime_error("apply migration failed: " + message);
    }
}

CreatePaperAccountResult PostgresPaperOrderRepository::create_account(
    const PaperOrderProcessorRecoveryState& state
) {
    static_cast<void>(PaperOrderProcessor::restore(state));
    PostgresParameters parameters;
    parameters.add(state.account.account_id);
    add_account_state_parameters(parameters, state);
    const auto result = execute(
        impl_->connection,
        R"SQL(
            INSERT INTO paper_accounts (
                account_id,
                instrument,
                starting_cash_raw,
                cash_raw,
                position_raw,
                open_cost_basis_raw,
                gross_realized_pnl_raw,
                fees_paid_raw,
                last_fill_timestamp_ns,
                maximum_order_quantity_raw,
                maximum_order_notional_raw,
                maximum_position_quantity_raw,
                maximum_quote_age_ns,
                fee_per_fill_raw,
                is_healthy,
                active_stream_id,
                active_venue,
                active_event_timestamp_ns,
                active_receive_timestamp_ns,
                active_ingest_sequence,
                active_venue_sequence,
                active_quality_flags,
                active_bid_price_raw,
                active_bid_quantity_raw,
                active_ask_price_raw,
                active_ask_quantity_raw,
                consumed_bid_quantity_raw,
                consumed_ask_quantity_raw
            ) VALUES (
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $10,
                $11, $12, $13, $14, $15, $16, $17, $18, $19, $20,
                $21, $22, $23, $24, $25, $26, $27, $28
            )
            ON CONFLICT (account_id) DO NOTHING
            RETURNING version
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "create paper account");
    if (result.row_count() == 0) {
        return conflict(
            PaperPersistenceConflictCode::AccountAlreadyExists,
            "paper account already exists"
        );
    }
    return parse_integer<std::int64_t>(
        result.value(0, "version"),
        "version"
    );
}

std::optional<StoredPaperOrderProcessor>
PostgresPaperOrderRepository::load_account(std::string_view account_id) {
    PostgresTransaction transaction(impl_->connection);
    const auto isolation = execute(
        impl_->connection,
        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY"
    );
    require_status(
        isolation,
        PGRES_COMMAND_OK,
        "start consistent account recovery"
    );

    PostgresParameters parameters;
    parameters.add(account_id);
    const auto result = execute(
        impl_->connection,
        R"SQL(
            SELECT *
            FROM paper_accounts
            WHERE account_id = $1
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load paper account");
    if (result.row_count() == 0) {
        transaction.commit();
        return std::nullopt;
    }

    const auto required_int64 = [&result](std::string_view field) {
        return parse_integer<std::int64_t>(
            result.value(0, field),
            field
        );
    };
    const auto last_fill_timestamp_ns = optional_int64(
        result,
        0,
        "last_fill_timestamp_ns"
    );
    const std::optional<Timestamp> last_fill_timestamp =
        last_fill_timestamp_ns
            ? std::optional<Timestamp>(Timestamp::from_nanoseconds(
                  *last_fill_timestamp_ns
              ))
            : std::nullopt;
    PaperOrderProcessorRecoveryState state{
        .risk_limits = PreTradeRiskLimits{
            .maximum_order_quantity = Quantity::from_raw(
                required_int64("maximum_order_quantity_raw")
            ),
            .maximum_order_notional = Money::from_raw(
                required_int64("maximum_order_notional_raw")
            ),
            .maximum_position_quantity = Quantity::from_raw(
                required_int64("maximum_position_quantity_raw")
            ),
            .maximum_quote_age_nanoseconds =
                required_int64("maximum_quote_age_ns"),
            .fee_per_fill = Money::from_raw(
                required_int64("fee_per_fill_raw")
            ),
        },
        .account = PaperAccountRecoveryState{
            .account_id = result.value(0, "account_id"),
            .instrument = InstrumentId(result.value(0, "instrument")),
            .state = AccountState{
                .starting_cash = Money::from_raw(
                    required_int64("starting_cash_raw")
                ),
                .cash = Money::from_raw(required_int64("cash_raw")),
                .position = Quantity::from_raw(
                    required_int64("position_raw")
                ),
                .open_cost_basis = Money::from_raw(
                    required_int64("open_cost_basis_raw")
                ),
                .gross_realized_pnl = Money::from_raw(
                    required_int64("gross_realized_pnl_raw")
                ),
                .fees_paid = Money::from_raw(
                    required_int64("fees_paid_raw")
                ),
                .last_fill_timestamp = last_fill_timestamp,
            },
            .applied_fill_ids = {},
        },
        .execution = TopOfBookExecutionRecoveryState{
            .account_id = result.value(0, "account_id"),
            .instrument = InstrumentId(result.value(0, "instrument")),
            .active_market = std::nullopt,
            .consumed_bid_quantity = Quantity::from_raw(
                required_int64("consumed_bid_quantity_raw")
            ),
            .consumed_ask_quantity = Quantity::from_raw(
                required_int64("consumed_ask_quantity_raw")
            ),
            .processed_client_order_ids = {},
        },
        .is_healthy = parse_boolean(
            result.value(0, "is_healthy"),
            "is_healthy"
        ),
    };

    const auto stream = result.optional_value(0, "active_stream_id");
    if (stream) {
        const std::uint64_t ingest_sequence = parse_integer<std::uint64_t>(
            result.value(0, "active_ingest_sequence"),
            "active_ingest_sequence"
        );
        const std::uint32_t quality = parse_integer<std::uint32_t>(
            result.value(0, "active_quality_flags"),
            "active_quality_flags"
        );
        const auto bid_price = optional_int64(
            result,
            0,
            "active_bid_price_raw"
        );
        const auto bid_quantity = optional_int64(
            result,
            0,
            "active_bid_quantity_raw"
        );
        const auto ask_price = optional_int64(
            result,
            0,
            "active_ask_price_raw"
        );
        const auto ask_quantity = optional_int64(
            result,
            0,
            "active_ask_quantity_raw"
        );
        if (bid_price.has_value() != bid_quantity.has_value() ||
            ask_price.has_value() != ask_quantity.has_value()) {
            throw std::runtime_error(
                "persisted book level is only partially populated"
            );
        }
        const auto venue_text = result.optional_value(0, "active_venue");
        const std::optional<VenueId> venue = venue_text
            ? std::optional<VenueId>(VenueId(*venue_text))
            : std::nullopt;
        state.execution.active_market = TopOfBookEvent{
            .header = MarketEventHeader{
                .instrument = state.account.instrument,
                .stream = MarketDataStreamId(*stream),
                .venue = venue,
                .event_timestamp = Timestamp::from_nanoseconds(
                    required_int64("active_event_timestamp_ns")
                ),
                .receive_timestamp = Timestamp::from_nanoseconds(
                    required_int64("active_receive_timestamp_ns")
                ),
                .ingest_sequence = ingest_sequence,
                .venue_sequence = optional_uint64(
                    result,
                    0,
                    "active_venue_sequence"
                ),
                .quality = MarketDataQuality::from_raw(quality),
            },
            .bid = bid_price
                ? std::optional<BookLevel>(BookLevel{
                      .price = Price::from_raw(*bid_price),
                      .quantity = Quantity::from_raw(*bid_quantity),
                  })
                : std::nullopt,
            .ask = ask_price
                ? std::optional<BookLevel>(BookLevel{
                      .price = Price::from_raw(*ask_price),
                      .quantity = Quantity::from_raw(*ask_quantity),
                  })
                : std::nullopt,
        };
    }

    PostgresParameters id_parameters;
    id_parameters.add(account_id);
    const auto fill_ids = execute(
        impl_->connection,
        R"SQL(
            SELECT fill_id
            FROM paper_order_records
            WHERE account_id = $1
              AND outcome_kind = 'receipt'
              AND fill_id IS NOT NULL
            ORDER BY order_record_id
        )SQL",
        id_parameters
    );
    require_status(fill_ids, PGRES_TUPLES_OK, "load applied fill IDs");
    for (int row = 0; row < fill_ids.row_count(); ++row) {
        state.account.applied_fill_ids.push_back(
            fill_ids.value(row, "fill_id")
        );
    }

    PostgresParameters order_id_parameters;
    order_id_parameters.add(account_id);
    const auto order_ids = execute(
        impl_->connection,
        R"SQL(
            SELECT client_order_id
            FROM paper_order_records
            WHERE account_id = $1
              AND processed_by_execution
            ORDER BY order_record_id
        )SQL",
        order_id_parameters
    );
    require_status(
        order_ids,
        PGRES_TUPLES_OK,
        "load processed client order IDs"
    );
    for (int row = 0; row < order_ids.row_count(); ++row) {
        state.execution.processed_client_order_ids.push_back(
            order_ids.value(row, "client_order_id")
        );
    }

    const std::int64_t version = required_int64("version");
    StoredPaperOrderProcessor stored{
        .processor = PaperOrderProcessor::restore(std::move(state)),
        .version = version,
    };
    transaction.commit();
    return stored;
}

CommitPaperOrderResult PostgresPaperOrderRepository::commit_order(
    std::string_view account_id,
    std::int64_t expected_version,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    const PaperOrderProcessorRecoveryState& state_after
) {
    if (expected_version < 0) {
        throw std::invalid_argument("expected version must not be negative");
    }
    if (state_after.account.account_id != account_id ||
        state_after.execution.account_id != account_id) {
        throw std::invalid_argument(
            "persisted state does not belong to the requested account"
        );
    }
    static_cast<void>(PaperOrderProcessor::restore(state_after));
    if (const auto* receipt = std::get_if<PaperOrderReceipt>(&result)) {
        if (receipt->execution.intent != intent ||
            receipt->execution.market_ingest_sequence !=
                market.header.ingest_sequence ||
            receipt->account_after != state_after.account.state) {
            throw std::invalid_argument(
                "paper order receipt does not match the persisted request "
                "and state"
            );
        }
    }

    PostgresTransaction transaction(impl_->connection);
    PostgresParameters update_parameters;
    update_parameters.add(account_id);
    add_account_state_parameters(update_parameters, state_after);
    update_parameters.add(expected_version);
    const auto update = execute(
        impl_->connection,
        R"SQL(
            UPDATE paper_accounts SET
                instrument = $2,
                starting_cash_raw = $3,
                cash_raw = $4,
                position_raw = $5,
                open_cost_basis_raw = $6,
                gross_realized_pnl_raw = $7,
                fees_paid_raw = $8,
                last_fill_timestamp_ns = $9,
                maximum_order_quantity_raw = $10,
                maximum_order_notional_raw = $11,
                maximum_position_quantity_raw = $12,
                maximum_quote_age_ns = $13,
                fee_per_fill_raw = $14,
                is_healthy = $15,
                active_stream_id = $16,
                active_venue = $17,
                active_event_timestamp_ns = $18,
                active_receive_timestamp_ns = $19,
                active_ingest_sequence = $20,
                active_venue_sequence = $21,
                active_quality_flags = $22,
                active_bid_price_raw = $23,
                active_bid_quantity_raw = $24,
                active_ask_price_raw = $25,
                active_ask_quantity_raw = $26,
                consumed_bid_quantity_raw = $27,
                consumed_ask_quantity_raw = $28,
                version = version + 1,
                updated_at = clock_timestamp()
            WHERE account_id = $1 AND version = $29
            RETURNING version
        )SQL",
        update_parameters
    );
    require_status(update, PGRES_TUPLES_OK, "update paper account");
    if (update.row_count() == 0) {
        PostgresParameters exists_parameters;
        exists_parameters.add(account_id);
        const auto exists = execute(
            impl_->connection,
            "SELECT 1 FROM paper_accounts WHERE account_id = $1",
            exists_parameters
        );
        require_status(exists, PGRES_TUPLES_OK, "check paper account");
        return exists.row_count() == 0
            ? CommitPaperOrderResult(conflict(
                  PaperPersistenceConflictCode::AccountNotFound,
                  "paper account does not exist"
              ))
            : CommitPaperOrderResult(conflict(
                  PaperPersistenceConflictCode::VersionConflict,
                  "paper account was modified by another session"
              ));
    }
    const std::int64_t new_version = parse_integer<std::int64_t>(
        update.value(0, "version"),
        "version"
    );

    PostgresParameters order_parameters;
    add_order_record_parameters(
        order_parameters,
        account_id,
        intent,
        market,
        result,
        state_after
    );
    const auto order_record = execute(
        impl_->connection,
        R"SQL(
            INSERT INTO paper_order_records (
                account_id, client_order_id, intent_account_id, instrument,
                source_kind, source_id, side, order_type, quantity_raw,
                limit_price_raw, time_in_force, submitted_at_ns,
                market_instrument, market_stream_id, market_venue,
                market_event_timestamp_ns,
                market_receive_timestamp_ns, market_ingest_sequence,
                market_venue_sequence, market_quality_flags,
                market_bid_price_raw, market_bid_quantity_raw,
                market_ask_price_raw, market_ask_quantity_raw,
                outcome_kind, outcome_code, outcome_field, outcome_message,
                execution_status, unfilled_quantity_raw,
                cancellation_reason, execution_market_ingest_sequence,
                fill_id, fill_side, fill_price_raw, fill_quantity_raw,
                fill_fee_raw, fill_timestamp_ns, account_cash_after_raw,
                account_position_after_raw,
                account_open_cost_basis_after_raw,
                account_gross_realized_pnl_after_raw,
                account_fees_paid_after_raw,
                account_last_fill_timestamp_after_ns,
                processed_by_execution
            ) VALUES (
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $10,
                $11, $12, $13, $14, $15, $16, $17, $18, $19, $20,
                $21, $22, $23, $24, $25, $26, $27, $28, $29, $30,
                $31, $32, $33, $34, $35, $36, $37, $38, $39, $40,
                $41, $42, $43, $44, $45
            )
            RETURNING order_record_id
        )SQL",
        order_parameters
    );
    if (order_record.status() != PGRES_TUPLES_OK) {
        if (order_record.sql_state() == "23505" &&
            order_record.constraint_name() ==
                "paper_order_client_idempotency_idx") {
            return conflict(
                PaperPersistenceConflictCode::DuplicateClientOrderId,
                "client order ID has already been persisted"
            );
        }
        throw std::runtime_error(
            "insert paper order record failed: " +
            order_record.error_message()
        );
    }
    const std::int64_t order_record_id = parse_integer<std::int64_t>(
        order_record.value(0, "order_record_id"),
        "order_record_id"
    );

    const auto event_types = event_types_for(result);
    for (std::size_t index = 0; index < event_types.size(); ++index) {
        PostgresParameters event_parameters;
        event_parameters.add(account_id);
        event_parameters.add(order_record_id);
        event_parameters.add(intent.client_order_id);
        event_parameters.add(static_cast<std::int64_t>(index + 1));
        event_parameters.add(event_types[index]);
        const auto event = execute(
            impl_->connection,
            R"SQL(
                INSERT INTO paper_events (
                    account_id,
                    order_record_id,
                    client_order_id,
                    event_order,
                    event_type
                ) VALUES ($1, $2, $3, $4, $5)
            )SQL",
            event_parameters
        );
        require_status(event, PGRES_COMMAND_OK, "insert paper event");
    }

    transaction.commit();
    return new_version;
}

PaperAccountJournalSummary PostgresPaperOrderRepository::journal_summary(
    std::string_view account_id
) {
    PostgresParameters parameters;
    parameters.add(account_id);
    const auto result = execute(
        impl_->connection,
        R"SQL(
            SELECT
                (SELECT count(*) FROM paper_order_records
                 WHERE account_id = $1) AS order_count,
                (SELECT count(*) FROM paper_events
                 WHERE account_id = $1) AS event_count,
                (SELECT count(*) FROM paper_order_records
                 WHERE account_id = $1
                   AND outcome_kind = 'receipt'
                   AND fill_id IS NOT NULL) AS applied_fill_count,
                version AS account_version
            FROM paper_accounts
            WHERE account_id = $1
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load journal summary");
    if (result.row_count() == 0) {
        throw std::invalid_argument("paper account does not exist");
    }
    return PaperAccountJournalSummary{
        .order_count = parse_integer<std::int64_t>(
            result.value(0, "order_count"),
            "order_count"
        ),
        .event_count = parse_integer<std::int64_t>(
            result.value(0, "event_count"),
            "event_count"
        ),
        .applied_fill_count = parse_integer<std::int64_t>(
            result.value(0, "applied_fill_count"),
            "applied_fill_count"
        ),
        .account_version = parse_integer<std::int64_t>(
            result.value(0, "account_version"),
            "account_version"
        ),
    };
}

PersistentPaperOrderSession::PersistentPaperOrderSession(
    PostgresPaperOrderRepository& repository,
    PaperOrderProcessor processor,
    std::int64_t version
)
    : repository_(&repository),
      processor_(std::move(processor)),
      version_(version) {}

std::variant<PersistentPaperOrderSession, PaperPersistenceConflict>
PersistentPaperOrderSession::create(
    PostgresPaperOrderRepository& repository,
    std::string account_id,
    InstrumentId instrument,
    Money starting_cash,
    PreTradeRiskLimits risk_limits
) {
    PaperOrderProcessor processor(
        std::move(account_id),
        std::move(instrument),
        starting_cash,
        risk_limits
    );
    const CreatePaperAccountResult result = repository.create_account(
        processor.recovery_state()
    );
    if (std::holds_alternative<PaperPersistenceConflict>(result)) {
        return std::get<PaperPersistenceConflict>(result);
    }
    return PersistentPaperOrderSession(
        repository,
        std::move(processor),
        std::get<std::int64_t>(result)
    );
}

std::optional<PersistentPaperOrderSession>
PersistentPaperOrderSession::load(
    PostgresPaperOrderRepository& repository,
    std::string_view account_id
) {
    auto stored = repository.load_account(account_id);
    if (!stored) {
        return std::nullopt;
    }
    return PersistentPaperOrderSession(
        repository,
        std::move(stored->processor),
        stored->version
    );
}

PersistentPaperOrderResult PersistentPaperOrderSession::submit(
    const OrderIntent& intent,
    const TopOfBookEvent& market
) {
    PaperOrderProcessorRecoveryState state_before =
        processor_.recovery_state();
    PaperOrderResult result = processor_.submit(intent, market);
    try {
        CommitPaperOrderResult commit = repository_->commit_order(
            processor_.account().account_id(),
            version_,
            intent,
            market,
            result,
            processor_.recovery_state()
        );
        if (std::holds_alternative<PaperPersistenceConflict>(commit)) {
            processor_ = PaperOrderProcessor::restore(
                std::move(state_before)
            );
            return std::get<PaperPersistenceConflict>(std::move(commit));
        }
        version_ = std::get<std::int64_t>(commit);
    } catch (...) {
        processor_ = PaperOrderProcessor::restore(std::move(state_before));
        throw;
    }
    return PersistentPaperOrderReceipt{
        .result = std::move(result),
        .account_version = version_,
    };
}

}  // namespace chronos
