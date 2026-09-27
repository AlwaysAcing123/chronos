#include "chronos/replay_clock.hpp"

#include <optional>
#include <string>

namespace chronos {

std::optional<ReplayClockError> ReplayClock::check_advance(
    Timestamp requested_time
) const {
    if (requested_time.nanoseconds() < 0) {
        return ReplayClockError{
            .code = ReplayClockErrorCode::NegativeTimestamp,
            .message = "replay time must not be negative",
            .current_time = current_time_,
            .requested_time = requested_time,
        };
    }
    if (current_time_ && requested_time < *current_time_) {
        return ReplayClockError{
            .code = ReplayClockErrorCode::TimeMovedBackward,
            .message = "replay time must not move backward",
            .current_time = current_time_,
            .requested_time = requested_time,
        };
    }
    return std::nullopt;
}

ReplayClockAdvanceResult ReplayClock::advance_to(Timestamp requested_time) {
    if (auto error = check_advance(requested_time)) {
        return *error;
    }
    const bool time_changed = !current_time_ || *current_time_ != requested_time;
    current_time_ = requested_time;
    return ReplayClockAdvance{
        .current_time = requested_time,
        .time_changed = time_changed,
    };
}

void ReplayClock::reset() noexcept { current_time_.reset(); }

}  // namespace chronos
