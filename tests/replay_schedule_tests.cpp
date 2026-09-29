#include "chronos/replay_schedule.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

namespace {

using chronos::parse_replay_speed;
using chronos::ReplayDeadline;
using chronos::ReplaySchedule;
using chronos::ReplayScheduleOverflow;
using chronos::ReplaySpeed;
using chronos::replay_speed_multiple;
using chronos::replay_speed_name;
using chronos::Timestamp;
using namespace std::chrono_literals;

using time_point = ReplaySchedule::time_point;

constexpr std::int64_t kRecordingStart = 1'790'343'000'000'000'000;
const time_point kWallStart = time_point(1'000h);

Timestamp received(std::chrono::nanoseconds offset) {
    return Timestamp::from_nanoseconds(kRecordingStart + offset.count());
}

std::optional<time_point> due(ReplayDeadline deadline) {
    REQUIRE(std::holds_alternative<std::optional<time_point>>(deadline));
    return std::get<std::optional<time_point>>(deadline);
}

void test_speed_names_round_trip() {
    for (const ReplaySpeed speed : {
             ReplaySpeed::RealTime,
             ReplaySpeed::TenTimes,
             ReplaySpeed::HundredTimes,
             ReplaySpeed::Maximum,
         }) {
        REQUIRE(parse_replay_speed(replay_speed_name(speed)) == speed);
    }
    REQUIRE(replay_speed_name(ReplaySpeed::TenTimes) == "10x");
    REQUIRE(replay_speed_multiple(ReplaySpeed::HundredTimes) == 100);
    REQUIRE(!replay_speed_multiple(ReplaySpeed::Maximum).has_value());
    for (const auto* invalid : {"", "1", "2x", "10X", "1.5x", "fast", " max"}) {
        REQUIRE(!parse_replay_speed(invalid).has_value());
    }
}

void test_first_event_is_due_immediately() {
    ReplaySchedule schedule(ReplaySpeed::RealTime);
    REQUIRE(due(schedule.due_time(received(0s), kWallStart)) == kWallStart);
}

void test_real_time_deadlines_follow_receive_offsets() {
    ReplaySchedule schedule(ReplaySpeed::RealTime);
    static_cast<void>(schedule.due_time(received(0s), kWallStart));
    // Later calls use the anchor, not the time they are asked.
    REQUIRE(due(schedule.due_time(received(1s), kWallStart + 7s)) == kWallStart + 1s);
    REQUIRE(due(schedule.due_time(received(2500ms), kWallStart)) == kWallStart + 2500ms);
    REQUIRE(due(schedule.due_time(received(1ns), kWallStart)) == kWallStart + 1ns);
}

void test_faster_speeds_divide_offsets_exactly() {
    ReplaySchedule ten(ReplaySpeed::TenTimes);
    static_cast<void>(ten.due_time(received(0s), kWallStart));
    REQUIRE(due(ten.due_time(received(1s), kWallStart)) == kWallStart + 100ms);
    REQUIRE(due(ten.due_time(received(600s), kWallStart)) == kWallStart + 60s);

    ReplaySchedule hundred(ReplaySpeed::HundredTimes);
    static_cast<void>(hundred.due_time(received(0s), kWallStart));
    REQUIRE(due(hundred.due_time(received(2500ms), kWallStart)) == kWallStart + 25ms);
    REQUIRE(due(hundred.due_time(received(600s), kWallStart)) == kWallStart + 6s);
    // Sub-nanosecond remainders truncate.
    REQUIRE(due(hundred.due_time(received(199ns), kWallStart)) == kWallStart + 1ns);
}

void test_maximum_speed_never_waits() {
    ReplaySchedule schedule(ReplaySpeed::Maximum);
    REQUIRE(!due(schedule.due_time(received(0s), kWallStart)).has_value());
    REQUIRE(!due(schedule.due_time(received(3600s), kWallStart)).has_value());
}

void test_equal_receive_times_share_a_deadline() {
    ReplaySchedule schedule(ReplaySpeed::RealTime);
    static_cast<void>(schedule.due_time(received(0s), kWallStart));
    const auto first = due(schedule.due_time(received(3s), kWallStart));
    const auto second = due(schedule.due_time(received(3s), kWallStart));
    REQUIRE(first == second);
}

void test_earlier_receive_time_is_due_at_once() {
    ReplaySchedule schedule(ReplaySpeed::RealTime);
    static_cast<void>(schedule.due_time(received(5s), kWallStart));
    // Before the anchor: the deadline is in the past, so it is delivered and
    // the market state rejects the regression.
    REQUIRE(due(schedule.due_time(received(1s), kWallStart)) == kWallStart - 4s);
}

void test_postponing_shifts_later_deadlines() {
    ReplaySchedule schedule(ReplaySpeed::TenTimes);
    REQUIRE(schedule.postpone(90s));  // no anchor yet: nothing to shift
    static_cast<void>(schedule.due_time(received(0s), kWallStart));
    REQUIRE(schedule.postpone(100s));
    REQUIRE(due(schedule.due_time(received(1s), kWallStart)) == kWallStart + 100s + 100ms);
}

void test_overflow_is_reported_not_wrapped() {
    ReplaySchedule schedule(ReplaySpeed::RealTime);
    const time_point near_end = time_point::max() - 1s;
    static_cast<void>(schedule.due_time(received(0s), near_end));
    REQUIRE(due(schedule.due_time(received(1s), near_end)) == time_point::max() - 0s);
    REQUIRE(std::holds_alternative<ReplayScheduleOverflow>(
        schedule.due_time(received(1s + 1ns), near_end)
    ));
    REQUIRE(!schedule.postpone(2s));
    // A failed postponement leaves the schedule unchanged.
    REQUIRE(due(schedule.due_time(received(500ms), near_end)) == near_end + 500ms);

    // Receive offsets span the full timestamp range without overflowing.
    ReplaySchedule extreme(ReplaySpeed::HundredTimes);
    static_cast<void>(extreme.due_time(
        Timestamp::from_nanoseconds(std::numeric_limits<std::int64_t>::min()),
        time_point(0s)
    ));
    REQUIRE(!std::holds_alternative<ReplayScheduleOverflow>(extreme.due_time(
        Timestamp::from_nanoseconds(std::numeric_limits<std::int64_t>::max()),
        time_point(0s)
    )));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"speed names round trip", test_speed_names_round_trip},
        {"first event is due immediately", test_first_event_is_due_immediately},
        {"real-time deadlines follow receive offsets", test_real_time_deadlines_follow_receive_offsets},
        {"faster speeds divide offsets exactly", test_faster_speeds_divide_offsets_exactly},
        {"maximum speed never waits", test_maximum_speed_never_waits},
        {"equal receive times share a deadline", test_equal_receive_times_share_a_deadline},
        {"earlier receive time is due at once", test_earlier_receive_time_is_due_at_once},
        {"postponing shifts later deadlines", test_postponing_shifts_later_deadlines},
        {"overflow is reported, not wrapped", test_overflow_is_reported_not_wrapped},
    });
}
