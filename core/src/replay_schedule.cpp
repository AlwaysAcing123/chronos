#include "chronos/replay_schedule.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <string_view>
#include <type_traits>
#include <utility>

namespace chronos {
namespace {

// Deadlines are computed in the steady clock's own nanosecond ticks.
static_assert(
    std::is_same_v<
        std::chrono::steady_clock::period,
        std::nano
    >,
    "replay scheduling assumes a nanosecond steady clock"
);

__extension__ using WideInteger = __int128;

std::optional<std::int64_t> narrow(WideInteger value) noexcept {
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(value);
}

constexpr std::array<std::pair<ReplaySpeed, std::string_view>, 4>
    kSpeedNames{{
        {ReplaySpeed::RealTime, "1x"},
        {ReplaySpeed::TenTimes, "10x"},
        {ReplaySpeed::HundredTimes, "100x"},
        {ReplaySpeed::Maximum, "max"},
    }};

}  // namespace

std::optional<std::int64_t> replay_speed_multiple(
    ReplaySpeed speed
) noexcept {
    switch (speed) {
        case ReplaySpeed::RealTime:
            return 1;
        case ReplaySpeed::TenTimes:
            return 10;
        case ReplaySpeed::HundredTimes:
            return 100;
        case ReplaySpeed::Maximum:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string_view replay_speed_name(ReplaySpeed speed) noexcept {
    for (const auto& [candidate, name] : kSpeedNames) {
        if (candidate == speed) {
            return name;
        }
    }
    return "unknown";
}

std::optional<ReplaySpeed> parse_replay_speed(std::string_view name) noexcept {
    for (const auto& [speed, candidate] : kSpeedNames) {
        if (candidate == name) {
            return speed;
        }
    }
    return std::nullopt;
}

ReplayDeadline ReplaySchedule::due_time(
    Timestamp receive_time,
    time_point now
) {
    const std::optional<std::int64_t> multiple = replay_speed_multiple(speed_);
    if (!multiple) {
        return std::optional<time_point>();
    }
    if (!anchor_) {
        anchor_ = Anchor{.receive_time = receive_time, .wall_time = now};
        return std::optional<time_point>(now);
    }
    // An earlier receive time than the anchor is due at once; the market
    // state then rejects the regression.
    const WideInteger recorded_offset =
        static_cast<WideInteger>(receive_time.nanoseconds()) -
        anchor_->receive_time.nanoseconds();
    const std::optional<std::int64_t> deadline = narrow(
        static_cast<WideInteger>(anchor_->wall_time.time_since_epoch().count()) +
        recorded_offset / *multiple
    );
    if (!deadline) {
        return ReplayScheduleOverflow{};
    }
    return std::optional<time_point>(time_point(duration(*deadline)));
}

bool ReplaySchedule::postpone(duration paused) noexcept {
    if (!anchor_) {
        return true;
    }
    const std::optional<std::int64_t> wall = narrow(
        static_cast<WideInteger>(anchor_->wall_time.time_since_epoch().count()) +
        paused.count()
    );
    if (!wall) {
        return false;
    }
    anchor_->wall_time = time_point(duration(*wall));
    return true;
}

}  // namespace chronos
