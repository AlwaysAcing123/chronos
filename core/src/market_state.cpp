#include "chronos/market_state.hpp"

#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace chronos {

std::string_view market_state_unavailable_name(MarketStateUnavailableCode code) noexcept {
    switch (code) {
        case MarketStateUnavailableCode::StreamInterrupted: return "stream_interrupted";
        case MarketStateUnavailableCode::NoTopOfBook: return "no_top_of_book";
        case MarketStateUnavailableCode::SourceUnavailable: return "source_unavailable";
        case MarketStateUnavailableCode::AwaitingFreshQuote: return "awaiting_fresh_quote";
    }
    return "unknown";
}

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
    auto step = std::get<MarketReplayStep>(std::move(result));
    if (!first_ingest_sequence_) {
        first_ingest_sequence_ = replay_.last_accepted_ingest_sequence();
    }
    if (fresh_since_resume_ && !suspended_ &&
        step.market_view_update == MarketViewUpdate::TopOfBook) {
        fresh_since_resume_->insert(step.instrument.value());
    }
    return step;
}

std::optional<MarketStateUpdateError> MarketState::advance_time(
    Timestamp time
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
    if (auto error = replay_.advance_time(time)) {
        return MarketStateUpdateError{
            .code = MarketStateUpdateErrorCode::TimeMovedBackward,
            .message = error->message,
            .replay_error = std::nullopt,
        };
    }
    return std::nullopt;
}

void MarketState::set_quote_authority(const MarketQuoteAuthority* authority) {
    const std::lock_guard lock(mutex_);
    authority_ = authority;
}

void MarketState::suspend() {
    const std::lock_guard lock(mutex_);
    suspended_ = true;
}

void MarketState::resume() {
    const std::lock_guard lock(mutex_);
    suspended_ = false;
    fresh_since_resume_.emplace();
}

std::optional<MarketStateUpdateError> MarketState::continue_at(
    std::uint64_t next_ingest_sequence
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
    if (!suspended_) {
        return MarketStateUpdateError{
            .code = MarketStateUpdateErrorCode::ContinuationRefused,
            .message =
                "a stream can continue at a later sequence only while its "
                "market state is suspended",
            .replay_error = std::nullopt,
        };
    }
    if (!replay_.continue_at(next_ingest_sequence)) {
        return MarketStateUpdateError{
            .code = MarketStateUpdateErrorCode::ContinuationRefused,
            .message =
                "ingest sequence " + std::to_string(next_ingest_sequence) +
                " is behind the stream's next expected sequence",
            .replay_error = std::nullopt,
        };
    }
    return std::nullopt;
}

std::optional<std::string> MarketState::source_refusal_locked() const {
    return authority_ == nullptr ? std::nullopt : authority_->quote_refusal();
}

std::variant<TopOfBookEvent, MarketStateUnavailable>
MarketState::quote_locked(
    const InstrumentId& instrument,
    const std::optional<std::string>& source_refusal
) const {
    if (interruption_) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::StreamInterrupted,
            .message =
                "market-data stream is interrupted: " + interruption_->message,
        };
    }

    if (suspended_) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::SourceUnavailable,
            .message =
                "the live market-data source is not connected and "
                "subscribed, so no quote is current",
        };
    }

    if (source_refusal) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::SourceUnavailable,
            .message =
                "the live market-data source no longer vouches for its "
                "quotes: " + *source_refusal,
        };
    }

    const InstrumentMarketView* view = replay_.market_view().find(instrument);
    if (view == nullptr || !view->top_of_book) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::NoTopOfBook,
            .message = "no top of book has been received for the instrument",
        };
    }
    if (fresh_since_resume_ &&
        !fresh_since_resume_->contains(instrument.value())) {
        return MarketStateUnavailable{
            .code = MarketStateUnavailableCode::AwaitingFreshQuote,
            .message =
                "no quote for the instrument has arrived since the live "
                "source reconnected",
        };
    }
    return *view->top_of_book;
}

ExecutionQuoteResult MarketState::execution_quote(
    const InstrumentId& instrument
) const {
    const std::lock_guard lock(mutex_);
    auto quote = quote_locked(instrument, source_refusal_locked());
    if (auto* unavailable = std::get_if<MarketStateUnavailable>(&quote)) {
        return std::move(*unavailable);
    }
    // An accepted top of book always advanced the replay clock.
    return ExecutionQuote{
        .top_of_book = std::get<TopOfBookEvent>(std::move(quote)),
        .market_time = replay_.clock().current_time().value(),
    };
}

MarketQuotes MarketState::quotes(
    const std::vector<InstrumentId>& instruments
) const {
    const std::lock_guard lock(mutex_);
    MarketQuotes read{
        .stream = replay_.stream(),
        .market_time = replay_.clock().current_time(),
        .quotes = {},
    };
    // Asked once: every quote of one read shares the same instant.
    const std::optional<std::string> source_refusal = source_refusal_locked();
    for (const InstrumentId& instrument : instruments) {
        read.quotes.insert_or_assign(instrument, quote_locked(instrument, source_refusal));
    }
    return read;
}

MarketStateStatus MarketState::status() const {
    const std::lock_guard lock(mutex_);
    return MarketStateStatus{
        .stream = replay_.stream(),
        .last_ingest_sequence = replay_.last_accepted_ingest_sequence(),
        .market_time = replay_.clock().current_time(),
        .interruption = interruption_,
        .first_ingest_sequence = first_ingest_sequence_,
    };
}

}  // namespace chronos
