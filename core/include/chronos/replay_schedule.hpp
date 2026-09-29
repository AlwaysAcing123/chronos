#pragma once

#include "chronos/trading_types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace chronos {

// Playback rate for a recorded stream. Rates are whole multiples of real
// time, so scheduling is exact integer arithmetic.
enum class ReplaySpeed {
    RealTime,
    TenTimes,
    HundredTimes,
    // As fast as downstream validation allows, without waiting.
    Maximum,
};

// The playback multiple, or nullopt for Maximum.
[[nodiscard]] std::optional<std::int64_t> replay_speed_multiple(
    ReplaySpeed speed
) noexcept;

// "1x", "10x", "100x", or "max".
[[nodiscard]] std::string_view replay_speed_name(ReplaySpeed speed) noexcept;

[[nodiscard]] std::optional<ReplaySpeed> parse_replay_speed(
    std::string_view name
) noexcept;

struct ReplayScheduleOverflow {};

// Deliver at this steady-clock time, or at once when absent.
using ReplayDeadline = std::variant<
    std::optional<std::chrono::steady_clock::time_point>,
    ReplayScheduleOverflow
>;

// Maps recorded receive times to absolute steady-clock deadlines.
//
// The first event anchors the schedule: its receive time plays at the wall
// time it is first scheduled. Every later event is due at
//
//     anchor_wall + (receive_time - anchor_receive) / multiple
//
// Deadlines are absolute rather than relative to the previous delivery, so
// processing time and timer latency never accumulate into drift, and an
// event that is already late is simply due now. Time spent paused is added
// to the anchor with postpone(). The schedule reads no clock itself.
class ReplaySchedule final {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::steady_clock::duration;

    explicit ReplaySchedule(ReplaySpeed speed) noexcept : speed_(speed) {}

    [[nodiscard]] ReplaySpeed speed() const noexcept { return speed_; }

    // When the event received at `receive_time` is due, anchoring the
    // schedule on the first call. Overflow of any intermediate value is
    // reported rather than wrapped.
    [[nodiscard]] ReplayDeadline due_time(
        Timestamp receive_time,
        time_point now
    );

    // Moves every later deadline back by `paused`. Returns false, leaving
    // the schedule unchanged, when the anchor would overflow.
    [[nodiscard]] bool postpone(duration paused) noexcept;

private:
    struct Anchor {
        Timestamp receive_time;
        time_point wall_time;
    };

    ReplaySpeed speed_;
    std::optional<Anchor> anchor_;
};

}  // namespace chronos
