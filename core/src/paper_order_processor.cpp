#include "chronos/paper_order_processor.hpp"

#include "chronos/market_data_validation.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace chronos {
namespace {

bool is_marketable_at(const WorkingOrder& order, const BookLevel& level) {
    const Price limit = *order.intent.limit_price;
    return order.intent.side == Side::Buy ? limit >= level.price
                                          : limit <= level.price;
}

// Every reservation is covered by what the account holds: its cash, and
// for working sells the shares of their instrument.
bool is_covered(
    const AccountReservations& reservations,
    const AccountState& account
) {
    if (reservations.reserved_cash > account.cash) {
        return false;
    }
    return std::all_of(
        reservations.instruments.begin(),
        reservations.instruments.end(),
        [&account](const auto& entry) {
            return entry.second.reserved_sell_quantity <=
                account.quantity(entry.first);
        }
    );
}

void validate_recovered_working_order(
    const WorkingOrder& order,
    const PaperOrderProcessorRecoveryState& state,
    const std::unordered_set<std::string>& processed_client_order_ids,
    const std::unordered_set<std::string>& applied_fill_ids
) {
    const OrderIntent& intent = order.intent;
    if (intent.account_id != state.account.account_id) {
        throw std::invalid_argument(
            "a recovered working order belongs to another account"
        );
    }
    if (intent.type != OrderType::Limit || !intent.limit_price ||
        intent.limit_price->raw() <= 0 ||
        intent.time_in_force != TimeInForce::GoodTilCancelled ||
        (intent.side != Side::Buy && intent.side != Side::Sell) ||
        intent.quantity.raw() <= 0) {
        throw std::invalid_argument(
            "a recovered working order must be a good-til-cancelled limit "
            "order"
        );
    }
    if (order.filled_quantity.raw() < 0 ||
        order.filled_quantity >= intent.quantity) {
        throw std::invalid_argument(
            "a recovered working order must have an unfilled remainder"
        );
    }
    if ((order.fill_count == 0) != (order.filled_quantity.raw() == 0)) {
        throw std::invalid_argument(
            "a recovered working order's fills and filled quantity disagree"
        );
    }
    if (order.fee.raw() < 0) {
        throw std::invalid_argument(
            "a recovered working order's fee must not be negative"
        );
    }
    if (order.updated_at < intent.submitted_at) {
        throw std::invalid_argument(
            "a recovered working order changed before it was submitted"
        );
    }
    if (!processed_client_order_ids.contains(intent.client_order_id)) {
        throw std::invalid_argument(
            "a recovered working order was never executed"
        );
    }
    const auto execution = state.execution.instruments.find(intent.instrument);
    if (execution == state.execution.instruments.end() ||
        execution->second.active_market.header.ingest_sequence <
            order.accepted_ingest_sequence) {
        throw std::invalid_argument(
            "a recovered working order is newer than its instrument's "
            "execution state"
        );
    }
    for (std::uint32_t index = 0; index <= order.fill_count; ++index) {
        const bool applied =
            applied_fill_ids.contains(fill_id_for(intent.client_order_id, index));
        if (applied != (index < order.fill_count)) {
            throw std::invalid_argument(
                "a recovered working order's fills are not the account's"
            );
        }
        if (index == std::numeric_limits<std::uint32_t>::max()) {
            break;
        }
    }
}

}  // namespace

PaperOrderProcessor::PaperOrderProcessor(
    std::string account_id,
    Money starting_cash,
    PreTradeRiskLimits risk_limits
)
    : risk_engine_(risk_limits),
      account_(account_id, starting_cash),
      execution_simulator_(std::move(account_id)) {}

PaperOrderProcessorRecoveryState PaperOrderProcessor::recovery_state() const {
    return PaperOrderProcessorRecoveryState{
        .risk_limits = risk_engine_.limits(),
        .account = account_.recovery_state(),
        .execution = execution_simulator_.recovery_state(),
        .is_healthy = is_healthy_,
        .working_orders = working_orders_,
    };
}

PaperOrderProcessor PaperOrderProcessor::restore(
    PaperOrderProcessorRecoveryState recovery_state
) {
    if (recovery_state.account.account_id !=
        recovery_state.execution.account_id) {
        throw std::invalid_argument(
            "recovered account and execution account IDs do not match"
        );
    }
    // A position exists only through a fill, and a fill only through an
    // execution of the same instrument.
    for (const auto& [instrument, position] :
         recovery_state.account.state.positions) {
        if (!recovery_state.execution.instruments.contains(instrument)) {
            throw std::invalid_argument(
                "a recovered position has no execution state"
            );
        }
    }

    const std::unordered_set<std::string> processed(
        recovery_state.execution.processed_client_order_ids.begin(),
        recovery_state.execution.processed_client_order_ids.end()
    );
    const std::unordered_set<std::string> applied(
        recovery_state.account.applied_fill_ids.begin(),
        recovery_state.account.applied_fill_ids.end()
    );
    std::set<std::string> working_ids;
    for (const WorkingOrder& order : recovery_state.working_orders) {
        validate_recovered_working_order(
            order,
            recovery_state,
            processed,
            applied
        );
        if (!working_ids.insert(order.intent.client_order_id).second) {
            throw std::invalid_argument(
                "recovered working orders must be unique"
            );
        }
    }

    const std::string account_id = recovery_state.account.account_id;
    const Money starting_cash =
        recovery_state.account.state.starting_cash;
    PaperOrderProcessor processor(
        account_id,
        starting_cash,
        recovery_state.risk_limits
    );
    processor.account_ = PaperAccount::restore(
        std::move(recovery_state.account)
    );
    processor.execution_simulator_ = TopOfBookExecutionSimulator::restore(
        std::move(recovery_state.execution)
    );
    processor.is_healthy_ = recovery_state.is_healthy;
    processor.working_orders_ = std::move(recovery_state.working_orders);
    try {
        processor.refresh_reservations();
    } catch (const std::overflow_error&) {
        throw std::invalid_argument(
            "recovered reservations exceed the fixed-point range"
        );
    }
    if (!is_covered(processor.reservations_, processor.account_.state())) {
        throw std::invalid_argument(
            "recovered reservations exceed the account's cash or shares"
        );
    }
    return processor;
}

void PaperOrderProcessor::refresh_reservations() {
    reservations_ = account_reservations(working_orders_);
}

std::optional<Quantity> PaperOrderProcessor::immediate_fill_limit(
    const ApprovedOrderIntent& approved
) const {
    const OrderIntent& intent = approved.intent();
    const TopOfBookEvent& market = approved.market();
    const std::optional<BookLevel>& level =
        intent.side == Side::Buy ? market.ask : market.bid;
    if (!level || market_lineage_violation(executions(), market)) {
        // The simulator rejects or does not fill such an order.
        return std::nullopt;
    }
    WorkingOrder order{
        .intent = intent,
        .accepted_ingest_sequence = market.header.ingest_sequence,
        .fee = approved.fee(),
        .filled_quantity = Quantity{},
        .fill_count = 0,
        .updated_at = intent.submitted_at,
    };
    if (!is_marketable_at(order, *level)) {
        return std::nullopt;
    }
    const Quantity quantity = std::min(
        intent.quantity,
        execution_simulator_.available_liquidity(market, intent.side)
    );
    if (quantity.raw() == 0) {
        return std::nullopt;
    }
    // A fill accounting could not book, or one that would leave the rest of
    // the order unreserved, is not taken now; the whole order rests.
    const ApplyFillResult preview = account_.preview_fill(Fill{
        .fill_id = fill_id_for(intent.client_order_id, 0),
        .instrument = intent.instrument,
        .side = intent.side,
        .price = level->price,
        .quantity = quantity,
        .fee = approved.fee(),
        .timestamp = intent.submitted_at,
    });
    const auto* receipt = std::get_if<ApplyFillReceipt>(&preview);
    if (receipt == nullptr) {
        return Quantity{};
    }
    order.filled_quantity = quantity;
    order.fill_count = 1;
    try {
        const Money rest = order.remaining_quantity().raw() > 0
            ? reservation_of(order).cash
            : Money{};
        if (receipt->after.cash < reservations_.reserved_cash + rest) {
            return Quantity{};
        }
    } catch (const std::overflow_error&) {
        return Quantity{};
    }
    return std::nullopt;
}

PaperOrderResult PaperOrderProcessor::submit(
    const OrderIntent& intent,
    const TopOfBookEvent& market
) {
    if (!is_healthy_) {
        return PaperOrderProcessingError{
            .code = PaperOrderProcessingErrorCode::ProcessorFaulted,
            .message =
                "paper order processor is halted after an accounting "
                "invariant violation",
            .execution = std::nullopt,
            .accounting_error = std::nullopt,
        };
    }

    PreTradeRiskDecision risk_decision =
        risk_engine_.evaluate(intent, account_, reservations_, market);
    if (std::holds_alternative<PreTradeRiskRejection>(risk_decision)) {
        return std::get<PreTradeRiskRejection>(std::move(risk_decision));
    }
    const ApprovedOrderIntent& approved =
        std::get<ApprovedOrderIntent>(risk_decision);
    const bool rests =
        approved.intent().time_in_force == TimeInForce::GoodTilCancelled;

    const std::optional<Quantity> fill_limit = rests
        ? immediate_fill_limit(approved)
        : std::nullopt;
    SimulatedExecutionResult execution_result = execution_simulator_.execute(
        approved,
        fill_limit
    );
    if (std::holds_alternative<SimulatedExecutionError>(execution_result)) {
        return std::get<SimulatedExecutionError>(
            std::move(execution_result)
        );
    }

    SimulatedExecutionReport execution =
        std::get<SimulatedExecutionReport>(std::move(execution_result));
    // An engaged zero limit means the oldest order could not safely take a
    // marketable immediate fill. Consume this account's remaining shadow
    // liquidity on that side so a later submission on the same observation
    // cannot jump it.
    if (fill_limit && fill_limit->raw() == 0) {
        const Quantity remaining_liquidity =
            execution_simulator_.available_liquidity(market, intent.side);
        if (remaining_liquidity.raw() <= 0) {
            throw std::logic_error(
                "a suppressed immediate fill has no displayed liquidity"
            );
        }
        execution_simulator_.take_liquidity(
            market,
            intent.side,
            remaining_liquidity
        );
    }
    std::optional<ApplyFillReceipt> accounting;
    if (execution.fill) {
        ApplyFillResult apply_result = account_.apply_fill(*execution.fill);
        if (std::holds_alternative<ApplyFillError>(apply_result)) {
            is_healthy_ = false;
            return PaperOrderProcessingError{
                .code = PaperOrderProcessingErrorCode::
                    AccountingInvariantViolation,
                .message =
                    "risk-approved simulated fill was rejected by accounting",
                .execution = std::move(execution),
                .accounting_error = std::get<ApplyFillError>(
                    std::move(apply_result)
                ),
            };
        }
        accounting = std::get<ApplyFillReceipt>(std::move(apply_result));
    }

    if (rests && execution.unfilled_quantity.raw() > 0) {
        working_orders_.push_back(WorkingOrder{
            .intent = intent,
            .accepted_ingest_sequence = market.header.ingest_sequence,
            .fee = approved.fee(),
            .filled_quantity = execution.fill ? execution.fill->quantity
                                              : Quantity{},
            .fill_count = execution.fill ? 1U : 0U,
            .updated_at = intent.submitted_at,
        });
        refresh_reservations();
    }
    if (!is_covered(reservations_, account_.state())) {
        throw std::logic_error(
            "an accepted order left reservations uncovered"
        );
    }

    return PaperOrderReceipt{
        .execution = std::move(execution),
        .accounting = std::move(accounting),
        .account_after = account_.state(),
    };
}

WorkingOrderMatch PaperOrderProcessor::match(
    const TopOfBookEvent& observation,
    Timestamp market_time
) {
    WorkingOrderMatch report{
        .observation = observation,
        .fills = {},
        .skipped = {},
        .fault = std::nullopt,
    };
    const InstrumentId& instrument = observation.header.instrument;
    const bool has_candidates = std::any_of(
        working_orders_.begin(),
        working_orders_.end(),
        [&](const WorkingOrder& order) {
            return order.intent.instrument == instrument &&
                order.accepted_ingest_sequence <
                    observation.header.ingest_sequence;
        }
    );
    if (!is_healthy_ || !has_candidates) {
        return report;
    }
    if (std::holds_alternative<MarketEventValidationFailure>(
            validate_market_event(MarketEvent(observation))
        ) ||
        observation.header.quality.is_suspect() ||
        classify_market_observation_recency(
            observation.header.receive_timestamp,
            market_time,
            risk_engine_.limits().maximum_quote_age_nanoseconds
        ) != MarketObservationRecency::Current ||
        market_lineage_violation(executions(), observation)) {
        return report;
    }
    // The account already used this observation, and every working order
    // it had then was offered it.
    const auto executed = executions().find(instrument);
    if (executed != executions().end() &&
        executed->second.active_market.header.ingest_sequence ==
            observation.header.ingest_sequence) {
        return report;
    }

    // If the oldest marketable order cannot safely take a side's liquidity,
    // a newer order on that side must not jump it. Bid and ask liquidity are
    // independent, so a skipped buy does not block sells, or vice versa.
    bool buy_blocked = false;
    bool sell_blocked = false;
    for (auto order = working_orders_.begin(); order != working_orders_.end();) {
        const OrderIntent& intent = order->intent;
        const std::optional<BookLevel>& level =
            intent.side == Side::Buy ? observation.ask : observation.bid;
        if (intent.instrument != instrument ||
            order->accepted_ingest_sequence >=
                observation.header.ingest_sequence ||
            !level || !is_marketable_at(*order, *level)) {
            ++order;
            continue;
        }
        bool& side_blocked = intent.side == Side::Buy
            ? buy_blocked
            : sell_blocked;
        if (side_blocked) {
            ++order;
            continue;
        }
        const Quantity quantity = std::min(
            order->remaining_quantity(),
            execution_simulator_.available_liquidity(observation, intent.side)
        );
        if (quantity.raw() == 0) {
            ++order;
            continue;
        }
        if (order->fill_count == std::numeric_limits<std::uint32_t>::max()) {
            throw std::logic_error("a working order has too many fills");
        }
        const Fill fill{
            .fill_id = fill_id_for(intent.client_order_id, order->fill_count),
            .instrument = instrument,
            .side = intent.side,
            .price = level->price,
            .quantity = quantity,
            .fee = order->fee_charged() ? Money{} : order->fee,
            .timestamp = observation.header.receive_timestamp,
        };
        const auto skip = [&](WorkingOrderSkipReason reason) {
            report.skipped.push_back(WorkingOrderSkip{
                .client_order_id = intent.client_order_id,
                .reason = reason,
            });
            const Quantity remaining_liquidity =
                execution_simulator_.available_liquidity(
                    observation,
                    intent.side
                );
            if (remaining_liquidity.raw() <= 0) {
                throw std::logic_error(
                    "a skipped working order has no displayed liquidity"
                );
            }
            execution_simulator_.take_liquidity(
                observation,
                intent.side,
                remaining_liquidity
            );
            side_blocked = true;
            ++order;
        };

        const ApplyFillResult preview = account_.preview_fill(fill);
        if (const auto* error = std::get_if<ApplyFillError>(&preview)) {
            if (error->code == ApplyFillErrorCode::NotionalRoundedToZero) {
                skip(WorkingOrderSkipReason::NotionalRoundedToZero);
                continue;
            }
            if (error->code == ApplyFillErrorCode::ArithmeticOverflow) {
                skip(WorkingOrderSkipReason::ArithmeticOverflow);
                continue;
            }
            // Reservations guarantee cash and shares, and the account's
            // timeline guarantees fill order, so this is a defect.
            is_healthy_ = false;
            report.fault = WorkingOrderFault{
                .order = *order,
                .unbooked_fill = fill,
                .accounting_error = *error,
            };
            working_orders_.erase(order);
            refresh_reservations();
            break;
        }

        WorkingOrder after = *order;
        after.filled_quantity = after.filled_quantity + quantity;
        after.fill_count += 1;
        after.updated_at = fill.timestamp;
        const bool complete = after.remaining_quantity().raw() == 0;
        try {
            const Money reserved_after = reservations_.reserved_cash -
                reservation_of(*order).cash +
                (complete ? Money{} : reservation_of(after).cash);
            if (std::get<ApplyFillReceipt>(preview).after.cash <
                reserved_after) {
                skip(WorkingOrderSkipReason::ReservationShortfall);
                continue;
            }
        } catch (const std::overflow_error&) {
            skip(WorkingOrderSkipReason::ArithmeticOverflow);
            continue;
        }

        ApplyFillResult applied = account_.apply_fill(fill);
        if (!std::holds_alternative<ApplyFillReceipt>(applied)) {
            throw std::logic_error("a previewed fill could not be applied");
        }
        execution_simulator_.take_liquidity(observation, intent.side, quantity);
        report.fills.push_back(WorkingOrderFill{
            .accounting = std::get<ApplyFillReceipt>(std::move(applied)),
            .order = after,
            .status = complete ? PaperOrderStatus::Filled
                               : PaperOrderStatus::PartiallyFilled,
        });
        if (complete) {
            order = working_orders_.erase(order);
        } else {
            *order = std::move(after);
            ++order;
        }
        refresh_reservations();
    }
    if (!is_covered(reservations_, account_.state())) {
        throw std::logic_error("matching left reservations uncovered");
    }
    return report;
}

PaperOrderCancelResult PaperOrderProcessor::cancel(
    std::string_view client_order_id,
    Timestamp at
) {
    const auto order = std::find_if(
        working_orders_.begin(),
        working_orders_.end(),
        [client_order_id](const WorkingOrder& candidate) {
            return candidate.intent.client_order_id == client_order_id;
        }
    );
    if (order == working_orders_.end()) {
        return PaperOrderCancelError{
            .code = PaperOrderCancelErrorCode::OrderNotWorking,
            .message = "the order has no working remainder to cancel",
        };
    }
    WorkingOrderCancellation cancellation{
        .order = *order,
        .status = order->fee_charged()
            ? PaperOrderStatus::PartiallyFilledAndCancelled
            : PaperOrderStatus::Cancelled,
        .cancelled_at = std::max(at, order->updated_at),
    };
    working_orders_.erase(order);
    refresh_reservations();
    return cancellation;
}

}  // namespace chronos
