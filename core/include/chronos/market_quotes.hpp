#pragma once

#include "chronos/market_data_types.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace chronos {

// Why the server-owned market cannot supply an instrument's quote.
enum class MarketStateUnavailableCode {
    StreamInterrupted,
    NoTopOfBook,
    // A live source is not connected and subscribed, so no quote is current.
    SourceUnavailable,
    // A live source reconnected and has not yet delivered a quote for the
    // instrument, so the last one describes the market before the gap.
    AwaitingFreshQuote,
};

struct MarketStateUnavailable {
    MarketStateUnavailableCode code;
    std::string message;
};

// The stable name the API and deployment conditions share, such as
// "awaiting_fresh_quote".
[[nodiscard]] std::string_view market_state_unavailable_name(MarketStateUnavailableCode code) noexcept;

using InstrumentQuote = std::variant<TopOfBookEvent, MarketStateUnavailable>;

// Several instruments' quotes and the market clock, read together from one
// consistent market state, so every quote belongs to the same instant.
struct MarketQuotes {
    MarketDataStreamId stream;
    // Absent before the stream's first event.
    std::optional<Timestamp> market_time;
    std::map<InstrumentId, InstrumentQuote> quotes;
};

}  // namespace chronos
