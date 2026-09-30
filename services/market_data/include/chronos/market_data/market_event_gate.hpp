#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace chronos::market_data {

// Orders the application of market events against work that must not
// interleave with one: while anyone holds the gate, no event is applied
// or observed.
//
// The feed takes it exclusively around applying an event and offering it
// to its observer, the working-order matcher. A paper-order submission
// holds it shared from reading its quote until it commits. So every event
// is offered to exactly the working orders committed before it was
// applied, and an order accepted on one observation is offered every later
// one. Holders must take the gate before any account lock, as both the
// feed and submissions do, so the gate and row locks never wait on each
// other in a cycle.
class MarketEventGate final {
public:
    using Mutex = std::shared_timed_mutex;

    MarketEventGate() = default;
    MarketEventGate(const MarketEventGate&) = delete;
    MarketEventGate& operator=(const MarketEventGate&) = delete;

    [[nodiscard]] std::unique_lock<Mutex> apply_event() {
        return std::unique_lock(mutex_);
    }

    // Nullopt when an event's application and matching did not finish by
    // `deadline`.
    [[nodiscard]] std::optional<std::shared_lock<Mutex>> hold_market(
        std::chrono::steady_clock::time_point deadline
    ) {
        std::shared_lock lock(mutex_, deadline);
        if (!lock.owns_lock()) {
            return std::nullopt;
        }
        return lock;
    }

private:
    Mutex mutex_;
};

}  // namespace chronos::market_data
