#pragma once

#include "chronos/market_data_validation.hpp"

#include <databento/record.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::databento_adapter {

enum class FeedMode {
    HistoricalReplay,
    Live,
};

enum class EquitiesMiniMbp1ErrorCode {
    MissingSymbol,
    UnexpectedRecordType,
    UnexpectedPublisher,
    UnexpectedVenueSequence,
    UnexpectedOrderCount,
    UnknownAction,
    UnknownSide,
    UndefinedEventTimestamp,
    UndefinedReceiveTimestamp,
    TimestampOutOfRange,
    InvalidTradePrice,
    InvalidTradeQuantity,
    InconsistentBidLevel,
    InconsistentAskLevel,
    IngestSequenceExhausted,
    NormalizedEventRejected,
};

struct EquitiesMiniMbp1Error {
    EquitiesMiniMbp1ErrorCode code;
    std::string field;
    std::string message;
    std::vector<MarketEventValidationError> validation_errors;
};

struct EquitiesMiniMbp1Events {
    std::optional<ValidatedMarketEvent> trade;
    ValidatedMarketEvent top_of_book;

    [[nodiscard]] std::size_t size() const noexcept {
        return trade.has_value() ? 2 : 1;
    }
};

using EquitiesMiniMbp1Result = std::variant<
    EquitiesMiniMbp1Events,
    EquitiesMiniMbp1Error
>;

class EquitiesMiniMbp1Normalizer final {
public:
    EquitiesMiniMbp1Normalizer(
        MarketDataStreamId stream,
        FeedMode feed_mode,
        std::uint64_t initial_ingest_sequence = 0
    );

    EquitiesMiniMbp1Normalizer(const EquitiesMiniMbp1Normalizer&) = delete;
    EquitiesMiniMbp1Normalizer& operator=(
        const EquitiesMiniMbp1Normalizer&
    ) = delete;
    EquitiesMiniMbp1Normalizer(
        EquitiesMiniMbp1Normalizer&&
    ) noexcept = default;
    EquitiesMiniMbp1Normalizer& operator=(
        EquitiesMiniMbp1Normalizer&&
    ) noexcept = default;

    [[nodiscard]] const MarketDataStreamId& stream() const noexcept {
        return stream_;
    }

    [[nodiscard]] FeedMode feed_mode() const noexcept { return feed_mode_; }

    [[nodiscard]] std::optional<std::uint64_t>
    next_ingest_sequence() const noexcept {
        return next_ingest_sequence_;
    }

    [[nodiscard]] EquitiesMiniMbp1Result normalize(
        const ::databento::Mbp1Msg& record,
        std::string_view symbol
    );

private:
    MarketDataStreamId stream_;
    FeedMode feed_mode_;
    std::optional<std::uint64_t> next_ingest_sequence_;
};

}  // namespace chronos::databento_adapter
