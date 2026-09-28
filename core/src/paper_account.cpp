#include "chronos/paper_account.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace chronos {

Price AccountState::average_entry_price() const {
    if (position.raw() == 0) {
        return Price{};
    }
    return average_price(open_cost_basis, position);
}

PaperAccount::PaperAccount(
    std::string account_id,
    InstrumentId instrument,
    Money starting_cash
)
    : account_id_(std::move(account_id)),
      instrument_(std::move(instrument)),
      state_{
          .starting_cash = starting_cash,
          .cash = starting_cash,
          .position = Quantity{},
          .open_cost_basis = Money{},
          .gross_realized_pnl = Money{},
          .fees_paid = Money{},
          .last_fill_timestamp = std::nullopt,
      } {
    if (account_id_.empty()) {
        throw std::invalid_argument("paper account ID must not be empty");
    }
    if (starting_cash.raw() < 0) {
        throw std::invalid_argument("paper account starting cash must not be negative");
    }
}

PaperAccountRecoveryState PaperAccount::recovery_state() const {
    std::vector<std::string> applied_fill_ids(
        applied_fill_ids_.begin(),
        applied_fill_ids_.end()
    );
    std::sort(applied_fill_ids.begin(), applied_fill_ids.end());
    return PaperAccountRecoveryState{
        .account_id = account_id_,
        .instrument = instrument_,
        .state = state_,
        .applied_fill_ids = std::move(applied_fill_ids),
    };
}

PaperAccount PaperAccount::restore(
    PaperAccountRecoveryState recovery_state
) {
    const AccountState& state = recovery_state.state;
    if (state.starting_cash.raw() < 0) {
        throw std::invalid_argument(
            "recovered starting cash must not be negative"
        );
    }
    if (state.cash.raw() < 0) {
        throw std::invalid_argument("recovered cash must not be negative");
    }
    if (state.position.raw() < 0) {
        throw std::invalid_argument(
            "recovered position must not be negative"
        );
    }
    if (state.open_cost_basis.raw() < 0) {
        throw std::invalid_argument(
            "recovered open cost basis must not be negative"
        );
    }
    if (state.position.raw() == 0 && state.open_cost_basis.raw() != 0) {
        throw std::invalid_argument(
            "a recovered flat position must have zero open cost basis"
        );
    }
    if (state.position.raw() != 0 && state.open_cost_basis.raw() == 0) {
        throw std::invalid_argument(
            "a recovered open position must have positive cost basis"
        );
    }
    if (state.fees_paid.raw() < 0) {
        throw std::invalid_argument(
            "recovered fees paid must not be negative"
        );
    }
    if (state.last_fill_timestamp &&
        state.last_fill_timestamp->nanoseconds() < 0) {
        throw std::invalid_argument(
            "recovered fill timestamp must not be negative"
        );
    }
    if (state.last_fill_timestamp.has_value() !=
        !recovery_state.applied_fill_ids.empty()) {
        throw std::invalid_argument(
            "recovered fill history and latest fill timestamp do not match"
        );
    }

    const detail::WideInteger cash_and_open_cost =
        static_cast<detail::WideInteger>(state.cash.raw()) +
        state.open_cost_basis.raw();
    const detail::WideInteger expected_cash_and_open_cost =
        static_cast<detail::WideInteger>(state.starting_cash.raw()) +
        state.gross_realized_pnl.raw() - state.fees_paid.raw();
    if (cash_and_open_cost != expected_cash_and_open_cost) {
        throw std::invalid_argument(
            "recovered account violates the accounting identity"
        );
    }

    PaperAccount account(
        std::move(recovery_state.account_id),
        std::move(recovery_state.instrument),
        state.starting_cash
    );
    account.state_ = state;
    for (std::string& fill_id : recovery_state.applied_fill_ids) {
        if (fill_id.empty()) {
            throw std::invalid_argument(
                "recovered fill ID must not be empty"
            );
        }
        const bool inserted = account.applied_fill_ids_
            .insert(std::move(fill_id))
            .second;
        if (!inserted) {
            throw std::invalid_argument(
                "recovered fill IDs must be unique"
            );
        }
    }
    return account;
}

std::optional<ApplyFillError> PaperAccount::validate_fill(const Fill& fill) const {
    if (fill.fill_id.empty()) {
        return ApplyFillError{
            ApplyFillErrorCode::MissingFillId,
            "fill ID must not be empty",
        };
    }
    if (applied_fill_ids_.contains(fill.fill_id)) {
        return ApplyFillError{
            ApplyFillErrorCode::DuplicateFill,
            "fill has already been applied",
        };
    }
    if (fill.instrument != instrument_) {
        return ApplyFillError{
            ApplyFillErrorCode::InstrumentMismatch,
            "fill instrument does not match the paper account",
        };
    }
    if (fill.price.raw() <= 0) {
        return ApplyFillError{
            ApplyFillErrorCode::NonpositivePrice,
            "fill price must be positive",
        };
    }
    if (fill.quantity.raw() <= 0) {
        return ApplyFillError{
            ApplyFillErrorCode::NonpositiveQuantity,
            "fill quantity must be positive",
        };
    }
    if (fill.fee.raw() < 0) {
        return ApplyFillError{
            ApplyFillErrorCode::NegativeFee,
            "fill fee must not be negative",
        };
    }
    if (fill.timestamp.nanoseconds() < 0) {
        return ApplyFillError{
            ApplyFillErrorCode::NegativeTimestamp,
            "fill timestamp must not be negative",
        };
    }
    if (state_.last_fill_timestamp.has_value() &&
        fill.timestamp < *state_.last_fill_timestamp) {
        return ApplyFillError{
            ApplyFillErrorCode::TimestampMovedBackward,
            "fill timestamp must not move backward",
        };
    }
    if (fill.side != Side::Buy && fill.side != Side::Sell) {
        return ApplyFillError{
            ApplyFillErrorCode::UnknownSide,
            "fill side is not recognized",
        };
    }
    return std::nullopt;
}

ApplyFillResult PaperAccount::apply_fill(const Fill& fill) {
    if (const auto error = validate_fill(fill); error.has_value()) {
        return *error;
    }

    const AccountState before = state_;
    AccountState after = before;

    try {
        const Money fill_notional = calculate_notional(fill.price, fill.quantity);
        if (fill_notional.raw() == 0) {
            return ApplyFillError{
                ApplyFillErrorCode::NotionalRoundedToZero,
                "fill notional is smaller than the supported money precision",
            };
        }
        Money realized_change;

        if (fill.side == Side::Buy) {
            const Money cash_required = fill_notional + fill.fee;
            if (cash_required > before.cash) {
                return ApplyFillError{
                    ApplyFillErrorCode::InsufficientCash,
                    "paper account does not have enough cash for the fill and fee",
                };
            }

            after.cash = before.cash - cash_required;
            after.position = before.position + fill.quantity;
            after.open_cost_basis = before.open_cost_basis + fill_notional;
        } else {
            if (fill.quantity > before.position) {
                return ApplyFillError{
                    ApplyFillErrorCode::InsufficientPosition,
                    "sell fill exceeds the paper account position",
                };
            }

            const Money removed_cost_basis = fill.quantity == before.position
                ? before.open_cost_basis
                : proportional_amount(
                      before.open_cost_basis,
                      fill.quantity,
                      before.position
                  );
            realized_change = fill_notional - removed_cost_basis;
            after.cash = before.cash + fill_notional - fill.fee;
            if (after.cash.raw() < 0) {
                return ApplyFillError{
                    ApplyFillErrorCode::InsufficientCash,
                    "paper account does not have enough cash to pay the sell fee",
                };
            }
            after.position = before.position - fill.quantity;
            after.open_cost_basis = before.open_cost_basis - removed_cost_basis;
            after.gross_realized_pnl = before.gross_realized_pnl + realized_change;

            if (after.position.raw() == 0) {
                after.open_cost_basis = Money{};
            }
        }

        after.fees_paid = before.fees_paid + fill.fee;
        after.last_fill_timestamp = fill.timestamp;

        applied_fill_ids_.insert(fill.fill_id);
        state_ = after;
        return ApplyFillReceipt{
            .fill = fill,
            .notional = fill_notional,
            .gross_realized_pnl_change = realized_change,
            .before = before,
            .after = after,
        };
    } catch (const std::overflow_error&) {
        return ApplyFillError{
            ApplyFillErrorCode::ArithmeticOverflow,
            "fill arithmetic exceeds the supported fixed-point range",
        };
    }
}

AccountSnapshot PaperAccount::snapshot(Price mark_price) const {
    if (mark_price.raw() <= 0) {
        throw std::invalid_argument("mark price must be positive");
    }

    const Money market_value = calculate_notional(mark_price, state_.position);
    const Money unrealized_pnl = market_value - state_.open_cost_basis;
    const Money gross_total_pnl = state_.gross_realized_pnl + unrealized_pnl;
    const Money net_total_pnl = gross_total_pnl - state_.fees_paid;
    const Money equity = state_.cash + market_value;

    if (equity - state_.starting_cash != net_total_pnl) {
        throw std::logic_error("paper account accounting invariant failed");
    }

    return AccountSnapshot{
        .state = state_,
        .mark_price = mark_price,
        .market_value = market_value,
        .gross_unrealized_pnl = unrealized_pnl,
        .gross_total_pnl = gross_total_pnl,
        .net_total_pnl = net_total_pnl,
        .equity = equity,
    };
}

}  // namespace chronos
