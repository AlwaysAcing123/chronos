#include "chronos/databento/equities_mini_mbp1_normalizer.hpp"
#include "test_support.hpp"

#include <databento/constants.hpp>
#include <databento/datetime.hpp>
#include <databento/enums.hpp>
#include <databento/flag_set.hpp>
#include <databento/publishers.hpp>
#include <databento/record.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

namespace db = ::databento;
namespace adapter = chronos::databento_adapter;

using adapter::EquitiesMiniMbp1Error;
using adapter::EquitiesMiniMbp1ErrorCode;
using adapter::EquitiesMiniMbp1Events;
using adapter::EquitiesMiniMbp1Normalizer;
using adapter::EquitiesMiniMbp1Result;
using adapter::FeedMode;
using chronos::AggressorSide;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEventValidationErrorCode;
using chronos::Price;
using chronos::Quantity;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;

static_assert(!std::is_copy_constructible_v<EquitiesMiniMbp1Normalizer>);
static_assert(std::is_move_constructible_v<EquitiesMiniMbp1Normalizer>);

db::UnixNanos unix_nanos(std::uint64_t value) {
    return db::UnixNanos{db::UnixNanos::duration{value}};
}

db::Mbp1Msg mbp1_record() {
    return db::Mbp1Msg{
        .hd = db::RecordHeader{
            .length = static_cast<std::uint8_t>(
                sizeof(db::Mbp1Msg) / db::RecordHeader::kLengthMultiplier
            ),
            .rtype = db::r_type::Mbp1,
            .publisher_id = static_cast<std::uint16_t>(
                db::Publisher::EqusMiniEqus
            ),
            .instrument_id = 38,
            .ts_event = unix_nanos(1'000),
        },
        .price = db::kUndefPrice,
        .size = 0,
        .action = db::action::Add,
        .side = db::side::Bid,
        .flags = db::FlagSet{},
        .depth = 0,
        .ts_recv = unix_nanos(1'100),
        .ts_in_delta = db::TimeDeltaNanos{0},
        .sequence = 0,
        .levels = std::array<db::BidAskPair, 1>{db::BidAskPair{
            .bid_px = 100'000'000'000,
            .ask_px = 100'010'000'000,
            .bid_sz = 5,
            .ask_sz = 6,
            .bid_ct = 0,
            .ask_ct = 0,
        }},
    };
}

EquitiesMiniMbp1Events require_events(EquitiesMiniMbp1Result result) {
    REQUIRE(std::holds_alternative<EquitiesMiniMbp1Events>(result));
    return std::get<EquitiesMiniMbp1Events>(std::move(result));
}

EquitiesMiniMbp1Error require_error(
    EquitiesMiniMbp1Result result,
    EquitiesMiniMbp1ErrorCode code
) {
    REQUIRE(std::holds_alternative<EquitiesMiniMbp1Error>(result));
    auto error = std::get<EquitiesMiniMbp1Error>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

void require_atomic_rejection(
    db::Mbp1Msg record,
    EquitiesMiniMbp1ErrorCode code,
    std::string_view symbol = "AAPL"
) {
    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("historical"),
        FeedMode::HistoricalReplay
    );
    require_error(normalizer.normalize(record, symbol), code);
    REQUIRE(normalizer.next_ingest_sequence() == 0);
}

void test_quote_normalizes_to_validated_top_of_book() {
    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("historical"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(
        normalizer.normalize(mbp1_record(), "AAPL")
    );

    REQUIRE(events.size() == 1);
    REQUIRE(!events.trade.has_value());
    const auto& top = std::get<TopOfBookEvent>(events.top_of_book.value());
    REQUIRE(top.header.instrument.value() == "AAPL");
    REQUIRE(top.header.stream == MarketDataStreamId("historical"));
    REQUIRE(!top.header.venue.has_value());
    REQUIRE(top.header.event_timestamp.nanoseconds() == 1'000);
    REQUIRE(top.header.receive_timestamp.nanoseconds() == 1'100);
    REQUIRE(top.header.ingest_sequence == 0);
    REQUIRE(!top.header.venue_sequence.has_value());
    REQUIRE(top.header.quality.contains(MarketDataQualityFlag::Replay));
    REQUIRE(top.bid->price == Price::from_raw(100'000'000'000));
    REQUIRE(top.bid->quantity == Quantity::from_raw(500'000'000));
    REQUIRE(top.ask->price == Price::from_raw(100'010'000'000));
    REQUIRE(top.ask->quantity == Quantity::from_raw(600'000'000));
    REQUIRE(normalizer.next_ingest_sequence() == 1);
}

void test_trade_emits_trade_before_top_of_book() {
    db::Mbp1Msg record = mbp1_record();
    record.action = db::action::Trade;
    record.side = db::side::Bid;
    record.price = 100'005'000'000;
    record.size = 3;

    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("historical"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(normalizer.normalize(record, "AAPL"));

    REQUIRE(events.size() == 2);
    REQUIRE(events.trade.has_value());
    const auto& trade = std::get<TradeEvent>(events.trade->value());
    const auto& top = std::get<TopOfBookEvent>(events.top_of_book.value());
    REQUIRE(trade.header.ingest_sequence == 0);
    REQUIRE(top.header.ingest_sequence == 1);
    REQUIRE(trade.price == Price::from_raw(100'005'000'000));
    REQUIRE(trade.quantity == Quantity::from_raw(300'000'000));
    REQUIRE(trade.aggressor_side == AggressorSide::Buy);
    REQUIRE(normalizer.next_ingest_sequence() == 2);
}

void test_trade_side_mapping() {
    db::Mbp1Msg record = mbp1_record();
    record.action = db::action::Trade;
    record.price = 100'005'000'000;
    record.size = 1;

    record.side = db::side::Ask;
    EquitiesMiniMbp1Normalizer sell_normalizer(
        MarketDataStreamId("sell"),
        FeedMode::HistoricalReplay
    );
    const auto sell = require_events(
        sell_normalizer.normalize(record, "AAPL")
    );
    REQUIRE(
        std::get<TradeEvent>(sell.trade->value()).aggressor_side ==
        AggressorSide::Sell
    );

    record.side = db::side::None;
    EquitiesMiniMbp1Normalizer unknown_normalizer(
        MarketDataStreamId("unknown"),
        FeedMode::HistoricalReplay
    );
    const auto unknown = require_events(
        unknown_normalizer.normalize(record, "AAPL")
    );
    REQUIRE(
        std::get<TradeEvent>(unknown.trade->value()).aggressor_side ==
        AggressorSide::Unknown
    );
}

void test_live_mode_does_not_set_replay_quality() {
    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("live"),
        FeedMode::Live
    );
    const auto events = require_events(
        normalizer.normalize(mbp1_record(), "AAPL")
    );
    const auto& top = std::get<TopOfBookEvent>(events.top_of_book.value());

    REQUIRE(!top.header.quality.contains(MarketDataQualityFlag::Replay));
}

void test_provider_quality_flags_are_mapped() {
    db::Mbp1Msg record = mbp1_record();
    record.flags = db::FlagSet{
        static_cast<std::uint8_t>(
            db::FlagSet::kLast |
            db::FlagSet::kTob |
            db::FlagSet::kSnapshot |
            db::FlagSet::kMbp |
            db::FlagSet::kBadTsRecv |
            db::FlagSet::kMaybeBadBook |
            db::FlagSet::kPublisherSpecific
        )
    };

    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("historical"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(normalizer.normalize(record, "AAPL"));
    const auto& quality =
        std::get<TopOfBookEvent>(events.top_of_book.value()).header.quality;

    REQUIRE(quality.contains(MarketDataQualityFlag::Replay));
    REQUIRE(quality.contains(MarketDataQualityFlag::Snapshot));
    REQUIRE(quality.contains(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    ));
    REQUIRE(quality.contains(MarketDataQualityFlag::PotentiallyInvalidBook));
    REQUIRE(quality.contains(MarketDataQualityFlag::SourceSpecific));
}

void test_crossed_book_is_preserved_and_flagged() {
    db::Mbp1Msg record = mbp1_record();
    record.levels[0].bid_px = 101'000'000'000;

    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("historical"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(normalizer.normalize(record, "AAPL"));
    const auto& top = std::get<TopOfBookEvent>(events.top_of_book.value());

    REQUIRE(top.bid->price > top.ask->price);
    REQUIRE(top.header.quality.contains(
        MarketDataQualityFlag::PotentiallyInvalidBook
    ));
}

void test_empty_book_sides_become_absent_levels() {
    db::Mbp1Msg record = mbp1_record();
    record.levels[0].bid_px = db::kUndefPrice;
    record.levels[0].bid_sz = 0;
    record.levels[0].ask_px = db::kUndefPrice;
    record.levels[0].ask_sz = 0;

    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("historical"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(normalizer.normalize(record, "AAPL"));
    const auto& top = std::get<TopOfBookEvent>(events.top_of_book.value());

    REQUIRE(!top.bid.has_value());
    REQUIRE(!top.ask.has_value());
}

void test_invalid_book_levels_are_rejected_atomically() {
    db::Mbp1Msg undefined_bid_with_size = mbp1_record();
    undefined_bid_with_size.levels[0].bid_px = db::kUndefPrice;
    require_atomic_rejection(
        undefined_bid_with_size,
        EquitiesMiniMbp1ErrorCode::InconsistentBidLevel
    );

    db::Mbp1Msg positive_ask_without_size = mbp1_record();
    positive_ask_without_size.levels[0].ask_sz = 0;
    require_atomic_rejection(
        positive_ask_without_size,
        EquitiesMiniMbp1ErrorCode::InconsistentAskLevel
    );
}

void test_provider_identity_fields_are_rejected_atomically() {
    require_atomic_rejection(
        mbp1_record(),
        EquitiesMiniMbp1ErrorCode::MissingSymbol,
        ""
    );

    db::Mbp1Msg wrong_type = mbp1_record();
    wrong_type.hd.rtype = db::r_type::Mbp10;
    require_atomic_rejection(
        wrong_type,
        EquitiesMiniMbp1ErrorCode::UnexpectedRecordType
    );

    db::Mbp1Msg wrong_publisher = mbp1_record();
    wrong_publisher.hd.publisher_id = static_cast<std::uint16_t>(
        db::Publisher::XnasItchXnas
    );
    require_atomic_rejection(
        wrong_publisher,
        EquitiesMiniMbp1ErrorCode::UnexpectedPublisher
    );

    db::Mbp1Msg venue_sequence = mbp1_record();
    venue_sequence.sequence = 1;
    require_atomic_rejection(
        venue_sequence,
        EquitiesMiniMbp1ErrorCode::UnexpectedVenueSequence
    );

    db::Mbp1Msg unexpected_order_count = mbp1_record();
    unexpected_order_count.levels[0].bid_ct = 1;
    require_atomic_rejection(
        unexpected_order_count,
        EquitiesMiniMbp1ErrorCode::UnexpectedOrderCount
    );
}

void test_nonzero_update_depth_still_emits_top_of_book() {
    db::Mbp1Msg record = mbp1_record();
    record.depth = 1;
    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("nonzero-update-depth"),
        FeedMode::HistoricalReplay
    );

    const auto events = require_events(normalizer.normalize(record, "AAPL"));
    const auto& top_of_book = std::get<TopOfBookEvent>(
        events.top_of_book.value()
    );
    REQUIRE(top_of_book.bid.has_value());
    REQUIRE(top_of_book.ask.has_value());
}

void test_unknown_provider_values_are_rejected_atomically() {
    db::Mbp1Msg unknown_action = mbp1_record();
    unknown_action.action = static_cast<db::Action>('?');
    require_atomic_rejection(
        unknown_action,
        EquitiesMiniMbp1ErrorCode::UnknownAction
    );

    db::Mbp1Msg unknown_side = mbp1_record();
    unknown_side.side = static_cast<db::Side>('?');
    require_atomic_rejection(
        unknown_side,
        EquitiesMiniMbp1ErrorCode::UnknownSide
    );

    db::Mbp1Msg reserved_flag = mbp1_record();
    reserved_flag.flags = db::FlagSet{1U};
    EquitiesMiniMbp1Normalizer normalizer(
        MarketDataStreamId("reserved-provider-flag"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(
        normalizer.normalize(reserved_flag, "AAPL")
    );
    const auto& top_of_book = std::get<TopOfBookEvent>(
        events.top_of_book.value()
    );
    REQUIRE(
        top_of_book.header.quality.raw() ==
        static_cast<std::uint32_t>(MarketDataQualityFlag::Replay)
    );
}

void test_invalid_timestamps_are_rejected_atomically() {
    db::Mbp1Msg undefined_event = mbp1_record();
    undefined_event.hd.ts_event = unix_nanos(db::kUndefTimestamp);
    require_atomic_rejection(
        undefined_event,
        EquitiesMiniMbp1ErrorCode::UndefinedEventTimestamp
    );

    db::Mbp1Msg undefined_receive = mbp1_record();
    undefined_receive.ts_recv = unix_nanos(db::kUndefTimestamp);
    require_atomic_rejection(
        undefined_receive,
        EquitiesMiniMbp1ErrorCode::UndefinedReceiveTimestamp
    );

    db::Mbp1Msg out_of_range = mbp1_record();
    out_of_range.hd.ts_event = unix_nanos(
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()
        ) + 1
    );
    require_atomic_rejection(
        out_of_range,
        EquitiesMiniMbp1ErrorCode::TimestampOutOfRange
    );
}

void test_timestamp_inversion_requires_provider_quality_flag() {
    db::Mbp1Msg record = mbp1_record();
    record.hd.ts_event = unix_nanos(1'200);

    EquitiesMiniMbp1Normalizer rejected_normalizer(
        MarketDataStreamId("rejected"),
        FeedMode::HistoricalReplay
    );
    const auto error = require_error(
        rejected_normalizer.normalize(record, "AAPL"),
        EquitiesMiniMbp1ErrorCode::NormalizedEventRejected
    );
    REQUIRE(error.validation_errors.size() == 1);
    REQUIRE(
        error.validation_errors[0].code ==
        MarketEventValidationErrorCode::UnexplainedTimestampInversion
    );
    REQUIRE(rejected_normalizer.next_ingest_sequence() == 0);

    record.flags.SetBadTsRecv();
    EquitiesMiniMbp1Normalizer accepted_normalizer(
        MarketDataStreamId("accepted"),
        FeedMode::HistoricalReplay
    );
    const auto events = require_events(
        accepted_normalizer.normalize(record, "AAPL")
    );
    const auto& top = std::get<TopOfBookEvent>(events.top_of_book.value());
    REQUIRE(top.header.quality.contains(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    ));
}

void test_invalid_trade_fields_are_rejected_atomically() {
    db::Mbp1Msg invalid_price = mbp1_record();
    invalid_price.action = db::action::Trade;
    invalid_price.price = db::kUndefPrice;
    invalid_price.size = 1;
    require_atomic_rejection(
        invalid_price,
        EquitiesMiniMbp1ErrorCode::InvalidTradePrice
    );

    db::Mbp1Msg invalid_size = mbp1_record();
    invalid_size.action = db::action::Trade;
    invalid_size.price = 100'000'000'000;
    invalid_size.size = 0;
    require_atomic_rejection(
        invalid_size,
        EquitiesMiniMbp1ErrorCode::InvalidTradeQuantity
    );
}

void test_ingest_sequence_exhaustion_is_atomic() {
    constexpr std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();

    EquitiesMiniMbp1Normalizer quote_normalizer(
        MarketDataStreamId("quote"),
        FeedMode::HistoricalReplay,
        maximum
    );
    require_events(quote_normalizer.normalize(mbp1_record(), "AAPL"));
    REQUIRE(!quote_normalizer.next_ingest_sequence().has_value());
    require_error(
        quote_normalizer.normalize(mbp1_record(), "AAPL"),
        EquitiesMiniMbp1ErrorCode::IngestSequenceExhausted
    );

    db::Mbp1Msg trade = mbp1_record();
    trade.action = db::action::Trade;
    trade.price = 100'000'000'000;
    trade.size = 1;
    EquitiesMiniMbp1Normalizer trade_normalizer(
        MarketDataStreamId("trade"),
        FeedMode::HistoricalReplay,
        maximum
    );
    require_error(
        trade_normalizer.normalize(trade, "AAPL"),
        EquitiesMiniMbp1ErrorCode::IngestSequenceExhausted
    );
    REQUIRE(trade_normalizer.next_ingest_sequence() == maximum);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"quote normalization", test_quote_normalizes_to_validated_top_of_book},
        {"trade and top ordering", test_trade_emits_trade_before_top_of_book},
        {"trade side mapping", test_trade_side_mapping},
        {"live feed quality", test_live_mode_does_not_set_replay_quality},
        {"provider quality flags", test_provider_quality_flags_are_mapped},
        {"crossed book quality", test_crossed_book_is_preserved_and_flagged},
        {"empty book sides", test_empty_book_sides_become_absent_levels},
        {"invalid book levels", test_invalid_book_levels_are_rejected_atomically},
        {"provider identity", test_provider_identity_fields_are_rejected_atomically},
        {
            "nonzero update depth",
            test_nonzero_update_depth_still_emits_top_of_book,
        },
        {"unknown provider values", test_unknown_provider_values_are_rejected_atomically},
        {"invalid timestamps", test_invalid_timestamps_are_rejected_atomically},
        {
            "timestamp inversion",
            test_timestamp_inversion_requires_provider_quality_flag,
        },
        {"invalid trade fields", test_invalid_trade_fields_are_rejected_atomically},
        {"ingest sequence exhaustion", test_ingest_sequence_exhaustion_is_atomic},
    });
}
