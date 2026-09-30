#include "chronos/working_order.hpp"

#include <stdexcept>
#include <string>

namespace chronos {

OrderReservation reservation_of(const WorkingOrder& order) {
    const OrderIntent& intent = order.intent;
    if (intent.type != OrderType::Limit || !intent.limit_price ||
        intent.time_in_force != TimeInForce::GoodTilCancelled) {
        throw std::invalid_argument(
            "only good-til-cancelled limit orders reserve resources"
        );
    }
    const Quantity remaining = order.remaining_quantity();
    if (order.filled_quantity.raw() < 0 || remaining.raw() <= 0) {
        throw std::invalid_argument(
            "a working order must have an unfilled remainder"
        );
    }
    const Money fee = order.fee_charged() ? Money{} : order.fee;
    switch (intent.side) {
        case Side::Buy:
            return OrderReservation{
                .cash = calculate_notional(*intent.limit_price, remaining) + fee,
                .quantity = Quantity{},
            };
        case Side::Sell:
            return OrderReservation{
                .cash = fee,
                .quantity = remaining,
            };
    }
    throw std::invalid_argument("working order side is not recognized");
}

std::string fill_id_for(
    const std::string& client_order_id,
    std::uint32_t index
) {
    return client_order_id + ":fill:" + std::to_string(index);
}

Quantity AccountReservations::reserved_sell_quantity(
    const InstrumentId& instrument
) const {
    const auto found = instruments.find(instrument);
    return found == instruments.end() ? Quantity{}
                                      : found->second.reserved_sell_quantity;
}

Quantity AccountReservations::working_buy_quantity(
    const InstrumentId& instrument
) const {
    const auto found = instruments.find(instrument);
    return found == instruments.end() ? Quantity{}
                                      : found->second.working_buy_quantity;
}

AccountReservations account_reservations(
    const std::vector<WorkingOrder>& orders
) {
    AccountReservations total;
    for (const WorkingOrder& order : orders) {
        const OrderReservation reservation = reservation_of(order);
        total.reserved_cash = total.reserved_cash + reservation.cash;
        InstrumentReservations& instrument =
            total.instruments[order.intent.instrument];
        instrument.reserved_sell_quantity =
            instrument.reserved_sell_quantity + reservation.quantity;
        if (order.intent.side == Side::Buy) {
            instrument.working_buy_quantity =
                instrument.working_buy_quantity + order.remaining_quantity();
        }
    }
    return total;
}

}  // namespace chronos
