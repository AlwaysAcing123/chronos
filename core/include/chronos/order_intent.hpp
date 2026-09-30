#pragma once

#include "chronos/trading_types.hpp"

#include <optional>
#include <string>

namespace chronos {

enum class OrderSourceKind {
    Manual,
    Model,
};

struct OrderSource {
    OrderSourceKind kind{OrderSourceKind::Manual};
    std::string source_id;

    friend bool operator==(const OrderSource&, const OrderSource&) = default;
};

enum class OrderType {
    Market,
    Limit,
};

enum class TimeInForce {
    ImmediateOrCancel,
    // Only limit orders: the remainder rests until it fills or is cancelled.
    GoodTilCancelled,
};

struct OrderIntent {
    std::string client_order_id;
    std::string account_id;
    OrderSource source;
    InstrumentId instrument;
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    Quantity quantity;
    std::optional<Price> limit_price;
    TimeInForce time_in_force{TimeInForce::ImmediateOrCancel};
    Timestamp submitted_at;

    friend bool operator==(const OrderIntent&, const OrderIntent&) = default;
};

}  // namespace chronos
