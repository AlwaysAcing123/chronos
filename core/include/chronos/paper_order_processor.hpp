#pragma once

#include "chronos/top_of_book_execution.hpp"
#include "chronos/working_order.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos {

struct PaperOrderReceipt {
    SimulatedExecutionReport execution;
    std::optional<ApplyFillReceipt> accounting;
    AccountState account_after;
};

enum class PaperOrderProcessingErrorCode {
    ProcessorFaulted,
    AccountingInvariantViolation,
};

struct PaperOrderProcessingError {
    PaperOrderProcessingErrorCode code;
    std::string message;
    std::optional<SimulatedExecutionReport> execution;
    std::optional<ApplyFillError> accounting_error;
};

using PaperOrderResult = std::variant<
    PaperOrderReceipt,
    PreTradeRiskRejection,
    SimulatedExecutionError,
    PaperOrderProcessingError
>;

// A working order filled by a market observation after it was accepted.
struct WorkingOrderFill {
    ApplyFillReceipt accounting;
    // The order after the fill.
    WorkingOrder order;
    // PartiallyFilled while a remainder still rests; Filled once none does.
    PaperOrderStatus status;
};

// Why an eligible, marketable working order took nothing from an
// observation. The order keeps working and no fill or accounting change is
// made. The observation's remaining liquidity on that side is nevertheless
// consumed for this paper account, so a newer order cannot jump the skipped
// order; a later observation may fill it.
enum class WorkingOrderSkipReason {
    // Each fill's notional is rounded, so a partial buy fill can cost up to
    // one money unit (1e-8 USD) more than the reservation it releases.
    // With less than that unreserved, the fill would leave the account's
    // other reservations uncovered.
    ReservationShortfall,
    NotionalRoundedToZero,
    // An account total would leave the fixed-point range.
    ArithmeticOverflow,
};

struct WorkingOrderSkip {
    std::string client_order_id;
    WorkingOrderSkipReason reason;
};

// A working order whose fill accounting rejected. The order is faulted,
// its fill was never booked, and the account is halted.
struct WorkingOrderFault {
    WorkingOrder order;
    Fill unbooked_fill;
    ApplyFillError accounting_error;
};

// What one market observation did to one account's working orders.
struct WorkingOrderMatch {
    TopOfBookEvent observation;
    // In priority order.
    std::vector<WorkingOrderFill> fills;
    std::vector<WorkingOrderSkip> skipped;
    std::optional<WorkingOrderFault> fault;

    [[nodiscard]] bool changed_account() const noexcept {
        return !fills.empty() || !skipped.empty() || fault.has_value();
    }
};

// A working order's remainder cancelled at a client's request. Its fills
// and their one fee stand.
struct WorkingOrderCancellation {
    // The order as it was just before cancellation.
    WorkingOrder order;
    // Cancelled when nothing filled, else PartiallyFilledAndCancelled.
    PaperOrderStatus status;
    Timestamp cancelled_at;
};

enum class PaperOrderCancelErrorCode {
    OrderNotWorking,
};

struct PaperOrderCancelError {
    PaperOrderCancelErrorCode code;
    std::string message;
};

using PaperOrderCancelResult = std::variant<
    WorkingOrderCancellation,
    PaperOrderCancelError
>;

struct PaperOrderProcessorRecoveryState {
    PreTradeRiskLimits risk_limits;
    PaperAccountRecoveryState account;
    TopOfBookExecutionRecoveryState execution;
    bool is_healthy{true};
    // In priority order: the earliest accepted first.
    std::vector<WorkingOrder> working_orders{};
};

// One paper account's order processing: pre-trade risk, top-of-book
// execution, accounting, and its good-til-cancelled working orders.
//
// Working orders rest in acceptance order, which is their priority. Each
// market observation of an instrument is offered to that instrument's
// working orders in priority order (see match); the account's own earlier
// takings from the same observation are never offered again. A submission
// should offer its observation to the working orders first, so they keep
// priority over the new order on that observation.
class PaperOrderProcessor final {
public:
    PaperOrderProcessor(
        std::string account_id,
        Money starting_cash,
        PreTradeRiskLimits risk_limits
    );

    PaperOrderProcessor(const PaperOrderProcessor&) = delete;
    PaperOrderProcessor& operator=(const PaperOrderProcessor&) = delete;
    PaperOrderProcessor(PaperOrderProcessor&&) noexcept = default;
    PaperOrderProcessor& operator=(PaperOrderProcessor&&) noexcept = default;

    [[nodiscard]] const PaperAccount& account() const noexcept {
        return account_;
    }

    [[nodiscard]] const PreTradeRiskLimits& risk_limits() const noexcept {
        return risk_engine_.limits();
    }

    [[nodiscard]] bool is_healthy() const noexcept { return is_healthy_; }

    // Each executed instrument's latest execution observation and consumed
    // liquidity. Together they fix the account's market timeline.
    [[nodiscard]] const InstrumentExecutionStates& executions()
        const noexcept {
        return execution_simulator_.instruments();
    }

    // In priority order.
    [[nodiscard]] const std::vector<WorkingOrder>& working_orders()
        const noexcept {
        return working_orders_;
    }

    [[nodiscard]] const AccountReservations& reservations() const noexcept {
        return reservations_;
    }

    [[nodiscard]] PaperOrderProcessorRecoveryState recovery_state() const;
    [[nodiscard]] static PaperOrderProcessor restore(
        PaperOrderProcessorRecoveryState recovery_state
    );

    [[nodiscard]] PaperOrderResult submit(
        const OrderIntent& intent,
        const TopOfBookEvent& market
    );

    // Offers `observation` to the working orders of its instrument, in
    // priority order, at the market clock `market_time`. Each marketable
    // order accepted before the observation fills at the opposite best
    // price, up to the displayed quantity that earlier orders of this
    // account left on it. If an older marketable order must skip the
    // observation, newer orders on the same side cannot take it. Nothing
    // happens when the account is halted, when
    // the observation is invalid, suspect, stale, on another stream, or not
    // newer than the instrument's latest execution observation. Fills are
    // timestamped with the observation's receive time, so matching one
    // observation always has the same result.
    [[nodiscard]] WorkingOrderMatch match(
        const TopOfBookEvent& observation,
        Timestamp market_time
    );

    // Cancels a working order's remainder at `at`, or at the order's latest
    // change if that is later. A halted account may still cancel.
    [[nodiscard]] PaperOrderCancelResult cancel(
        std::string_view client_order_id,
        Timestamp at
    );

private:
    [[nodiscard]] std::optional<Quantity> immediate_fill_limit(
        const ApprovedOrderIntent& approved
    ) const;
    void refresh_reservations();

    PreTradeRiskEngine risk_engine_;
    PaperAccount account_;
    TopOfBookExecutionSimulator execution_simulator_;
    bool is_healthy_{true};
    std::vector<WorkingOrder> working_orders_;
    AccountReservations reservations_;
};

}  // namespace chronos
