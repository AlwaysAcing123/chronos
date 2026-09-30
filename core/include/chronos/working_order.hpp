#pragma once

#include "chronos/order_intent.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace chronos {

// Where a durable order is in its lifecycle. Working and PartiallyFilled
// are the two working states: the order still has a remainder that later
// market observations can fill. Every other status is terminal. The two
// cancelled statuses differ only in whether anything filled first.
enum class PaperOrderStatus {
    Working,
    PartiallyFilled,
    Filled,
    PartiallyFilledAndCancelled,
    Cancelled,
    Rejected,
    Faulted,
};

[[nodiscard]] constexpr bool is_working(PaperOrderStatus status) noexcept {
    return status == PaperOrderStatus::Working ||
        status == PaperOrderStatus::PartiallyFilled;
}

// A good-til-cancelled limit order that has a remainder to fill.
//
// Its intent passed pre-trade risk when it was accepted. Only market
// observations newer than `accepted_ingest_sequence`, the observation it
// was accepted against, may fill it; that observation itself was already
// used when the order was submitted. The order pays `fee` once, with its
// first fill.
struct WorkingOrder {
    OrderIntent intent;
    std::uint64_t accepted_ingest_sequence{0};
    Money fee;
    Quantity filled_quantity;
    // Fills booked so far; the next fill's index.
    std::uint32_t fill_count{0};
    // Market time of the latest change: acceptance, then each fill.
    Timestamp updated_at;

    [[nodiscard]] Quantity remaining_quantity() const {
        return intent.quantity - filled_quantity;
    }

    [[nodiscard]] bool fee_charged() const noexcept {
        return filled_quantity.raw() > 0;
    }

    [[nodiscard]] PaperOrderStatus status() const noexcept {
        return fee_charged() ? PaperOrderStatus::PartiallyFilled
                             : PaperOrderStatus::Working;
    }

    friend bool operator==(const WorkingOrder&, const WorkingOrder&) = default;
};

// What one working order holds back from new orders. Reservations never
// change cash, positions, or any accounting total; they only limit what
// risk lets later orders use.
struct OrderReservation {
    Money cash;
    Quantity quantity;

    friend bool operator==(const OrderReservation&, const OrderReservation&) =
        default;
};

// A buy reserves its limit price times its remainder, rounded like any
// notional; a sell reserves its remainder of the position. Either also
// reserves its fee from cash until its first fill has paid it. Throws
// std::overflow_error when the amount leaves the fixed-point range and
// std::invalid_argument for an order that is not a working limit order.
[[nodiscard]] OrderReservation reservation_of(const WorkingOrder& order);

// The name of the fill with `index` among one order's fills. A client order
// ID never contains ':', so these names cannot collide across orders.
[[nodiscard]] std::string fill_id_for(
    const std::string& client_order_id,
    std::uint32_t index
);

struct InstrumentReservations {
    // Shares held back for working sells.
    Quantity reserved_sell_quantity;
    // Unfilled shares of working buys, which count toward the position
    // limit.
    Quantity working_buy_quantity;

    friend bool operator==(
        const InstrumentReservations&,
        const InstrumentReservations&
    ) = default;
};

struct AccountReservations {
    Money reserved_cash;
    // Only instruments with a working order appear.
    std::map<InstrumentId, InstrumentReservations> instruments;

    [[nodiscard]] Quantity reserved_sell_quantity(
        const InstrumentId& instrument
    ) const;

    [[nodiscard]] Quantity working_buy_quantity(
        const InstrumentId& instrument
    ) const;

    friend bool operator==(
        const AccountReservations&,
        const AccountReservations&
    ) = default;
};

// The sum of every order's reservation, with checked arithmetic. Throws
// like reservation_of.
[[nodiscard]] AccountReservations account_reservations(
    const std::vector<WorkingOrder>& orders
);

}  // namespace chronos
