#include "chronos/alpaca/alpaca_credentials.hpp"
#include "chronos/alpaca/alpaca_normalizer.hpp"
#include "chronos/alpaca/alpaca_protocol.hpp"
#include "chronos/alpaca/alpaca_subscription.hpp"
#include "chronos/alpaca/alpaca_timestamp.hpp"
#include "chronos/alpaca/alpaca_websocket_transport.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::InstrumentId;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::Price;
using chronos::Quantity;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::ValidatedMarketEvent;
using namespace chronos::alpaca_adapter;

constexpr std::string_view kStream = "alpaca-iex-test";
// 2026-09-28T13:30:00Z
constexpr std::int64_t kOpen = 1'790'602'200'000'000'000;
// Obviously fake credentials; no real value appears in any test.
constexpr std::string_view kFakeKeyId = "FAKEKEYID000000TEST";
constexpr std::string_view kFakeSecret = "fake-secret-value-for-tests-only";

std::vector<AlpacaMessage> decode(std::string_view frame) {
    auto result = decode_alpaca_frame(frame);
    if (const auto* error = std::get_if<AlpacaDecodeError>(&result)) {
        throw chronos::test::TestFailure("unexpected decode error: " + error->message);
    }
    return std::get<std::vector<AlpacaMessage>>(std::move(result));
}

std::string decode_error(std::string_view frame) {
    auto result = decode_alpaca_frame(frame);
    REQUIRE(std::holds_alternative<AlpacaDecodeError>(result));
    return std::get<AlpacaDecodeError>(result).message;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

AlpacaSubscription subscription(std::vector<std::string> symbols = {"AAPL", "SPY"}) {
    std::vector<InstrumentId> instruments;
    for (auto& symbol : symbols) {
        instruments.emplace_back(std::move(symbol));
    }
    auto created = AlpacaSubscription::create(instruments);
    REQUIRE(std::holds_alternative<AlpacaSubscription>(created));
    return std::get<AlpacaSubscription>(std::move(created));
}

AlpacaNormalizer normalizer() {
    return AlpacaNormalizer(MarketDataStreamId(std::string(kStream)), subscription());
}

AlpacaQuote quote(
    std::string bid_price = "227.34",
    std::string bid_size = "300",
    std::string ask_price = "227.36",
    std::string ask_size = "100"
) {
    return AlpacaQuote{
        .symbol = "AAPL",
        .bid_exchange = "V",
        .bid_price = std::move(bid_price),
        .bid_size = std::move(bid_size),
        .ask_exchange = "V",
        .ask_price = std::move(ask_price),
        .ask_size = std::move(ask_size),
        .timestamp = "2026-09-28T13:30:00.123456789Z",
        .conditions = {"R"},
        .tape = "C",
    };
}

AlpacaTrade trade(std::string price = "227.35", std::string size = "57") {
    return AlpacaTrade{
        .symbol = "SPY",
        .trade_id = "52983525029461",
        .exchange = "V",
        .price = std::move(price),
        .size = std::move(size),
        .timestamp = "2026-09-28T13:30:00.5Z",
        .conditions = {"@", "I"},
        .tape = "B",
    };
}

Timestamp received(std::int64_t offset_nanoseconds = 200'000'000) {
    return Timestamp::from_nanoseconds(kOpen + offset_nanoseconds);
}

std::vector<ValidatedMarketEvent> normalize_ok(
    const AlpacaNormalizer& normalizer,
    std::vector<AlpacaDataMessage> messages,
    Timestamp at = received(),
    std::uint64_t first_sequence = 0
) {
    auto result = normalizer.normalize(messages, at, first_sequence);
    if (const auto* error = std::get_if<AlpacaNormalizationError>(&result)) {
        throw chronos::test::TestFailure("unexpected normalization error: " + error->message);
    }
    return std::get<std::vector<ValidatedMarketEvent>>(std::move(result));
}

std::string normalize_error(const AlpacaNormalizer& normalizer, AlpacaDataMessage message) {
    auto result = normalizer.normalize({std::move(message)}, received(), 0);
    REQUIRE(std::holds_alternative<AlpacaNormalizationError>(result));
    return std::get<AlpacaNormalizationError>(result).message;
}

const TopOfBookEvent& top_of_book(const ValidatedMarketEvent& event) {
    REQUIRE(std::holds_alternative<TopOfBookEvent>(event.value()));
    return std::get<TopOfBookEvent>(event.value());
}

// --- wire format -----------------------------------------------------------

void test_decodes_control_messages() {
    auto connected = decode(R"([{"T":"success","msg":"connected"}])");
    REQUIRE(connected.size() == 1U);
    REQUIRE(std::get<AlpacaSuccess>(connected[0]).message == "connected");
    REQUIRE(is_control_message(connected[0]));

    auto error = decode(R"([{"T":"error","code":402,"msg":"auth failed"}])");
    REQUIRE(std::get<AlpacaErrorMessage>(error[0]).code == 402);
    REQUIRE(std::get<AlpacaErrorMessage>(error[0]).message == "auth failed");

    auto subscribed = decode(
        R"([{"T":"subscription","trades":["AAPL"],"quotes":["AAPL","SPY"],)"
        R"("bars":[],"updatedBars":[],"dailyBars":[],"statuses":[],"lulds":[],)"
        R"("corrections":["AAPL"],"cancelErrors":["AAPL"]}])"
    );
    const auto& acknowledgement = std::get<AlpacaSubscriptionMessage>(subscribed[0]);
    REQUIRE((acknowledgement.trades == std::vector<std::string>{"AAPL"}));
    REQUIRE((acknowledgement.quotes == std::vector<std::string>{"AAPL", "SPY"}));
}

void test_decodes_quotes_and_trades_with_exact_number_text() {
    auto messages = decode(
        R"([{"T":"q","S":"AAPL","bx":"V","bp":227.340000001,"bs":300,"ax":"V",)"
        R"("ap":227.36,"as":1,"c":["R"],"z":"C","t":"2026-09-28T13:30:00.123456789Z"},)"
        R"({"T":"t","S":"SPY","i":52983525029461,"x":"V","p":655.1,"s":57,)"
        R"("c":["@","I"],"z":"B","t":"2026-09-28T13:30:00.5Z"}])"
    );
    REQUIRE(messages.size() == 2U);
    const auto& q = std::get<AlpacaQuote>(messages[0]);
    REQUIRE(q.symbol == "AAPL");
    // A value double cannot hold exactly keeps its exact text.
    REQUIRE(q.bid_price == "227.340000001");
    REQUIRE(q.bid_size == "300");
    REQUIRE(q.ask_price == "227.36");
    REQUIRE(q.ask_size == "1");
    REQUIRE(q.bid_exchange == std::optional<std::string>("V"));
    REQUIRE((q.conditions == std::vector<std::string>{"R"}));
    REQUIRE(q.tape == std::optional<std::string>("C"));
    const auto& t = std::get<AlpacaTrade>(messages[1]);
    REQUIRE(t.trade_id == "52983525029461");
    REQUIRE(t.price == "655.1");
    REQUIRE(t.size == "57");
    REQUIRE(!is_control_message(messages[1]));

    // Integers beyond 64 bits and exponents are kept verbatim for the
    // normalizer to judge; nlohmann's double is never read.
    auto large = decode(
        R"([{"T":"q","S":"AAPL","bp":123456789012345678901234567890,"bs":1e2,)"
        R"("ap":1.5E+2,"as":0,"t":"x"}])"
    );
    const auto& unusual = std::get<AlpacaQuote>(large[0]);
    REQUIRE(unusual.bid_price == "123456789012345678901234567890");
    REQUIRE(unusual.bid_size == "1e2");
    REQUIRE(unusual.ask_price == "1.5E+2");
}

void test_ignores_unknown_fields_and_documented_side_channels() {
    auto messages = decode(
        R"([{"T":"q","S":"AAPL","bx":"V","bp":1,"bs":1,"ax":"V","ap":2,"as":1,)"
        R"("t":"2026-09-28T13:30:00Z","future":{"nested":[1,{"deep":true}]},)"
        R"("flags":[1,2],"note":null},)"
        R"({"T":"c","S":"AAPL","oi":1,"ci":2},{"T":"x","S":"AAPL","i":1},)"
        R"({"T":"b","S":"AAPL"},{"T":"s","S":"AAPL"},{"T":"l","S":"AAPL"},)"
        R"({"T":"d"},{"T":"u"},{"T":"i"}])"
    );
    REQUIRE(messages.size() == 9U);
    REQUIRE(std::holds_alternative<AlpacaQuote>(messages[0]));
    REQUIRE(std::get<AlpacaQuote>(messages[0]).conditions.empty());
    REQUIRE(!std::get<AlpacaQuote>(messages[0]).tape.has_value());
    for (std::size_t index = 1; index < messages.size(); ++index) {
        REQUIRE(std::holds_alternative<AlpacaIgnoredMessage>(messages[index]));
    }
    REQUIRE(std::get<AlpacaIgnoredMessage>(messages[1]).type == "c");
    REQUIRE(decode("[]").empty());
}

void test_rejects_malformed_frames_deliberately() {
    REQUIRE(contains(decode_error("not json"), "not valid JSON"));
    REQUIRE(contains(decode_error(R"({"T":"success"})"), "JSON array"));
    REQUIRE(contains(decode_error(R"("text")"), "JSON array"));
    REQUIRE(contains(decode_error(R"([1])"), "message 1 is not a JSON object"));
    REQUIRE(contains(decode_error(R"([[{}]])"), "message 1 is not a JSON object"));
    REQUIRE(contains(decode_error(R"([{"T":"q"}] trailing)"), "not valid JSON"));
    REQUIRE(contains(decode_error(R"([{"msg":"x"}])"), "field \"T\" is missing"));
    REQUIRE(contains(decode_error(R"([{"T":5}])"), "field \"T\" must be a string"));
    REQUIRE(contains(decode_error(R"([{"T":""}])"), "must not be empty"));
    REQUIRE(contains(decode_error(R"([{"T":"zz"}])"), "unknown type \"zz\""));
    REQUIRE(contains(
        decode_error(R"([{"T":"success","msg":"a","msg":"b"}])"),
        "repeats the field \"msg\""
    ));
    REQUIRE(contains(decode_error(R"([{"T":"error","code":4.5,"msg":"x"}])"), "integer"));
    REQUIRE(contains(decode_error(R"([{"T":"error","code":"402","msg":"x"}])"), "number"));
    REQUIRE(contains(decode_error(R"([{"T":"subscription","trades":[]}])"), "\"quotes\" is missing"));
    REQUIRE(contains(
        decode_error(R"([{"T":"subscription","trades":[1],"quotes":[]}])"),
        "array of strings"
    ));

    const std::string quote_fields =
        R"("S":"AAPL","bx":"V","bp":1,"bs":1,"ax":"V","ap":2,"as":1,"t":"2026-09-28T13:30:00Z")";
    for (const std::string_view required : {"\"S\"", "\"bp\"", "\"bs\"", "\"ap\"", "\"as\"", "\"t\""}) {
        std::string fields = quote_fields;
        const auto at = fields.find(required);
        const auto end = fields.find(',', at);
        fields.erase(at, end == std::string::npos ? std::string::npos : end - at + 1);
        if (!fields.empty() && fields.back() == ',') {
            fields.pop_back();
        }
        const std::string error = decode_error("[{\"T\":\"q\"," + fields + "}]");
        REQUIRE(contains(error, "message 1 (type \"q\")"));
        REQUIRE(contains(error, "is missing"));
    }
    REQUIRE(contains(
        decode_error(R"([{"T":"q","S":"AAPL","bp":"1","bs":1,"ap":2,"as":1,"t":"x"}])"),
        "field \"bp\" must be a number"
    ));
    REQUIRE(contains(
        decode_error(R"([{"T":"t","S":"SPY","i":1,"x":"V","p":1,"s":1,"t":7}])"),
        "field \"t\" must be a string"
    ));
    REQUIRE(contains(
        decode_error(R"([{"T":"t","S":"SPY","i":1,"x":"V","p":1,"s":1,"t":"x","c":"@"}])"),
        "field \"c\" must be an array of strings"
    ));
    // A second message is named, and its content is not repeated.
    const std::string second = decode_error(
        R"([{"T":"success","msg":"ok"},{"T":"q","S":"AAPL","bp":1}])"
    );
    REQUIRE(contains(second, "message 2"));
    REQUIRE(!contains(second, "AAPL"));
}

void test_printable_excerpt_bounds_diagnostic_text() {
    REQUIRE(printable_excerpt("plain") == "plain");
    REQUIRE(printable_excerpt(std::string("a\nb\x01", 4)) == "a?b?");
    REQUIRE(printable_excerpt(std::string(200, 'x')).size() == 123U);
}

// --- timestamps -------------------------------------------------------------

Timestamp timestamp_ok(std::string_view text) {
    auto result = parse_rfc3339_timestamp(text);
    if (const auto* error = std::get_if<TimestampParseError>(&result)) {
        throw chronos::test::TestFailure("unexpected timestamp error: " + error->message);
    }
    return std::get<Timestamp>(result);
}

void require_timestamp_error(std::string_view text, std::string_view reason) {
    auto result = parse_rfc3339_timestamp(text);
    REQUIRE(std::holds_alternative<TimestampParseError>(result));
    REQUIRE(contains(std::get<TimestampParseError>(result).message, reason));
}

void test_parses_rfc3339_with_nanosecond_precision() {
    REQUIRE(timestamp_ok("1970-01-01T00:00:00Z").nanoseconds() == 0);
    REQUIRE(timestamp_ok("2026-09-28T13:30:00Z").nanoseconds() == kOpen);
    REQUIRE(timestamp_ok("2026-09-28T13:30:00.123456789Z").nanoseconds() ==
        kOpen + 123'456'789);
    REQUIRE(timestamp_ok("2026-09-28T13:30:00.5Z").nanoseconds() == kOpen + 500'000'000);
    REQUIRE(timestamp_ok("2026-09-28T13:30:00.000000001Z").nanoseconds() == kOpen + 1);
    REQUIRE(timestamp_ok("2026-09-28t13:30:00z").nanoseconds() == kOpen);
    // Offsets are applied exactly.
    REQUIRE(timestamp_ok("2026-09-28T09:30:00-04:00").nanoseconds() == kOpen);
    REQUIRE(timestamp_ok("2026-09-28T19:00:00.25+05:30").nanoseconds() ==
        kOpen + 250'000'000);
    REQUIRE(timestamp_ok("2024-02-29T00:00:00Z").nanoseconds() ==
        1'709'164'800'000'000'000);
    REQUIRE(timestamp_ok("2262-04-11T23:47:16.854775807Z").nanoseconds() ==
        std::numeric_limits<std::int64_t>::max());
}

void test_rejects_malformed_and_out_of_range_timestamps() {
    require_timestamp_error("2026-09-28T13:30:00.1234567891Z", "nine fractional");
    require_timestamp_error("2026-09-28T13:30:00.Z", "fractional digits");
    require_timestamp_error("2026-09-28T13:30:00", "UTC offset");
    require_timestamp_error("2026-09-28 13:30:00Z", "YYYY-MM-DD");
    require_timestamp_error("2026-9-28T13:30:00Z", "YYYY-MM-DD");
    require_timestamp_error("2026-09-28T13:30Z", "YYYY-MM-DD");
    require_timestamp_error("", "YYYY-MM-DD");
    require_timestamp_error("2026-02-29T00:00:00Z", "calendar date");
    require_timestamp_error("2026-13-01T00:00:00Z", "calendar date");
    require_timestamp_error("2026-09-31T00:00:00Z", "calendar date");
    require_timestamp_error("2026-09-28T24:00:00Z", "time of day");
    require_timestamp_error("2026-09-28T13:60:00Z", "time of day");
    require_timestamp_error("2016-12-31T23:59:60Z", "leap seconds");
    require_timestamp_error("2026-09-28T13:30:00+5:30", "UTC offset");
    require_timestamp_error("2026-09-28T13:30:00+24:00", "UTC offset");
    require_timestamp_error("2026-09-28T13:30:00Zjunk", "after the UTC offset");
    require_timestamp_error("1969-12-31T23:59:59Z", "before the Unix epoch");
    require_timestamp_error("2262-04-11T23:47:16.854775808Z", "signed nanosecond range");
    require_timestamp_error("9999-12-31T23:59:59Z", "signed nanosecond range");
}

// --- normalization ---------------------------------------------------------

void test_maps_a_quote_exactly_to_top_of_book() {
    const AlpacaNormalizer normalizer = ::normalizer();
    auto events = normalize_ok(normalizer, {quote("227.340000001", "300", "227.36", "100")}, received(), 41);
    REQUIRE(events.size() == 1U);
    const TopOfBookEvent& book = top_of_book(events[0]);
    REQUIRE(book.header.instrument == InstrumentId("AAPL"));
    REQUIRE(book.header.stream == MarketDataStreamId(std::string(kStream)));
    REQUIRE(book.header.venue == chronos::VenueId("IEX"));
    REQUIRE(book.header.event_timestamp.nanoseconds() == kOpen + 123'456'789);
    REQUIRE(book.header.receive_timestamp == received());
    REQUIRE(book.header.ingest_sequence == 41U);
    REQUIRE(!book.header.venue_sequence.has_value());
    REQUIRE(book.header.quality.contains(MarketDataQualityFlag::SourceSpecific));
    REQUIRE(!book.header.quality.contains(MarketDataQualityFlag::Replay));
    REQUIRE(!book.header.quality.is_suspect());
    REQUIRE(book.bid->price.raw() == 227'340'000'001);
    REQUIRE(book.bid->quantity == Quantity::parse("300"));
    REQUIRE(book.ask->price == Price::parse("227.36"));
    REQUIRE(book.ask->quantity == Quantity::parse("100"));
}

void test_maps_a_trade_exactly() {
    AlpacaNormalizer normalizer = ::normalizer();
    auto events = normalize_ok(normalizer, {trade("655.123456789", "57")});
    REQUIRE(std::holds_alternative<TradeEvent>(events[0].value()));
    const auto& print = std::get<TradeEvent>(events[0].value());
    REQUIRE(print.header.instrument == InstrumentId("SPY"));
    REQUIRE(print.header.venue == chronos::VenueId("IEX"));
    REQUIRE(print.header.event_timestamp.nanoseconds() == kOpen + 500'000'000);
    REQUIRE(print.price.raw() == 655'123'456'789);
    REQUIRE(print.quantity == Quantity::parse("57"));
    REQUIRE(print.aggressor_side == chronos::AggressorSide::Unknown);
    REQUIRE(print.header.quality.contains(MarketDataQualityFlag::SourceSpecific));
}

void test_rejects_inexact_or_out_of_range_numbers() {
    AlpacaNormalizer normalizer = ::normalizer();
    // Nine decimal places is the most a Chronos price holds.
    REQUIRE(!normalize_ok(normalizer, {quote("0.000000001", "1", "0.000000002", "1")}).empty());
    REQUIRE(contains(
        normalize_error(normalizer, quote("227.3400000001")),
        "bid price 227.3400000001 is not exact at nine decimal places"
    ));
    REQUIRE(contains(normalize_error(normalizer, quote("2.2734e2")), "exponent notation"));
    REQUIRE(contains(normalize_error(normalizer, quote("227.34", "3E2")), "whole, non-negative"));
    REQUIRE(contains(normalize_error(normalizer, quote("227.34", "300.5")), "whole, non-negative"));
    REQUIRE(contains(normalize_error(normalizer, quote("227.34", "-3")), "whole, non-negative"));
    REQUIRE(contains(
        normalize_error(normalizer, quote("123456789012345678901234567890")),
        "exceeds the Chronos price range"
    ));
    // The largest price the fixed-point range holds, and one beyond it.
    REQUIRE(!normalize_ok(
        normalizer,
        {quote("1", "1", "9223372036.854775807", "1")}
    ).empty());
    REQUIRE(contains(
        normalize_error(normalizer, quote("1", "1", "9223372036.854775808", "1")),
        "ask price 9223372036.854775808 exceeds the Chronos price range"
    ));
    REQUIRE(!normalize_ok(normalizer, {quote("1", "92233720368", "2", "1")}).empty());
    REQUIRE(contains(
        normalize_error(normalizer, quote("1", "92233720369", "2", "1")),
        "exceeds the Chronos quantity range"
    ));
    REQUIRE(contains(normalize_error(normalizer, trade("0", "5")), "must be positive"));
    REQUIRE(contains(normalize_error(normalizer, trade("-1", "5")), "must be positive"));
    REQUIRE(contains(normalize_error(normalizer, trade("1", "0")), "must be positive"));
    REQUIRE(contains(normalize_error(normalizer, trade("1.5", "0.5")), "whole, non-negative"));
}

void test_empty_sides_and_inconsistent_levels() {
    AlpacaNormalizer normalizer = ::normalizer();
    auto no_bid = quote("0", "0", "227.36", "100");
    no_bid.bid_exchange.reset();
    auto events = normalize_ok(normalizer, {no_bid});
    REQUIRE(!top_of_book(events[0]).bid.has_value());
    REQUIRE(top_of_book(events[0]).ask.has_value());

    auto events_empty = normalize_ok(normalizer, {quote("0.0", "0", "0", "0")});
    REQUIRE(!top_of_book(events_empty[0]).bid.has_value());
    REQUIRE(!top_of_book(events_empty[0]).ask.has_value());

    REQUIRE(contains(
        normalize_error(normalizer, quote("227.34", "0")),
        "bid must be empty (price and size 0) or have a positive price and size"
    ));
    REQUIRE(contains(normalize_error(normalizer, quote("0", "100")), "bid must be empty"));
    REQUIRE(contains(
        normalize_error(normalizer, quote("227.34", "1", "-227.36", "1")),
        "ask must be empty"
    ));
}

void test_crossed_quotes_are_flagged_and_locked_quotes_are_not() {
    AlpacaNormalizer normalizer = ::normalizer();
    auto crossed = normalize_ok(normalizer, {quote("227.37", "1", "227.36", "1")});
    REQUIRE(top_of_book(crossed[0]).header.quality.contains(
        MarketDataQualityFlag::PotentiallyInvalidBook
    ));
    REQUIRE(top_of_book(crossed[0]).header.quality.is_suspect());
    auto locked = normalize_ok(normalizer, {quote("227.36", "1", "227.36", "1")});
    REQUIRE(!top_of_book(locked[0]).header.quality.is_suspect());
}

void test_receipt_before_the_event_time_is_flagged_not_changed() {
    AlpacaNormalizer normalizer = ::normalizer();
    const Timestamp early = received(100'000'000);
    auto events = normalize_ok(normalizer, {quote()}, early);
    const TopOfBookEvent& book = top_of_book(events[0]);
    REQUIRE(book.header.receive_timestamp == early);
    REQUIRE(book.header.event_timestamp.nanoseconds() == kOpen + 123'456'789);
    REQUIRE(book.header.quality.contains(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    ));
    REQUIRE(book.header.quality.is_suspect());
}

void test_requires_iex_lineage_and_subscribed_symbols() {
    AlpacaNormalizer normalizer = ::normalizer();
    auto other_venue = quote();
    other_venue.ask_exchange = "Q";
    REQUIRE(contains(
        normalize_error(normalizer, other_venue),
        "ask is from exchange \"Q\", not IEX"
    ));
    auto missing_venue = quote();
    missing_venue.bid_exchange.reset();
    REQUIRE(contains(normalize_error(normalizer, missing_venue), "bid has no exchange code"));
    auto other_trade = trade();
    other_trade.exchange = "D";
    REQUIRE(contains(normalize_error(normalizer, other_trade), "not IEX"));

    auto unsubscribed = quote();
    unsubscribed.symbol = "MSFT";
    REQUIRE(contains(
        normalize_error(normalizer, unsubscribed),
        "quote for MSFT: the symbol is not subscribed"
    ));
    auto bad_time = trade();
    bad_time.timestamp = "2026-09-28T13:30:00";
    REQUIRE(contains(normalize_error(normalizer, bad_time), "field \"t\""));
}

void test_frames_are_atomic_ordered_and_contiguous() {
    const AlpacaNormalizer normalizer = ::normalizer();
    auto events = normalize_ok(normalizer, {quote(), trade(), quote("227.35")}, received(), 10);
    REQUIRE(events.size() == 3U);
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& header = chronos::market_event_header(events[index].value());
        REQUIRE(header.ingest_sequence == 10U + index);
        REQUIRE(header.receive_timestamp == received());
    }
    REQUIRE(std::holds_alternative<TopOfBookEvent>(events[0].value()));
    REQUIRE(std::holds_alternative<TradeEvent>(events[1].value()));
    REQUIRE(top_of_book(events[2]).bid->price == Price::parse("227.35"));

    // A rejected message yields no event, even after valid ones.
    auto rejected = normalizer.normalize({quote(), trade("0")}, received(), 13);
    REQUIRE(std::holds_alternative<AlpacaNormalizationError>(rejected));
    REQUIRE(normalize_ok(normalizer, {}, received(), 13).empty());
    auto next = normalize_ok(normalizer, {trade()}, received(), 13);
    REQUIRE(chronos::market_event_header(next[0].value()).ingest_sequence == 13U);
}

void test_ingest_sequence_exhaustion() {
    constexpr std::uint64_t kLast = std::numeric_limits<std::uint64_t>::max();
    const AlpacaNormalizer normalizer = ::normalizer();
    // Two sequences remain from kLast − 1; three messages cannot fit.
    auto too_many = normalizer.normalize({quote(), quote(), quote()}, received(), kLast - 1);
    REQUIRE(std::holds_alternative<AlpacaNormalizationError>(too_many));
    REQUIRE(contains(std::get<AlpacaNormalizationError>(too_many).message, "exhaust"));

    auto last_two = normalize_ok(normalizer, {quote(), quote()}, received(), kLast - 1);
    REQUIRE(chronos::market_event_header(last_two[1].value()).ingest_sequence == kLast);
    auto last = normalize_ok(normalizer, {quote()}, received(), kLast);
    REQUIRE(chronos::market_event_header(last[0].value()).ingest_sequence == kLast);
    auto beyond = normalizer.normalize({quote(), quote()}, received(), kLast);
    REQUIRE(std::holds_alternative<AlpacaNormalizationError>(beyond));
}

// --- subscription ----------------------------------------------------------

void test_subscription_dedupes_and_limits_symbols() {
    auto deduped = subscription({"SPY", "AAPL", "SPY", "AAPL", "MSFT"});
    REQUIRE((deduped.instruments() == std::vector<InstrumentId>{
        InstrumentId("SPY"), InstrumentId("AAPL"), InstrumentId("MSFT"),
    }));
    REQUIRE(deduped.subscribe_message() ==
        R"({"action":"subscribe","trades":["SPY","AAPL","MSFT"],"quotes":["SPY","AAPL","MSFT"]})");
    REQUIRE(deduped.find("AAPL") != nullptr);
    REQUIRE(deduped.find("aapl") == nullptr);

    std::vector<InstrumentId> thirty;
    for (int index = 0; index < 30; ++index) {
        thirty.emplace_back("S" + std::to_string(index));
    }
    thirty.emplace_back("S0");
    REQUIRE(std::holds_alternative<AlpacaSubscription>(AlpacaSubscription::create(thirty)));
    thirty.emplace_back("S30");
    auto too_many = AlpacaSubscription::create(thirty);
    REQUIRE(std::holds_alternative<AlpacaSubscriptionError>(too_many));
    REQUIRE(contains(std::get<AlpacaSubscriptionError>(too_many).message, "at most 30 symbols"));

    REQUIRE(std::holds_alternative<AlpacaSubscriptionError>(AlpacaSubscription::create({})));
    for (const std::string_view invalid : {"aapl", "BRK/B", "A B", "\"X", "ABCDEFGHIJKLMNOPQ"}) {
        REQUIRE(std::holds_alternative<AlpacaSubscriptionError>(
            AlpacaSubscription::create({InstrumentId(std::string(invalid))})
        ));
    }
    REQUIRE(std::holds_alternative<AlpacaSubscription>(
        AlpacaSubscription::create({InstrumentId("BRK.B")})
    ));
}

void test_subscription_acknowledgement_must_match_exactly() {
    const AlpacaSubscription requested = subscription();
    REQUIRE(!requested.acknowledgement_mismatch(AlpacaSubscriptionMessage{
        .trades = {"SPY", "AAPL"},
        .quotes = {"AAPL", "SPY"},
    }));
    auto missing = requested.acknowledgement_mismatch(AlpacaSubscriptionMessage{
        .trades = {"AAPL", "SPY"},
        .quotes = {"AAPL"},
    });
    REQUIRE(missing.has_value());
    REQUIRE(contains(*missing, "did not subscribe quotes for SPY"));
    auto extra = requested.acknowledgement_mismatch(AlpacaSubscriptionMessage{
        .trades = {"AAPL", "SPY", "TSLA"},
        .quotes = {"AAPL", "SPY"},
    });
    REQUIRE(extra.has_value());
    REQUIRE(contains(*extra, "unrequested symbol TSLA"));
}

// --- credentials -----------------------------------------------------------

AlpacaCredentials::EnvironmentLookup environment(
    std::map<std::string, std::string, std::less<>> values
) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

std::string credentials_error(std::map<std::string, std::string, std::less<>> values) {
    auto result = AlpacaCredentials::from_environment(environment(std::move(values)));
    REQUIRE(std::holds_alternative<AlpacaCredentialsError>(result));
    return std::get<AlpacaCredentialsError>(result).message;
}

void test_missing_credentials_name_the_variable_only() {
    REQUIRE(credentials_error({}) == "APCA_API_KEY_ID is not set");
    REQUIRE(credentials_error({{"APCA_API_KEY_ID", std::string(kFakeKeyId)}}) ==
        "APCA_API_SECRET_KEY is not set");
    REQUIRE(credentials_error({{"APCA_API_KEY_ID", ""}}) == "APCA_API_KEY_ID is empty");

    const std::string padded = credentials_error({
        {"APCA_API_KEY_ID", std::string(kFakeKeyId)},
        {"APCA_API_SECRET_KEY", std::string(kFakeSecret) + "\n"},
    });
    REQUIRE(contains(padded, "APCA_API_SECRET_KEY contains whitespace"));
    REQUIRE(!contains(padded, kFakeSecret));
    REQUIRE(!contains(padded, kFakeKeyId));
}

void test_credentials_build_auth_and_redact_themselves() {
    auto loaded = AlpacaCredentials::from_environment(environment({
        {"APCA_API_KEY_ID", std::string(kFakeKeyId)},
        {"APCA_API_SECRET_KEY", std::string(kFakeSecret)},
    }));
    REQUIRE(std::holds_alternative<AlpacaCredentials>(loaded));
    const AlpacaCredentials credentials = std::get<AlpacaCredentials>(std::move(loaded));
    REQUIRE(credentials.authentication_message() ==
        std::string(R"({"action":"auth","key":")") + std::string(kFakeKeyId) +
        R"(","secret":")" + std::string(kFakeSecret) + R"("})");

    const std::string redacted = credentials.redact(
        "echo " + std::string(kFakeSecret) + " and " + std::string(kFakeKeyId) +
        " and " + std::string(kFakeSecret)
    );
    REQUIRE(redacted == "echo [REDACTED] and [REDACTED] and [REDACTED]");
    REQUIRE(credentials.redact("nothing secret") == "nothing secret");

    std::string secret = "sensitive";
    wipe(secret);
    REQUIRE(secret.empty());
}

void test_process_environment_reads_real_variables_without_values_here() {
    const auto lookup = AlpacaCredentials::process_environment();
    REQUIRE(!lookup("CHRONOS_TEST_VARIABLE_THAT_IS_NEVER_SET").has_value());
}

void test_only_the_iex_market_data_stream_is_reachable() {
    REQUIRE(kAlpacaIexStreamUrl == "wss://stream.data.alpaca.markets/v2/iex");
    REQUIRE(make_alpaca_iex_transport() != nullptr);
    REQUIRE(make_loopback_websocket_transport("ws://127.0.0.1:1/v2/iex") != nullptr);
    for (const std::string_view url : {
             "wss://api.alpaca.markets/v2/orders",
             "wss://paper-api.alpaca.markets/stream",
             "wss://stream.data.alpaca.markets/v2/sip",
             "ws://127.0.0.2:1/",
             "ws://localhost.example.com:1/",
         }) {
        chronos::test::require_throws<std::invalid_argument>([url] {
            static_cast<void>(make_loopback_websocket_transport(std::string(url)));
        }, __LINE__);
    }
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"decodes control messages", test_decodes_control_messages},
        {"decodes quotes and trades with exact number text",
         test_decodes_quotes_and_trades_with_exact_number_text},
        {"ignores unknown fields and documented side channels",
         test_ignores_unknown_fields_and_documented_side_channels},
        {"rejects malformed frames deliberately", test_rejects_malformed_frames_deliberately},
        {"printable excerpt bounds diagnostic text", test_printable_excerpt_bounds_diagnostic_text},
        {"parses RFC 3339 with nanosecond precision", test_parses_rfc3339_with_nanosecond_precision},
        {"rejects malformed and out-of-range timestamps",
         test_rejects_malformed_and_out_of_range_timestamps},
        {"maps a quote exactly to top of book", test_maps_a_quote_exactly_to_top_of_book},
        {"maps a trade exactly", test_maps_a_trade_exactly},
        {"rejects inexact or out-of-range numbers", test_rejects_inexact_or_out_of_range_numbers},
        {"empty sides and inconsistent levels", test_empty_sides_and_inconsistent_levels},
        {"crossed quotes are flagged and locked quotes are not",
         test_crossed_quotes_are_flagged_and_locked_quotes_are_not},
        {"receipt before the event time is flagged, not changed",
         test_receipt_before_the_event_time_is_flagged_not_changed},
        {"requires IEX lineage and subscribed symbols",
         test_requires_iex_lineage_and_subscribed_symbols},
        {"frames are atomic, ordered, and contiguous", test_frames_are_atomic_ordered_and_contiguous},
        {"ingest sequence exhaustion", test_ingest_sequence_exhaustion},
        {"subscription dedupes and limits symbols", test_subscription_dedupes_and_limits_symbols},
        {"subscription acknowledgement must match exactly",
         test_subscription_acknowledgement_must_match_exactly},
        {"missing credentials name the variable only", test_missing_credentials_name_the_variable_only},
        {"credentials build auth and redact themselves",
         test_credentials_build_auth_and_redact_themselves},
        {"process environment lookup", test_process_environment_reads_real_variables_without_values_here},
        {"only the IEX market-data stream is reachable",
         test_only_the_iex_market_data_stream_is_reachable},
    });
}
