#include "chronos/market_data/reconnect_backoff.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <utility>

namespace chronos::market_data {

ReconnectBackoff::ReconnectBackoff(
    ReconnectBackoffPolicy policy,
    BackoffJitter jitter
)
    : policy_(policy),
      jitter_(std::move(jitter)) {
    if (policy_.initial <= MonotonicClock::duration::zero() ||
        policy_.maximum < policy_.initial) {
        throw std::invalid_argument(
            "reconnect backoff requires 0 < initial <= maximum"
        );
    }
    if (!jitter_) {
        throw std::invalid_argument("reconnect backoff requires a jitter source");
    }
}

MonotonicClock::duration ReconnectBackoff::next_delay() {
    // Doubling stops at the maximum, so no step can overflow.
    MonotonicClock::duration ceiling = policy_.initial;
    for (std::uint32_t step = 0;
         step < failures_ && ceiling < policy_.maximum;
         ++step) {
        ceiling = ceiling > policy_.maximum / 2 ? policy_.maximum : ceiling * 2;
    }
    if (failures_ < std::numeric_limits<std::uint32_t>::max()) {
        ++failures_;
    }

    const MonotonicClock::duration half = ceiling / 2;
    const MonotonicClock::duration spread = ceiling - half;
    const MonotonicClock::duration jitter = std::clamp(
        jitter_(spread),
        MonotonicClock::duration::zero(),
        spread
    );
    return half + jitter;
}

BackoffJitter random_backoff_jitter() {
    struct Generator {
        std::mutex mutex;
        std::minstd_rand engine{std::random_device{}()};
    };
    auto generator = std::make_shared<Generator>();
    return [generator](MonotonicClock::duration bound) {
        const std::lock_guard lock(generator->mutex);
        std::uniform_int_distribution<MonotonicClock::duration::rep> uniform(
            0,
            bound.count()
        );
        return MonotonicClock::duration(uniform(generator->engine));
    };
}

}  // namespace chronos::market_data
