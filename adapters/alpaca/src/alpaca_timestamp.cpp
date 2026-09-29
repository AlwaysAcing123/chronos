#include "chronos/alpaca/alpaca_timestamp.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::alpaca_adapter {
namespace {

__extension__ using WideInteger = __int128;

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::size_t kMaximumFractionDigits = 9;

// Reads exactly `count` ASCII digits at `index`.
std::optional<int> digits(std::string_view text, std::size_t index, std::size_t count) {
    if (index + count > text.size()) {
        return std::nullopt;
    }
    int value = 0;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const char character = text[index + offset];
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        value = value * 10 + (character - '0');
    }
    return value;
}

bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && is_leap_year(year) ? 29 : kDays[month - 1];
}

// Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
// days_from_civil).
std::int64_t days_from_civil(int year, int month, int day) {
    const int adjusted_year = month <= 2 ? year - 1 : year;
    const int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    const int year_of_era = adjusted_year - era * 400;
    const int shifted_month = month > 2 ? month - 3 : month + 9;
    const int day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
    const int day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146'097 + day_of_era - 719'468;
}

TimestampParseError invalid(std::string_view reason) {
    return TimestampParseError{
        .message = "timestamp is not an RFC 3339 date-time: " + std::string(reason),
    };
}

bool is_separator(char character, char upper) {
    return character == upper || character == upper + ('a' - 'A');
}

}  // namespace

std::variant<Timestamp, TimestampParseError> parse_rfc3339_timestamp(
    std::string_view text
) {
    // YYYY-MM-DDTHH:MM:SS
    const auto year = digits(text, 0, 4);
    const auto month = digits(text, 5, 2);
    const auto day = digits(text, 8, 2);
    const auto hour = digits(text, 11, 2);
    const auto minute = digits(text, 14, 2);
    const auto second = digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second ||
        text[4] != '-' || text[7] != '-' || !is_separator(text[10], 'T') ||
        text[13] != ':' || text[16] != ':') {
        return invalid("expected YYYY-MM-DDTHH:MM:SS");
    }
    if (*month < 1 || *month > 12 || *day < 1 ||
        *day > days_in_month(*year, *month)) {
        return invalid("the calendar date does not exist");
    }
    if (*hour > 23 || *minute > 59) {
        return invalid("the time of day does not exist");
    }
    if (*second == 60) {
        return invalid("leap seconds cannot be represented");
    }
    if (*second > 59) {
        return invalid("the time of day does not exist");
    }

    std::size_t index = 19;
    std::int64_t fraction = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        std::size_t fraction_digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (fraction_digits == kMaximumFractionDigits) {
                return invalid("more than nine fractional-second digits");
            }
            fraction = fraction * 10 + (text[index] - '0');
            ++fraction_digits;
            ++index;
        }
        if (fraction_digits == 0) {
            return invalid("a decimal point needs fractional digits");
        }
        for (std::size_t digit = fraction_digits; digit < kMaximumFractionDigits; ++digit) {
            fraction *= 10;
        }
    }

    std::int64_t offset_seconds = 0;
    if (index < text.size() && is_separator(text[index], 'Z')) {
        ++index;
    } else if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        const bool behind = text[index] == '-';
        const auto offset_hour = digits(text, index + 1, 2);
        const auto offset_minute = digits(text, index + 4, 2);
        if (!offset_hour || !offset_minute || text[index + 3] != ':' ||
            *offset_hour > 23 || *offset_minute > 59) {
            return invalid("the UTC offset must be Z or +HH:MM");
        }
        offset_seconds = (*offset_hour * 3600 + *offset_minute * 60) * (behind ? -1 : 1);
        index += 6;
    } else {
        return invalid("a UTC offset (Z or +HH:MM) is required");
    }
    if (index != text.size()) {
        return invalid("unexpected text after the UTC offset");
    }

    // Local time minus its offset is UTC.
    const WideInteger seconds =
        static_cast<WideInteger>(days_from_civil(*year, *month, *day)) * 86'400 +
        *hour * 3600 + *minute * 60 + *second - offset_seconds;
    const WideInteger nanoseconds = seconds * kNanosecondsPerSecond + fraction;
    if (nanoseconds < 0) {
        return TimestampParseError{.message = "timestamp is before the Unix epoch"};
    }
    if (nanoseconds > std::numeric_limits<std::int64_t>::max()) {
        return TimestampParseError{
            .message = "timestamp is beyond the signed nanosecond range",
        };
    }
    return Timestamp::from_nanoseconds(static_cast<std::int64_t>(nanoseconds));
}

}  // namespace chronos::alpaca_adapter
