#pragma once

#include "chronos/market_data/monotonic_clock.hpp"

#include <cstdint>
#include <functional>

namespace chronos::market_data {

struct ReconnectBackoffPolicy {
    MonotonicClock::duration initial{std::chrono::seconds(1)};
    MonotonicClock::duration maximum{std::chrono::seconds(30)};
};

// Returns a duration from zero to `bound` inclusive. Injected so tests are
// deterministic; production uses random_backoff_jitter().
using BackoffJitter = std::function<
    MonotonicClock::duration(MonotonicClock::duration bound)
>;

// Bounded exponential reconnect delays with "equal jitter".
//
// Attempt n (from zero) has a ceiling of min(maximum, initial * 2^n); the
// delay is half the ceiling plus a jitter of up to the other half. Delays
// therefore never exceed the maximum, never fall below half the initial
// delay, and spread reconnecting clients apart. A jitter outside its bound
// is clamped, so a faulty source cannot produce an unbounded wait.
class ReconnectBackoff final {
public:
    // Throws std::invalid_argument unless 0 < initial <= maximum and jitter
    // is callable.
    ReconnectBackoff(ReconnectBackoffPolicy policy, BackoffJitter jitter);

    // The delay before the next attempt; each call counts one failure.
    [[nodiscard]] MonotonicClock::duration next_delay();

    // Forgets earlier failures, after a connection succeeded.
    void reset() noexcept { failures_ = 0; }

    [[nodiscard]] std::uint32_t failures() const noexcept { return failures_; }

private:
    ReconnectBackoffPolicy policy_;
    BackoffJitter jitter_;
    std::uint32_t failures_{0};
};

// Uniform jitter from a pseudo-random generator seeded by the system. The
// delay needs no unpredictability, only spread.
[[nodiscard]] BackoffJitter random_backoff_jitter();

}  // namespace chronos::market_data
