#include "chronos/paper_account.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace chronos {
namespace {

// Sums one money field over every ledger with checked arithmetic.
template <typename Field>
Money sum_positions(
    const std::map<InstrumentId, PositionState>& positions,
    Field field
) {
    Money total;
    for (const auto& [instrument, position] : positions) {
        total = total + field(position);
    }
    return total;
}

void validate_recovered_position(const PositionState& position) {
    if (position.quantity.raw() < 0) {
        throw std::invalid_argument(
            "recovered position must not be negative"
        );
    }
    if (position.open_cost_basis.raw() < 0) {
        throw std::invalid_argument(
            "recovered open cost basis must not be negative"
        );
    }
    if (position.quantity.raw() == 0 && position.open_cost_basis.raw() != 0) {
        throw std::invalid_argument(
            "a recovered closed position must have zero open cost basis"
        );
    }
    if (position.quantity.raw() != 0 && position.open_cost_basis.raw() == 0) {
        throw std::invalid_argument(
            "a recovered open position must have positive cost basis"
        );
    }
    if (position.fees_paid.raw() < 0) {
        throw std::invalid_argument(
            "recovered fees paid must not be negative"
        );
    }
    if (position.last_fill_timestamp.nanoseconds() < 0) {
        throw std::invalid_argument(
            "recovered fill timestamp must not be negative"
        );
    }
}

}  // namespace

Price PositionState::average_entry_price() const {
    if (quantity.raw() == 0) {
        return Price{};
    }
    return average_price(open_cost_basis, quantity);
}

Money AccountState::open_cost_basis() const {
    return sum_positions(positions, [](const PositionState& position) {
        return position.open_cost_basis;
    });
}

Money AccountState::gross_realized_pnl() const {
    return sum_positions(positions, [](const PositionState& position) {
        return position.gross_realized_pnl;
    });
}

Money AccountState::fees_paid() const {
    return sum_positions(positions, [](const PositionState& position) {
        return position.fees_paid;
    });
}

std::optional<Timestamp> AccountState::latest_fill_timestamp() const {
    std::optional<Timestamp> latest;
    for (const auto& [instrument, position] : positions) {
        if (!latest || position.last_fill_timestamp > *latest) {
            latest = position.last_fill_timestamp;
        }
    }
    return latest;
}

const PositionState* AccountState::position(
    const InstrumentId& instrument
) const {
    const auto found = positions.find(instrument);
    return found == positions.end() ? nullptr : &found->second;
}

Quantity AccountState::quantity(const InstrumentId& instrument) const {
    const PositionState* ledger = position(instrument);
    return ledger == nullptr ? Quantity{} : ledger->quantity;
}

PaperAccount::PaperAccount(std::string account_id, Money starting_cash)
    : account_id_(std::move(account_id)),
      state_{
          .starting_cash = starting_cash,
          .cash = starting_cash,
          .positions = {},
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
    for (const auto& [instrument, position] : state.positions) {
        validate_recovered_position(position);
    }
    // A ledger exists exactly when some fill was applied.
    if (state.positions.empty() != recovery_state.applied_fill_ids.empty()) {
        throw std::invalid_argument(
            "recovered fill history and positions do not match"
        );
    }

    Money open_cost_basis;
    Money gross_realized_pnl;
    Money fees_paid;
    try {
        open_cost_basis = state.open_cost_basis();
        gross_realized_pnl = state.gross_realized_pnl();
        fees_paid = state.fees_paid();
    } catch (const std::overflow_error&) {
        throw std::invalid_argument(
            "recovered account totals exceed the fixed-point range"
        );
    }
    const detail::WideInteger cash_and_open_cost =
        static_cast<detail::WideInteger>(state.cash.raw()) +
        open_cost_basis.raw();
    const detail::WideInteger expected_cash_and_open_cost =
        static_cast<detail::WideInteger>(state.starting_cash.raw()) +
        gross_realized_pnl.raw() - fees_paid.raw();
    if (cash_and_open_cost != expected_cash_and_open_cost) {
        throw std::invalid_argument(
            "recovered account violates the accounting identity"
        );
    }

    PaperAccount account(
        std::move(recovery_state.account_id),
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
    const PositionState* position = state_.position(fill.instrument);
    if (position != nullptr && fill.timestamp < position->last_fill_timestamp) {
        return ApplyFillError{
            ApplyFillErrorCode::TimestampMovedBackward,
            "fill timestamp must not precede the instrument's latest fill",
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

ApplyFillResult PaperAccount::preview_fill(const Fill& fill) const {
    if (const auto error = validate_fill(fill); error.has_value()) {
        return *error;
    }

    const AccountState& before = state_;
    AccountState after = before;
    const PositionState* existing = before.position(fill.instrument);
    PositionState position = existing != nullptr
        ? *existing
        : PositionState{
              .quantity = Quantity{},
              .open_cost_basis = Money{},
              .gross_realized_pnl = Money{},
              .fees_paid = Money{},
              .last_fill_timestamp = fill.timestamp,
          };

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
            position.quantity = position.quantity + fill.quantity;
            position.open_cost_basis = position.open_cost_basis + fill_notional;
        } else {
            if (fill.quantity > position.quantity) {
                return ApplyFillError{
                    ApplyFillErrorCode::InsufficientPosition,
                    "sell fill exceeds the instrument's position",
                };
            }

            const Money removed_cost_basis = fill.quantity == position.quantity
                ? position.open_cost_basis
                : proportional_amount(
                      position.open_cost_basis,
                      fill.quantity,
                      position.quantity
                  );
            realized_change = fill_notional - removed_cost_basis;
            after.cash = before.cash + fill_notional - fill.fee;
            if (after.cash.raw() < 0) {
                return ApplyFillError{
                    ApplyFillErrorCode::InsufficientCash,
                    "paper account does not have enough cash to pay the sell fee",
                };
            }
            position.quantity = position.quantity - fill.quantity;
            position.open_cost_basis = position.open_cost_basis - removed_cost_basis;
            position.gross_realized_pnl = position.gross_realized_pnl + realized_change;

            // Selling everything removes the complete cost basis, leaving no
            // rounding dust behind.
            if (position.quantity.raw() == 0) {
                position.open_cost_basis = Money{};
            }
        }

        position.fees_paid = position.fees_paid + fill.fee;
        position.last_fill_timestamp = fill.timestamp;
        after.positions.insert_or_assign(fill.instrument, position);

        // The account totals must stay representable, even though each
        // ledger fits, or no later valuation could be exact.
        static_cast<void>(after.open_cost_basis());
        static_cast<void>(after.gross_realized_pnl());
        static_cast<void>(after.fees_paid());

        return ApplyFillReceipt{
            .fill = fill,
            .notional = fill_notional,
            .gross_realized_pnl_change = realized_change,
            .before = before,
            .after = std::move(after),
        };
    } catch (const std::overflow_error&) {
        return ApplyFillError{
            ApplyFillErrorCode::ArithmeticOverflow,
            "fill arithmetic exceeds the supported fixed-point range",
        };
    }
}

ApplyFillResult PaperAccount::apply_fill(const Fill& fill) {
    ApplyFillResult result = preview_fill(fill);
    if (const auto* receipt = std::get_if<ApplyFillReceipt>(&result)) {
        applied_fill_ids_.insert(fill.fill_id);
        state_ = receipt->after;
    }
    return result;
}

AccountSnapshot PaperAccount::snapshot(
    const std::map<InstrumentId, Price>& marks
) const {
    std::vector<PositionSnapshot> open_positions;
    Money market_value;
    Money gross_unrealized_pnl;
    for (const auto& [instrument, position] : state_.positions) {
        if (!position.is_open()) {
            continue;
        }
        const auto mark = marks.find(instrument);
        if (mark == marks.end() || mark->second.raw() <= 0) {
            throw std::invalid_argument(
                "every open position needs a positive mark price"
            );
        }
        const Money position_value = calculate_notional(
            mark->second,
            position.quantity
        );
        const Money position_unrealized = position_value - position.open_cost_basis;
        market_value = market_value + position_value;
        gross_unrealized_pnl = gross_unrealized_pnl + position_unrealized;
        open_positions.push_back(PositionSnapshot{
            .instrument = instrument,
            .position = position,
            .mark_price = mark->second,
            .market_value = position_value,
            .gross_unrealized_pnl = position_unrealized,
        });
    }

    const Money open_cost_basis = state_.open_cost_basis();
    const Money gross_realized_pnl = state_.gross_realized_pnl();
    const Money fees_paid = state_.fees_paid();
    const Money gross_total_pnl = gross_realized_pnl + gross_unrealized_pnl;
    const Money net_total_pnl = gross_total_pnl - fees_paid;
    const Money equity = state_.cash + market_value;

    if (equity - state_.starting_cash != net_total_pnl ||
        market_value - open_cost_basis != gross_unrealized_pnl) {
        throw std::logic_error("paper account accounting invariant failed");
    }

    return AccountSnapshot{
        .state = state_,
        .open_positions = std::move(open_positions),
        .open_cost_basis = open_cost_basis,
        .market_value = market_value,
        .gross_unrealized_pnl = gross_unrealized_pnl,
        .gross_realized_pnl = gross_realized_pnl,
        .fees_paid = fees_paid,
        .gross_total_pnl = gross_total_pnl,
        .net_total_pnl = net_total_pnl,
        .equity = equity,
    };
}

}  // namespace chronos
