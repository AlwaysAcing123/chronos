#pragma once

#include "chronos/trading_types.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

namespace chronos {

struct AccountState {
    Money starting_cash;
    Money cash;
    Quantity position;
    Money open_cost_basis;
    Money gross_realized_pnl;
    Money fees_paid;
    std::optional<Timestamp> last_fill_timestamp;

    [[nodiscard]] Price average_entry_price() const;
};

struct AccountSnapshot {
    AccountState state;
    Price mark_price;
    Money market_value;
    Money gross_unrealized_pnl;
    Money gross_total_pnl;
    Money net_total_pnl;
    Money equity;
};

enum class ApplyFillErrorCode {
    MissingFillId,
    DuplicateFill,
    InstrumentMismatch,
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

class PaperAccount final {
public:
    PaperAccount(
        std::string account_id,
        InstrumentId instrument,
        Money starting_cash
    );

    [[nodiscard]] const std::string& account_id() const { return account_id_; }
    [[nodiscard]] const InstrumentId& instrument() const { return instrument_; }
    [[nodiscard]] const AccountState& state() const { return state_; }

    [[nodiscard]] ApplyFillResult apply_fill(const Fill& fill);
    [[nodiscard]] AccountSnapshot snapshot(Price mark_price) const;

private:
    [[nodiscard]] std::optional<ApplyFillError> validate_fill(
        const Fill& fill
    ) const;

    std::string account_id_;
    InstrumentId instrument_;
    AccountState state_;
    std::unordered_set<std::string> applied_fill_ids_;
};

}  // namespace chronos
