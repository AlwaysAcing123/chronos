#pragma once

#include "chronos/fixed_decimal.hpp"

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace chronos {

template <typename Tag>
class Identifier final {
public:
    explicit Identifier(std::string value) : value_(std::move(value)) {
        if (value_.empty()) {
            throw std::invalid_argument("identifier must not be empty");
        }
    }

    [[nodiscard]] const std::string& value() const { return value_; }

    friend auto operator<=>(const Identifier&, const Identifier&) = default;

private:
    std::string value_;
};

struct InstrumentIdTag;
using InstrumentId = Identifier<InstrumentIdTag>;

class Timestamp final {
public:
    constexpr Timestamp() = default;

    static constexpr Timestamp from_nanoseconds(std::int64_t nanoseconds) {
        return Timestamp(nanoseconds);
    }

    [[nodiscard]] constexpr std::int64_t nanoseconds() const {
        return nanoseconds_;
    }

    friend constexpr auto operator<=>(const Timestamp&, const Timestamp&) = default;

private:
    explicit constexpr Timestamp(std::int64_t nanoseconds)
        : nanoseconds_(nanoseconds) {}

    std::int64_t nanoseconds_{0};
};

enum class Side {
    Buy,
    Sell,
};

struct Fill {
    std::string fill_id;
    InstrumentId instrument;
    Side side{Side::Buy};
    Price price;
    Quantity quantity;
    Money fee;
    Timestamp timestamp;
};

}  // namespace chronos
