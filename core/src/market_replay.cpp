#include "chronos/market_replay.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos {
namespace {

MarketReplayError stream_replay_error(
    MarketDataStreamValidationError error
) {
    return MarketReplayError{
        .code = MarketReplayErrorCode::StreamValidationFailed,
        .field = error.field,
        .message = "market replay rejected stream event: " + error.message,
        .stream_error = std::move(error),
        .clock_error = std::nullopt,
    };
}

MarketReplayError clock_replay_error(ReplayClockError error) {
    return MarketReplayError{
        .code = MarketReplayErrorCode::ReceiveTimeMovedBackward,
        .field = "header.receive_timestamp",
        .message = "market replay rejected receive time: " + error.message,
        .stream_error = std::nullopt,
        .clock_error = std::move(error),
    };
}

}  // namespace

MarketReplay::MarketReplay(
    MarketDataStreamId stream,
    std::uint64_t initial_ingest_sequence
) : stream_validator_(std::move(stream), initial_ingest_sequence) {}

MarketReplayResult MarketReplay::accept(
    const ValidatedMarketEvent& event
) {
    const auto& header = market_event_header(event.value());

    if (auto error = clock_.check_advance(header.receive_timestamp)) {
        return clock_replay_error(std::move(*error));
    }

    auto stream_result = stream_validator_.accept(event);
    if (std::holds_alternative<MarketDataStreamValidationError>(
            stream_result
        )) {
        return stream_replay_error(
            std::get<MarketDataStreamValidationError>(
                std::move(stream_result)
            )
        );
    }

    const auto clock_result = clock_.advance_to(header.receive_timestamp);
    const auto& advance = std::get<ReplayClockAdvance>(clock_result);
    const auto update = market_view_.apply(event);
    return MarketReplayStep{
        .replay_time = advance.current_time,
        .ingest_sequence = header.ingest_sequence,
        .instrument = header.instrument,
        .market_view_update = update,
    };
}

void MarketReplay::reset(
    std::uint64_t next_expected_ingest_sequence
) noexcept {
    stream_validator_.reset(next_expected_ingest_sequence);
    clock_.reset();
    market_view_.reset();
}

}  // namespace chronos
