#include "chronos/market_data_validation.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using chronos::AggressorSide;
using chronos::BookLevel;
using chronos::CurrencyCode;
using chronos::InstrumentDefinitionEvent;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEvent;
using chronos::MarketEventHeader;
using chronos::MarketEventValidationErrorCode;
using chronos::MarketEventValidationFailure;
using chronos::MarketEventValidationResult;
using chronos::MarketStatusEvent;
using chronos::Price;
using chronos::Quantity;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::TradingStatus;
using chronos::ValidatedMarketEvent;

static_assert(!std::is_default_constructible_v<ValidatedMarketEvent>);
static_assert(!std::is_constructible_v<ValidatedMarketEvent, MarketEvent>);

Price price(std::string_view value) {
    return Price::parse(value);
}

Quantity quantity(std::string_view value) {
    return Quantity::parse(value);
}

MarketEventHeader header(
    std::int64_t event_timestamp = 1'000,
    std::int64_t receive_timestamp = 1'100,
    MarketDataQuality quality = MarketDataQuality{}
) {
    return MarketEventHeader{
        .instrument = InstrumentId("AAPL"),
        .stream = MarketDataStreamId("test-stream"),
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(event_timestamp),
        .receive_timestamp = Timestamp::from_nanoseconds(receive_timestamp),
        .ingest_sequence = 0,
        .venue_sequence = std::nullopt,
        .quality = quality,
    };
}

TradeEvent trade(MarketEventHeader event_header = header()) {
    return TradeEvent{
        .header = std::move(event_header),
        .price = price("100.25"),
        .quantity = quantity("10"),
        .aggressor_side = AggressorSide::Unknown,
    };
}

TopOfBookEvent top_of_book(MarketEventHeader event_header = header()) {
    return TopOfBookEvent{
        .header = std::move(event_header),
        .bid = BookLevel{
            .price = price("100"),
            .quantity = quantity("5"),
        },
        .ask = BookLevel{
            .price = price("100.01"),
            .quantity = quantity("6"),
        },
    };
}

MarketStatusEvent market_status(MarketEventHeader event_header = header()) {
    return MarketStatusEvent{
        .header = std::move(event_header),
        .status = TradingStatus::Open,
        .reason = "",
    };
}

InstrumentDefinitionEvent instrument_definition(
    MarketEventHeader event_header = header()
) {
    return InstrumentDefinitionEvent{
        .header = std::move(event_header),
        .display_symbol = "AAPL",
        .currency = CurrencyCode("USD"),
        .minimum_price_increment = price("0.01"),
        .minimum_quantity_increment = quantity("1"),
        .effective_from = Timestamp::from_nanoseconds(900),
        .effective_until = std::nullopt,
    };
}

ValidatedMarketEvent require_valid(MarketEventValidationResult result) {
    REQUIRE(std::holds_alternative<ValidatedMarketEvent>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

MarketEventValidationFailure require_failure(
    MarketEventValidationResult result
) {
    REQUIRE(std::holds_alternative<MarketEventValidationFailure>(result));
    return std::get<MarketEventValidationFailure>(std::move(result));
}

void require_error(
    const MarketEventValidationFailure& failure,
    std::size_t index,
    MarketEventValidationErrorCode code,
    std::string_view field
) {
    REQUIRE(index < failure.errors.size());
    REQUIRE(failure.errors[index].code == code);
    REQUIRE(failure.errors[index].field == field);
    REQUIRE(!failure.errors[index].message.empty());
}

void require_single_error(
    MarketEventValidationResult result,
    MarketEventValidationErrorCode code,
    std::string_view field
) {
    const auto failure = require_failure(std::move(result));
    REQUIRE(failure.errors.size() == 1);
    require_error(failure, 0, code, field);
}

void test_valid_event_is_preserved() {
    const MarketEvent original = trade();
    const auto result = chronos::validate_market_event(original);

    REQUIRE(require_valid(result).value() == original);
}

void test_all_event_categories_accept_valid_values() {
    require_valid(chronos::validate_market_event(trade()));

    TopOfBookEvent one_sided = top_of_book();
    one_sided.ask = std::nullopt;
    require_valid(chronos::validate_market_event(std::move(one_sided)));

    TopOfBookEvent cleared = top_of_book();
    cleared.bid = std::nullopt;
    cleared.ask = std::nullopt;
    require_valid(chronos::validate_market_event(std::move(cleared)));

    TopOfBookEvent locked = top_of_book();
    locked.ask->price = locked.bid->price;
    require_valid(chronos::validate_market_event(std::move(locked)));

    MarketStatusEvent unknown_status = market_status();
    unknown_status.status = TradingStatus::Unknown;
    require_valid(chronos::validate_market_event(std::move(unknown_status)));

    InstrumentDefinitionEvent bounded_definition = instrument_definition();
    bounded_definition.effective_until = Timestamp::from_nanoseconds(2'000);
    require_valid(
        chronos::validate_market_event(std::move(bounded_definition))
    );
}

void test_header_rejects_negative_timestamps() {
    require_single_error(
        chronos::validate_market_event(trade(header(-1, 1'100))),
        MarketEventValidationErrorCode::NegativeEventTimestamp,
        "header.event_timestamp"
    );
    require_single_error(
        chronos::validate_market_event(trade(header(1'000, -1))),
        MarketEventValidationErrorCode::NegativeReceiveTimestamp,
        "header.receive_timestamp"
    );
}

void test_timestamp_inversion_requires_quality_flag() {
    require_single_error(
        chronos::validate_market_event(trade(header(1'100, 1'000))),
        MarketEventValidationErrorCode::UnexplainedTimestampInversion,
        "header.receive_timestamp"
    );

    const MarketDataQuality questionable = MarketDataQuality{}.with(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    );
    require_valid(
        chronos::validate_market_event(trade(header(1'100, 1'000, questionable)))
    );
}

void test_header_rejects_unknown_quality_bits() {
    const MarketDataQuality unknown_quality = MarketDataQuality{}.with(
        static_cast<MarketDataQualityFlag>(1U << 31U)
    );
    require_single_error(
        chronos::validate_market_event(
            trade(header(1'000, 1'100, unknown_quality))
        ),
        MarketEventValidationErrorCode::UnknownQualityFlags,
        "header.quality"
    );
}

void test_header_accepts_every_known_quality_flag() {
    const MarketDataQuality all_known_flags = MarketDataQuality{}
        .with(MarketDataQualityFlag::Snapshot)
        .with(MarketDataQualityFlag::Replay)
        .with(MarketDataQualityFlag::QuestionableReceiveTimestamp)
        .with(MarketDataQualityFlag::PotentiallyInvalidBook)
        .with(MarketDataQualityFlag::SourceSpecific);

    require_valid(
        chronos::validate_market_event(
            trade(header(1'000, 1'000, all_known_flags))
        )
    );
}

void test_trade_rejects_invalid_fields() {
    TradeEvent invalid = trade();
    invalid.price = price("-1");
    invalid.quantity = Quantity{};
    invalid.aggressor_side = static_cast<AggressorSide>(99);

    const auto failure = require_failure(
        chronos::validate_market_event(std::move(invalid))
    );
    REQUIRE(failure.errors.size() == 3);
    require_error(
        failure,
        0,
        MarketEventValidationErrorCode::NonpositivePrice,
        "trade.price"
    );
    require_error(
        failure,
        1,
        MarketEventValidationErrorCode::NonpositiveQuantity,
        "trade.quantity"
    );
    require_error(
        failure,
        2,
        MarketEventValidationErrorCode::UnknownAggressorSide,
        "trade.aggressor_side"
    );
}

void test_book_rejects_invalid_present_levels() {
    TopOfBookEvent invalid = top_of_book();
    invalid.bid->price = Price{};
    invalid.bid->quantity = quantity("-1");
    invalid.ask->price = price("-2");
    invalid.ask->quantity = Quantity{};

    const auto failure = require_failure(
        chronos::validate_market_event(std::move(invalid))
    );
    REQUIRE(failure.errors.size() == 4);
    require_error(
        failure,
        0,
        MarketEventValidationErrorCode::NonpositivePrice,
        "top_of_book.bid.price"
    );
    require_error(
        failure,
        1,
        MarketEventValidationErrorCode::NonpositiveQuantity,
        "top_of_book.bid.quantity"
    );
    require_error(
        failure,
        2,
        MarketEventValidationErrorCode::NonpositivePrice,
        "top_of_book.ask.price"
    );
    require_error(
        failure,
        3,
        MarketEventValidationErrorCode::NonpositiveQuantity,
        "top_of_book.ask.quantity"
    );
}

void test_crossed_book_requires_quality_flag() {
    TopOfBookEvent crossed = top_of_book();
    crossed.bid->price = price("101");

    require_single_error(
        chronos::validate_market_event(crossed),
        MarketEventValidationErrorCode::UnflaggedCrossedBook,
        "top_of_book"
    );

    crossed.header.quality = MarketDataQuality{}.with(
        MarketDataQualityFlag::PotentiallyInvalidBook
    );
    require_valid(chronos::validate_market_event(std::move(crossed)));
}

void test_market_status_rejects_unknown_enum_value() {
    MarketStatusEvent invalid = market_status();
    invalid.status = static_cast<TradingStatus>(99);

    require_single_error(
        chronos::validate_market_event(std::move(invalid)),
        MarketEventValidationErrorCode::UnknownTradingStatus,
        "market_status.status"
    );
}

void test_instrument_definition_rejects_invalid_fields() {
    InstrumentDefinitionEvent invalid = instrument_definition();
    invalid.display_symbol = "";
    invalid.minimum_price_increment = Price{};
    invalid.minimum_quantity_increment = quantity("-1");
    invalid.effective_from = Timestamp::from_nanoseconds(-2);
    invalid.effective_until = Timestamp::from_nanoseconds(-1);

    const auto failure = require_failure(
        chronos::validate_market_event(std::move(invalid))
    );
    REQUIRE(failure.errors.size() == 5);
    require_error(
        failure,
        0,
        MarketEventValidationErrorCode::MissingDisplaySymbol,
        "instrument_definition.display_symbol"
    );
    require_error(
        failure,
        1,
        MarketEventValidationErrorCode::NonpositiveMinimumPriceIncrement,
        "instrument_definition.minimum_price_increment"
    );
    require_error(
        failure,
        2,
        MarketEventValidationErrorCode::NonpositiveMinimumQuantityIncrement,
        "instrument_definition.minimum_quantity_increment"
    );
    require_error(
        failure,
        3,
        MarketEventValidationErrorCode::NegativeEffectiveTimestamp,
        "instrument_definition.effective_from"
    );
    require_error(
        failure,
        4,
        MarketEventValidationErrorCode::NegativeEffectiveTimestamp,
        "instrument_definition.effective_until"
    );
}

void test_instrument_definition_rejects_invalid_effective_range() {
    InstrumentDefinitionEvent equal_end = instrument_definition();
    equal_end.effective_until = equal_end.effective_from;
    require_single_error(
        chronos::validate_market_event(std::move(equal_end)),
        MarketEventValidationErrorCode::InvalidEffectiveRange,
        "instrument_definition.effective_until"
    );

    InstrumentDefinitionEvent earlier_end = instrument_definition();
    earlier_end.effective_until = Timestamp::from_nanoseconds(899);
    require_single_error(
        chronos::validate_market_event(std::move(earlier_end)),
        MarketEventValidationErrorCode::InvalidEffectiveRange,
        "instrument_definition.effective_until"
    );
}

void test_validation_collects_errors_in_stable_order() {
    const MarketDataQuality unknown_quality = MarketDataQuality{}.with(
        static_cast<MarketDataQualityFlag>(1U << 31U)
    );
    TradeEvent invalid = trade(header(-2, -1, unknown_quality));
    invalid.price = Price{};
    invalid.quantity = Quantity{};
    invalid.aggressor_side = static_cast<AggressorSide>(99);

    const auto failure = require_failure(
        chronos::validate_market_event(std::move(invalid))
    );
    REQUIRE(failure.errors.size() == 6);
    require_error(
        failure,
        0,
        MarketEventValidationErrorCode::NegativeEventTimestamp,
        "header.event_timestamp"
    );
    require_error(
        failure,
        1,
        MarketEventValidationErrorCode::NegativeReceiveTimestamp,
        "header.receive_timestamp"
    );
    require_error(
        failure,
        2,
        MarketEventValidationErrorCode::UnknownQualityFlags,
        "header.quality"
    );
    require_error(
        failure,
        3,
        MarketEventValidationErrorCode::NonpositivePrice,
        "trade.price"
    );
    require_error(
        failure,
        4,
        MarketEventValidationErrorCode::NonpositiveQuantity,
        "trade.quantity"
    );
    require_error(
        failure,
        5,
        MarketEventValidationErrorCode::UnknownAggressorSide,
        "trade.aggressor_side"
    );
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"valid event is preserved", test_valid_event_is_preserved},
        {
            "valid event categories",
            test_all_event_categories_accept_valid_values,
        },
        {"negative header timestamps", test_header_rejects_negative_timestamps},
        {
            "timestamp inversion flag",
            test_timestamp_inversion_requires_quality_flag,
        },
        {"unknown quality bits", test_header_rejects_unknown_quality_bits},
        {"known quality bits", test_header_accepts_every_known_quality_flag},
        {"invalid trade fields", test_trade_rejects_invalid_fields},
        {"invalid book levels", test_book_rejects_invalid_present_levels},
        {"crossed-book flag", test_crossed_book_requires_quality_flag},
        {
            "unknown market status",
            test_market_status_rejects_unknown_enum_value,
        },
        {
            "invalid instrument definition",
            test_instrument_definition_rejects_invalid_fields,
        },
        {
            "invalid effective range",
            test_instrument_definition_rejects_invalid_effective_range,
        },
        {
            "stable error collection",
            test_validation_collects_errors_in_stable_order,
        },
    });
}
