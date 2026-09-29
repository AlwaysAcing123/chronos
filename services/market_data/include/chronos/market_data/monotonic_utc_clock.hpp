#pragma once

#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/trading_types.hpp"

namespace chronos::market_data {

// Local UTC receive time for a live feed that never moves backward.
//
// The system clock is read once, when the server starts, and every later
// reading adds the monotonic clock's elapsed time to it. A wall-clock step,
// such as an NTP correction or a manual change, therefore cannot reorder
// receive timestamps or make market time regress. The cost is that the
// readings keep the system clock's error at startup plus the monotonic
// clock's drift since; they are receipt times on this machine, not
// exchange-synchronized time.
class MonotonicUtcClock final {
public:
    MonotonicUtcClock(const MonotonicClock& clock, Timestamp utc_now);

    // Throws std::overflow_error once time leaves the signed-nanosecond
    // range, which a running server cannot reach.
    [[nodiscard]] Timestamp now() const;

private:
    const MonotonicClock& clock_;
    MonotonicClock::time_point anchor_;
    Timestamp utc_anchor_;
};

// The system clock's current UTC time in nanoseconds since the Unix epoch.
// Only the server's startup reads it, to anchor a MonotonicUtcClock.
[[nodiscard]] Timestamp system_utc_now();

}  // namespace chronos::market_data
