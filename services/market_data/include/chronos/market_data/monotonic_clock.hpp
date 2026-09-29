#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace chronos::market_data {

// The wall-clock time base for pacing and throttling. It is injected so
// tests control time exactly instead of sleeping.
class MonotonicClock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::steady_clock::duration;

    virtual ~MonotonicClock() = default;

    [[nodiscard]] virtual time_point now() const = 0;

    // Waits on `condition` until `wake()` is true or `deadline` has passed,
    // and returns wake(). `lock` holds the mutex that guards what `wake`
    // reads, and `condition` is notified whenever that state changes, so a
    // pause or stop interrupts the wait at once.
    virtual bool wait_until(
        std::unique_lock<std::mutex>& lock,
        std::condition_variable& condition,
        time_point deadline,
        const std::function<bool()>& wake
    ) const = 0;
};

class SteadyClock final : public MonotonicClock {
public:
    [[nodiscard]] time_point now() const override;

    bool wait_until(
        std::unique_lock<std::mutex>& lock,
        std::condition_variable& condition,
        time_point deadline,
        const std::function<bool()>& wake
    ) const override;
};

}  // namespace chronos::market_data
