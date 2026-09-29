#pragma once

#include "chronos/trading_types.hpp"

#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace chronos {

// One instrument's ledger in a paper account. The instrument's first fill
// creates it; it is kept when the quantity returns to zero (a closed
// position), so the realized PnL and fees that instrument produced stay
// attributable. An instrument that was never filled has no ledger.
struct PositionState {
    Quantity quantity;
    Money open_cost_basis;
    Money gross_realized_pnl;
    // Fees of this instrument's fills.
    Money fees_paid;
    Timestamp last_fill_timestamp;

    [[nodiscard]] bool is_open() const noexcept { return quantity.raw() != 0; }

    // Zero for a closed position.
    [[nodiscard]] Price average_entry_price() const;

    friend bool operator==(const PositionState&, const PositionState&) = default;
};

// Shared cash and one ledger per traded instrument, ordered by instrument.
// Account totals are derived from the ledgers, never stored, so there is one
// source of truth. Every valid state satisfies
//
//   cash + open_cost_basis() = starting_cash + gross_realized_pnl() - fees_paid()
struct AccountState {
    Money starting_cash;
    Money cash;
    std::map<InstrumentId, PositionState> positions;

    // Sums over every ledger, including closed ones. Each throws
    // std::overflow_error when the total leaves the fixed-point range.
    [[nodiscard]] Money open_cost_basis() const;
    [[nodiscard]] Money gross_realized_pnl() const;
    [[nodiscard]] Money fees_paid() const;

    // The latest fill of any instrument; absent before the first fill.
    [[nodiscard]] std::optional<Timestamp> latest_fill_timestamp() const;

    // The instrument's ledger, or nullptr if it was never filled.
    [[nodiscard]] const PositionState* position(
        const InstrumentId& instrument
    ) const;

    // The held quantity, zero without an open position.
    [[nodiscard]] Quantity quantity(const InstrumentId& instrument) const;

    friend bool operator==(const AccountState&, const AccountState&) = default;
};

// An open position marked at an explicit price.
struct PositionSnapshot {
    InstrumentId instrument;
    PositionState position;
    Price mark_price;
    Money market_value;
    Money gross_unrealized_pnl;
};

// The account marked at one price per open position. Totals reconcile
// exactly: equity - starting cash = net total PnL.
struct AccountSnapshot {
    AccountState state;
    // Open positions only, ordered by instrument.
    std::vector<PositionSnapshot> open_positions;
    Money open_cost_basis;
    Money market_value;
    Money gross_unrealized_pnl;
    Money gross_realized_pnl;
    Money fees_paid;
    Money gross_total_pnl;
    Money net_total_pnl;
    Money equity;
};

enum class ApplyFillErrorCode {
    MissingFillId,
    DuplicateFill,
    NonpositivePrice,
    NonpositiveQuantity,
    NotionalRoundedToZero,
    NegativeFee,
    NegativeTimestamp,
    TimestampMovedBackward,
    InsufficientCash,
    InsufficientPosition,
    UnknownSide,
    ArithmeticOverflow,
};

struct ApplyFillError {
    ApplyFillErrorCode code;
    std::string message;
};

struct ApplyFillReceipt {
    Fill fill;
    Money notional;
    Money gross_realized_pnl_change;
    AccountState before;
    AccountState after;
};

using ApplyFillResult = std::variant<ApplyFillReceipt, ApplyFillError>;

struct PaperAccountRecoveryState {
    std::string account_id;
    AccountState state;
    std::vector<std::string> applied_fill_ids;
};

// A long-only USD paper account that holds any number of instruments.
//
// A fill changes only shared cash and its own instrument's ledger. Fill
// times must not move backward within one instrument; fills of different
// instruments may interleave in any order, because the account timeline is
// enforced where orders are accepted (see PreTradeRiskEngine).
class PaperAccount final {
public:
    PaperAccount(std::string account_id, Money starting_cash);

    [[nodiscard]] const std::string& account_id() const { return account_id_; }
    [[nodiscard]] const AccountState& state() const { return state_; }

    [[nodiscard]] PaperAccountRecoveryState recovery_state() const;
    [[nodiscard]] static PaperAccount restore(
        PaperAccountRecoveryState recovery_state
    );

    // The receipt applying `fill` would produce, without applying it.
    [[nodiscard]] ApplyFillResult preview_fill(const Fill& fill) const;

    [[nodiscard]] ApplyFillResult apply_fill(const Fill& fill);

    // Marks every open position at its price in `marks`. Throws
    // std::invalid_argument when an open position has no positive mark,
    // std::overflow_error when a value leaves the fixed-point range, and
    // std::logic_error when the totals do not reconcile.
    [[nodiscard]] AccountSnapshot snapshot(
        const std::map<InstrumentId, Price>& marks
    ) const;

private:
    [[nodiscard]] std::optional<ApplyFillError> validate_fill(
        const Fill& fill
    ) const;

    std::string account_id_;
    AccountState state_;
    std::unordered_set<std::string> applied_fill_ids_;
};

}  // namespace chronos
