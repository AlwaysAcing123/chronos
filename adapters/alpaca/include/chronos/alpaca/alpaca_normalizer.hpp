#pragma once

#include "chronos/alpaca/alpaca_protocol.hpp"
#include "chronos/alpaca/alpaca_subscription.hpp"
#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {

// Alpaca's exchange code for the Investors Exchange, and the Chronos venue
// its events carry.
inline constexpr std::string_view kIexExchangeCode = "V";
inline constexpr std::string_view kIexVenue = "IEX";

using AlpacaDataMessage = std::variant<AlpacaQuote, AlpacaTrade>;

struct AlpacaNormalizationError {
    // A safe explanation naming the symbol and field.
    std::string message;
    std::vector<MarketEventValidationError> validation_errors;
};

using AlpacaNormalizationResult = std::variant<
    std::vector<ValidatedMarketEvent>,
    AlpacaNormalizationError
>;

// Maps Alpaca IEX quotes and trades to validated Chronos events.
//
//   quote -> TopOfBookEvent      trade -> TradeEvent (aggressor unknown)
//   "S"   -> instrument, which must be subscribed
//   "t"   -> event_timestamp (RFC 3339, nanoseconds)
//   local receipt time, supplied by the caller -> receive_timestamp
//   adapter-assigned contiguous sequence -> ingest_sequence
//   venue "IEX", venue_sequence null: Alpaca publishes no venue sequence,
//   so gaps cannot be detected
//   quality: SourceSpecific always, marking single-venue IEX data; plus
//   PotentiallyInvalidBook for a crossed quote and
//   QuestionableReceiveTimestamp when receipt precedes the event time
//
// Prices and sizes are parsed from their exact JSON text. Exponent
// notation, more decimal places than a Chronos price holds (nine),
// fractional or negative sizes, and values beyond the fixed-point range are
// rejected, never rounded. Raw IEX sizes are treated as shares under the
// conservative provider assumption documented in docs/alpaca-adapter.md.
// A side with price and size zero is empty; any other zero or negative value
// is inconsistent. Every non-empty side and trade must carry the IEX
// exchange code.
class AlpacaNormalizer final {
public:
    AlpacaNormalizer(
        MarketDataStreamId stream,
        AlpacaSubscription subscription,
        std::uint64_t initial_ingest_sequence = 0
    );

    // Normalizes one frame's data messages in order, all received at
    // `receive_timestamp`. Either every message becomes an event and the
    // ingest sequence advances by their number, or none does and the
    // sequence is unchanged.
    [[nodiscard]] AlpacaNormalizationResult normalize(
        const std::vector<AlpacaDataMessage>& messages,
        Timestamp receive_timestamp
    );

    // The sequence of the next event, or nullopt once exhausted.
    [[nodiscard]] std::optional<std::uint64_t> next_ingest_sequence() const noexcept {
        return next_ingest_sequence_;
    }

    [[nodiscard]] const AlpacaSubscription& subscription() const noexcept {
        return subscription_;
    }

private:
    MarketDataStreamId stream_;
    AlpacaSubscription subscription_;
    std::optional<std::uint64_t> next_ingest_sequence_;
};

}  // namespace chronos::alpaca_adapter
