#pragma once

#include "chronos/current_market_view.hpp"
#include "chronos/market_data_stream_validator.hpp"
#include "chronos/replay_clock.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace chronos {

enum class MarketReplayErrorCode {
    StreamValidationFailed,
    ReceiveTimeMovedBackward,
};

struct MarketReplayError {
    MarketReplayErrorCode code;
    std::string field;
    std::string message;
    std::optional<MarketDataStreamValidationError> stream_error;
    std::optional<ReplayClockError> clock_error;

    friend bool operator==(const MarketReplayError&, const MarketReplayError&) =
        default;
};

struct MarketReplayStep {
    Timestamp replay_time;
    std::uint64_t ingest_sequence;
    InstrumentId instrument;
    MarketViewUpdate market_view_update;

    friend bool operator==(const MarketReplayStep&, const MarketReplayStep&) =
        default;
};

using MarketReplayResult = std::variant<MarketReplayStep, MarketReplayError>;

class MarketReplay final {
public:
    explicit MarketReplay(
        MarketDataStreamId stream,
        std::uint64_t initial_ingest_sequence = 0
    );

    MarketReplay(const MarketReplay&) = delete;
    MarketReplay& operator=(const MarketReplay&) = delete;
    MarketReplay(MarketReplay&&) noexcept = default;
    MarketReplay& operator=(MarketReplay&&) noexcept = default;

    [[nodiscard]] MarketReplayResult accept(
        const ValidatedMarketEvent& event
    );

    // Advances replay time without an event. A live feed calls this so that
    // silence still ages its quotes; replaying a recording never does.
    // Time may stay equal but never move backward.
    [[nodiscard]] std::optional<ReplayClockError> advance_time(
        Timestamp time
    );

    [[nodiscard]] const MarketDataStreamId& stream() const noexcept {
        return stream_validator_.stream();
    }

    [[nodiscard]] const ReplayClock& clock() const noexcept { return clock_; }

    [[nodiscard]] const CurrentMarketView& market_view() const noexcept {
        return market_view_;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    last_accepted_ingest_sequence() const noexcept {
        return stream_validator_.last_accepted_ingest_sequence();
    }

    // The stream continues at `next_ingest_sequence`; see
    // MarketDataStreamValidator::continue_at. The clock and the view are
    // unchanged.
    [[nodiscard]] bool continue_at(std::uint64_t next_ingest_sequence) noexcept {
        return stream_validator_.continue_at(next_ingest_sequence);
    }

    void reset(std::uint64_t next_expected_ingest_sequence = 0) noexcept;

private:
    MarketDataStreamValidator stream_validator_;
    ReplayClock clock_;
    CurrentMarketView market_view_;
};

}  // namespace chronos
