#pragma once

#include "chronos/trading_types.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace chronos::alpaca_adapter {

struct TimestampParseError {
    std::string message;
};

// Parses an RFC 3339 date-time, such as "2026-09-28T13:30:00.123456789Z",
// into nanoseconds since the Unix epoch.
//
// The fraction may have one to nine digits; more would need rounding, so it
// is rejected. A numeric offset ("+05:30") is applied exactly. A leap
// second, an invalid calendar date, and a time before 1970 or after the
// signed nanosecond range (2262) are rejected.
[[nodiscard]] std::variant<Timestamp, TimestampParseError> parse_rfc3339_timestamp(
    std::string_view text
);

}  // namespace chronos::alpaca_adapter
