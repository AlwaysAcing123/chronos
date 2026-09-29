#include "chronos/market_data/monotonic_clock.hpp"

namespace chronos::market_data {

MonotonicClock::time_point SteadyClock::now() const {
    return std::chrono::steady_clock::now();
}

bool SteadyClock::wait_until(
    std::unique_lock<std::mutex>& lock,
    std::condition_variable& condition,
    time_point deadline,
    const std::function<bool()>& wake
) const {
    return condition.wait_until(lock, deadline, wake);
}

}  // namespace chronos::market_data
