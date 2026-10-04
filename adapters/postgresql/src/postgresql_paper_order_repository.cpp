#include "chronos/postgresql_paper_order_repository.hpp"

#include "postgres_support.hpp"

#include "chronos/paper_order_codes.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
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

using namespace postgres_support;

// Malformed intents are journaled faithfully, so an undeclared intent enum
// value is preserved as its underlying integer instead of being renamed.
template <typename Enum>
std::string intent_text(Enum value) {
    if (const auto name = canonical_name(value)) {
        return std::string(*name);
    }
    return "unknown:" + std::to_string(static_cast<int>(value));
}

// Outcomes are produced by Chronos itself; an undeclared value is a defect
// that must not be written to the journal.
template <typename Enum>
std::string outcome_text(Enum value, std::string_view description) {
    if (const auto name = canonical_name(value)) {
        return std::string(*name);
    }
    throw std::invalid_argument(
        "cannot persist unknown " + std::string(description)
    );
}

// The paper_accounts columns after account_id, in insert order.
void add_account_parameters(
    PostgresParameters& parameters,
    const PaperOrderProcessorRecoveryState& state
) {
    parameters.add(state.account.state.starting_cash.raw());
    parameters.add(state.account.state.cash.raw());
    parameters.add(state.risk_limits.maximum_order_quantity.raw());
    parameters.add(state.risk_limits.maximum_order_notional.raw());
    parameters.add(state.risk_limits.maximum_position_quantity.raw());
    parameters.add(state.risk_limits.maximum_quote_age_nanoseconds);
    parameters.add(state.risk_limits.fee_per_fill.raw());
    parameters.add(state.is_healthy);
    const auto stream = bound_market_stream(state.execution.instruments);
    parameters.add(
        stream ? std::optional<std::string>(stream->value()) : std::nullopt
    );
}

// The eleven `<prefix>stream_id` through `<prefix>ask_quantity_raw` columns
// shared by execution states and the order journal.
void add_top_of_book_parameters(
    PostgresParameters& parameters,
    const TopOfBookEvent& market
) {
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
}

// A position or execution row an order creates or changes.
template <typename Row>
struct RowChange {
    const InstrumentId* instrument;
    const Row* row;
    bool is_new;
};

// Rows keyed by instrument are created and changed, never removed: a closed
// position keeps its ledger and an execution state is replaced in place.
template <typename Row>
std::vector<RowChange<Row>> changed_rows(
    const std::map<InstrumentId, Row>& before,
    const std::map<InstrumentId, Row>& after,
    std::string_view description
) {
    for (const auto& entry : before) {
        if (!after.contains(entry.first)) {
            throw std::invalid_argument(
                "a persisted " + std::string(description) +
                " cannot be removed"
            );
        }
    }
    std::vector<RowChange<Row>> changes;
    for (const auto& [instrument, row] : after) {
        const auto found = before.find(instrument);
        if (found == before.end()) {
            changes.push_back({&instrument, &row, true});
        } else if (found->second != row) {
            changes.push_back({&instrument, &row, false});
        }
    }
    return changes;
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
                    .code = outcome_text(outcome.code, "risk error code"),
                    .field = outcome.field,
                    .message = outcome.message,
                    .outcome_event = "risk_rejected",
                };
            } else if constexpr (
                std::is_same_v<Outcome, SimulatedExecutionError>
            ) {
                return PersistedOutcomeFields{
                    .kind = "execution_error",
                    .code = outcome_text(
                        outcome.code,
                        "execution error code"
                    ),
                    .field = outcome.field,
                    .message = outcome.message,
                    .outcome_event = "execution_rejected",
                };
            } else {
                return PersistedOutcomeFields{
                    .kind = "processing_error",
                    .code = outcome_text(
                        outcome.code,
                        "processing error code"
                    ),
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
    parameters.add(intent_text(intent.source.kind));
    parameters.add(intent.source.source_id);
    parameters.add(intent_text(intent.side));
    parameters.add(intent_text(intent.type));
    parameters.add(intent.quantity.raw());
    parameters.add_optional(
        intent.limit_price
            ? std::optional<std::int64_t>(intent.limit_price->raw())
            : std::nullopt
    );
    parameters.add(intent_text(intent.time_in_force));
    parameters.add(intent.submitted_at.nanoseconds());
    parameters.add(market.header.instrument.value());
    add_top_of_book_parameters(parameters, market);
    parameters.add(outcome.kind);
    parameters.add(outcome.code);
    parameters.add(outcome.field);
    parameters.add(outcome.message);

    const SimulatedExecutionReport* execution = outcome.execution;
    parameters.add(execution
        ? std::optional<std::string>(
              outcome_text(
                  execution->status,
                  "execution status"
              )
          )
        : std::nullopt);
    parameters.add_optional(execution
        ? std::optional<std::int64_t>(execution->unfilled_quantity.raw())
        : std::nullopt);
    parameters.add(execution && execution->cancellation_reason
        ? std::optional<std::string>(outcome_text(
              *execution->cancellation_reason,
              "cancellation reason"
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
        ? std::optional<std::string>(intent_text(fill->side))
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

    // An audit snapshot of the account's cash and the order instrument's
    // ledger after the order; an instrument never filled has an empty one.
    // Recovery reads the position tables, never these columns.
    const AccountState& account = state_after.account.state;
    const PositionState* position = account.position(intent.instrument);
    parameters.add(account.cash.raw());
    parameters.add(intent.instrument.value());
    parameters.add(position ? position->quantity.raw() : std::int64_t{0});
    parameters.add(
        position ? position->open_cost_basis.raw() : std::int64_t{0}
    );
    parameters.add(
        position ? position->gross_realized_pnl.raw() : std::int64_t{0}
    );
    parameters.add(position ? position->fees_paid.raw() : std::int64_t{0});
    parameters.add_optional(position
        ? std::optional<std::int64_t>(
              position->last_fill_timestamp.nanoseconds()
          )
        : std::nullopt);
    parameters.add(outcome.processed_by_execution);
}

// The events a submission appends, in order. A good-til-cancelled order is
// accepted rather than completed, and reports when it filled completely.
std::vector<std::string> event_types_for(
    const OrderIntent& intent,
    const PaperOrderResult& result
) {
    const PersistedOutcomeFields outcome = outcome_fields(result);
    const bool rests = intent.time_in_force == TimeInForce::GoodTilCancelled;
    std::vector<std::string> events{"order_submitted"};
    if (outcome.kind == "receipt" && rests) {
        events.emplace_back("order_accepted");
    } else {
        events.push_back(outcome.outcome_event);
    }
    if (outcome.fill_applied) {
        events.emplace_back("fill_applied");
    }
    if (outcome.kind == "receipt" && rests &&
        outcome.execution->status == SimulatedExecutionStatus::Filled) {
        events.emplace_back("order_filled");
    }
    if (outcome.kind == "receipt" || outcome.kind == "processing_error") {
        events.emplace_back("account_state_updated");
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

std::int64_t persisted_int64(
    const PostgresResult& result,
    int row,
    std::string_view field
) {
    return parse_integer<std::int64_t>(result.value(row, field), field);
}

std::uint64_t persisted_uint64(
    const PostgresResult& result,
    int row,
    std::string_view field
) {
    return parse_integer<std::uint64_t>(result.value(row, field), field);
}

// Reads a top-of-book observation from the `<prefix>stream_id`,
// `<prefix>bid_price_raw`, and related columns. Execution states and the
// order journal share this shape with the `market_` prefix.
TopOfBookEvent persisted_top_of_book(
    const PostgresResult& result,
    int row,
    std::string_view prefix,
    InstrumentId instrument
) {
    const auto column = [prefix](std::string_view name) {
        return std::string(prefix) + std::string(name);
    };
    const auto bid_price = optional_int64(result, row, column("bid_price_raw"));
    const auto bid_quantity = optional_int64(
        result,
        row,
        column("bid_quantity_raw")
    );
    const auto ask_price = optional_int64(result, row, column("ask_price_raw"));
    const auto ask_quantity = optional_int64(
        result,
        row,
        column("ask_quantity_raw")
    );
    if (bid_price.has_value() != bid_quantity.has_value() ||
        ask_price.has_value() != ask_quantity.has_value()) {
        throw std::runtime_error(
            "persisted book level is only partially populated"
        );
    }
    const auto venue = result.optional_value(row, column("venue"));
    return TopOfBookEvent{
        .header = MarketEventHeader{
            .instrument = std::move(instrument),
            .stream = MarketDataStreamId(
                result.value(row, column("stream_id"))
            ),
            .venue = venue
                ? std::optional<VenueId>(VenueId(*venue))
                : std::nullopt,
            .event_timestamp = Timestamp::from_nanoseconds(
                persisted_int64(result, row, column("event_timestamp_ns"))
            ),
            .receive_timestamp = Timestamp::from_nanoseconds(
                persisted_int64(result, row, column("receive_timestamp_ns"))
            ),
            .ingest_sequence = persisted_uint64(
                result,
                row,
                column("ingest_sequence")
            ),
            .venue_sequence = optional_uint64(
                result,
                row,
                column("venue_sequence")
            ),
            .quality = MarketDataQuality::from_raw(parse_integer<std::uint32_t>(
                result.value(row, column("quality_flags")),
                column("quality_flags")
            )),
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

// Every read in one operation sees the same committed database state.
void start_consistent_read(PGconn* connection, std::string_view operation) {
    const auto isolation = execute(
        connection,
        "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ READ ONLY"
    );
    require_status(isolation, PGRES_COMMAND_OK, operation);
}

bool account_exists(PGconn* connection, std::string_view account_id) {
    PostgresParameters parameters;
    parameters.add(account_id);
    const auto result = execute(
        connection,
        "SELECT 1 FROM paper_accounts WHERE account_id = $1",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "check paper account");
    return result.row_count() == 1;
}

// ---------------------------------------------------------------------------
// Journal read model

[[noreturn]] void inconsistent_record(std::string_view detail) {
    throw std::runtime_error(
        "persisted paper order record is inconsistent: " + std::string(detail)
    );
}

// Outcomes, fills, and the intents of executed orders are written with
// canonical names only (see outcome_text).
template <typename Enum>
Enum canonical_value(const std::string& text, std::string_view field) {
    if (const auto value = from_canonical_name<Enum>(text)) {
        return *value;
    }
    inconsistent_record(std::string(field) + " is not a recognized value");
}

// A malformed intent is journaled faithfully as "unknown:<integer>" (see
// intent_text), so its exact in-memory value is restored rather than
// rejected. A declared value must have been written by its canonical name.
template <typename Enum>
Enum journaled_intent_value(const std::string& text, std::string_view field) {
    if (const auto value = from_canonical_name<Enum>(text)) {
        return *value;
    }
    constexpr std::string_view kUndeclaredPrefix = "unknown:";
    if (text.starts_with(kUndeclaredPrefix)) {
        const auto value = static_cast<Enum>(
            parse_integer<std::underlying_type_t<Enum>>(
                std::string_view(text).substr(kUndeclaredPrefix.size()),
                field
            )
        );
        if (!canonical_name(value) && intent_text(value) == text) {
            return value;
        }
    }
    inconsistent_record(std::string(field) + " is not a journaled value");
}

// Journal columns, and each executed order's current state from its
// paper_orders row (absent for rejections). Queries select them FROM
// kOrderRecordSource.
constexpr std::string_view kOrderRecordColumns = R"SQL(
    journal.order_record_id, journal.client_order_id,
    journal.intent_account_id, journal.instrument, journal.source_kind,
    journal.source_id, journal.side, journal.order_type, journal.quantity_raw,
    journal.limit_price_raw, journal.time_in_force, journal.submitted_at_ns,
    journal.market_instrument, journal.market_stream_id, journal.market_venue,
    journal.market_event_timestamp_ns, journal.market_receive_timestamp_ns,
    journal.market_ingest_sequence, journal.market_venue_sequence,
    journal.market_quality_flags, journal.market_bid_price_raw,
    journal.market_bid_quantity_raw, journal.market_ask_price_raw,
    journal.market_ask_quantity_raw, journal.outcome_kind,
    journal.outcome_code, journal.outcome_field, journal.outcome_message,
    journal.execution_status, journal.unfilled_quantity_raw,
    journal.cancellation_reason, journal.execution_market_ingest_sequence,
    journal.fill_id, journal.fill_side, journal.fill_price_raw,
    journal.fill_quantity_raw, journal.fill_fee_raw, journal.fill_timestamp_ns,
    journal.processed_by_execution,
    latest.order_record_id AS latest_order_record_id,
    latest.status AS latest_status,
    latest.filled_quantity_raw AS latest_filled_quantity_raw,
    latest.remaining_quantity_raw AS latest_remaining_quantity_raw,
    latest.filled_notional_raw AS latest_filled_notional_raw,
    latest.fill_count AS latest_fill_count,
    latest.order_fee_raw AS latest_order_fee_raw,
    latest.cumulative_fee_raw AS latest_cumulative_fee_raw,
    latest.reserved_cash_raw AS latest_reserved_cash_raw,
    latest.reserved_quantity_raw AS latest_reserved_quantity_raw,
    latest.cancellation_reason AS latest_cancellation_reason,
    latest.cancel_request_id AS latest_cancel_request_id,
    latest.updated_at_ns AS latest_updated_at_ns,
    latest.instrument AS latest_instrument,
    latest.side AS latest_side,
    latest.order_type AS latest_order_type,
    latest.time_in_force AS latest_time_in_force,
    latest.limit_price_raw AS latest_limit_price_raw,
    latest.quantity_raw AS latest_quantity_raw
)SQL";

constexpr std::string_view kOrderRecordSource = R"SQL(
    FROM paper_order_records AS journal
    LEFT JOIN paper_orders AS latest
        ON latest.order_record_id = journal.order_record_id
)SQL";

OrderIntent persisted_intent(const PostgresResult& result, int row) {
    const auto limit_price = optional_int64(result, row, "limit_price_raw");
    return OrderIntent{
        .client_order_id = result.value(row, "client_order_id"),
        .account_id = result.value(row, "intent_account_id"),
        .source = OrderSource{
            .kind = journaled_intent_value<OrderSourceKind>(
                result.value(row, "source_kind"),
                "source_kind"
            ),
            .source_id = result.value(row, "source_id"),
        },
        .instrument = InstrumentId(result.value(row, "instrument")),
        .side = journaled_intent_value<Side>(result.value(row, "side"), "side"),
        .type = journaled_intent_value<OrderType>(
            result.value(row, "order_type"),
            "order_type"
        ),
        .quantity = Quantity::from_raw(
            persisted_int64(result, row, "quantity_raw")
        ),
        .limit_price = limit_price
            ? std::optional<Price>(Price::from_raw(*limit_price))
            : std::nullopt,
        .time_in_force = journaled_intent_value<TimeInForce>(
            result.value(row, "time_in_force"),
            "time_in_force"
        ),
        .submitted_at = Timestamp::from_nanoseconds(
            persisted_int64(result, row, "submitted_at_ns")
        ),
    };
}

// The simulator produces exactly these status, fill, and quantity shapes:
// an immediate-or-cancel order ends, a good-til-cancelled one may rest.
bool is_consistent_execution(
    SimulatedExecutionStatus status,
    const std::optional<SimulatedCancellationReason>& reason,
    Quantity unfilled,
    const std::optional<Fill>& fill,
    const OrderIntent& intent
) {
    const Quantity ordered = intent.quantity;
    const bool rests = intent.time_in_force == TimeInForce::GoodTilCancelled;
    const bool partial = fill && unfilled.raw() > 0 &&
        fill->quantity + unfilled == ordered;
    switch (status) {
        case SimulatedExecutionStatus::Filled:
            return fill && !reason && unfilled.raw() == 0 &&
                fill->quantity == ordered;
        case SimulatedExecutionStatus::PartiallyFilledAndCancelled:
            return !rests && partial &&
                reason ==
                    SimulatedCancellationReason::DisplayedLiquidityExhausted;
        case SimulatedExecutionStatus::Cancelled:
            return !rests && !fill && unfilled == ordered &&
                (reason == SimulatedCancellationReason::LimitNotMarketable ||
                 reason ==
                     SimulatedCancellationReason::DisplayedLiquidityExhausted);
        case SimulatedExecutionStatus::Working:
            return rests && !fill && !reason && unfilled == ordered;
        case SimulatedExecutionStatus::PartiallyFilled:
            return rests && partial && !reason;
    }
    return false;
}

SimulatedExecutionReport persisted_execution(
    const PostgresResult& result,
    int row,
    const OrderIntent& intent,
    const TopOfBookEvent& market
) {
    const auto status = canonical_value<SimulatedExecutionStatus>(
        result.value(row, "execution_status"),
        "execution_status"
    );
    const auto reason_text = result.optional_value(row, "cancellation_reason");
    const std::optional<SimulatedCancellationReason> reason = reason_text
        ? std::optional<SimulatedCancellationReason>(
              canonical_value<SimulatedCancellationReason>(
                  *reason_text,
                  "cancellation_reason"
              )
          )
        : std::nullopt;
    const Quantity unfilled = Quantity::from_raw(
        persisted_int64(result, row, "unfilled_quantity_raw")
    );
    const std::uint64_t market_sequence = persisted_uint64(
        result,
        row,
        "execution_market_ingest_sequence"
    );
    if (market_sequence != market.header.ingest_sequence) {
        inconsistent_record("execution used a different market observation");
    }

    std::optional<Fill> fill;
    if (!result.is_null(row, "fill_id")) {
        fill = Fill{
            .fill_id = result.value(row, "fill_id"),
            .instrument = intent.instrument,
            .side = canonical_value<Side>(
                result.value(row, "fill_side"),
                "fill_side"
            ),
            .price = Price::from_raw(
                persisted_int64(result, row, "fill_price_raw")
            ),
            .quantity = Quantity::from_raw(
                persisted_int64(result, row, "fill_quantity_raw")
            ),
            .fee = Money::from_raw(
                persisted_int64(result, row, "fill_fee_raw")
            ),
            .timestamp = Timestamp::from_nanoseconds(
                persisted_int64(result, row, "fill_timestamp_ns")
            ),
        };
        if (fill->side != intent.side) {
            inconsistent_record("fill side differs from the order side");
        }
    }
    if (!is_consistent_execution(status, reason, unfilled, fill, intent)) {
        inconsistent_record("execution status, fill, and quantities disagree");
    }
    return SimulatedExecutionReport{
        .intent = intent,
        .status = status,
        .fill = std::move(fill),
        .unfilled_quantity = unfilled,
        .cancellation_reason = reason,
        .market_ingest_sequence = market_sequence,
    };
}

PaperOrderRecordOutcome persisted_outcome(
    const PostgresResult& result,
    int row,
    const OrderIntent& intent,
    const TopOfBookEvent& market
) {
    const std::string kind = result.value(row, "outcome_kind");
    const bool processed = parse_boolean(
        result.value(row, "processed_by_execution"),
        "processed_by_execution"
    );
    const auto code = result.optional_value(row, "outcome_code");
    const auto field = result.optional_value(row, "outcome_field");
    const auto message = result.optional_value(row, "outcome_message");

    if (kind == "receipt") {
        if (code || field || message || !processed) {
            inconsistent_record("an executed order carries a rejection");
        }
        return persisted_execution(result, row, intent, market);
    }
    if (kind == "risk_rejection" || kind == "execution_error") {
        if (!code || !field || !message || processed) {
            inconsistent_record("a rejection is incomplete or was executed");
        }
        if (kind == "risk_rejection") {
            return PreTradeRiskRejection{
                .code = canonical_value<PreTradeRiskErrorCode>(
                    *code,
                    "outcome_code"
                ),
                .field = *field,
                .message = *message,
            };
        }
        return SimulatedExecutionError{
            .code = canonical_value<SimulatedExecutionErrorCode>(
                *code,
                "outcome_code"
            ),
            .field = *field,
            .message = *message,
        };
    }
    if (kind == "processing_error") {
        if (!code || field || !message) {
            inconsistent_record("a processing fault is incomplete");
        }
        return PaperOrderFaultRecord{
            .code = canonical_value<PaperOrderProcessingErrorCode>(
                *code,
                "outcome_code"
            ),
            .message = *message,
            .unbooked_execution = processed
                ? std::optional<SimulatedExecutionReport>(
                      persisted_execution(result, row, intent, market)
                  )
                : std::nullopt,
        };
    }
    inconsistent_record("outcome kind is not recognized");
}

// The reservation a stored order must have: its working reservation, or
// nothing once it is terminal.
OrderReservation expected_reservation(
    const OrderIntent& intent,
    PaperOrderStatus status,
    Quantity filled,
    std::uint32_t fill_count,
    Money order_fee
) {
    if (!is_working(status)) {
        return OrderReservation{};
    }
    return reservation_of(WorkingOrder{
        .intent = intent,
        .accepted_ingest_sequence = 0,
        .fee = order_fee,
        .filled_quantity = filled,
        .fill_count = fill_count,
        .updated_at = intent.submitted_at,
    });
}

// An executed order's current state, checked against its intent, its
// journaled submission outcome, and the rules paper_orders enforces.
PaperOrderState persisted_state(
    const PostgresResult& result,
    int row,
    const OrderIntent& intent,
    const PaperOrderRecordOutcome& outcome
) {
    const auto status = canonical_value<PaperOrderStatus>(
        result.value(row, "latest_status"),
        "latest_status"
    );
    const auto reason_text =
        result.optional_value(row, "latest_cancellation_reason");
    const std::int64_t fill_count =
        persisted_int64(result, row, "latest_fill_count");
    if (fill_count < 0 ||
        fill_count > std::numeric_limits<std::uint32_t>::max()) {
        inconsistent_record("fill count is out of range");
    }
    PaperOrderState state{
        .status = status,
        .filled_quantity = Quantity::from_raw(
            persisted_int64(result, row, "latest_filled_quantity_raw")
        ),
        .remaining_quantity = Quantity::from_raw(
            persisted_int64(result, row, "latest_remaining_quantity_raw")
        ),
        .filled_notional = Money::from_raw(
            persisted_int64(result, row, "latest_filled_notional_raw")
        ),
        .fill_count = static_cast<std::uint32_t>(fill_count),
        .order_fee = Money::from_raw(
            persisted_int64(result, row, "latest_order_fee_raw")
        ),
        .cumulative_fee = Money::from_raw(
            persisted_int64(result, row, "latest_cumulative_fee_raw")
        ),
        .reservation = OrderReservation{
            .cash = Money::from_raw(
                persisted_int64(result, row, "latest_reserved_cash_raw")
            ),
            .quantity = Quantity::from_raw(
                persisted_int64(result, row, "latest_reserved_quantity_raw")
            ),
        },
        .cancellation_reason = reason_text
            ? std::optional<SimulatedCancellationReason>(
                  canonical_value<SimulatedCancellationReason>(
                      *reason_text,
                      "latest_cancellation_reason"
                  )
              )
            : std::nullopt,
        .cancel_request_id =
            result.optional_value(row, "latest_cancel_request_id"),
        .updated_at = Timestamp::from_nanoseconds(
            persisted_int64(result, row, "latest_updated_at_ns")
        ),
    };

    // The order state repeats the intent it was created from.
    const auto limit_price = optional_int64(result, row, "latest_limit_price_raw");
    if (result.value(row, "latest_instrument") != intent.instrument.value() ||
        result.value(row, "latest_side") != intent_text(intent.side) ||
        result.value(row, "latest_order_type") != intent_text(intent.type) ||
        result.value(row, "latest_time_in_force") !=
            intent_text(intent.time_in_force) ||
        limit_price !=
            (intent.limit_price
                 ? std::optional<std::int64_t>(intent.limit_price->raw())
                 : std::nullopt) ||
        persisted_int64(result, row, "latest_quantity_raw") !=
            intent.quantity.raw()) {
        inconsistent_record("order state does not repeat its journaled intent");
    }

    const bool filled_any = state.filled_quantity.raw() > 0;
    if (status == PaperOrderStatus::Rejected ||
        state.filled_quantity.raw() < 0 ||
        state.remaining_quantity.raw() < 0 ||
        state.filled_quantity + state.remaining_quantity != intent.quantity ||
        (state.fill_count > 0) != filled_any ||
        (state.filled_notional.raw() > 0) != filled_any ||
        state.cumulative_fee != (filled_any ? state.order_fee : Money{}) ||
        state.updated_at < intent.submitted_at ||
        state.cancel_request_id.has_value() !=
            (state.cancellation_reason ==
             SimulatedCancellationReason::UserRequested)) {
        inconsistent_record("order state is not internally consistent");
    }
    try {
        if (state.reservation != expected_reservation(
                intent,
                status,
                state.filled_quantity,
                state.fill_count,
                state.order_fee
            )) {
            inconsistent_record("order reservation disagrees with the order");
        }
    } catch (const std::invalid_argument&) {
        inconsistent_record("a working order is not a good-til-cancelled one");
    } catch (const std::overflow_error&) {
        inconsistent_record("order reservation exceeds the fixed-point range");
    }

    // Where the order can be, given how its submission ended.
    if (const auto* fault = std::get_if<PaperOrderFaultRecord>(&outcome)) {
        if (!fault->unbooked_execution || status != PaperOrderStatus::Faulted ||
            filled_any) {
            inconsistent_record("a faulted submission's state is not faulted");
        }
        return state;
    }
    const auto* execution = std::get_if<SimulatedExecutionReport>(&outcome);
    if (execution == nullptr) {
        inconsistent_record("a rejected submission has an order state");
    }
    const Quantity submitted_fill =
        execution->fill ? execution->fill->quantity : Quantity{};
    if (intent.time_in_force == TimeInForce::ImmediateOrCancel) {
        // An immediate-or-cancel order never changes after submission.
        if (*canonical_name(status) != *canonical_name(execution->status) ||
            state.filled_quantity != submitted_fill ||
            state.cancellation_reason != execution->cancellation_reason ||
            state.fill_count != (execution->fill ? 1U : 0U)) {
            inconsistent_record("an immediate order changed after submission");
        }
        return state;
    }
    if (state.filled_quantity < submitted_fill ||
        (execution->status == SimulatedExecutionStatus::Filled &&
         status != PaperOrderStatus::Filled) ||
        (status == PaperOrderStatus::Working &&
         execution->status != SimulatedExecutionStatus::Working) ||
        (state.cancellation_reason &&
         state.cancellation_reason !=
             SimulatedCancellationReason::UserRequested)) {
        inconsistent_record("a working order's state cannot follow its "
                            "submission");
    }
    return state;
}

PaperOrderRecord persisted_order(const PostgresResult& result, int row) {
    OrderIntent intent = persisted_intent(result, row);
    TopOfBookEvent market = persisted_top_of_book(
        result,
        row,
        "market_",
        InstrumentId(result.value(row, "market_instrument"))
    );
    PaperOrderRecordOutcome outcome = persisted_outcome(
        result,
        row,
        intent,
        market
    );
    const bool executed = parse_boolean(
        result.value(row, "processed_by_execution"),
        "processed_by_execution"
    );
    const bool has_state = !result.is_null(row, "latest_order_record_id");
    if (executed != has_state) {
        inconsistent_record("an executed order has no current state, or a "
                            "rejection has one");
    }
    std::optional<PaperOrderState> state;
    if (has_state) {
        state = persisted_state(result, row, intent, outcome);
    }
    return PaperOrderRecord{
        .record_id = persisted_int64(result, row, "order_record_id"),
        .intent = std::move(intent),
        .market = std::move(market),
        .outcome = std::move(outcome),
        .state = std::move(state),
    };
}

// Columns of paper_fills joined to its order's journal record.
constexpr std::string_view kFillColumns = R"SQL(
    booked.fill_record_id, booked.account_id, booked.client_order_id,
    booked.fill_index, booked.fill_id, booked.instrument, booked.side,
    booked.price_raw, booked.quantity_raw, booked.fee_raw,
    booked.notional_raw, booked.filled_at_ns, booked.market_ingest_sequence,
    journal.side AS order_side, journal.source_kind, journal.source_id
)SQL";

constexpr std::string_view kFillSource = R"SQL(
    FROM paper_fills AS booked
    JOIN paper_order_records AS journal
        ON journal.order_record_id = booked.order_record_id
)SQL";

// Only executed orders have fills, so their intents passed risk and use
// canonical names.
Fill persisted_booked_fill(
    const PostgresResult& result,
    int row,
    std::string_view prefix
) {
    const auto column = [prefix](std::string_view name) {
        return std::string(prefix) + std::string(name);
    };
    Fill fill{
        .fill_id = result.value(row, column("fill_id")),
        .instrument = InstrumentId(result.value(row, column("instrument"))),
        .side = canonical_value<Side>(result.value(row, column("side")), "side"),
        .price = Price::from_raw(persisted_int64(result, row, column("price_raw"))),
        .quantity = Quantity::from_raw(
            persisted_int64(result, row, column("quantity_raw"))
        ),
        .fee = Money::from_raw(persisted_int64(result, row, column("fee_raw"))),
        .timestamp = Timestamp::from_nanoseconds(
            persisted_int64(result, row, column("filled_at_ns"))
        ),
    };
    if (calculate_notional(fill.price, fill.quantity) !=
        Money::from_raw(persisted_int64(result, row, column("notional_raw")))) {
        inconsistent_record("a fill's notional disagrees with its price");
    }
    return fill;
}

PaperFillRecord persisted_fill(const PostgresResult& result, int row) {
    Fill fill = persisted_booked_fill(result, row, "");
    if (fill.side != canonical_value<Side>(
            result.value(row, "order_side"),
            "order_side"
        )) {
        inconsistent_record("fill side differs from the order side");
    }
    const std::int64_t index = persisted_int64(result, row, "fill_index");
    const std::string client_order_id = result.value(row, "client_order_id");
    if (index < 0 || index > std::numeric_limits<std::uint32_t>::max() ||
        fill.fill_id !=
            fill_id_for(client_order_id, static_cast<std::uint32_t>(index))) {
        inconsistent_record("a fill's ID does not name its order and index");
    }
    return PaperFillRecord{
        .record_id = persisted_int64(result, row, "fill_record_id"),
        .account_id = result.value(row, "account_id"),
        .client_order_id = client_order_id,
        .source = OrderSource{
            .kind = canonical_value<OrderSourceKind>(
                result.value(row, "source_kind"),
                "source_kind"
            ),
            .source_id = result.value(row, "source_id"),
        },
        .fill = std::move(fill),
        .fill_index = static_cast<std::uint32_t>(index),
        .market_ingest_sequence =
            persisted_uint64(result, row, "market_ingest_sequence"),
    };
}

void validate_page_request(const PaperJournalPageRequest& request) {
    if (request.limit == 0 ||
        request.limit >=
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("journal page limit is out of range");
    }
    if (request.before_record_id && *request.before_record_id <= 0) {
        throw std::invalid_argument(
            "journal page must start before a positive record"
        );
    }
}

void add_page_parameters(
    PostgresParameters& parameters,
    std::string_view account_id,
    const PaperJournalPageRequest& request
) {
    parameters.add(account_id);
    parameters.add_optional(request.before_record_id);
    parameters.add(static_cast<std::int64_t>(request.limit + 1));
    parameters.add(
        request.instrument
            ? std::optional<std::string>(request.instrument->value())
            : std::nullopt
    );
}

// The query fetched at most limit + 1 rows, newest first; the extra row only
// signals that an older page exists.
template <typename Record, typename MapRow>
PaperJournalPage<Record> journal_page(
    const PostgresResult& result,
    std::size_t limit,
    MapRow map_row
) {
    const auto rows = static_cast<std::size_t>(result.row_count());
    const std::size_t returned = std::min(rows, limit);
    PaperJournalPage<Record> page;
    page.records.reserve(returned);
    for (std::size_t row = 0; row < returned; ++row) {
        Record record = map_row(result, static_cast<int>(row));
        if (!page.records.empty() &&
            record.record_id >= page.records.back().record_id) {
            inconsistent_record("journal page is not newest first");
        }
        page.records.push_back(std::move(record));
    }
    if (rows > limit) {
        page.next_before_record_id = page.records.back().record_id;
    }
    return page;
}

void validate_forward_request(const PaperForwardPageRequest& request) {
    if (request.limit == 0 ||
        request.limit >=
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("page limit is out of range");
    }
    if (request.after && *request.after < 0) {
        throw std::invalid_argument("page must start after a position");
    }
}

// The query fetched at most limit + 1 rows, oldest first; the extra row only
// signals that a later page exists.
template <typename Record, typename MapRow, typename PositionOf>
PaperForwardPage<Record> forward_page(
    const PostgresResult& result,
    std::size_t limit,
    MapRow map_row,
    PositionOf position_of
) {
    const auto rows = static_cast<std::size_t>(result.row_count());
    const std::size_t returned = std::min(rows, limit);
    PaperForwardPage<Record> page;
    page.records.reserve(returned);
    for (std::size_t row = 0; row < returned; ++row) {
        Record record = map_row(result, static_cast<int>(row));
        if (!page.records.empty() &&
            position_of(record) <= position_of(page.records.back())) {
            inconsistent_record("page is not oldest first");
        }
        page.records.push_back(std::move(record));
    }
    if (rows > limit) {
        page.next_after = position_of(page.records.back());
    }
    return page;
}

// ---------------------------------------------------------------------------
// Account recovery

// An account's working orders, in priority order, checked against the
// reservations and the stream PostgreSQL stored for them.
std::vector<WorkingOrder> load_working_orders(
    PGconn* connection,
    std::string_view account_id,
    const std::optional<std::string>& market_stream
) {
    PostgresParameters parameters;
    parameters.add(account_id);
    const auto result = execute(
        connection,
        R"SQL(
            SELECT
                latest.status AS latest_status,
                latest.filled_quantity_raw AS latest_filled_quantity_raw,
                latest.remaining_quantity_raw AS latest_remaining_quantity_raw,
                latest.fill_count AS latest_fill_count,
                latest.order_fee_raw AS latest_order_fee_raw,
                latest.reserved_cash_raw AS latest_reserved_cash_raw,
                latest.reserved_quantity_raw AS latest_reserved_quantity_raw,
                latest.accepted_market_ingest_sequence,
                latest.updated_at_ns AS latest_updated_at_ns,
                latest.market_stream_id AS latest_market_stream_id,
                journal.client_order_id, journal.intent_account_id,
                journal.instrument, journal.source_kind, journal.source_id,
                journal.side, journal.order_type, journal.quantity_raw,
                journal.limit_price_raw, journal.time_in_force,
                journal.submitted_at_ns, journal.market_ingest_sequence
            FROM paper_orders AS latest
            JOIN paper_order_records AS journal
                ON journal.order_record_id = latest.order_record_id
            WHERE latest.account_id = $1
              AND latest.status IN ('working', 'partially_filled')
            ORDER BY latest.order_record_id
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load working orders");
    std::vector<WorkingOrder> orders;
    orders.reserve(static_cast<std::size_t>(result.row_count()));
    for (int row = 0; row < result.row_count(); ++row) {
        const std::int64_t fill_count =
            persisted_int64(result, row, "latest_fill_count");
        if (fill_count < 0 ||
            fill_count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("persisted fill count is out of range");
        }
        WorkingOrder order{
            .intent = persisted_intent(result, row),
            .accepted_ingest_sequence = persisted_uint64(
                result,
                row,
                "accepted_market_ingest_sequence"
            ),
            .fee = Money::from_raw(
                persisted_int64(result, row, "latest_order_fee_raw")
            ),
            .filled_quantity = Quantity::from_raw(
                persisted_int64(result, row, "latest_filled_quantity_raw")
            ),
            .fill_count = static_cast<std::uint32_t>(fill_count),
            .updated_at = Timestamp::from_nanoseconds(
                persisted_int64(result, row, "latest_updated_at_ns")
            ),
        };
        const OrderReservation stored{
            .cash = Money::from_raw(
                persisted_int64(result, row, "latest_reserved_cash_raw")
            ),
            .quantity = Quantity::from_raw(
                persisted_int64(result, row, "latest_reserved_quantity_raw")
            ),
        };
        OrderReservation computed;
        try {
            computed = reservation_of(order);
        } catch (const std::exception&) {
            throw std::runtime_error(
                "persisted working order is not a working limit order"
            );
        }
        if (canonical_name(order.status()) !=
                std::optional<std::string_view>(
                    result.value(row, "latest_status")
                ) ||
            order.remaining_quantity() != Quantity::from_raw(persisted_int64(
                result,
                row,
                "latest_remaining_quantity_raw"
            )) ||
            stored != computed ||
            order.accepted_ingest_sequence !=
                persisted_uint64(result, row, "market_ingest_sequence") ||
            !market_stream ||
            result.value(row, "latest_market_stream_id") != *market_stream) {
            throw std::runtime_error(
                "persisted working order disagrees with its journal, stream, "
                "or reservation"
            );
        }
        orders.push_back(std::move(order));
    }
    return orders;
}

// Recovers an account inside an open transaction: a repeatable-read
// snapshot for reads, or, with `lock`, the transaction that takes the
// account's row lock before anything else.
std::optional<StoredPaperOrderProcessor> load_account_rows(
    PGconn* connection,
    std::string_view account_id,
    bool lock
) {
    PostgresParameters parameters;
    parameters.add(account_id);
    const auto result = execute(
        connection,
        std::string(R"SQL(
            SELECT
                account_id, starting_cash_raw, cash_raw,
                maximum_order_quantity_raw, maximum_order_notional_raw,
                maximum_position_quantity_raw, maximum_quote_age_ns,
                fee_per_fill_raw, is_healthy, market_stream_id, version
            FROM paper_accounts
            WHERE account_id = $1
        )SQL") + (lock ? " FOR UPDATE" : ""),
        parameters
    );
    // Another session held the account longer than the lock timeout. The
    // connection is intact, so the caller may simply try again.
    if (result.status() != PGRES_TUPLES_OK && result.sql_state() == "55P03") {
        throw PaperPersistenceUnavailable(
            "timed out waiting for the paper account's lock"
        );
    }
    require_status(result, PGRES_TUPLES_OK, "load paper account");
    if (result.row_count() == 0) {
        return std::nullopt;
    }

    const auto required_int64 = [&result](std::string_view field) {
        return parse_integer<std::int64_t>(
            result.value(0, field),
            field
        );
    };
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
            .state = AccountState{
                .starting_cash = Money::from_raw(
                    required_int64("starting_cash_raw")
                ),
                .cash = Money::from_raw(required_int64("cash_raw")),
                .positions = {},
            },
            .applied_fill_ids = {},
        },
        .execution = TopOfBookExecutionRecoveryState{
            .account_id = result.value(0, "account_id"),
            .instruments = {},
            .processed_client_order_ids = {},
        },
        .is_healthy = parse_boolean(
            result.value(0, "is_healthy"),
            "is_healthy"
        ),
    };
    const auto market_stream = result.optional_value(0, "market_stream_id");

    PostgresParameters position_parameters;
    position_parameters.add(account_id);
    const auto positions = execute(
        connection,
        R"SQL(
            SELECT
                instrument, quantity_raw, open_cost_basis_raw,
                gross_realized_pnl_raw, fees_paid_raw, last_fill_timestamp_ns
            FROM paper_positions
            WHERE account_id = $1
        )SQL",
        position_parameters
    );
    require_status(positions, PGRES_TUPLES_OK, "load paper positions");
    for (int row = 0; row < positions.row_count(); ++row) {
        state.account.state.positions.emplace(
            InstrumentId(positions.value(row, "instrument")),
            PositionState{
                .quantity = Quantity::from_raw(
                    persisted_int64(positions, row, "quantity_raw")
                ),
                .open_cost_basis = Money::from_raw(
                    persisted_int64(positions, row, "open_cost_basis_raw")
                ),
                .gross_realized_pnl = Money::from_raw(
                    persisted_int64(positions, row, "gross_realized_pnl_raw")
                ),
                .fees_paid = Money::from_raw(
                    persisted_int64(positions, row, "fees_paid_raw")
                ),
                .last_fill_timestamp = Timestamp::from_nanoseconds(
                    persisted_int64(positions, row, "last_fill_timestamp_ns")
                ),
            }
        );
    }

    PostgresParameters execution_parameters;
    execution_parameters.add(account_id);
    const auto executions = execute(
        connection,
        R"SQL(
            SELECT
                instrument, market_stream_id, market_venue,
                market_event_timestamp_ns, market_receive_timestamp_ns,
                market_ingest_sequence, market_venue_sequence,
                market_quality_flags, market_bid_price_raw,
                market_bid_quantity_raw, market_ask_price_raw,
                market_ask_quantity_raw, consumed_bid_quantity_raw,
                consumed_ask_quantity_raw
            FROM paper_execution_states
            WHERE account_id = $1
        )SQL",
        execution_parameters
    );
    require_status(executions, PGRES_TUPLES_OK, "load execution states");
    for (int row = 0; row < executions.row_count(); ++row) {
        InstrumentId instrument(executions.value(row, "instrument"));
        if (!market_stream ||
            executions.value(row, "market_stream_id") != *market_stream) {
            throw std::runtime_error(
                "persisted execution state is not on the account's stream"
            );
        }
        TopOfBookEvent active_market = persisted_top_of_book(
            executions,
            row,
            "market_",
            instrument
        );
        state.execution.instruments.emplace(
            std::move(instrument),
            InstrumentExecutionState{
                .active_market = std::move(active_market),
                .consumed_bid_quantity = Quantity::from_raw(persisted_int64(
                    executions,
                    row,
                    "consumed_bid_quantity_raw"
                )),
                .consumed_ask_quantity = Quantity::from_raw(persisted_int64(
                    executions,
                    row,
                    "consumed_ask_quantity_raw"
                )),
            }
        );
    }
    if (market_stream && state.execution.instruments.empty()) {
        throw std::runtime_error(
            "persisted account is bound to a stream without executing"
        );
    }

    PostgresParameters id_parameters;
    id_parameters.add(account_id);
    const auto fill_ids = execute(
        connection,
        R"SQL(
            SELECT fill_id
            FROM paper_fills
            WHERE account_id = $1
            ORDER BY fill_record_id
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
        connection,
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

    state.working_orders =
        load_working_orders(connection, account_id, market_stream);

    const std::int64_t version = required_int64("version");
    return StoredPaperOrderProcessor{
        .processor = PaperOrderProcessor::restore(std::move(state)),
        .version = version,
    };
}

// ---------------------------------------------------------------------------
// Account changes
//
// Every mutation writes, in this order and inside one transaction: the
// account row, which is locked first and version-checked; changed
// execution states and positions; working orders' fills on the
// observation; the new submission's journal record, order state, and fill;
// a cancellation; and each changed order's events. Nothing here begins or
// ends the transaction.

struct AccountChange {
    // Working orders' fills on an observation, before any submission.
    const WorkingOrderMatch* match{nullptr};
    // A submission: all three, or none.
    const OrderIntent* intent{nullptr};
    const TopOfBookEvent* market{nullptr};
    const PaperOrderResult* result{nullptr};
    const WorkingOrderCancellation* cancellation{nullptr};
    std::string_view cancel_request_id;
};

void append_event(
    PGconn* connection,
    std::string_view account_id,
    std::int64_t order_record_id,
    std::string_view client_order_id,
    std::string_view type,
    Timestamp occurred_at,
    const std::optional<std::string>& fill_id = std::nullopt
) {
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(order_record_id);
    parameters.add(client_order_id);
    parameters.add(type);
    parameters.add(occurred_at.nanoseconds());
    parameters.add(fill_id);
    // The account row lock serializes writers of this order, so the next
    // position cannot be taken concurrently.
    const auto event = execute(
        connection,
        R"SQL(
            INSERT INTO paper_events (
                account_id,
                order_record_id,
                client_order_id,
                event_order,
                event_type,
                occurred_at_ns,
                fill_id
            ) VALUES (
                $1, $2, $3,
                (SELECT coalesce(max(event_order), 0) + 1
                 FROM paper_events WHERE order_record_id = $2),
                $4, $5, $6
            )
        )SQL",
        parameters
    );
    require_status(event, PGRES_COMMAND_OK, "insert paper event");
}

void insert_fill(
    PGconn* connection,
    std::string_view account_id,
    std::int64_t order_record_id,
    std::string_view client_order_id,
    std::uint32_t fill_index,
    const ApplyFillReceipt& accounting,
    const TopOfBookEvent& observation
) {
    const Fill& fill = accounting.fill;
    if (fill.fill_id != fill_id_for(std::string(client_order_id), fill_index)) {
        throw std::invalid_argument("a fill ID must name its order and index");
    }
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(order_record_id);
    parameters.add(client_order_id);
    parameters.add(fill_index);
    parameters.add(fill.fill_id);
    parameters.add(fill.instrument.value());
    parameters.add(outcome_text(fill.side, "fill side"));
    parameters.add(fill.price.raw());
    parameters.add(fill.quantity.raw());
    parameters.add(fill.fee.raw());
    parameters.add(accounting.notional.raw());
    parameters.add(fill.timestamp.nanoseconds());
    parameters.add(observation.header.stream.value());
    parameters.add(observation.header.ingest_sequence);
    const auto inserted = execute(
        connection,
        R"SQL(
            INSERT INTO paper_fills (
                account_id, order_record_id, client_order_id, fill_index,
                fill_id, instrument, side, price_raw, quantity_raw, fee_raw,
                notional_raw, filled_at_ns, market_stream_id,
                market_ingest_sequence
            ) VALUES (
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14
            )
        )SQL",
        parameters
    );
    require_status(inserted, PGRES_COMMAND_OK, "insert paper fill");
}

// A working order's stored row, at exactly the state `before`, moved to
// `status` with the quantities of `after`. Returns its journal record ID,
// or nullopt when the cancel request ID already cancelled another order.
std::optional<std::int64_t> write_order_transition(
    PGconn* connection,
    std::string_view account_id,
    const WorkingOrder& before,
    const WorkingOrder& after,
    PaperOrderStatus status,
    Money added_notional,
    Timestamp updated_at,
    std::optional<std::string_view> cancel_request_id = std::nullopt
) {
    const bool user_cancelled = cancel_request_id.has_value();
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(before.intent.client_order_id);
    parameters.add(after.filled_quantity.raw());
    parameters.add(after.remaining_quantity().raw());
    parameters.add(added_notional.raw());
    parameters.add(after.fill_count);
    parameters.add(outcome_text(status, "order status"));
    parameters.add(user_cancelled
        ? std::optional<std::string>(outcome_text(
              SimulatedCancellationReason::UserRequested,
              "cancellation reason"
          ))
        : std::nullopt);
    parameters.add(user_cancelled
        ? std::optional<std::string>(std::string(*cancel_request_id))
        : std::nullopt);
    parameters.add(updated_at.nanoseconds());
    parameters.add(before.fill_count);
    parameters.add(before.filled_quantity.raw());
    const auto updated = execute(
        connection,
        R"SQL(
            UPDATE paper_orders SET
                filled_quantity_raw = $3,
                remaining_quantity_raw = $4,
                filled_notional_raw = filled_notional_raw + $5,
                fill_count = $6,
                status = $7,
                cancellation_reason = $8,
                cancel_request_id = $9,
                updated_at_ns = $10,
                updated_at = clock_timestamp()
            WHERE account_id = $1
              AND client_order_id = $2
              AND status IN ('working', 'partially_filled')
              AND fill_count = $11
              AND filled_quantity_raw = $12
            RETURNING order_record_id
        )SQL",
        parameters
    );
    if (updated.status() != PGRES_TUPLES_OK && updated.sql_state() == "23505" &&
        updated.constraint_name() ==
            "paper_orders_cancel_request_key") {
        return std::nullopt;
    }
    require_status(updated, PGRES_TUPLES_OK, "update working order");
    if (updated.row_count() != 1) {
        throw std::runtime_error(
            "stored working order differs from the recovered state"
        );
    }
    return parse_integer<std::int64_t>(
        updated.value(0, "order_record_id"),
        "order_record_id"
    );
}

const WorkingOrder& working_order_before(
    const PaperOrderProcessorRecoveryState& before,
    std::string_view client_order_id
) {
    const auto found = std::find_if(
        before.working_orders.begin(),
        before.working_orders.end(),
        [client_order_id](const WorkingOrder& order) {
            return order.intent.client_order_id == client_order_id;
        }
    );
    if (found == before.working_orders.end()) {
        throw std::invalid_argument(
            "a changed order was not working before the change"
        );
    }
    return *found;
}

void write_match(
    PGconn* connection,
    std::string_view account_id,
    const WorkingOrderMatch& match,
    const PaperOrderProcessorRecoveryState& before
) {
    for (const WorkingOrderFill& filled : match.fills) {
        const WorkingOrder& order =
            working_order_before(before, filled.order.intent.client_order_id);
        const std::int64_t order_record_id = *write_order_transition(
            connection,
            account_id,
            order,
            filled.order,
            filled.status,
            filled.accounting.notional,
            filled.accounting.fill.timestamp
        );
        insert_fill(
            connection,
            account_id,
            order_record_id,
            order.intent.client_order_id,
            order.fill_count,
            filled.accounting,
            match.observation
        );
        const Timestamp at = filled.accounting.fill.timestamp;
        append_event(
            connection,
            account_id,
            order_record_id,
            order.intent.client_order_id,
            "fill_applied",
            at,
            filled.accounting.fill.fill_id
        );
        if (filled.status == PaperOrderStatus::Filled) {
            append_event(
                connection,
                account_id,
                order_record_id,
                order.intent.client_order_id,
                "order_filled",
                at
            );
        }
        append_event(
            connection,
            account_id,
            order_record_id,
            order.intent.client_order_id,
            "account_state_updated",
            at
        );
    }
    if (match.fault) {
        const WorkingOrder& order =
            working_order_before(before, match.fault->order.intent.client_order_id);
        const Timestamp at = match.fault->unbooked_fill.timestamp;
        const std::int64_t order_record_id = *write_order_transition(
            connection,
            account_id,
            order,
            order,
            PaperOrderStatus::Faulted,
            Money{},
            std::max(at, order.updated_at)
        );
        append_event(
            connection,
            account_id,
            order_record_id,
            order.intent.client_order_id,
            "processor_faulted",
            at
        );
        append_event(
            connection,
            account_id,
            order_record_id,
            order.intent.client_order_id,
            "account_state_updated",
            at
        );
    }
}

// An executed submission's order state, as its execution left it.
void insert_order_state(
    PGconn* connection,
    std::string_view account_id,
    std::int64_t order_record_id,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const SimulatedExecutionReport& execution,
    const std::optional<ApplyFillReceipt>& accounting,
    bool faulted,
    Money account_fee
) {
    const std::optional<Fill>& fill = accounting
        ? std::optional<Fill>(accounting->fill)
        : std::nullopt;
    const Quantity filled = fill ? fill->quantity : Quantity{};
    PostgresParameters parameters;
    parameters.add(order_record_id);
    parameters.add(account_id);
    parameters.add(intent.client_order_id);
    parameters.add(intent.instrument.value());
    parameters.add(outcome_text(intent.side, "order side"));
    parameters.add(outcome_text(intent.type, "order type"));
    parameters.add(outcome_text(intent.time_in_force, "time in force"));
    parameters.add_optional(
        intent.limit_price
            ? std::optional<std::int64_t>(intent.limit_price->raw())
            : std::nullopt
    );
    parameters.add(intent.quantity.raw());
    parameters.add(filled.raw());
    parameters.add((intent.quantity - filled).raw());
    parameters.add(accounting ? accounting->notional.raw() : std::int64_t{0});
    parameters.add(fill ? std::int64_t{1} : std::int64_t{0});
    parameters.add(fill ? fill->fee.raw() : account_fee.raw());
    parameters.add(
        faulted ? std::string("faulted")
                : outcome_text(execution.status, "execution status")
    );
    parameters.add(!faulted && execution.cancellation_reason
        ? std::optional<std::string>(outcome_text(
              *execution.cancellation_reason,
              "cancellation reason"
          ))
        : std::nullopt);
    parameters.add(market.header.stream.value());
    parameters.add(market.header.ingest_sequence);
    parameters.add(intent.submitted_at.nanoseconds());
    const auto inserted = execute(
        connection,
        R"SQL(
            INSERT INTO paper_orders (
                order_record_id, account_id, client_order_id, instrument,
                side, order_type, time_in_force, limit_price_raw,
                quantity_raw, filled_quantity_raw, remaining_quantity_raw,
                filled_notional_raw, fill_count, order_fee_raw, status,
                cancellation_reason, market_stream_id,
                accepted_market_ingest_sequence, submitted_at_ns,
                updated_at_ns
            ) VALUES (
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14,
                $15, $16, $17, $18, $19, $19
            )
        )SQL",
        parameters
    );
    require_status(inserted, PGRES_COMMAND_OK, "insert order state");
}

// The journal record, order state, fill, and events of one submission.
// Returns nullopt when the client order ID was already recorded.
std::optional<std::int64_t> write_submission(
    PGconn* connection,
    std::string_view account_id,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    const PaperOrderProcessorRecoveryState& after
) {
    PostgresParameters order_parameters;
    add_order_record_parameters(
        order_parameters,
        account_id,
        intent,
        market,
        result,
        after
    );
    const auto order_record = execute(
        connection,
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
                position_after_instrument, position_quantity_after_raw,
                position_open_cost_basis_after_raw,
                position_gross_realized_pnl_after_raw,
                position_fees_paid_after_raw,
                position_last_fill_timestamp_after_ns,
                processed_by_execution
            ) VALUES (
                $1, $2, $3, $4, $5, $6, $7, $8, $9, $10,
                $11, $12, $13, $14, $15, $16, $17, $18, $19, $20,
                $21, $22, $23, $24, $25, $26, $27, $28, $29, $30,
                $31, $32, $33, $34, $35, $36, $37, $38, $39, $40,
                $41, $42, $43, $44, $45, $46
            )
            RETURNING order_record_id
        )SQL",
        order_parameters
    );
    if (order_record.status() != PGRES_TUPLES_OK) {
        if (order_record.sql_state() == "23505" &&
            order_record.constraint_name() ==
                "paper_order_client_idempotency_idx") {
            return std::nullopt;
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

    std::optional<std::string> fill_id;
    if (const auto* receipt = std::get_if<PaperOrderReceipt>(&result)) {
        insert_order_state(
            connection,
            account_id,
            order_record_id,
            intent,
            market,
            receipt->execution,
            receipt->accounting,
            false,
            after.risk_limits.fee_per_fill
        );
        if (receipt->accounting) {
            insert_fill(
                connection,
                account_id,
                order_record_id,
                intent.client_order_id,
                0,
                *receipt->accounting,
                market
            );
            fill_id = receipt->accounting->fill.fill_id;
        }
    } else if (const auto* fault =
                   std::get_if<PaperOrderProcessingError>(&result);
               fault != nullptr && fault->execution) {
        // The simulated fill accounting rejected is never booked.
        insert_order_state(
            connection,
            account_id,
            order_record_id,
            intent,
            market,
            *fault->execution,
            std::nullopt,
            true,
            after.risk_limits.fee_per_fill
        );
    }

    for (const std::string& type : event_types_for(intent, result)) {
        append_event(
            connection,
            account_id,
            order_record_id,
            intent.client_order_id,
            type,
            intent.submitted_at,
            type == "fill_applied" ? fill_id : std::nullopt
        );
    }
    return order_record_id;
}

// The working orders a change must leave, derived from the orders before it
// and the change itself; the processor's state after the change must be
// exactly these, so the rows written describe the recovered state.
void require_working_orders_follow(
    const AccountChange& change,
    const PaperOrderProcessorRecoveryState& before,
    const PaperOrderProcessorRecoveryState& after
) {
    std::vector<WorkingOrder> expected = before.working_orders;
    const auto remove = [&expected](std::string_view client_order_id) {
        const auto found = std::find_if(
            expected.begin(),
            expected.end(),
            [client_order_id](const WorkingOrder& order) {
                return order.intent.client_order_id == client_order_id;
            }
        );
        if (found == expected.end()) {
            throw std::invalid_argument(
                "a changed order was not working before the change"
            );
        }
        return expected.erase(found);
    };
    if (change.match) {
        for (const WorkingOrderFill& filled : change.match->fills) {
            const auto position = remove(filled.order.intent.client_order_id);
            if (filled.status == PaperOrderStatus::PartiallyFilled) {
                expected.insert(position, filled.order);
            } else if (filled.status != PaperOrderStatus::Filled) {
                throw std::invalid_argument(
                    "a working order's fill leaves it partially filled or "
                    "filled"
                );
            }
        }
        if (change.match->fault) {
            static_cast<void>(
                remove(change.match->fault->order.intent.client_order_id)
            );
        }
    }
    if (change.cancellation) {
        static_cast<void>(
            remove(change.cancellation->order.intent.client_order_id)
        );
    }
    if (change.result) {
        const auto* receipt = std::get_if<PaperOrderReceipt>(change.result);
        const bool rests = receipt != nullptr &&
            is_working(*from_canonical_name<PaperOrderStatus>(
                *canonical_name(receipt->execution.status)
            ));
        if (rests) {
            if (after.working_orders.size() != expected.size() + 1) {
                throw std::invalid_argument(
                    "a resting submission must add one working order"
                );
            }
            const WorkingOrder& added = after.working_orders.back();
            if (added.intent != *change.intent ||
                added.accepted_ingest_sequence !=
                    change.market->header.ingest_sequence ||
                added.remaining_quantity() !=
                    receipt->execution.unfilled_quantity) {
                throw std::invalid_argument(
                    "the added working order is not the submission's"
                );
            }
            expected.push_back(added);
        }
    }
    if (expected != after.working_orders) {
        throw std::invalid_argument(
            "working orders after the change do not follow from it"
        );
    }
}

CommitPaperOrderResult write_account_change(
    PGconn* connection,
    std::string_view account_id,
    std::int64_t expected_version,
    const AccountChange& change,
    const PaperOrderProcessorRecoveryState& state_before,
    const PaperOrderProcessorRecoveryState& state_after
) {
    if (expected_version < 0) {
        throw std::invalid_argument("expected version must not be negative");
    }
    if (state_before.account.account_id != account_id ||
        state_before.execution.account_id != account_id ||
        state_after.account.account_id != account_id ||
        state_after.execution.account_id != account_id) {
        throw std::invalid_argument(
            "persisted state does not belong to the requested account"
        );
    }
    if ((change.intent == nullptr) != (change.result == nullptr) ||
        (change.intent == nullptr) != (change.market == nullptr)) {
        throw std::invalid_argument("a submission needs its intent, market, "
                                    "and result");
    }
    static_cast<void>(PaperOrderProcessor::restore(state_after));
    const auto execution_changes = changed_rows(
        state_before.execution.instruments,
        state_after.execution.instruments,
        "execution state"
    );
    const auto position_changes = changed_rows(
        state_before.account.state.positions,
        state_after.account.state.positions,
        "position"
    );
    if (change.result) {
        if (const auto* receipt =
                std::get_if<PaperOrderReceipt>(change.result)) {
            if (receipt->execution.intent != *change.intent ||
                receipt->execution.market_ingest_sequence !=
                    change.market->header.ingest_sequence ||
                receipt->account_after != state_after.account.state) {
                throw std::invalid_argument(
                    "paper order receipt does not match the persisted request "
                    "and state"
                );
            }
        }
    }
    require_working_orders_follow(change, state_before, state_after);

    PostgresParameters update_parameters;
    update_parameters.add(account_id);
    add_account_parameters(update_parameters, state_after);
    update_parameters.add(expected_version);
    const auto update = execute(
        connection,
        R"SQL(
            UPDATE paper_accounts SET
                starting_cash_raw = $2,
                cash_raw = $3,
                maximum_order_quantity_raw = $4,
                maximum_order_notional_raw = $5,
                maximum_position_quantity_raw = $6,
                maximum_quote_age_ns = $7,
                fee_per_fill_raw = $8,
                is_healthy = $9,
                market_stream_id = $10,
                version = version + 1,
                updated_at = clock_timestamp()
            WHERE account_id = $1 AND version = $11
            RETURNING version
        )SQL",
        update_parameters
    );
    require_status(update, PGRES_TUPLES_OK, "update paper account");
    if (update.row_count() == 0) {
        PostgresParameters exists_parameters;
        exists_parameters.add(account_id);
        const auto exists = execute(
            connection,
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

    // The version matched, so the stored rows are exactly state_before. A
    // position references its execution state, so executions go first.
    for (const auto& execution_change : execution_changes) {
        PostgresParameters execution_parameters;
        execution_parameters.add(account_id);
        execution_parameters.add(execution_change.instrument->value());
        add_top_of_book_parameters(
            execution_parameters,
            execution_change.row->active_market
        );
        execution_parameters.add(
            execution_change.row->consumed_bid_quantity.raw()
        );
        execution_parameters.add(
            execution_change.row->consumed_ask_quantity.raw()
        );
        const auto written = execute(
            connection,
            execution_change.is_new
                ? R"SQL(
                    INSERT INTO paper_execution_states (
                        account_id, instrument, market_stream_id,
                        market_venue, market_event_timestamp_ns,
                        market_receive_timestamp_ns, market_ingest_sequence,
                        market_venue_sequence, market_quality_flags,
                        market_bid_price_raw, market_bid_quantity_raw,
                        market_ask_price_raw, market_ask_quantity_raw,
                        consumed_bid_quantity_raw, consumed_ask_quantity_raw
                    ) VALUES (
                        $1, $2, $3, $4, $5, $6, $7, $8, $9, $10,
                        $11, $12, $13, $14, $15
                    )
                    RETURNING instrument
                )SQL"
                : R"SQL(
                    UPDATE paper_execution_states SET
                        market_stream_id = $3,
                        market_venue = $4,
                        market_event_timestamp_ns = $5,
                        market_receive_timestamp_ns = $6,
                        market_ingest_sequence = $7,
                        market_venue_sequence = $8,
                        market_quality_flags = $9,
                        market_bid_price_raw = $10,
                        market_bid_quantity_raw = $11,
                        market_ask_price_raw = $12,
                        market_ask_quantity_raw = $13,
                        consumed_bid_quantity_raw = $14,
                        consumed_ask_quantity_raw = $15
                    WHERE account_id = $1 AND instrument = $2
                    RETURNING instrument
                )SQL",
            execution_parameters
        );
        require_status(written, PGRES_TUPLES_OK, "write execution state");
        if (written.row_count() != 1) {
            throw std::runtime_error(
                "stored execution states differ from the recovered state"
            );
        }
    }
    for (const auto& position_change : position_changes) {
        PostgresParameters position_parameters;
        position_parameters.add(account_id);
        position_parameters.add(position_change.instrument->value());
        position_parameters.add(position_change.row->quantity.raw());
        position_parameters.add(position_change.row->open_cost_basis.raw());
        position_parameters.add(position_change.row->gross_realized_pnl.raw());
        position_parameters.add(position_change.row->fees_paid.raw());
        position_parameters.add(
            position_change.row->last_fill_timestamp.nanoseconds()
        );
        const auto written = execute(
            connection,
            position_change.is_new
                ? R"SQL(
                    INSERT INTO paper_positions (
                        account_id, instrument, quantity_raw,
                        open_cost_basis_raw, gross_realized_pnl_raw,
                        fees_paid_raw, last_fill_timestamp_ns
                    ) VALUES ($1, $2, $3, $4, $5, $6, $7)
                    RETURNING instrument
                )SQL"
                : R"SQL(
                    UPDATE paper_positions SET
                        quantity_raw = $3,
                        open_cost_basis_raw = $4,
                        gross_realized_pnl_raw = $5,
                        fees_paid_raw = $6,
                        last_fill_timestamp_ns = $7
                    WHERE account_id = $1 AND instrument = $2
                    RETURNING instrument
                )SQL",
            position_parameters
        );
        require_status(written, PGRES_TUPLES_OK, "write paper position");
        if (written.row_count() != 1) {
            throw std::runtime_error(
                "stored positions differ from the recovered state"
            );
        }
    }

    if (change.match) {
        write_match(connection, account_id, *change.match, state_before);
    }
    if (change.result) {
        if (!write_submission(
                connection,
                account_id,
                *change.intent,
                *change.market,
                *change.result,
                state_after
            )) {
            return conflict(
                PaperPersistenceConflictCode::DuplicateClientOrderId,
                "client order ID has already been persisted"
            );
        }
    }
    if (change.cancellation) {
        const WorkingOrderCancellation& cancellation = *change.cancellation;
        const auto order_record_id = write_order_transition(
            connection,
            account_id,
            cancellation.order,
            cancellation.order,
            cancellation.status,
            Money{},
            cancellation.cancelled_at,
            change.cancel_request_id
        );
        if (!order_record_id) {
            return conflict(
                PaperPersistenceConflictCode::DuplicateCancelRequestId,
                "cancel request ID already cancelled another order"
            );
        }
        for (const std::string_view type :
             {"order_cancelled", "account_state_updated"}) {
            append_event(
                connection,
                account_id,
                *order_record_id,
                cancellation.order.intent.client_order_id,
                type,
                cancellation.cancelled_at
            );
        }
    }
    return new_version;
}

}  // namespace


PostgresPaperOrderRepository::PostgresPaperOrderRepository(
    std::string connection_string,
    std::optional<std::chrono::milliseconds> lock_timeout
)
    : impl_(std::make_unique<Impl>(std::move(connection_string), lock_timeout)) {}

PostgresPaperOrderRepository::~PostgresPaperOrderRepository() = default;

void PostgresPaperOrderRepository::apply_migration(std::string_view sql) {
    if (sql.empty()) {
        throw std::invalid_argument("migration SQL must not be empty");
    }
    impl_->ensure_connected();
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
    // A new account has traded nothing, so it is only its account row.
    if (!state.account.state.positions.empty() ||
        !state.execution.instruments.empty() ||
        !state.account.applied_fill_ids.empty() ||
        !state.execution.processed_client_order_ids.empty()) {
        throw std::invalid_argument("a new paper account must not have traded");
    }
    PostgresParameters parameters;
    parameters.add(state.account.account_id);
    add_account_parameters(parameters, state);
    impl_->ensure_connected();
    const auto result = execute(
        impl_->connection,
        R"SQL(
            INSERT INTO paper_accounts (
                account_id,
                starting_cash_raw,
                cash_raw,
                maximum_order_quantity_raw,
                maximum_order_notional_raw,
                maximum_position_quantity_raw,
                maximum_quote_age_ns,
                fee_per_fill_raw,
                is_healthy,
                market_stream_id
            ) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)
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
    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(
        impl_->connection,
        "start consistent account recovery"
    );
    auto stored = load_account_rows(impl_->connection, account_id, false);
    transaction.commit();
    return stored;
}

CommitPaperOrderResult PostgresPaperOrderRepository::commit_order(
    std::string_view account_id,
    std::int64_t expected_version,
    const WorkingOrderMatch& earlier_orders,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    const PaperOrderProcessorRecoveryState& state_before,
    const PaperOrderProcessorRecoveryState& state_after
) {
    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    const CommitPaperOrderResult written = write_account_change(
        impl_->connection,
        account_id,
        expected_version,
        AccountChange{
            .match = &earlier_orders,
            .intent = &intent,
            .market = &market,
            .result = &result,
            .cancellation = nullptr,
            .cancel_request_id = {},
        },
        state_before,
        state_after
    );
    // A conflict wrote nothing that must survive; the transaction rolls back.
    if (std::holds_alternative<std::int64_t>(written)) {
        transaction.commit();
    }
    return written;
}

PaperAccountJournalSummary PostgresPaperOrderRepository::journal_summary(
    std::string_view account_id
) {
    PostgresParameters parameters;
    parameters.add(account_id);
    impl_->ensure_connected();
    const auto result = execute(
        impl_->connection,
        R"SQL(
            SELECT
                (SELECT count(*) FROM paper_order_records
                 WHERE account_id = $1) AS order_count,
                (SELECT count(*) FROM paper_events
                 WHERE account_id = $1) AS event_count,
                (SELECT count(*) FROM paper_fills
                 WHERE account_id = $1) AS applied_fill_count,
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

PaperOrderPageResult PostgresPaperOrderRepository::list_orders(
    std::string_view account_id,
    const PaperJournalPageRequest& request
) {
    validate_page_request(request);
    PostgresParameters parameters;
    add_page_parameters(parameters, account_id, request);

    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(impl_->connection, "start order history read");
    if (!account_exists(impl_->connection, account_id)) {
        transaction.commit();
        return PaperJournalReadMiss::AccountNotFound;
    }
    const auto result = execute(
        impl_->connection,
        "SELECT " + std::string(kOrderRecordColumns) +
            std::string(kOrderRecordSource) + R"SQL(
            WHERE journal.account_id = $1
              AND ($2::bigint IS NULL OR journal.order_record_id < $2)
              AND ($4::text IS NULL OR journal.instrument = $4)
            ORDER BY journal.order_record_id DESC
            LIMIT $3
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load order history page");
    auto page = journal_page<PaperOrderRecord>(
        result,
        request.limit,
        persisted_order
    );
    transaction.commit();
    return page;
}

PaperFillPageResult PostgresPaperOrderRepository::list_fills(
    std::string_view account_id,
    const PaperJournalPageRequest& request
) {
    validate_page_request(request);
    PostgresParameters parameters;
    add_page_parameters(parameters, account_id, request);

    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(impl_->connection, "start fill history read");
    if (!account_exists(impl_->connection, account_id)) {
        transaction.commit();
        return PaperJournalReadMiss::AccountNotFound;
    }
    // paper_fills holds booked fills only. A processing fault may journal
    // the simulated fill that accounting rejected; it never reaches it.
    const auto result = execute(
        impl_->connection,
        "SELECT " + std::string(kFillColumns) + std::string(kFillSource) +
            R"SQL(
            WHERE booked.account_id = $1
              AND ($2::bigint IS NULL OR booked.fill_record_id < $2)
              AND ($4::text IS NULL OR booked.instrument = $4)
            ORDER BY booked.fill_record_id DESC
            LIMIT $3
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load fill history page");
    auto page = journal_page<PaperFillRecord>(
        result,
        request.limit,
        persisted_fill
    );
    transaction.commit();
    return page;
}

namespace {

// Rejects identifiers libpq would truncate before any query runs, so an
// invalid request never depends on what is stored.
void require_text_parameters(
    std::initializer_list<std::string_view> values
) {
    PostgresParameters parameters;
    for (const std::string_view value : values) {
        parameters.add(value);
    }
}

// Looks up one order inside an open transaction; the account's existence
// is the caller's to establish.
std::optional<PaperOrderRecord> find_order_in_transaction(
    PGconn* connection,
    std::string_view account_id,
    std::string_view client_order_id
) {
    // An empty client order ID is not unique, so it cannot identify an order.
    if (client_order_id.empty()) {
        throw std::invalid_argument("client order ID must not be empty");
    }
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(client_order_id);
    const auto result = execute(
        connection,
        "SELECT " + std::string(kOrderRecordColumns) +
            std::string(kOrderRecordSource) + R"SQL(
            WHERE journal.account_id = $1
              AND journal.client_order_id = $2
              AND journal.client_order_id <> ''
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "look up paper order");
    if (result.row_count() > 1) {
        inconsistent_record("client order ID is recorded more than once");
    }
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return persisted_order(result, 0);
}

// The journal record ID of an order, or nullopt when it is not recorded.
std::optional<std::int64_t> order_record_id_of(
    PGconn* connection,
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (client_order_id.empty()) {
        throw std::invalid_argument("client order ID must not be empty");
    }
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(client_order_id);
    const auto result = execute(
        connection,
        R"SQL(
            SELECT order_record_id
            FROM paper_order_records
            WHERE account_id = $1
              AND client_order_id = $2
              AND client_order_id <> ''
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "look up paper order");
    if (result.row_count() > 1) {
        inconsistent_record("client order ID is recorded more than once");
    }
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return persisted_int64(result, 0, "order_record_id");
}

PaperOrderEventRecord persisted_event(const PostgresResult& result, int row) {
    const auto type = canonical_value<PaperOrderEventType>(
        result.value(row, "event_type"),
        "event_type"
    );
    const bool has_fill = !result.is_null(row, "fill_id");
    if (has_fill != (type == PaperOrderEventType::FillApplied) ||
        has_fill == result.is_null(row, "booked_fill_id")) {
        inconsistent_record("a fill event does not reference its fill");
    }
    return PaperOrderEventRecord{
        .sequence = persisted_int64(result, row, "event_order"),
        .type = type,
        .occurred_at = Timestamp::from_nanoseconds(
            persisted_int64(result, row, "occurred_at_ns")
        ),
        .fill = has_fill ? std::optional<Fill>(
                               persisted_booked_fill(result, row, "booked_")
                           )
                         : std::nullopt,
    };
}

}  // namespace

PaperOrderLookupResult PostgresPaperOrderRepository::find_order(
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (client_order_id.empty()) {
        throw std::invalid_argument("client order ID must not be empty");
    }
    require_text_parameters({account_id, client_order_id});
    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(impl_->connection, "start order lookup");
    if (!account_exists(impl_->connection, account_id)) {
        transaction.commit();
        return PaperJournalReadMiss::AccountNotFound;
    }
    auto record = find_order_in_transaction(
        impl_->connection,
        account_id,
        client_order_id
    );
    transaction.commit();
    if (!record) {
        return PaperJournalReadMiss::OrderNotFound;
    }
    return std::move(*record);
}

PaperWorkingOrderPageResult PostgresPaperOrderRepository::list_working_orders(
    std::string_view account_id,
    const PaperForwardPageRequest& request
) {
    validate_forward_request(request);
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add_optional(request.after);
    parameters.add(static_cast<std::int64_t>(request.limit + 1));
    parameters.add(
        request.instrument
            ? std::optional<std::string>(request.instrument->value())
            : std::nullopt
    );

    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(impl_->connection, "start working order read");
    if (!account_exists(impl_->connection, account_id)) {
        transaction.commit();
        return PaperJournalReadMiss::AccountNotFound;
    }
    const auto result = execute(
        impl_->connection,
        "SELECT " + std::string(kOrderRecordColumns) + R"SQL(
            FROM paper_orders AS latest
            JOIN paper_order_records AS journal
                ON journal.order_record_id = latest.order_record_id
            WHERE latest.account_id = $1
              AND latest.status IN ('working', 'partially_filled')
              AND ($2::bigint IS NULL OR latest.order_record_id > $2)
              AND ($4::text IS NULL OR latest.instrument = $4)
            ORDER BY latest.order_record_id
            LIMIT $3
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load working orders page");
    auto page = forward_page<PaperOrderRecord>(
        result,
        request.limit,
        persisted_order,
        [](const PaperOrderRecord& record) { return record.record_id; }
    );
    transaction.commit();
    return page;
}

PaperOrderFillPageResult PostgresPaperOrderRepository::list_order_fills(
    std::string_view account_id,
    std::string_view client_order_id,
    const PaperForwardPageRequest& request
) {
    validate_forward_request(request);
    if (client_order_id.empty()) {
        throw std::invalid_argument("client order ID must not be empty");
    }
    require_text_parameters({account_id, client_order_id});
    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(impl_->connection, "start order fill read");
    if (!account_exists(impl_->connection, account_id)) {
        transaction.commit();
        return PaperJournalReadMiss::AccountNotFound;
    }
    const auto order_record_id =
        order_record_id_of(impl_->connection, account_id, client_order_id);
    if (!order_record_id) {
        transaction.commit();
        return PaperJournalReadMiss::OrderNotFound;
    }
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(*order_record_id);
    parameters.add_optional(request.after);
    parameters.add(static_cast<std::int64_t>(request.limit + 1));
    const auto result = execute(
        impl_->connection,
        "SELECT " + std::string(kFillColumns) + std::string(kFillSource) +
            R"SQL(
            WHERE booked.account_id = $1
              AND booked.order_record_id = $2
              AND ($3::bigint IS NULL OR booked.fill_index > $3)
            ORDER BY booked.fill_index
            LIMIT $4
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load order fills page");
    auto page = forward_page<PaperFillRecord>(
        result,
        request.limit,
        persisted_fill,
        [](const PaperFillRecord& record) {
            return static_cast<std::int64_t>(record.fill_index);
        }
    );
    transaction.commit();
    return page;
}

PaperOrderEventPageResult PostgresPaperOrderRepository::list_order_events(
    std::string_view account_id,
    std::string_view client_order_id,
    const PaperForwardPageRequest& request
) {
    validate_forward_request(request);
    if (client_order_id.empty()) {
        throw std::invalid_argument("client order ID must not be empty");
    }
    require_text_parameters({account_id, client_order_id});
    impl_->ensure_connected();
    PostgresTransaction transaction(impl_->connection);
    start_consistent_read(impl_->connection, "start order event read");
    if (!account_exists(impl_->connection, account_id)) {
        transaction.commit();
        return PaperJournalReadMiss::AccountNotFound;
    }
    const auto order_record_id =
        order_record_id_of(impl_->connection, account_id, client_order_id);
    if (!order_record_id) {
        transaction.commit();
        return PaperJournalReadMiss::OrderNotFound;
    }
    PostgresParameters parameters;
    parameters.add(account_id);
    parameters.add(*order_record_id);
    parameters.add_optional(request.after);
    parameters.add(static_cast<std::int64_t>(request.limit + 1));
    const auto result = execute(
        impl_->connection,
        R"SQL(
            SELECT
                event.event_order, event.event_type, event.occurred_at_ns,
                event.fill_id,
                booked.fill_id AS booked_fill_id,
                booked.instrument AS booked_instrument,
                booked.side AS booked_side,
                booked.price_raw AS booked_price_raw,
                booked.quantity_raw AS booked_quantity_raw,
                booked.fee_raw AS booked_fee_raw,
                booked.notional_raw AS booked_notional_raw,
                booked.filled_at_ns AS booked_filled_at_ns
            FROM paper_events AS event
            LEFT JOIN paper_fills AS booked
                ON booked.account_id = event.account_id
               AND booked.fill_id = event.fill_id
            WHERE event.account_id = $1
              AND event.order_record_id = $2
              AND ($3::bigint IS NULL OR event.event_order > $3)
            ORDER BY event.event_order
            LIMIT $4
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "load order events page");
    auto page = forward_page<PaperOrderEventRecord>(
        result,
        request.limit,
        persisted_event,
        [](const PaperOrderEventRecord& record) { return record.sequence; }
    );
    transaction.commit();
    return page;
}

std::vector<std::string> PostgresPaperOrderRepository::accounts_to_match(
    const TopOfBookEvent& observation
) {
    PostgresParameters parameters;
    parameters.add(observation.header.instrument.value());
    parameters.add(observation.header.stream.value());
    parameters.add(observation.header.ingest_sequence);
    // A missing side matches no order of that side.
    parameters.add_optional(
        observation.ask
            ? std::optional<std::int64_t>(observation.ask->price.raw())
            : std::nullopt
    );
    parameters.add_optional(
        observation.bid
            ? std::optional<std::int64_t>(observation.bid->price.raw())
            : std::nullopt
    );
    constexpr const char* kStatement = "chronos_accounts_to_match";
    impl_->ensure_connected();
    if (!impl_->candidates_prepared) {
        const auto prepared = checked_result(
            impl_->connection,
            PQprepare(
                impl_->connection,
                kStatement,
                R"SQL(
                    SELECT DISTINCT latest.account_id
                    FROM paper_orders AS latest
                    JOIN paper_accounts AS owner
                        ON owner.account_id = latest.account_id
                    WHERE latest.status IN ('working', 'partially_filled')
                      AND latest.instrument = $1
                      AND latest.market_stream_id = $2
                      AND latest.accepted_market_ingest_sequence < $3::numeric
                      AND (
                          (latest.side = 'buy' AND
                           latest.limit_price_raw >= $4::bigint) OR
                          (latest.side = 'sell' AND
                           latest.limit_price_raw <= $5::bigint)
                      )
                      AND owner.is_healthy
                    ORDER BY latest.account_id
                )SQL",
                5,
                nullptr
            )
        );
        require_status(prepared, PGRES_COMMAND_OK, "prepare accounts to match");
        impl_->candidates_prepared = true;
    }
    const auto& values = parameters.pointers();
    const auto result = checked_result(
        impl_->connection,
        PQexecPrepared(
            impl_->connection,
            kStatement,
            parameters.size(),
            values.data(),
            nullptr,
            nullptr,
            0
        )
    );
    require_status(result, PGRES_TUPLES_OK, "find accounts to match");
    std::vector<std::string> accounts;
    accounts.reserve(static_cast<std::size_t>(result.row_count()));
    for (int row = 0; row < result.row_count(); ++row) {
        accounts.push_back(result.value(row, "account_id"));
    }
    return accounts;
}

bool PostgresPaperOrderRepository::check_availability() {
    // One failed probe may be the first observation of a dropped
    // connection; the second attempt starts with a reconnect.
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            impl_->ensure_connected();
            const auto result = execute(impl_->connection, "SELECT 1");
            require_status(result, PGRES_TUPLES_OK, "probe PostgreSQL");
            return true;
        } catch (const PaperPersistenceUnavailable&) {
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Account locks


PaperAccountLock::PaperAccountLock(std::unique_ptr<State> state)
    : state_(std::move(state)) {}

PaperAccountLock::PaperAccountLock(PaperAccountLock&& other) noexcept =
    default;

PaperAccountLock::~PaperAccountLock() = default;

PaperOrderProcessor& PaperAccountLock::processor() noexcept {
    return *state_->processor;
}

std::int64_t PaperAccountLock::version() const noexcept {
    return state_->version;
}

namespace {

void require_open(const std::unique_ptr<PaperAccountLock::State>& state) {
    if (state == nullptr || !state->transaction) {
        throw std::logic_error("the account lock was already released");
    }
}

}  // namespace

std::optional<PaperOrderRecord> PaperAccountLock::find_order(
    std::string_view client_order_id
) {
    require_open(state_);
    return find_order_in_transaction(
        state_->connection,
        state_->account_id,
        client_order_id
    );
}

std::optional<std::string> PaperAccountLock::order_cancelled_by(
    std::string_view cancel_request_id
) {
    require_open(state_);
    PostgresParameters parameters;
    parameters.add(state_->account_id);
    parameters.add(cancel_request_id);
    const auto result = execute(
        state_->connection,
        R"SQL(
            SELECT client_order_id
            FROM paper_orders
            WHERE account_id = $1 AND cancel_request_id = $2
        )SQL",
        parameters
    );
    require_status(result, PGRES_TUPLES_OK, "look up cancel request");
    if (result.row_count() == 0) {
        return std::nullopt;
    }
    return result.value(0, "client_order_id");
}

namespace {

// A deployment decision resolved by the order a change records.
struct DeploymentDecisionResolution {
    std::string_view deployment_id;
    std::int64_t decision_sequence{0};
    const DeploymentOrderResolution* resolution{nullptr};
};

// Writes `change` in the lock's transaction and commits it. The lock is
// released afterwards whatever happens: its recovered state no longer
// describes the database. A deployment's decision is resolved in the same
// transaction, so its order and its resolution commit together or not at
// all.
CommitPaperOrderResult commit_locked(
    PaperAccountLock::State& state,
    const AccountChange& change,
    const DeploymentDecisionResolution* decision = nullptr
) {
    struct Release {
        PaperAccountLock::State& state;
        ~Release() { state.transaction.reset(); }
    } release{state};
    const PaperOrderProcessorRecoveryState after =
        state.processor->recovery_state();
    const CommitPaperOrderResult written = write_account_change(
        state.connection,
        state.account_id,
        state.version,
        change,
        *state.before,
        after
    );
    if (const auto* version = std::get_if<std::int64_t>(&written)) {
        if (decision != nullptr) {
            postgres_support::resolve_deployment_order(
                state.connection,
                decision->deployment_id,
                decision->decision_sequence,
                *decision->resolution
            );
        }
        state.transaction->commit();
        state.version = *version;
    }
    return written;
}

}  // namespace

CommitPaperOrderResult PaperAccountLock::commit_submission(
    const WorkingOrderMatch& earlier_orders,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result
) {
    require_open(state_);
    return commit_locked(*state_, AccountChange{
        .match = &earlier_orders,
        .intent = &intent,
        .market = &market,
        .result = &result,
        .cancellation = nullptr,
        .cancel_request_id = {},
    });
}

CommitPaperOrderResult PaperAccountLock::commit_deployment_submission(
    const WorkingOrderMatch& earlier_orders,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    std::string_view deployment_id,
    std::int64_t decision_sequence,
    const DeploymentOrderResolution& resolution
) {
    require_open(state_);
    if (intent.source.kind != OrderSourceKind::Model ||
        intent.source.source_id != deployment_id) {
        throw std::invalid_argument(
            "a deployment order's source must be its deployment"
        );
    }
    const DeploymentDecisionResolution decision{
        .deployment_id = deployment_id,
        .decision_sequence = decision_sequence,
        .resolution = &resolution,
    };
    return commit_locked(*state_, AccountChange{
        .match = &earlier_orders,
        .intent = &intent,
        .market = &market,
        .result = &result,
        .cancellation = nullptr,
        .cancel_request_id = {},
    }, &decision);
}

CommitPaperOrderResult PaperAccountLock::commit_match(
    const WorkingOrderMatch& match
) {
    require_open(state_);
    return commit_locked(*state_, AccountChange{
        .match = &match,
        .intent = nullptr,
        .market = nullptr,
        .result = nullptr,
        .cancellation = nullptr,
        .cancel_request_id = {},
    });
}

CommitPaperOrderResult PaperAccountLock::commit_cancellation(
    const WorkingOrderCancellation& cancellation,
    std::string_view cancel_request_id
) {
    require_open(state_);
    if (cancel_request_id.empty()) {
        throw std::invalid_argument("cancel request ID must not be empty");
    }
    return commit_locked(*state_, AccountChange{
        .match = nullptr,
        .intent = nullptr,
        .market = nullptr,
        .result = nullptr,
        .cancellation = &cancellation,
        .cancel_request_id = cancel_request_id,
    });
}

void PaperAccountLock::release() {
    if (state_ != nullptr) {
        state_->transaction.reset();
    }
}

std::optional<PaperAccountLock> PostgresPaperOrderRepository::lock_account(
    std::string_view account_id
) {
    impl_->ensure_connected();
    auto state = std::make_unique<PaperAccountLock::State>(
        impl_->connection,
        std::string(account_id)
    );
    auto stored = load_account_rows(impl_->connection, account_id, true);
    if (!stored) {
        return std::nullopt;
    }
    state->before = stored->processor.recovery_state();
    state->version = stored->version;
    state->processor.emplace(std::move(stored->processor));
    return PaperAccountLock(std::move(state));
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
    Money starting_cash,
    PreTradeRiskLimits risk_limits
) {
    PaperOrderProcessor processor(
        std::move(account_id),
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
    WorkingOrderMatch earlier_orders =
        processor_.match(market, intent.submitted_at);
    PaperOrderResult result = processor_.submit(intent, market);
    try {
        CommitPaperOrderResult commit = repository_->commit_order(
            processor_.account().account_id(),
            version_,
            earlier_orders,
            intent,
            market,
            result,
            state_before,
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
        .earlier_orders = std::move(earlier_orders),
    };
}

PaperOrderRecord submitted_order_record(
    std::int64_t record_id,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    Money account_fee
) {
    // The state of an executed order as its execution left it.
    const auto executed_state = [&](
        const SimulatedExecutionReport& execution,
        const std::optional<ApplyFillReceipt>& accounting,
        bool faulted
    ) {
        const Quantity filled =
            accounting ? accounting->fill.quantity : Quantity{};
        const Money fee = accounting ? accounting->fill.fee : account_fee;
        const PaperOrderStatus status = faulted
            ? PaperOrderStatus::Faulted
            : *from_canonical_name<PaperOrderStatus>(
                  *canonical_name(execution.status)
              );
        return PaperOrderState{
            .status = status,
            .filled_quantity = filled,
            .remaining_quantity = intent.quantity - filled,
            .filled_notional = accounting ? accounting->notional : Money{},
            .fill_count = accounting ? 1U : 0U,
            .order_fee = fee,
            .cumulative_fee = accounting ? fee : Money{},
            .reservation = expected_reservation(
                intent,
                status,
                filled,
                accounting ? 1U : 0U,
                fee
            ),
            .cancellation_reason =
                faulted ? std::nullopt : execution.cancellation_reason,
            .cancel_request_id = std::nullopt,
            .updated_at = intent.submitted_at,
        };
    };
    return std::visit(
        [&](const auto& outcome) -> PaperOrderRecord {
            using Outcome = std::decay_t<decltype(outcome)>;
            if constexpr (std::is_same_v<Outcome, PaperOrderReceipt>) {
                return PaperOrderRecord{
                    .record_id = record_id,
                    .intent = intent,
                    .market = market,
                    .outcome = outcome.execution,
                    .state = executed_state(
                        outcome.execution,
                        outcome.accounting,
                        false
                    ),
                };
            } else if constexpr (
                std::is_same_v<Outcome, PaperOrderProcessingError>
            ) {
                return PaperOrderRecord{
                    .record_id = record_id,
                    .intent = intent,
                    .market = market,
                    .outcome = PaperOrderFaultRecord{
                        .code = outcome.code,
                        .message = outcome.message,
                        .unbooked_execution = outcome.execution,
                    },
                    .state = outcome.execution
                        ? std::optional<PaperOrderState>(executed_state(
                              *outcome.execution,
                              std::nullopt,
                              true
                          ))
                        : std::nullopt,
                };
            } else {
                return PaperOrderRecord{
                    .record_id = record_id,
                    .intent = intent,
                    .market = market,
                    .outcome = outcome,
                    .state = std::nullopt,
                };
            }
        },
        result
    );
}

std::optional<std::string_view> canonical_name(
    PaperOrderEventType type
) noexcept {
    switch (type) {
        case PaperOrderEventType::OrderSubmitted:
            return "order_submitted";
        case PaperOrderEventType::RiskRejected:
            return "risk_rejected";
        case PaperOrderEventType::ExecutionRejected:
            return "execution_rejected";
        case PaperOrderEventType::OrderCompleted:
            return "order_completed";
        case PaperOrderEventType::FillApplied:
            return "fill_applied";
        case PaperOrderEventType::AccountStateUpdated:
            return "account_state_updated";
        case PaperOrderEventType::ProcessorFaulted:
            return "processor_faulted";
        case PaperOrderEventType::OrderAccepted:
            return "order_accepted";
        case PaperOrderEventType::OrderFilled:
            return "order_filled";
        case PaperOrderEventType::OrderCancelled:
            return "order_cancelled";
    }
    return std::nullopt;
}

}  // namespace chronos
