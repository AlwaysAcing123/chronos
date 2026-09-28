#include "chronos/databento/equities_mini_mbp1_normalizer.hpp"

#include <databento/constants.hpp>
#include <databento/enums.hpp>
#include <databento/flag_set.hpp>
#include <databento/publishers.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::databento_adapter {
namespace {

EquitiesMiniMbp1Error adapter_error(
    EquitiesMiniMbp1ErrorCode code,
    std::string_view field,
    std::string_view message
) {
    return EquitiesMiniMbp1Error{
        .code = code,
        .field = std::string(field),
        .message = std::string(message),
        .validation_errors = {},
    };
}

bool is_known_action(::databento::Action action) {
    switch (action) {
        case ::databento::action::Modify:
        case ::databento::action::Trade:
        case ::databento::action::Fill:
        case ::databento::action::Cancel:
        case ::databento::action::Add:
        case ::databento::action::Clear:
        case ::databento::action::None:
            return true;
        default:
            return false;
    }
}

bool is_known_side(::databento::Side side) {
    switch (side) {
        case ::databento::side::Ask:
        case ::databento::side::Bid:
        case ::databento::side::None:
            return true;
        default:
            return false;
    }
}

std::optional<EquitiesMiniMbp1Error> validate_book_level(
    std::int64_t price,
    std::uint32_t size,
    std::string_view side
) {
    const bool price_is_undefined = price == ::databento::kUndefPrice;
    const bool size_is_undefined = size == ::databento::kUndefOrderSize;
    const bool is_empty = price_is_undefined && size == 0;
    if (is_empty) {
        return std::nullopt;
    }

    if (price_is_undefined || size_is_undefined || price <= 0 || size == 0) {
        const bool is_bid = side == "bid";
        return adapter_error(
            is_bid
                ? EquitiesMiniMbp1ErrorCode::InconsistentBidLevel
                : EquitiesMiniMbp1ErrorCode::InconsistentAskLevel,
            is_bid ? "levels[0].bid" : "levels[0].ask",
            is_bid
                ? "bid must be an undefined-price/zero-size empty level or a positive level"
                : "ask must be an undefined-price/zero-size empty level or a positive level"
        );
    }
    return std::nullopt;
}

std::optional<EquitiesMiniMbp1Error> validate_record(
    const ::databento::Mbp1Msg& record,
    std::string_view symbol
) {
    if (symbol.empty()) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::MissingSymbol,
            "symbol",
            "resolved symbol must not be empty"
        );
    }
    if (record.hd.rtype != ::databento::r_type::Mbp1) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UnexpectedRecordType,
            "hd.rtype",
            "EQUS.MINI MBP-1 normalizer requires an MBP-1 record type"
        );
    }
    if (record.hd.Publisher() != ::databento::Publisher::EqusMiniEqus) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UnexpectedPublisher,
            "hd.publisher_id",
            "record publisher is not the EQUS.MINI composite publisher"
        );
    }
    if (record.sequence != 0) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UnexpectedVenueSequence,
            "sequence",
            "EQUS.MINI records must have venue sequence zero"
        );
    }
    if (record.levels[0].bid_ct != 0 || record.levels[0].ask_ct != 0) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UnexpectedOrderCount,
            "levels[0]",
            "EQUS.MINI does not publish component-venue order counts"
        );
    }
    if (!is_known_action(record.action)) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UnknownAction,
            "action",
            "record contains an unknown Databento action"
        );
    }
    if (!is_known_side(record.side)) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UnknownSide,
            "side",
            "record contains an unknown Databento side"
        );
    }
    const std::uint64_t event_timestamp =
        record.hd.ts_event.time_since_epoch().count();
    const std::uint64_t receive_timestamp =
        record.ts_recv.time_since_epoch().count();
    if (event_timestamp == ::databento::kUndefTimestamp) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UndefinedEventTimestamp,
            "hd.ts_event",
            "event timestamp is undefined"
        );
    }
    if (receive_timestamp == ::databento::kUndefTimestamp) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::UndefinedReceiveTimestamp,
            "ts_recv",
            "receive timestamp is undefined"
        );
    }
    constexpr std::uint64_t kMaximumTimestamp =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (event_timestamp > kMaximumTimestamp ||
        receive_timestamp > kMaximumTimestamp) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::TimestampOutOfRange,
            event_timestamp > kMaximumTimestamp ? "hd.ts_event" : "ts_recv",
            "timestamp exceeds the Chronos signed nanosecond range"
        );
    }

    const ::databento::BidAskPair& level = record.levels[0];
    if (auto error = validate_book_level(level.bid_px, level.bid_sz, "bid")) {
        return error;
    }
    if (auto error = validate_book_level(level.ask_px, level.ask_sz, "ask")) {
        return error;
    }

    if (record.action == ::databento::action::Trade) {
        if (record.price == ::databento::kUndefPrice || record.price <= 0) {
            return adapter_error(
                EquitiesMiniMbp1ErrorCode::InvalidTradePrice,
                "price",
                "trade price must be defined and positive"
            );
        }
        if (record.size == ::databento::kUndefOrderSize || record.size == 0) {
            return adapter_error(
                EquitiesMiniMbp1ErrorCode::InvalidTradeQuantity,
                "size",
                "trade size must be defined and positive"
            );
        }
    }
    return std::nullopt;
}

Quantity quantity_from_databento_size(std::uint32_t size) {
    return Quantity::from_raw(
        static_cast<std::int64_t>(size) * Quantity::scale()
    );
}

std::optional<BookLevel> book_level(
    std::int64_t price,
    std::uint32_t size
) {
    if (price == ::databento::kUndefPrice) {
        return std::nullopt;
    }
    return BookLevel{
        .price = Price::from_raw(price),
        .quantity = quantity_from_databento_size(size),
    };
}

AggressorSide aggressor_side(::databento::Side side) {
    switch (side) {
        case ::databento::side::Bid:
            return AggressorSide::Buy;
        case ::databento::side::Ask:
            return AggressorSide::Sell;
        case ::databento::side::None:
            return AggressorSide::Unknown;
        default:
            return AggressorSide::Unknown;
    }
}

MarketDataQuality market_data_quality(
    const ::databento::Mbp1Msg& record,
    FeedMode feed_mode
) {
    // Structural flags and Databento's ignorable reserved bit deliberately do
    // not become Chronos quality flags.
    MarketDataQuality quality;
    if (feed_mode == FeedMode::HistoricalReplay) {
        quality = quality.with(MarketDataQualityFlag::Replay);
    }
    if (record.flags.IsSnapshot()) {
        quality = quality.with(MarketDataQualityFlag::Snapshot);
    }
    if (record.flags.IsBadTsRecv()) {
        quality = quality.with(
            MarketDataQualityFlag::QuestionableReceiveTimestamp
        );
    }
    if (record.flags.IsMaybeBadBook()) {
        quality = quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    }
    if (record.flags.IsPublisherSpecific()) {
        quality = quality.with(MarketDataQualityFlag::SourceSpecific);
    }

    const ::databento::BidAskPair& level = record.levels[0];
    const bool is_crossed =
        level.bid_px != ::databento::kUndefPrice &&
        level.ask_px != ::databento::kUndefPrice &&
        level.bid_px > level.ask_px;
    if (is_crossed) {
        quality = quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    }
    return quality;
}

MarketEventHeader event_header(
    const EquitiesMiniMbp1Normalizer& normalizer,
    std::string_view symbol,
    const ::databento::Mbp1Msg& record,
    MarketDataQuality quality,
    std::uint64_t ingest_sequence
) {
    return MarketEventHeader{
        .instrument = InstrumentId(std::string(symbol)),
        .stream = normalizer.stream(),
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(
            static_cast<std::int64_t>(
                record.hd.ts_event.time_since_epoch().count()
            )
        ),
        .receive_timestamp = Timestamp::from_nanoseconds(
            static_cast<std::int64_t>(record.ts_recv.time_since_epoch().count())
        ),
        .ingest_sequence = ingest_sequence,
        .venue_sequence = std::nullopt,
        .quality = quality,
    };
}

EquitiesMiniMbp1Error normalized_event_error(
    MarketEventValidationFailure failure
) {
    return EquitiesMiniMbp1Error{
        .code = EquitiesMiniMbp1ErrorCode::NormalizedEventRejected,
        .field = "normalized_event",
        .message = "normalized event failed the Chronos market-data contract",
        .validation_errors = std::move(failure.errors),
    };
}

}  // namespace

EquitiesMiniMbp1Normalizer::EquitiesMiniMbp1Normalizer(
    MarketDataStreamId stream,
    FeedMode feed_mode,
    std::uint64_t initial_ingest_sequence
)
    : stream_(std::move(stream)),
      feed_mode_(feed_mode),
      next_ingest_sequence_(initial_ingest_sequence) {}

EquitiesMiniMbp1Result EquitiesMiniMbp1Normalizer::normalize(
    const ::databento::Mbp1Msg& record,
    std::string_view symbol
) {
    if (auto error = validate_record(record, symbol)) {
        return std::move(*error);
    }

    const bool has_trade = record.action == ::databento::action::Trade;
    if (!next_ingest_sequence_.has_value() ||
        (has_trade && *next_ingest_sequence_ ==
            std::numeric_limits<std::uint64_t>::max())) {
        return adapter_error(
            EquitiesMiniMbp1ErrorCode::IngestSequenceExhausted,
            "ingest_sequence",
            "normalizing this record would exhaust the Chronos ingest sequence"
        );
    }

    const std::uint64_t first_sequence = *next_ingest_sequence_;
    const MarketDataQuality quality = market_data_quality(record, feed_mode_);
    std::optional<ValidatedMarketEvent> normalized_trade;
    if (has_trade) {
        MarketEvent trade = TradeEvent{
            .header = event_header(
                *this,
                symbol,
                record,
                quality,
                first_sequence
            ),
            .price = Price::from_raw(record.price),
            .quantity = quantity_from_databento_size(record.size),
            .aggressor_side = aggressor_side(record.side),
        };
        auto result = validate_market_event(std::move(trade));
        if (auto* failure =
                std::get_if<MarketEventValidationFailure>(&result)) {
            return normalized_event_error(std::move(*failure));
        }
        normalized_trade = std::get<ValidatedMarketEvent>(std::move(result));
    }

    const std::uint64_t top_of_book_sequence =
        first_sequence + static_cast<std::uint64_t>(has_trade);
    const ::databento::BidAskPair& level = record.levels[0];
    MarketEvent top_of_book = TopOfBookEvent{
        .header = event_header(
            *this,
            symbol,
            record,
            quality,
            top_of_book_sequence
        ),
        .bid = book_level(level.bid_px, level.bid_sz),
        .ask = book_level(level.ask_px, level.ask_sz),
    };
    auto top_of_book_result = validate_market_event(std::move(top_of_book));
    if (auto* failure =
            std::get_if<MarketEventValidationFailure>(&top_of_book_result)) {
        return normalized_event_error(std::move(*failure));
    }
    auto normalized_top_of_book = std::get<ValidatedMarketEvent>(
        std::move(top_of_book_result)
    );

    if (top_of_book_sequence ==
        std::numeric_limits<std::uint64_t>::max()) {
        next_ingest_sequence_ = std::nullopt;
    } else {
        next_ingest_sequence_ = top_of_book_sequence + 1;
    }

    return EquitiesMiniMbp1Events{
        .trade = std::move(normalized_trade),
        .top_of_book = std::move(normalized_top_of_book),
    };
}

}  // namespace chronos::databento_adapter
