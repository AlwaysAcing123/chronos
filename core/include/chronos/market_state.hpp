#pragma once

#include "chronos/market_quotes.hpp"
#include "chronos/market_replay.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace chronos {

// A top of book that an order may execute against, read together with the
// market time from one consistent snapshot.
struct ExecutionQuote {
    TopOfBookEvent top_of_book;
    Timestamp market_time;
};

using ExecutionQuoteResult = std::variant<
    ExecutionQuote,
    MarketStateUnavailable
>;

enum class MarketStateUpdateErrorCode {
    ReplayRejected,
    StreamInterrupted,
    TimeMovedBackward,
};

struct MarketStateUpdateError {
    MarketStateUpdateErrorCode code;
    std::string message;
    std::optional<MarketReplayError> replay_error;
};

using MarketStateUpdateResult = std::variant<
    MarketReplayStep,
    MarketStateUpdateError
>;

struct MarketStateStatus {
    MarketDataStreamId stream;
    std::optional<std::uint64_t> last_ingest_sequence;
    std::optional<Timestamp> market_time;
    std::optional<MarketReplayError> interruption;
};

// The server-owned current market for one validated stream. A feed or replay
// driver applies events; order submission reads execution quotes. Clients
// never supply the quote they trade against.
//
// Market time is the receive timestamp of the latest accepted event, the
// replay clock. Orders are timestamped with it, so quote age is measured in
// the stream's own time and a replay is reproducible regardless of wall-clock
// time. A quote ages only while later events arrive, so a live feed also
// advances time between events (advance_time) from its local receive clock;
// a replay never does.
//
// A source can also become unavailable without corrupting an accepted event:
// a live feed can disconnect, or a replay reader can fail after a valid
// prefix. It suspends the state, making every quote unavailable. A resumable
// source starts a new freshness epoch when it returns: each instrument stays
// unavailable until a new top of book for it is accepted, so a quote from
// before the gap is never served as current. A successfully completed replay
// remains available at its final market state.
//
// Every rejected event interrupts the state permanently: a gap, duplicate,
// stream mismatch, or receive-time regression means the view may no longer
// describe the market, so no further quote is served for execution. Quality
// flags are preserved unchanged; the risk engine decides whether a suspect
// book is tradeable.
//
// All member functions are safe to call concurrently.
class MarketState final {
public:
    explicit MarketState(
        MarketDataStreamId stream,
        std::uint64_t initial_ingest_sequence = 0
    );

    MarketState(const MarketState&) = delete;
    MarketState& operator=(const MarketState&) = delete;
    MarketState(MarketState&&) = delete;
    MarketState& operator=(MarketState&&) = delete;

    [[nodiscard]] MarketStateUpdateResult apply(
        const ValidatedMarketEvent& event
    );

    [[nodiscard]] ExecutionQuoteResult execution_quote(
        const InstrumentId& instrument
    ) const;

    [[nodiscard]] MarketStateStatus status() const;

    // The execution quote of each instrument and the market clock, all read
    // under one lock, so valuing several positions uses one instant.
    [[nodiscard]] MarketQuotes quotes(
        const std::vector<InstrumentId>& instruments
    ) const;

    // Advances market time without an event; it may stay equal but never
    // move backward. Only a live source calls this.
    [[nodiscard]] std::optional<MarketStateUpdateError> advance_time(
        Timestamp time
    );

    // Makes every quote unavailable until resume(). Idempotent.
    void suspend();

    // Ends a suspension and starts a new freshness epoch: each instrument's
    // quote stays unavailable until a top of book for it is accepted.
    void resume();

private:
    [[nodiscard]] std::variant<TopOfBookEvent, MarketStateUnavailable>
    quote_locked(const InstrumentId& instrument) const;

    mutable std::mutex mutex_;
    MarketReplay replay_;
    std::optional<MarketReplayError> interruption_;
    bool suspended_{false};
    // Instruments with a top of book accepted since the latest resume().
    // Absent until the first resume(), so a state that is never suspended
    // requires no fresh quote.
    std::optional<std::set<std::string, std::less<>>> fresh_since_resume_;
};

}  // namespace chronos
