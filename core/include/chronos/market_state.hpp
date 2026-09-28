#pragma once

#include "chronos/market_replay.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace chronos {

// A top of book that an order may execute against, read together with the
// market time from one consistent snapshot.
struct ExecutionQuote {
    TopOfBookEvent top_of_book;
    Timestamp market_time;
};

enum class MarketStateUnavailableCode {
    StreamInterrupted,
    NoTopOfBook,
};

struct MarketStateUnavailable {
    MarketStateUnavailableCode code;
    std::string message;
};

using ExecutionQuoteResult = std::variant<
    ExecutionQuote,
    MarketStateUnavailable
>;

enum class MarketStateUpdateErrorCode {
    ReplayRejected,
    StreamInterrupted,
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
// time. A quote ages only while later events arrive; a future live feed must
// therefore also advance time with heartbeats.
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

private:
    mutable std::mutex mutex_;
    MarketReplay replay_;
    std::optional<MarketReplayError> interruption_;
};

}  // namespace chronos
