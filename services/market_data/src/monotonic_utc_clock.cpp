#include "chronos/market_data/monotonic_utc_clock.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <stdexcept>
#include <type_traits>

namespace chronos::market_data {
namespace {

__extension__ using WideInteger = __int128;

static_assert(
    std::is_same_v<MonotonicClock::duration::period, std::nano>,
    "receive time assumes a nanosecond steady clock"
);

std::int64_t checked_nanoseconds(WideInteger value) {
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max()) {
        throw std::overflow_error(
            "receive time exceeds the signed nanosecond range"
        );
    }
    return static_cast<std::int64_t>(value);
}

}  // namespace

MonotonicUtcClock::MonotonicUtcClock(
    const MonotonicClock& clock,
    Timestamp utc_now
)
    : clock_(clock),
      anchor_(clock.now()),
      utc_anchor_(utc_now) {}

Timestamp MonotonicUtcClock::now() const {
    const WideInteger elapsed = (clock_.now() - anchor_).count();
    return Timestamp::from_nanoseconds(checked_nanoseconds(
        static_cast<WideInteger>(utc_anchor_.nanoseconds()) + elapsed
    ));
}

Timestamp system_utc_now() {
    const auto since_epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    );
    return Timestamp::from_nanoseconds(checked_nanoseconds(since_epoch.count()));
}

}  // namespace chronos::market_data
