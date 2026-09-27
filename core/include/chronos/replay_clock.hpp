#pragma once

#include "chronos/trading_types.hpp"

#include <optional>
#include <string>
#include <variant>

namespace chronos {

enum class ReplayClockErrorCode {
    NegativeTimestamp,
    TimeMovedBackward,
};

struct ReplayClockError {
    ReplayClockErrorCode code;
    std::string message;
    std::optional<Timestamp> current_time;
    Timestamp requested_time;

    friend bool operator==(const ReplayClockError&, const ReplayClockError&) =
        default;
};

struct ReplayClockAdvance {
    Timestamp current_time;
    bool time_changed;

    friend bool operator==(
        const ReplayClockAdvance&,
        const ReplayClockAdvance&
    ) = default;
};

using ReplayClockAdvanceResult = std::variant<
    ReplayClockAdvance,
    ReplayClockError
>;

class ReplayClock final {
public:
    [[nodiscard]] const std::optional<Timestamp>& current_time()
        const noexcept {
        return current_time_;
    }

    [[nodiscard]] std::optional<ReplayClockError> check_advance(
        Timestamp requested_time
    ) const;

    [[nodiscard]] ReplayClockAdvanceResult advance_to(
        Timestamp requested_time
    );

    void reset() noexcept;

    friend bool operator==(const ReplayClock&, const ReplayClock&) = default;

private:
    std::optional<Timestamp> current_time_;
};

}  // namespace chronos
