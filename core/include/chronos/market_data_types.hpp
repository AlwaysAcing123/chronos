#pragma once

#include "chronos/trading_types.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace chronos {

struct MarketDataStreamIdTag;
struct VenueIdTag;
struct CurrencyCodeTag;

using MarketDataStreamId = Identifier<MarketDataStreamIdTag>;
using VenueId = Identifier<VenueIdTag>;
using CurrencyCode = Identifier<CurrencyCodeTag>;

enum class MarketDataQualityFlag : std::uint32_t {
    Snapshot = 1U << 0U,
    Replay = 1U << 1U,
    QuestionableReceiveTimestamp = 1U << 2U,
    PotentiallyInvalidBook = 1U << 3U,
    SourceSpecific = 1U << 4U,
};

class MarketDataQuality final {
public:
    constexpr MarketDataQuality() = default;

    [[nodiscard]] static constexpr MarketDataQuality from_raw(
        std::uint32_t bits
    ) {
        return MarketDataQuality(bits);
    }

    [[nodiscard]] constexpr bool contains(MarketDataQualityFlag flag) const {
        return (bits_ & bit_for(flag)) != 0;
    }

    [[nodiscard]] constexpr MarketDataQuality with(
        MarketDataQualityFlag flag
    ) const {
        return MarketDataQuality(bits_ | bit_for(flag));
    }

    [[nodiscard]] constexpr std::uint32_t raw() const { return bits_; }

    friend constexpr auto operator<=>(
        const MarketDataQuality&,
        const MarketDataQuality&
    ) = default;

private:
    explicit constexpr MarketDataQuality(std::uint32_t bits) : bits_(bits) {}

    [[nodiscard]] static constexpr std::uint32_t bit_for(
        MarketDataQualityFlag flag
    ) {
        return static_cast<std::uint32_t>(flag);
    }

    std::uint32_t bits_{0};
};

struct MarketEventHeader {
    InstrumentId instrument;
    MarketDataStreamId stream;
    std::optional<VenueId> venue;
    Timestamp event_timestamp;
    Timestamp receive_timestamp;
    std::uint64_t ingest_sequence{0};
    std::optional<std::uint64_t> venue_sequence;
    MarketDataQuality quality;

    friend auto operator<=>(const MarketEventHeader&, const MarketEventHeader&) = default;
};

struct BookLevel {
    Price price;
    Quantity quantity;

    friend auto operator<=>(const BookLevel&, const BookLevel&) = default;
};

enum class AggressorSide {
    Buy,
    Sell,
    Unknown,
};

struct TradeEvent {
    MarketEventHeader header;
    Price price;
    Quantity quantity;
    AggressorSide aggressor_side{AggressorSide::Unknown};

    friend auto operator<=>(const TradeEvent&, const TradeEvent&) = default;
};

struct TopOfBookEvent {
    MarketEventHeader header;
    std::optional<BookLevel> bid;
    std::optional<BookLevel> ask;

    friend auto operator<=>(const TopOfBookEvent&, const TopOfBookEvent&) = default;
};

enum class TradingStatus {
    Unknown,
    PreOpen,
    Open,
    Halted,
    Closed,
};

struct MarketStatusEvent {
    MarketEventHeader header;
    TradingStatus status{TradingStatus::Unknown};
    std::string reason;

    friend auto operator<=>(const MarketStatusEvent&, const MarketStatusEvent&) = default;
};

struct InstrumentDefinitionEvent {
    MarketEventHeader header;
    std::string display_symbol;
    CurrencyCode currency;
    Price minimum_price_increment;
    Quantity minimum_quantity_increment;
    Timestamp effective_from;
    std::optional<Timestamp> effective_until;

    friend auto operator<=>(
        const InstrumentDefinitionEvent&,
        const InstrumentDefinitionEvent&
    ) = default;
};

using MarketEvent = std::variant<
    TradeEvent,
    TopOfBookEvent,
    MarketStatusEvent,
    InstrumentDefinitionEvent
>;

[[nodiscard]] inline const MarketEventHeader& market_event_header(
    const MarketEvent& event
) {
    return std::visit(
        [](const auto& concrete_event) -> const MarketEventHeader& {
            return concrete_event.header;
        },
        event
    );
}

}  // namespace chronos
