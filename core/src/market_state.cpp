#include "chronos/market_state.hpp"

#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace chronos {

MarketState::MarketState(
    MarketDataStreamId stream,
    std::uint64_t initial_ingest_sequence
) : replay_(std::move(stream), initial_ingest_sequence) {}

MarketStateUpdateResult MarketState::apply(
    const ValidatedMarketEvent& event
) {
    const std::lock_guard lock(mutex_);
    if (interruption_) {
        return MarketStateUpdateError{
            .code = MarketStateUpdateErrorCode::StreamInterrupted,
            .message =
                "market state stopped accepting events after an earlier "
                "stream rejection",
            .replay_error = std::nullopt,
        };
    }

    MarketReplayResult result = replay_.accept(event);
    if (auto* error = std::get_if<MarketReplayError>(&result)) {
        interruption_ = *error;
        return MarketStateUpdateError{
            .code = MarketStateUpdateErrorCode::ReplayRejected,
            .message = error->message,
            .replay_error = std::move(*error),
        };
    }
    return std::get<MarketReplayStep>(std::move(result));
}

ExecutionQuoteResult MarketState::execution_quote(
    const InstrumentId& instrument
) const {
    const std::lock_guard lock(mutex_);
    if (interruption_) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::StreamInterrupted,
            .message =
                "market-data stream is interrupted: " + interruption_->message,
        };
    }

    const InstrumentMarketView* view = replay_.market_view().find(instrument);
    if (view == nullptr || !view->top_of_book) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::NoTopOfBook,
            .message = "no top of book has been received for the instrument",
        };
    }
    // An accepted top of book always advanced the replay clock.
    return ExecutionQuote{
        .top_of_book = *view->top_of_book,
        .market_time = replay_.clock().current_time().value(),
    };
}

MarketStateStatus MarketState::status() const {
    const std::lock_guard lock(mutex_);
    return MarketStateStatus{
        .stream = replay_.stream(),
        .last_ingest_sequence = replay_.last_accepted_ingest_sequence(),
        .market_time = replay_.clock().current_time(),
        .interruption = interruption_,
    };
}

}  // namespace chronos
