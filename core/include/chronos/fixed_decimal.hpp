#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chronos {

inline constexpr std::int64_t kMoneyScale = 100'000'000;
inline constexpr std::int64_t kPriceScale = 1'000'000'000;
inline constexpr std::int64_t kQuantityScale = 100'000'000;
inline constexpr std::int64_t kPercentageScale = 100'000'000;

enum class RoundingMode {
    TowardZero,
    HalfAwayFromZero,
};

namespace detail {

#if !defined(__SIZEOF_INT128__)
#error "Chronos fixed-point arithmetic requires a signed 128-bit integer type"
#endif

// Clang and GCC provide this extension for overflow-safe intermediate
// arithmetic. Keeping it behind one alias documents and isolates the
// non-standard type while preserving strict warnings everywhere else.
__extension__ typedef __int128 WideInteger;

consteval int decimal_places_for_scale(std::int64_t scale) {
    if (scale <= 0) {
        return -1;
    }

    int decimal_places = 0;
    while (scale > 1) {
        if (scale % 10 != 0) {
            return -1;
        }
        scale /= 10;
        ++decimal_places;
    }
    return decimal_places;
}

inline std::int64_t checked_int64(WideInteger value) {
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error("fixed-point result exceeds the int64 range");
    }
    return static_cast<std::int64_t>(value);
}

inline std::int64_t rounded_divide(
    WideInteger numerator,
    std::int64_t denominator,
    RoundingMode mode
) {
    if (denominator <= 0) {
        throw std::invalid_argument("fixed-point denominator must be positive");
    }

    const bool negative = numerator < 0;
    const WideInteger magnitude = negative ? -numerator : numerator;
    WideInteger quotient = magnitude / denominator;
    const WideInteger remainder = magnitude % denominator;

    if (mode == RoundingMode::HalfAwayFromZero && remainder * 2 >= denominator) {
        ++quotient;
    }

    return checked_int64(negative ? -quotient : quotient);
}

}  // namespace detail

template <typename Tag, std::int64_t Scale>
class FixedDecimal final {
public:
    static constexpr int kDecimalPlaces = detail::decimal_places_for_scale(Scale);

    static_assert(
        kDecimalPlaces >= 0,
        "a fixed-point scale must be a positive power of ten"
    );

    constexpr FixedDecimal() = default;

    static constexpr FixedDecimal from_raw(std::int64_t raw) {
        return FixedDecimal(raw);
    }

    static FixedDecimal parse(std::string_view text) {
        if (text.empty()) {
            throw std::invalid_argument("fixed-point text is empty");
        }

        bool negative = false;
        std::size_t index = 0;
        if (text[index] == '+' || text[index] == '-') {
            negative = text[index] == '-';
            ++index;
        }
        if (index == text.size()) {
            throw std::invalid_argument("fixed-point text has no digits");
        }

        constexpr detail::WideInteger maximum_magnitude =
            static_cast<detail::WideInteger>(
                std::numeric_limits<std::int64_t>::max()
            ) + 1;
        constexpr detail::WideInteger maximum_whole = maximum_magnitude / Scale;

        detail::WideInteger whole = 0;
        bool saw_digit = false;
        while (index < text.size() && text[index] != '.') {
            const char character = text[index++];
            if (character < '0' || character > '9') {
                throw std::invalid_argument("fixed-point text contains an invalid character");
            }
            saw_digit = true;
            const int digit = character - '0';
            if (whole > maximum_whole / 10 ||
                (whole == maximum_whole / 10 && digit > maximum_whole % 10)) {
                throw std::overflow_error("fixed-point whole part is too large");
            }
            whole = whole * 10 + digit;
        }

        detail::WideInteger fractional = 0;
        std::size_t fractional_digits = 0;
        if (index < text.size()) {
            ++index;
            while (index < text.size()) {
                const char character = text[index++];
                if (character < '0' || character > '9') {
                    throw std::invalid_argument("fixed-point text contains an invalid character");
                }
                saw_digit = true;
                if (fractional_digits == static_cast<std::size_t>(kDecimalPlaces)) {
                    throw std::invalid_argument(
                        "fixed-point text has more fractional digits than its scale supports"
                    );
                }
                fractional = fractional * 10 + (character - '0');
                ++fractional_digits;
            }
        }

        if (!saw_digit) {
            throw std::invalid_argument("fixed-point text has no digits");
        }

        while (fractional_digits < static_cast<std::size_t>(kDecimalPlaces)) {
            fractional *= 10;
            ++fractional_digits;
        }

        const detail::WideInteger magnitude = whole * Scale + fractional;
        const detail::WideInteger allowed_magnitude = negative
            ? maximum_magnitude
            : std::numeric_limits<std::int64_t>::max();
        if (magnitude > allowed_magnitude) {
            throw std::overflow_error("fixed-point value exceeds the int64 range");
        }

        return from_raw(detail::checked_int64(negative ? -magnitude : magnitude));
    }

    [[nodiscard]] constexpr std::int64_t raw() const { return raw_; }
    [[nodiscard]] static constexpr std::int64_t scale() { return Scale; }

    [[nodiscard]] std::string to_string() const {
        const bool negative = raw_ < 0;
        const detail::WideInteger magnitude =
            negative ? -static_cast<detail::WideInteger>(raw_) : raw_;
        const auto whole = static_cast<std::uint64_t>(magnitude / Scale);
        const auto fractional = static_cast<std::uint64_t>(magnitude % Scale);

        std::string result = negative ? "-" : "";
        result += std::to_string(whole);
        if (fractional == 0) {
            return result;
        }

        std::string fractional_text =
            std::to_string(fractional + Scale).substr(1);
        while (!fractional_text.empty() && fractional_text.back() == '0') {
            fractional_text.pop_back();
        }
        return result + "." + fractional_text;
    }

    friend constexpr auto operator<=>(
        const FixedDecimal&,
        const FixedDecimal&
    ) = default;

    friend FixedDecimal operator+(FixedDecimal left, FixedDecimal right) {
        return from_raw(detail::checked_int64(
            static_cast<detail::WideInteger>(left.raw_) + right.raw_
        ));
    }

    friend FixedDecimal operator-(FixedDecimal left, FixedDecimal right) {
        return from_raw(detail::checked_int64(
            static_cast<detail::WideInteger>(left.raw_) - right.raw_
        ));
    }

private:
    explicit constexpr FixedDecimal(std::int64_t raw) : raw_(raw) {}

    std::int64_t raw_{0};
};

struct MoneyTag;
struct PriceTag;
struct QuantityTag;
struct PercentageTag;

using Money = FixedDecimal<MoneyTag, kMoneyScale>;
using Price = FixedDecimal<PriceTag, kPriceScale>;
using Quantity = FixedDecimal<QuantityTag, kQuantityScale>;
// Percentage points: 1.5 means 1.5%, not 150%.
using Percentage = FixedDecimal<PercentageTag, kPercentageScale>;

static_assert(
    Money::scale() == Quantity::scale(),
    "notional calculation assumes equal money and quantity scales"
);

inline Money calculate_notional(
    Price price,
    Quantity quantity,
    RoundingMode mode = RoundingMode::HalfAwayFromZero
) {
    return Money::from_raw(detail::rounded_divide(
        static_cast<detail::WideInteger>(price.raw()) * quantity.raw(),
        Price::scale(),
        mode
    ));
}

inline Money proportional_amount(
    Money total,
    Quantity part,
    Quantity whole,
    RoundingMode mode = RoundingMode::HalfAwayFromZero
) {
    if (part.raw() < 0 || whole.raw() <= 0 || part > whole) {
        throw std::invalid_argument("invalid fixed-point proportion");
    }
    return Money::from_raw(detail::rounded_divide(
        static_cast<detail::WideInteger>(total.raw()) * part.raw(),
        whole.raw(),
        mode
    ));
}

// `part` as a percentage of `whole`, rounded to the percentage scale:
// raw = round(part raw * 100 * percentage scale / whole raw). The ratio of
// two Money values is scale-free, so no money-scale factor appears.
inline Percentage percentage_of(
    Money part,
    Money whole,
    RoundingMode mode = RoundingMode::HalfAwayFromZero
) {
    if (whole.raw() <= 0) {
        throw std::invalid_argument("percentage base must be positive");
    }
    return Percentage::from_raw(detail::rounded_divide(
        static_cast<detail::WideInteger>(part.raw()) * 100 *
            Percentage::scale(),
        whole.raw(),
        mode
    ));
}

inline Price average_price(
    Money cost_basis,
    Quantity quantity,
    RoundingMode mode = RoundingMode::HalfAwayFromZero
) {
    if (cost_basis.raw() < 0 || quantity.raw() <= 0) {
        throw std::invalid_argument("average price requires positive quantity and cost");
    }
    return Price::from_raw(detail::rounded_divide(
        static_cast<detail::WideInteger>(cost_basis.raw()) * Price::scale(),
        quantity.raw(),
        mode
    ));
}

}  // namespace chronos
