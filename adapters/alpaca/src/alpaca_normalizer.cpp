#include "chronos/alpaca/alpaca_normalizer.hpp"

#include "chronos/alpaca/alpaca_timestamp.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::alpaca_adapter {
namespace {

using Failure = std::string;

enum class DecimalFailure {
    Inexact,
    OutOfRange,
};

template <typename Decimal>
std::variant<Decimal, DecimalFailure> exact_decimal(std::string_view lexeme) {
    try {
        return Decimal::parse(lexeme);
    } catch (const std::overflow_error&) {
        return DecimalFailure::OutOfRange;
    } catch (const std::invalid_argument&) {
        return DecimalFailure::Inexact;
    }
}

std::variant<Price, Failure> exact_price(std::string_view name, std::string_view lexeme) {
    const std::string subject =
        std::string(name) + " " + printable_excerpt(lexeme);
    if (lexeme.find_first_of("eE") != std::string_view::npos) {
        return subject + " uses exponent notation, which Chronos does not accept";
    }
    auto price = exact_decimal<Price>(lexeme);
    if (const auto* failure = std::get_if<DecimalFailure>(&price)) {
        return subject + (*failure == DecimalFailure::OutOfRange
            ? " exceeds the Chronos price range"
            : " is not exact at nine decimal places");
    }
    return std::get<Price>(price);
}

std::variant<Quantity, Failure> exact_size(std::string_view name, std::string_view lexeme) {
    const std::string subject =
        std::string(name) + " " + printable_excerpt(lexeme);
    const bool whole_shares = !lexeme.empty() &&
        std::all_of(lexeme.begin(), lexeme.end(), [](char character) {
            return character >= '0' && character <= '9';
        });
    if (!whole_shares) {
        return subject + " is not a whole, non-negative number of shares";
    }
    auto size = exact_decimal<Quantity>(lexeme);
    if (std::holds_alternative<DecimalFailure>(size)) {
        return subject + " exceeds the Chronos quantity range";
    }
    return std::get<Quantity>(size);
}

std::optional<Failure> require_iex(
    std::string_view name,
    const std::optional<std::string>& exchange
) {
    if (!exchange) {
        return std::string(name) + " has no exchange code";
    }
    if (*exchange != kIexExchangeCode) {
        return std::string(name) + " is from exchange \"" +
            printable_excerpt(*exchange) + "\", not IEX (\"" +
            std::string(kIexExchangeCode) + "\")";
    }
    return std::nullopt;
}

std::variant<std::optional<BookLevel>, Failure> book_side(
    std::string_view side,
    const std::optional<std::string>& exchange,
    std::string_view price_lexeme,
    std::string_view size_lexeme
) {
    auto price = exact_price(std::string(side) + " price", price_lexeme);
    if (auto* failure = std::get_if<Failure>(&price)) {
        return std::move(*failure);
    }
    auto size = exact_size(std::string(side) + " size", size_lexeme);
    if (auto* failure = std::get_if<Failure>(&size)) {
        return std::move(*failure);
    }
    const Price level_price = std::get<Price>(price);
    const Quantity level_size = std::get<Quantity>(size);
    if (level_price.raw() == 0 && level_size.raw() == 0) {
        return std::optional<BookLevel>();
    }
    if (level_price.raw() <= 0 || level_size.raw() <= 0) {
        return std::string(side) +
            " must be empty (price and size 0) or have a positive price and size";
    }
    if (auto failure = require_iex(side, exchange)) {
        return std::move(*failure);
    }
    return std::optional<BookLevel>(BookLevel{.price = level_price, .quantity = level_size});
}

class EventBuilder final {
public:
    EventBuilder(
        const MarketDataStreamId& stream,
        const AlpacaSubscription& subscription,
        Timestamp receive_timestamp
    )
        : stream_(stream),
          subscription_(subscription),
          receive_timestamp_(receive_timestamp) {}

    std::variant<MarketEvent, Failure> quote(
        const AlpacaQuote& quote,
        std::uint64_t ingest_sequence
    ) const {
        const std::string subject = "quote for " + printable_excerpt(quote.symbol);
        auto header = event_header(quote.symbol, quote.timestamp, ingest_sequence);
        if (auto* failure = std::get_if<Failure>(&header)) {
            return subject + ": " + *failure;
        }
        auto bid = book_side("bid", quote.bid_exchange, quote.bid_price, quote.bid_size);
        if (auto* failure = std::get_if<Failure>(&bid)) {
            return subject + ": " + *failure;
        }
        auto ask = book_side("ask", quote.ask_exchange, quote.ask_price, quote.ask_size);
        if (auto* failure = std::get_if<Failure>(&ask)) {
            return subject + ": " + *failure;
        }

        TopOfBookEvent event{
            .header = std::get<MarketEventHeader>(std::move(header)),
            .bid = std::get<std::optional<BookLevel>>(bid),
            .ask = std::get<std::optional<BookLevel>>(ask),
        };
        if (event.bid && event.ask && event.bid->price > event.ask->price) {
            event.header.quality =
                event.header.quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
        }
        return MarketEvent(std::move(event));
    }

    std::variant<MarketEvent, Failure> trade(
        const AlpacaTrade& trade,
        std::uint64_t ingest_sequence
    ) const {
        const std::string subject = "trade for " + printable_excerpt(trade.symbol);
        auto header = event_header(trade.symbol, trade.timestamp, ingest_sequence);
        if (auto* failure = std::get_if<Failure>(&header)) {
            return subject + ": " + *failure;
        }
        auto price = exact_price("price", trade.price);
        if (auto* failure = std::get_if<Failure>(&price)) {
            return subject + ": " + *failure;
        }
        auto size = exact_size("size", trade.size);
        if (auto* failure = std::get_if<Failure>(&size)) {
            return subject + ": " + *failure;
        }
        if (std::get<Price>(price).raw() <= 0 || std::get<Quantity>(size).raw() <= 0) {
            return subject + ": price and size must be positive";
        }
        if (auto failure = require_iex("trade", trade.exchange)) {
            return subject + ": " + *failure;
        }
        return MarketEvent(TradeEvent{
            .header = std::get<MarketEventHeader>(std::move(header)),
            .price = std::get<Price>(price),
            .quantity = std::get<Quantity>(size),
            // Alpaca does not report which side initiated a trade.
            .aggressor_side = AggressorSide::Unknown,
        });
    }

private:
    std::variant<MarketEventHeader, Failure> event_header(
        const std::string& symbol,
        std::string_view timestamp,
        std::uint64_t ingest_sequence
    ) const {
        const InstrumentId* instrument = subscription_.find(symbol);
        if (instrument == nullptr) {
            return Failure("the symbol is not subscribed");
        }
        auto event_timestamp = parse_rfc3339_timestamp(timestamp);
        if (auto* failure = std::get_if<TimestampParseError>(&event_timestamp)) {
            return "field \"t\": " + failure->message;
        }
        const Timestamp event_time = std::get<Timestamp>(event_timestamp);
        MarketDataQuality quality =
            MarketDataQuality{}.with(MarketDataQualityFlag::SourceSpecific);
        // Clock skew can put local receipt before the provider's event time.
        // The receive time is kept as observed and flagged, never adjusted.
        if (receive_timestamp_ < event_time) {
            quality = quality.with(MarketDataQualityFlag::QuestionableReceiveTimestamp);
        }
        return MarketEventHeader{
            .instrument = *instrument,
            .stream = stream_,
            .venue = VenueId(std::string(kIexVenue)),
            .event_timestamp = event_time,
            .receive_timestamp = receive_timestamp_,
            .ingest_sequence = ingest_sequence,
            .venue_sequence = std::nullopt,
            .quality = quality,
        };
    }

    const MarketDataStreamId& stream_;
    const AlpacaSubscription& subscription_;
    Timestamp receive_timestamp_;
};

}  // namespace

AlpacaNormalizer::AlpacaNormalizer(
    MarketDataStreamId stream,
    AlpacaSubscription subscription,
    std::uint64_t initial_ingest_sequence
)
    : stream_(std::move(stream)),
      subscription_(std::move(subscription)),
      next_ingest_sequence_(initial_ingest_sequence) {}

AlpacaNormalizationResult AlpacaNormalizer::normalize(
    const std::vector<AlpacaDataMessage>& messages,
    Timestamp receive_timestamp
) {
    if (messages.empty()) {
        return std::vector<ValidatedMarketEvent>{};
    }
    if (!next_ingest_sequence_ ||
        messages.size() - 1 >
            std::numeric_limits<std::uint64_t>::max() - *next_ingest_sequence_) {
        return AlpacaNormalizationError{
            .message = "normalizing this frame would exhaust the Chronos ingest sequence",
            .validation_errors = {},
        };
    }

    const EventBuilder builder(stream_, subscription_, receive_timestamp);
    const std::uint64_t first = *next_ingest_sequence_;
    std::vector<ValidatedMarketEvent> events;
    events.reserve(messages.size());
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const std::uint64_t sequence = first + index;
        auto event = std::visit(
            [&](const auto& message) {
                using Message = std::decay_t<decltype(message)>;
                if constexpr (std::is_same_v<Message, AlpacaQuote>) {
                    return builder.quote(message, sequence);
                } else {
                    return builder.trade(message, sequence);
                }
            },
            messages[index]
        );
        if (auto* failure = std::get_if<Failure>(&event)) {
            return AlpacaNormalizationError{
                .message = std::move(*failure),
                .validation_errors = {},
            };
        }
        auto validated = validate_market_event(std::get<MarketEvent>(std::move(event)));
        if (auto* failure = std::get_if<MarketEventValidationFailure>(&validated)) {
            return AlpacaNormalizationError{
                .message = "a normalized Alpaca event failed the Chronos market-data contract",
                .validation_errors = std::move(failure->errors),
            };
        }
        events.push_back(std::get<ValidatedMarketEvent>(std::move(validated)));
    }

    const std::uint64_t last = first + (messages.size() - 1);
    next_ingest_sequence_ = last == std::numeric_limits<std::uint64_t>::max()
        ? std::nullopt
        : std::optional<std::uint64_t>(last + 1);
    return events;
}

}  // namespace chronos::alpaca_adapter
