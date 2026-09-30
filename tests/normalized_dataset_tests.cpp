#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_catalog.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_quality.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/datasets/ndjson_file.hpp"
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data/paced_replay_source.hpp"
#include "chronos/market_replay.hpp"
#include "chronos/market_state.hpp"
#include "chronos/sha256.hpp"
#include "dataset_test_support.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace ds = chronos::datasets;
using chronos::AggressorSide;
using chronos::BookLevel;
using chronos::CurrencyCode;
using chronos::InstrumentDefinitionEvent;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketEvent;
using chronos::MarketReplay;
using chronos::MarketStatusEvent;
using chronos::Price;
using chronos::Quantity;
using chronos::Sha256;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::TradingStatus;
using chronos::ValidatedMarketEvent;
using chronos::VenueId;
using chronos::test::header;
using chronos::test::level;
using chronos::test::quote;
using chronos::test::read_file;
using chronos::test::sample_session;
using chronos::test::sample_source;
using chronos::test::TemporaryDirectory;
using chronos::test::trade;
using chronos::test::valid;
using chronos::test::write_file;

ds::DatasetErrorCode decode_error(std::string_view record) {
    auto decoded = ds::decode_market_event(record);
    REQUIRE(std::holds_alternative<ds::DatasetError>(decoded));
    return std::get<ds::DatasetError>(decoded).code;
}

ValidatedMarketEvent decoded(std::string_view record) {
    auto result = ds::decode_market_event(record);
    if (auto* error = std::get_if<ds::DatasetError>(&result)) {
        throw chronos::test::TestFailure("decode failed: " + error->field + ": " + error->message);
    }
    return std::get<ValidatedMarketEvent>(std::move(result));
}

// Every variant, every quality flag, both optional header fields, and every
// optional event field in both states.
std::vector<ValidatedMarketEvent> every_kind_of_event() {
    std::vector<ValidatedMarketEvent> events;
    MarketDataQuality all_flags;
    for (const MarketDataQualityFlag flag : ds::kQualityFlags) {
        all_flags = all_flags.with(flag);
    }
    auto with_venue = header("AAPL", 0, 5, 7, all_flags);
    with_venue.venue = VenueId("XNAS");
    with_venue.venue_sequence = 42;
    events.push_back(valid(TradeEvent{
        .header = with_venue,
        .price = Price::parse("189.1234"),
        .quantity = Quantity::parse("0.5"),
        .aggressor_side = AggressorSide::Sell,
    }));
    for (const AggressorSide side : {AggressorSide::Buy, AggressorSide::Unknown}) {
        events.push_back(valid(TradeEvent{
            .header = header("AAPL", 1, 5, 7),
            .price = Price::parse("1"),
            .quantity = Quantity::parse("1"),
            .aggressor_side = side,
        }));
    }
    events.push_back(quote(header("AAPL", 2, 5, 7), level("99", "1"), level("101", "2")));
    events.push_back(quote(header("AAPL", 2, 5, 7), level("99", "1"), std::nullopt));
    events.push_back(quote(header("AAPL", 2, 5, 7), std::nullopt, level("101", "2")));
    events.push_back(quote(header("AAPL", 2, 5, 7), std::nullopt, std::nullopt));
    events.push_back(quote(
        header("AAPL", 2, 5, 7, MarketDataQuality{}.with(MarketDataQualityFlag::PotentiallyInvalidBook)),
        level("102", "1"),
        level("101", "2")
    ));
    for (const TradingStatus trading_status : {
             TradingStatus::Unknown, TradingStatus::PreOpen, TradingStatus::Open,
             TradingStatus::Halted, TradingStatus::Closed,
         }) {
        events.push_back(valid(MarketStatusEvent{
            .header = header("AAPL", 3, 5, 7),
            .status = trading_status,
            .reason = "halt \"news\"\n\\ pending — ünïcödé",
        }));
    }
    for (const bool bounded : {true, false}) {
        events.push_back(valid(InstrumentDefinitionEvent{
            .header = header("AAPL", 4, 5, 7),
            .display_symbol = "AAPL",
            .currency = CurrencyCode("USD"),
            .minimum_price_increment = Price::parse("0.01"),
            .minimum_quantity_increment = Quantity::parse("1"),
            .effective_from = Timestamp::from_nanoseconds(0),
            .effective_until = bounded ? std::optional(Timestamp::from_nanoseconds(10)) : std::nullopt,
        }));
    }
    return events;
}

void test_every_event_variant_round_trips_exactly() {
    for (const auto& event : every_kind_of_event()) {
        const std::string record = chronos::test::encoded(event);
        REQUIRE(record.find('\n') == std::string::npos);
        REQUIRE(decoded(record) == event);
        REQUIRE(chronos::test::encoded(decoded(record)) == record);
    }
}

void test_the_record_format_is_pinned() {
    auto first = header("AAPL", 7, 1'000, 1'100);
    first.venue_sequence = 9;
    REQUIRE(chronos::test::encoded(quote(first, level("100.5", "3"), std::nullopt)) ==
        R"({"ask":null,"bid":{"price_raw":"100500000000","quantity_raw":"300000000"},)"
        R"("event_timestamp_ns":"1000","ingest_sequence":"7","instrument":"AAPL",)"
        R"("quality":["replay"],"receive_timestamp_ns":"1100","stream":"dataset-test-stream",)"
        R"("type":"top_of_book","venue":null,"venue_sequence":"9","version":"1"})");
    REQUIRE(chronos::test::encoded(trade(header("SPY", 0, 1, 2, MarketDataQuality{}), "1", "2")) ==
        R"({"aggressor_side":"buy","event_timestamp_ns":"1","ingest_sequence":"0",)"
        R"("instrument":"SPY","price_raw":"1000000000","quality":[],"quantity_raw":"200000000",)"
        R"("receive_timestamp_ns":"2","stream":"dataset-test-stream","type":"trade",)"
        R"("venue":null,"venue_sequence":null,"version":"1"})");
}

void test_boundary_values_round_trip() {
    constexpr std::int64_t kMaximum = std::numeric_limits<std::int64_t>::max();
    auto extreme = header("AAPL", std::numeric_limits<std::uint64_t>::max(), kMaximum, kMaximum);
    extreme.venue_sequence = std::numeric_limits<std::uint64_t>::max();
    const auto event = valid(TopOfBookEvent{
        .header = extreme,
        .bid = BookLevel{.price = Price::from_raw(1), .quantity = Quantity::from_raw(1)},
        .ask = BookLevel{.price = Price::from_raw(kMaximum), .quantity = Quantity::from_raw(kMaximum)},
    });
    const std::string record = chronos::test::encoded(event);
    REQUIRE(record.find("\"price_raw\":\"9223372036854775807\"") != std::string::npos);
    REQUIRE(record.find("\"ingest_sequence\":\"18446744073709551615\"") != std::string::npos);
    REQUIRE(decoded(record) == event);
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t position = text.find(from);
    REQUIRE(position != std::string::npos);
    return text.replace(position, from.size(), to);
}

void test_invalid_records_are_rejected() {
    const std::string record = chronos::test::encoded(
        quote(header("AAPL", 7, 1'000, 1'100), level("100.5", "3"), level("101", "1"))
    );
    static_cast<void>(decoded(record));
    using Code = ds::DatasetErrorCode;
    REQUIRE(decode_error("") == Code::MalformedJson);
    REQUIRE(decode_error(record.substr(0, record.size() - 1)) == Code::MalformedJson);
    REQUIRE(decode_error(record + "x") == Code::MalformedJson);
    REQUIRE(decode_error(replaced(record, "{\"ask\"", "{\"ask\":null,\"ask\"")) == Code::MalformedJson);
    REQUIRE(decode_error(replaced(record, "\"version\":\"1\"", "\"version\":\"1\",\"extra\":null")) ==
        Code::UnknownField);
    REQUIRE(decode_error(replaced(record, "\"venue\":null,", "")) == Code::MissingField);
    REQUIRE(decode_error(replaced(record, "\"version\":\"1\"", "\"version\":\"2\"")) ==
        Code::UnsupportedVersion);
    REQUIRE(decode_error(replaced(record, "\"version\":\"1\"", "\"version\":1")) == Code::MalformedJson);
    REQUIRE(decode_error(replaced(record, "\"top_of_book\"", "\"order_book\"")) == Code::UnknownEventType);
    // Decimal strings: one spelling, and it must fit.
    for (const std::string_view invalid : {"\"0100\"", "\"1.5\"", "\"+100\"", "\" 100\"", "\"1e3\"", "\"\""}) {
        REQUIRE(decode_error(replaced(record, "\"1000\"", invalid)) == Code::InvalidValue);
    }
    REQUIRE(decode_error(replaced(record, "\"100500000000\"", "\"9223372036854775808\"")) == Code::InvalidValue);
    REQUIRE(decode_error(replaced(record, "\"ingest_sequence\":\"7\"", "\"ingest_sequence\":\"-7\"")) ==
        Code::InvalidValue);
    // Enumerations are closed.
    REQUIRE(decode_error(replaced(record, "[\"replay\"]", "[\"stale\"]")) == Code::InvalidValue);
    const std::string trade_record = chronos::test::encoded(trade(header("AAPL", 0, 1, 2), "1", "1"));
    REQUIRE(decode_error(replaced(trade_record, "\"buy\"", "\"up\"")) == Code::InvalidValue);
    // Chronos validation still applies: a crossed book must be flagged,
    // prices must be positive, and timestamps must not be negative.
    REQUIRE(decode_error(replaced(record, "\"101000000000\"", "\"100000000000\"")) == Code::InvalidEvent);
    REQUIRE(decode_error(replaced(record, "\"100500000000\"", "\"0\"")) == Code::InvalidEvent);
    REQUIRE(decode_error(replaced(record, "\"event_timestamp_ns\":\"1000\"", "\"event_timestamp_ns\":\"-1\"")) ==
        Code::InvalidEvent);
    // One spelling of everything else too.
    REQUIRE(decode_error(replaced(record, "{\"ask\"", "{ \"ask\"")) == Code::NotCanonical);
    REQUIRE(decode_error(replaced(record, "\"AAPL\"", "\"\\u0041APL\"")) == Code::NotCanonical);
    REQUIRE(decode_error(replaced(record, "[\"replay\"]", "[\"replay\",\"replay\"]")) == Code::NotCanonical);
    REQUIRE(decode_error(replaced(record, "\"instrument\":\"AAPL\"", "\"instrument\":\"\"")) == Code::InvalidValue);
}

void test_text_json_cannot_hold_is_refused() {
    const auto event = valid(MarketStatusEvent{
        .header = header("AAPL", 0, 1, 2),
        .status = TradingStatus::Halted,
        .reason = std::string("bad \xff byte"),
    });
    auto record = ds::encode_market_event(event);
    REQUIRE(std::holds_alternative<ds::DatasetError>(record));
    REQUIRE(std::get<ds::DatasetError>(record).code == ds::DatasetErrorCode::UnrepresentableText);
}

void test_a_dataset_round_trips_and_verifies() {
    const TemporaryDirectory catalog;
    const auto events = sample_session();
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));
    REQUIRE(dataset.outcome == ds::PublicationOutcome::Published);
    REQUIRE(dataset.directory == catalog.path() / dataset.manifest.dataset_id);
    REQUIRE(chronos::is_sha256_hex(dataset.manifest.dataset_id));

    const std::string events_bytes = read_file(dataset.directory / ds::kEventsFileName);
    std::string expected_bytes;
    for (const auto& event : events) {
        expected_bytes += chronos::test::encoded(event) + "\n";
    }
    REQUIRE(events_bytes == expected_bytes);
    REQUIRE(dataset.manifest.events.sha256 == Sha256::hex(events_bytes));
    REQUIRE(dataset.manifest.summary.events == 6);
    REQUIRE(dataset.manifest.summary.by_instrument.at("AAPL") == 4);
    REQUIRE(dataset.manifest.summary.by_type.top_of_book == 4);
    REQUIRE(dataset.manifest.summary.earliest_event_timestamp == Timestamp::from_nanoseconds(990));
    REQUIRE(dataset.manifest.summary.latest_receive_timestamp == Timestamp::from_nanoseconds(1'250));

    const auto outcome = chronos::test::read_dataset(dataset.directory);
    REQUIRE(!outcome.error);
    REQUIRE(outcome.events == events);
    REQUIRE(outcome.end_sha256 == dataset.manifest.events.sha256);
    REQUIRE(std::holds_alternative<ds::DatasetManifest>(ds::verify_dataset(dataset.directory)));
    REQUIRE(std::holds_alternative<ds::DatasetManifest>(ds::verify_dataset(dataset.directory / "")));
    REQUIRE(!dataset.warning);

    // Published artifacts are read-only; the staging directory is gone.
    const auto permissions = std::filesystem::status(dataset.directory).permissions();
    REQUIRE((permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none);
    REQUIRE((std::filesystem::status(dataset.directory / ds::kEventsFileName).permissions() &
             std::filesystem::perms::owner_write) == std::filesystem::perms::none);
    REQUIRE(std::distance(std::filesystem::directory_iterator(catalog.path()),
                          std::filesystem::directory_iterator()) == 1);
}

void test_conversion_is_deterministic_and_identity_tracks_meaning() {
    const TemporaryDirectory first;
    const TemporaryDirectory second;
    const auto events = sample_session();
    const auto one = chronos::test::published(chronos::test::write_dataset(first.path(), events));
    const auto two = chronos::test::published(chronos::test::write_dataset(second.path(), events));
    REQUIRE(one.manifest.dataset_id == two.manifest.dataset_id);
    for (const std::string_view file : {ds::kEventsFileName, ds::kManifestFileName, ds::kQualityFileName}) {
        REQUIRE(read_file(one.directory / file) == read_file(two.directory / file));
    }

    // A changed event, a changed source, and a changed stream each change
    // the identity.
    auto changed_events = events;
    changed_events[2] = quote(header("SPY", 2, 1'000, 1'100), level("500", "10"), level("500.01", "31"));
    const TemporaryDirectory third;
    const auto changed = chronos::test::published(chronos::test::write_dataset(third.path(), changed_events));
    REQUIRE(changed.manifest.dataset_id != one.manifest.dataset_id);

    const TemporaryDirectory fourth;
    const auto other_source = chronos::test::published(
        chronos::test::write_dataset(fourth.path(), events, sample_source(std::string(64, 'b')))
    );
    REQUIRE(other_source.manifest.dataset_id != one.manifest.dataset_id);
    REQUIRE(read_file(other_source.directory / ds::kEventsFileName) ==
        read_file(one.directory / ds::kEventsFileName));

    auto other_normalizer = sample_source();
    other_normalizer.normalizer_version = "2";
    const TemporaryDirectory fifth;
    REQUIRE(chronos::test::published(chronos::test::write_dataset(fifth.path(), events, other_normalizer))
                .manifest.dataset_id != one.manifest.dataset_id);

    auto moved = one.manifest;
    moved.stream = chronos::MarketDataStreamId("another-stream");
    REQUIRE(ds::dataset_identity(moved) != one.manifest.dataset_id);
    REQUIRE(ds::dataset_identity(one.manifest) == one.manifest.dataset_id);
}

bool catalog_is_empty(const std::filesystem::path& catalog) {
    return std::filesystem::directory_iterator(catalog) == std::filesystem::directory_iterator();
}

ds::DatasetError write_error(
    const std::filesystem::path& catalog,
    const std::vector<ValidatedMarketEvent>& events,
    ds::DatasetSource source = sample_source()
) {
    auto result = chronos::test::write_dataset(catalog, events, std::move(source));
    REQUIRE(std::holds_alternative<ds::DatasetError>(result));
    return std::get<ds::DatasetError>(result);
}

void test_invalid_input_publishes_nothing() {
    using Code = ds::DatasetErrorCode;
    const TemporaryDirectory catalog;
    REQUIRE(write_error(catalog.path(), {}).code == Code::EmptyDataset);
    REQUIRE(catalog_is_empty(catalog.path()));

    auto events = sample_session();
    auto gap = events;
    gap.erase(gap.begin() + 2);
    REQUIRE(write_error(catalog.path(), gap).code == Code::IngestSequenceGap);
    auto duplicate = events;
    duplicate.insert(duplicate.begin() + 2, duplicate[1]);
    REQUIRE(write_error(catalog.path(), duplicate).code == Code::DuplicateIngestSequence);
    auto backward = events;
    std::swap(backward[3], backward[4]);
    REQUIRE(write_error(catalog.path(), backward).code == Code::IngestSequenceGap);
    std::vector<ValidatedMarketEvent> rewound = {
        events[0], events[1], events[2],
        quote(header("AAPL", 1, 1'000, 1'300), level("100", "1"), level("101", "1")),
    };
    REQUIRE(write_error(catalog.path(), rewound).code == Code::IngestSequenceMovedBackward);
    std::vector<ValidatedMarketEvent> regress = {events[0], events[1], quote(header("SPY", 2, 1, 1'000), level("1", "1"), level("2", "1"))};
    REQUIRE(write_error(catalog.path(), regress).code == Code::ReceiveTimeMovedBackward);
    auto other_stream = header("AAPL", 0, 1, 2);
    other_stream.stream = chronos::MarketDataStreamId("elsewhere");
    REQUIRE(write_error(catalog.path(), {quote(other_stream, level("1", "1"), std::nullopt)}).code ==
        Code::StreamMismatch);
    auto bad_source = sample_source();
    bad_source.recording_sha256 = "not-a-hash";
    REQUIRE(write_error(catalog.path(), events, bad_source).code == Code::InvalidArgument);
    bad_source = sample_source();
    bad_source.provider.clear();
    REQUIRE(write_error(catalog.path(), events, bad_source).code == Code::InvalidArgument);
    // Every failed write removed its own staging directory and nothing else.
    REQUIRE(catalog_is_empty(catalog.path()));

    REQUIRE(std::holds_alternative<ds::DatasetError>(
        ds::NormalizedMarketDatasetWriter::create(catalog.path() / "missing", chronos::MarketDataStreamId("s"))
    ));

    // Nothing is published that its reader would refuse: a record longer
    // than the format allows is refused when it is written.
    const auto long_reason = valid(MarketStatusEvent{
        .header = header("AAPL", 0, 1, 2),
        .status = TradingStatus::Halted,
        .reason = std::string(ds::kMaximumRecordBytes, 'r'),
    });
    const auto too_long = write_error(catalog.path(), {long_reason});
    REQUIRE(too_long.code == Code::LineTooLong);
    REQUIRE(too_long.line == 1U);
    REQUIRE(catalog_is_empty(catalog.path()));
}

void test_an_abandoned_writer_leaves_nothing() {
    const TemporaryDirectory catalog;
    {
        auto created = ds::NormalizedMarketDatasetWriter::create(
            catalog.path(), chronos::MarketDataStreamId(std::string(chronos::test::kDatasetStream))
        );
        auto& writer = std::get<std::unique_ptr<ds::NormalizedMarketDatasetWriter>>(created);
        for (const auto& event : sample_session()) {
            REQUIRE(!writer->append(event));
        }
        REQUIRE(!catalog_is_empty(catalog.path()));
    }
    REQUIRE(catalog_is_empty(catalog.path()));
}

void test_existing_destinations_are_never_replaced() {
    const TemporaryDirectory catalog;
    const auto events = sample_session();
    const auto first = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));
    const std::string manifest_before = read_file(first.directory / ds::kManifestFileName);

    // The same content again is already present, and nothing is written.
    const auto again = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));
    REQUIRE(again.outcome == ds::PublicationOutcome::AlreadyPresent);
    REQUIRE(again.manifest == first.manifest);
    REQUIRE(std::distance(std::filesystem::directory_iterator(catalog.path()),
                          std::filesystem::directory_iterator()) == 1);

    // A corrupt entry with the same name is a collision and is left alone.
    const TemporaryDirectory other;
    const std::filesystem::path squatter = other.path() / first.manifest.dataset_id;
    std::filesystem::create_directory(squatter);
    write_file(squatter / "junk", "not a dataset");
    const auto collision = write_error(other.path(), events);
    REQUIRE(collision.code == ds::DatasetErrorCode::Collision);
    REQUIRE(read_file(squatter / "junk") == "not a dataset");

    // So is a symlink, which is not followed: its target stays untouched.
    const TemporaryDirectory linked;
    const TemporaryDirectory target;
    std::filesystem::create_directory_symlink(target.path(), linked.path() / first.manifest.dataset_id);
    REQUIRE(write_error(linked.path(), events).code == ds::DatasetErrorCode::Collision);
    REQUIRE(std::filesystem::is_symlink(linked.path() / first.manifest.dataset_id));
    REQUIRE(catalog_is_empty(target.path()));

    REQUIRE(read_file(first.directory / ds::kManifestFileName) == manifest_before);
}

// A published dataset copied to a writable directory, so a test can damage
// one file at a time.
struct Fixture {
    TemporaryDirectory catalog;
    ds::PublishedDataset dataset;
    std::string events;
    std::string quality;

    Fixture()
        : dataset(chronos::test::published(chronos::test::write_dataset(catalog.path(), sample_session()))),
          events(read_file(dataset.directory / ds::kEventsFileName)),
          quality(read_file(dataset.directory / ds::kQualityFileName)) {}

    // A forged dataset with these events and a manifest that otherwise
    // agrees with them, so only the reader's own checks can catch a fault.
    std::filesystem::path forge(std::string_view forged_events, ds::DatasetManifest manifest) {
        return chronos::test::forge_dataset(catalog.path(), std::move(manifest), forged_events, quality);
    }

    std::filesystem::path forge(std::string_view forged_events) { return forge(forged_events, dataset.manifest); }
};

ds::DatasetError read_error(const std::filesystem::path& directory) {
    const auto outcome = chronos::test::read_dataset(directory);
    REQUIRE(outcome.error.has_value());
    return *outcome.error;
}

std::string with_line(std::string events, std::size_t index, const std::string& line) {
    std::size_t start = 0;
    for (std::size_t skipped = 0; skipped < index; ++skipped) {
        start = events.find('\n', start) + 1;
    }
    const std::size_t end = events.find('\n', start);
    return events.replace(start, end - start, line);
}

void test_damaged_records_are_detected_where_they_are() {
    using Code = ds::DatasetErrorCode;
    Fixture fixture;
    const auto first_line = [&] { return fixture.events.substr(0, fixture.events.find('\n')); };

    auto error = read_error(fixture.forge(with_line(fixture.events, 2, "{not json")));
    REQUIRE(error.code == Code::MalformedJson);
    REQUIRE(error.line == 3U);
    REQUIRE(read_error(fixture.forge(with_line(fixture.events, 1, " " + first_line()))).code ==
        Code::NotCanonical);
    REQUIRE(read_error(fixture.forge(with_line(fixture.events, 0, replaced(first_line(), "{", "{ ")))).code ==
        Code::NotCanonical);
    REQUIRE(read_error(fixture.forge(with_line(
        fixture.events, 0, replaced(first_line(), "\"version\":\"1\"", "\"version\":\"9\"")
    ))).code == Code::UnsupportedVersion);
    REQUIRE(read_error(fixture.forge(with_line(fixture.events, 4, ""))).code == Code::MalformedJson);

    // The stream contract is checked again on reading.
    const std::string lines = fixture.events;
    const std::size_t second = lines.find('\n') + 1;
    const std::size_t third = lines.find('\n', second) + 1;
    REQUIRE(read_error(fixture.forge(lines.substr(0, second) + lines.substr(third))).code ==
        Code::IngestSequenceGap);
    REQUIRE(read_error(fixture.forge(lines.substr(0, third) + lines.substr(second))).code ==
        Code::DuplicateIngestSequence);

    // An instrument the manifest does not list.
    const auto stranger = chronos::test::encoded(
        quote(header("MSFT", 6, 1'300, 1'300), level("1", "1"), std::nullopt)
    );
    auto manifest = fixture.dataset.manifest;
    REQUIRE(read_error(fixture.forge(lines + stranger + "\n", manifest)).code == Code::CountMismatch);
    manifest.summary.events += 1;
    manifest.summary.by_type.top_of_book += 1;
    manifest.summary.by_instrument["AAPL"] += 1;
    manifest.summary.last_ingest_sequence += 1;
    REQUIRE(read_error(fixture.forge(lines + stranger + "\n", manifest)).code == Code::UnknownInstrument);
}

void test_truncation_and_integrity_faults_fail_closed() {
    using Code = ds::DatasetErrorCode;
    Fixture fixture;
    const std::string& lines = fixture.events;
    // A truncated file, or one missing its final newline, is never read as
    // a shorter dataset.
    REQUIRE(read_error(fixture.forge(lines.substr(0, lines.size() - 1))).code == Code::MissingFinalNewline);
    REQUIRE(read_error(fixture.forge(lines.substr(0, lines.size() - 20))).code == Code::MissingFinalNewline);
    const std::size_t last_line = lines.rfind('\n', lines.size() - 2) + 1;
    REQUIRE(read_error(fixture.forge(lines.substr(0, last_line))).code == Code::CountMismatch);

    // The manifest's checksum, count, and summary must all hold.
    auto wrong_checksum = fixture.dataset.manifest;
    wrong_checksum.events.sha256 = std::string(64, '0');
    REQUIRE(read_error(chronos::test::forge_dataset(
        fixture.catalog.path(), wrong_checksum, lines, fixture.quality, true
    )).code == Code::ChecksumMismatch);
    auto wrong_summary = fixture.dataset.manifest;
    wrong_summary.summary.latest_event_timestamp = Timestamp::from_nanoseconds(5'000);
    REQUIRE(read_error(fixture.forge(lines, wrong_summary)).code == Code::SummaryMismatch);

    // A quality report that differs from what the events produce.
    auto altered_quality = replaced(fixture.quality, "\"unflagged\":\"0\"", "\"unflagged\":\"1\"");
    REQUIRE(read_error(chronos::test::forge_dataset(
        fixture.catalog.path(), fixture.dataset.manifest, lines, altered_quality
    )).code == Code::QualityMismatch);

    // Damage to a published dataset's files is caught before any event is
    // read.
    const TemporaryDirectory copies;
    const auto copy = [&](std::string_view name) {
        const auto directory = copies.path() / fixture.dataset.manifest.dataset_id;
        std::filesystem::remove_all(directory);
        std::filesystem::create_directory(directory);
        for (const std::string_view file : {ds::kEventsFileName, ds::kManifestFileName, ds::kQualityFileName}) {
            std::filesystem::copy_file(fixture.dataset.directory / file, directory / file);
            std::filesystem::permissions(directory / file, std::filesystem::perms::owner_write,
                std::filesystem::perm_options::add);
        }
        return directory / name;
    };
    auto target = copy(ds::kEventsFileName);
    write_file(target, lines.substr(0, lines.size() - 1));
    REQUIRE(read_error(target.parent_path()).code == Code::SizeMismatch);
    target = copy(ds::kEventsFileName);
    // Same size, still valid, and every summary unchanged: only the
    // checksum can tell.
    write_file(target, replaced(lines, "\"quantity_raw\":\"300000000\"", "\"quantity_raw\":\"400000000\""));
    REQUIRE(read_error(target.parent_path()).code == Code::ChecksumMismatch);
    target = copy(ds::kEventsFileName);
    write_file(target, replaced(lines, "\"AAPL\"", "\"AAPM\""));
    REQUIRE(read_error(target.parent_path()).code == Code::UnknownInstrument);
    target = copy(ds::kQualityFileName);
    write_file(target, replaced(fixture.quality, "\"unflagged\":\"0\"", "\"unflagged\":\"1\""));
    REQUIRE(read_error(target.parent_path()).code == Code::ChecksumMismatch);
    target = copy("notes.txt");
    write_file(target, "unexpected");
    REQUIRE(read_error(target.parent_path()).code == Code::UnexpectedEntry);
    target = copy(ds::kEventsFileName);
    std::filesystem::remove(target);
    std::filesystem::create_symlink(fixture.dataset.directory / ds::kEventsFileName, target);
    REQUIRE(read_error(target.parent_path()).code == Code::SymlinkRejected);
    const auto renamed = copies.path() / std::string(64, 'c');
    std::filesystem::rename(target.parent_path(), renamed);
    REQUIRE(read_error(renamed).code == Code::SymlinkRejected);
    std::filesystem::remove(renamed / ds::kEventsFileName);
    std::filesystem::copy_file(fixture.dataset.directory / ds::kEventsFileName, renamed / ds::kEventsFileName);
    REQUIRE(read_error(renamed).code == Code::IdentityMismatch);
    const TemporaryDirectory links;
    std::filesystem::create_directory_symlink(fixture.dataset.directory, links.path() / fixture.dataset.manifest.dataset_id);
    REQUIRE(read_error(links.path() / fixture.dataset.manifest.dataset_id).code == Code::SymlinkRejected);
}

ds::DatasetErrorCode manifest_error(std::string_view bytes) {
    auto parsed = ds::parse_manifest(bytes);
    REQUIRE(std::holds_alternative<ds::DatasetError>(parsed));
    return std::get<ds::DatasetError>(parsed).code;
}

void test_manifests_are_parsed_strictly() {
    using Code = ds::DatasetErrorCode;
    Fixture fixture;
    const ds::DatasetManifest& manifest = fixture.dataset.manifest;
    const std::string bytes = ds::serialize_manifest(manifest);
    auto reparsed = ds::parse_manifest(bytes);
    REQUIRE(std::holds_alternative<ds::DatasetManifest>(reparsed));
    REQUIRE(std::get<ds::DatasetManifest>(reparsed) == manifest);

    REQUIRE(manifest_error(bytes.substr(0, bytes.size() - 1)) == Code::MissingFinalNewline);
    REQUIRE(manifest_error(bytes + "\n") == Code::NotCanonical);
    REQUIRE(manifest_error(bytes.substr(0, bytes.size() - 1) + "x\n") == Code::MalformedJson);
    REQUIRE(manifest_error(chronos::canonical_json::serialize_indented(
        std::get<chronos::canonical_json::Value>(chronos::canonical_json::parse(bytes))
    ) + "\n") == Code::NotCanonical);
    // Editing any content without recomputing the ID breaks the identity.
    REQUIRE(manifest_error(replaced(bytes, "\"events\":\"6\"", "\"events\":\"7\"")) == Code::IdentityMismatch);
    REQUIRE(manifest_error(replaced(bytes, "\"dataset_id\":\"", "\"dataset_id\":\"x")) == Code::InvalidValue);
    REQUIRE(manifest_error(replaced(bytes, "{\"counts\"", "{\"annotation\":\"now\",\"counts\"")) ==
        Code::UnknownField);
    REQUIRE(manifest_error(replaced(bytes, "\"version\":\"1\"},\"source\"", "\"version\":\"2\"},\"source\"")) ==
        Code::UnsupportedVersion);
    REQUIRE(manifest_error(replaced(bytes, "\"price_raw\":\"1000000000\"", "\"price_raw\":\"100\"")) ==
        Code::UnsupportedVersion);
    REQUIRE(manifest_error(replaced(bytes, "\"stream\":\"dataset-test-stream\",", "")) == Code::MissingField);

    // A manifest whose identity is right but whose content is impossible.
    const auto consistent = [](ds::DatasetManifest changed) {
        changed.dataset_id = ds::dataset_identity(changed);
        return ds::serialize_manifest(changed);
    };
    auto totals = manifest;
    totals.summary.by_type.trade += 1;
    REQUIRE(manifest_error(consistent(totals)) == Code::SummaryMismatch);
    auto sequences = manifest;
    sequences.summary.last_ingest_sequence += 1;
    REQUIRE(manifest_error(consistent(sequences)) == Code::SummaryMismatch);
    auto times = manifest;
    times.summary.latest_receive_timestamp = Timestamp::from_nanoseconds(0);
    REQUIRE(manifest_error(consistent(times)) == Code::SummaryMismatch);
    auto empty = manifest;
    empty.summary.by_instrument["ZZZ"] = 0;
    REQUIRE(manifest_error(consistent(empty)) == Code::InvalidValue);
    const std::string reordered = replaced(
        consistent(manifest),
        "\"instruments\":[\"AAPL\",\"SPY\"]",
        "\"instruments\":[\"SPY\",\"AAPL\"]"
    );
    REQUIRE(manifest_error(reordered) == Code::InvalidValue);
    auto overflow = manifest;
    overflow.summary.by_type.trade = std::numeric_limits<std::uint64_t>::max();
    REQUIRE(manifest_error(consistent(overflow)) == Code::SummaryMismatch);
}

void test_the_catalog_lists_datasets_and_reports_everything_else() {
    const TemporaryDirectory catalog;
    const auto first = chronos::test::published(chronos::test::write_dataset(catalog.path(), sample_session()));
    const auto second = chronos::test::published(chronos::test::write_dataset(
        catalog.path(), sample_session(), sample_source(std::string(64, 'e'))
    ));
    std::filesystem::create_directory(catalog.path() / "notes");
    std::filesystem::create_directory(catalog.path() / (std::string(ds::kStagingPrefix) + "1-2"));
    write_file(catalog.path() / std::string(64, '0'), "a file where a dataset should be");

    for (const auto check : {ds::CatalogCheck::Manifests, ds::CatalogCheck::Events}) {
        auto listed = ds::list_catalog(catalog.path(), check);
        REQUIRE(std::holds_alternative<std::vector<ds::CatalogEntry>>(listed));
        const auto& entries = std::get<std::vector<ds::CatalogEntry>>(listed);
        REQUIRE(entries.size() == 5);
        for (std::size_t index = 1; index < entries.size(); ++index) {
            REQUIRE(entries[index - 1].name < entries[index].name);
        }
        std::size_t valid_datasets = 0;
        for (const auto& entry : entries) {
            if (const auto* manifest = std::get_if<ds::DatasetManifest>(&entry.status)) {
                REQUIRE(manifest->dataset_id == entry.name);
                REQUIRE((entry.name == first.manifest.dataset_id || entry.name == second.manifest.dataset_id));
                ++valid_datasets;
            }
        }
        REQUIRE(valid_datasets == 2);
    }
    REQUIRE(std::holds_alternative<ds::DatasetError>(ds::list_catalog(catalog.path() / "missing")));

    // The root itself may be a symlink; what it holds is never followed.
    const TemporaryDirectory elsewhere;
    const auto linked_root = elsewhere.path() / "catalog";
    std::filesystem::create_directory_symlink(catalog.path(), linked_root);
    auto through_link = ds::list_catalog(linked_root);
    REQUIRE(std::get<std::vector<ds::CatalogEntry>>(through_link).size() == 5);
    const auto again = chronos::test::published(chronos::test::write_dataset(linked_root, sample_session()));
    REQUIRE(again.outcome == ds::PublicationOutcome::AlreadyPresent);
}

// Replay needs no knowledge of the provider: the same events drive the same
// market view whether they come from memory or a normalized dataset.
void test_replay_of_a_dataset_matches_replay_of_its_events() {
    const TemporaryDirectory catalog;
    const auto events = sample_session();
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));

    MarketReplay direct{chronos::MarketDataStreamId(std::string(chronos::test::kDatasetStream))};
    for (const auto& event : events) {
        REQUIRE(std::holds_alternative<chronos::MarketReplayStep>(direct.accept(event)));
    }
    MarketReplay from_dataset(dataset.manifest.stream);
    auto opened = ds::NormalizedMarketEventReader::open(dataset.directory);
    auto& reader = std::get<std::unique_ptr<ds::NormalizedMarketEventReader>>(opened);
    std::optional<std::string> checksum;
    while (!checksum) {
        auto read = reader->read_next();
        if (auto* event = std::get_if<ValidatedMarketEvent>(&read)) {
            REQUIRE(std::holds_alternative<chronos::MarketReplayStep>(from_dataset.accept(*event)));
        } else {
            REQUIRE(std::holds_alternative<chronos::EndOfMarketEvents>(read));
            checksum = std::get<chronos::EndOfMarketEvents>(read).source_sha256;
        }
    }
    REQUIRE(from_dataset.market_view() == direct.market_view());
    REQUIRE(from_dataset.clock().current_time() == direct.clock().current_time());
    REQUIRE(checksum == dataset.manifest.events.sha256);
}

std::string report_of(const std::vector<ValidatedMarketEvent>& events) {
    ds::DatasetQualityReport report;
    for (const auto& event : events) {
        REQUIRE(!report.observe(event));
    }
    return report.serialize();
}

void test_the_quality_report_format_is_pinned() {
    const std::string report = report_of({
        quote(header("AAPL", 0, 1'000, 1'100), level("99", "1"), level("101", "2")),
        trade(header("SPY", 1, 1'000, 1'200, MarketDataQuality{}), "500", "1"),
    });
    REQUIRE(report ==
        R"({"checks":{"chronos_event_validation":"passed","ingest_sequence":"contiguous",)"
        R"("receive_timestamp":"non_decreasing","stream":"single"},)"
        R"("events":{"by_event_type":{"instrument_definition":"0","market_status":"0",)"
        R"("top_of_book":"1","trade":"1"},"total":"2"},)"
        R"("instruments":{"AAPL":{"by_event_type":{"instrument_definition":"0","market_status":"0",)"
        R"("top_of_book":"1","trade":"0"},"event_time_regressions":"0","events":"1",)"
        R"("maximum_receive_gap_ns":null,"receive_timestamp_ns":{"first":"1100","last":"1100"},)"
        R"("top_of_book":{"ask_only":"0","bid_only":"0","crossed":"0","empty":"0","locked":"0",)"
        R"("maximum_spread_raw":"2000000000","minimum_spread_raw":"2000000000","suspect":"0",)"
        R"("two_sided":"1","valid_two_sided":"1"}},)"
        R"("SPY":{"by_event_type":{"instrument_definition":"0","market_status":"0",)"
        R"("top_of_book":"0","trade":"1"},"event_time_regressions":"0","events":"1",)"
        R"("maximum_receive_gap_ns":null,"receive_timestamp_ns":{"first":"1200","last":"1200"},)"
        R"("top_of_book":{"ask_only":"0","bid_only":"0","crossed":"0","empty":"0","locked":"0",)"
        R"("maximum_spread_raw":null,"minimum_spread_raw":null,"suspect":"0",)"
        R"("two_sided":"0","valid_two_sided":"0"}}},)"
        R"("instruments_without_valid_two_sided_quote":["SPY"],)"
        R"("quality_flags":{"potentially_invalid_book":"0","questionable_receive_timestamp":"0",)"
        R"("replay":"1","snapshot":"0","source_specific":"0","unflagged":"1"},)"
        R"("rules":{"name":"chronos.dataset_quality","version":"1"},)"
        R"("venue_sequence":{"events_with_venue_sequence":"0","gap_detection":"not_performed",)"
        R"("reason":"venue sequences are provider- and channel-specific; Chronos checks only its own )"
        R"(ingest-sequence continuity"}})" "\n");
}

std::string field_of(const std::string& report, std::string_view instrument, std::string_view path) {
    auto parsed = chronos::canonical_json::parse(report.substr(0, report.size() - 1));
    const auto* root = std::get<chronos::canonical_json::Value>(parsed).if_object();
    const chronos::canonical_json::Value* current = nullptr;
    const auto member = [](const chronos::canonical_json::Object& object, std::string_view key) {
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        throw chronos::test::TestFailure("missing key " + std::string(key));
    };
    current = member(*member(*root, "instruments")->if_object(), instrument);
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t dot = path.find('.', start);
        const std::string_view key = path.substr(start, dot == std::string_view::npos ? path.npos : dot - start);
        current = member(*current->if_object(), key);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return current->is_null() ? "null" : *current->if_string();
}

void test_the_quality_report_counts_what_it_observes() {
    const auto suspect_receive = MarketDataQuality{}.with(MarketDataQualityFlag::QuestionableReceiveTimestamp);
    const auto invalid_book = MarketDataQuality{}.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    std::vector<ValidatedMarketEvent> events = {
        quote(header("AAPL", 0, 1'000, 1'000), level("100", "1"), level("100.05", "1")),
        quote(header("AAPL", 1, 1'010, 1'500), level("100", "1"), level("100", "1")),        // locked
        quote(header("AAPL", 2, 1'005, 1'600, invalid_book), level("100.1", "1"), level("100", "1")),
        quote(header("AAPL", 3, 1'020, 1'600, suspect_receive), level("99", "1"), level("110", "1")),
        quote(header("AAPL", 4, 1'030, 4'600), level("100", "1"), std::nullopt),
        quote(header("AAPL", 5, 1'040, 4'700), std::nullopt, level("100.2", "1")),
        quote(header("AAPL", 6, 1'050, 4'800), std::nullopt, std::nullopt),
        quote(header("AAPL", 7, 1'060, 4'900), level("100", "1"), level("100.2", "1")),
        trade(header("SPY", 8, 1'000, 5'000), "500", "1"),
        quote(header("MSFT", 9, 1'000, 5'000, invalid_book), level("300", "1"), level("300.5", "1")),
    };
    const std::string report = report_of(events);
    REQUIRE(field_of(report, "AAPL", "events") == "8");
    REQUIRE(field_of(report, "AAPL", "top_of_book.two_sided") == "5");
    REQUIRE(field_of(report, "AAPL", "top_of_book.valid_two_sided") == "3");
    REQUIRE(field_of(report, "AAPL", "top_of_book.locked") == "1");
    REQUIRE(field_of(report, "AAPL", "top_of_book.crossed") == "1");
    REQUIRE(field_of(report, "AAPL", "top_of_book.suspect") == "2");
    REQUIRE(field_of(report, "AAPL", "top_of_book.bid_only") == "1");
    REQUIRE(field_of(report, "AAPL", "top_of_book.ask_only") == "1");
    REQUIRE(field_of(report, "AAPL", "top_of_book.empty") == "1");
    // Valid quotes only: the locked one (0) up to 0.2; the suspect 11-wide
    // quote and the crossed one are excluded.
    REQUIRE(field_of(report, "AAPL", "top_of_book.minimum_spread_raw") == "0");
    REQUIRE(field_of(report, "AAPL", "top_of_book.maximum_spread_raw") == "200000000");
    REQUIRE(field_of(report, "AAPL", "event_time_regressions") == "1");
    REQUIRE(field_of(report, "AAPL", "maximum_receive_gap_ns") == "3000");
    REQUIRE(field_of(report, "AAPL", "receive_timestamp_ns.last") == "4900");
    REQUIRE(field_of(report, "SPY", "top_of_book.minimum_spread_raw") == "null");
    REQUIRE(report.find(R"("instruments_without_valid_two_sided_quote":["MSFT","SPY"])") != std::string::npos);
    REQUIRE(report.find(R"("potentially_invalid_book":"2","questionable_receive_timestamp":"1","replay":"7")") !=
        std::string::npos);
    REQUIRE(report.find(R"("unflagged":"0")") != std::string::npos);
    REQUIRE(report_of(events) == report);
}

// The paced replay source the server uses plays a normalized dataset like
// any recording, and reports its checksum when it completes.
void test_the_paced_replay_source_plays_a_dataset() {
    namespace md = chronos::market_data;
    const TemporaryDirectory catalog;
    const auto events = sample_session();
    const auto dataset = chronos::test::published(chronos::test::write_dataset(catalog.path(), events));
    auto opened = ds::NormalizedMarketEventReader::open(dataset.directory);
    auto& reader = std::get<std::unique_ptr<ds::NormalizedMarketEventReader>>(opened);

    const md::SteadyClock clock;
    chronos::MarketState market{dataset.manifest.stream};
    md::MarketDataNotifier notifier(clock, md::MarketDataNotifier::Options{});
    md::MarketStateFeed feed(market, notifier);
    md::PacedReplaySource source(
        std::move(reader),
        feed,
        clock,
        md::MarketDataSourceDescription{
            .mode = md::MarketDataMode::HistoricalReplay,
            .provider = "Chronos",
            .feed = "normalized dataset",
            .label = "normalized dataset replay",
            .coverage_notice = std::nullopt,
            .coverage_label = std::nullopt,
            .instruments = {chronos::InstrumentId("AAPL"), chronos::InstrumentId("SPY")},
        },
        md::PacedReplayOptions{.speed = chronos::ReplaySpeed::Maximum, .start_immediately = true}
    );
    REQUIRE(chronos::test::eventually([&] {
        return source.status().state == md::MarketDataSourceState::Completed;
    }));
    REQUIRE(source.status().events_applied == events.size());
    REQUIRE(source.status().recording_sha256 == dataset.manifest.events.sha256);
    REQUIRE(market.status().last_ingest_sequence == events.size() - 1);

    MarketReplay direct{dataset.manifest.stream};
    for (const auto& event : events) {
        REQUIRE(std::holds_alternative<chronos::MarketReplayStep>(direct.accept(event)));
    }
    for (const std::string instrument : {"AAPL", "SPY"}) {
        auto quote_now = market.execution_quote(chronos::InstrumentId(instrument));
        REQUIRE(std::holds_alternative<chronos::ExecutionQuote>(quote_now));
        REQUIRE(std::get<chronos::ExecutionQuote>(quote_now).top_of_book ==
            direct.market_view().find(chronos::InstrumentId(instrument))->top_of_book);
    }
}

void test_line_reader_bounds_and_truncation() {
    const TemporaryDirectory directory;
    const auto path = directory.path() / "lines.ndjson";
    const auto lines_of = [&](std::string_view contents, std::size_t maximum) {
        write_file(path, contents);
        auto opened = ds::NdjsonFileReader::open(path, "lines", maximum);
        auto& reader = std::get<std::unique_ptr<ds::NdjsonFileReader>>(opened);
        std::vector<std::string> lines;
        while (true) {
            auto next = reader->next();
            if (auto* line = std::get_if<std::string_view>(&next)) {
                lines.emplace_back(*line);
            } else if (std::holds_alternative<ds::EndOfLines>(next)) {
                REQUIRE(reader->hex_digest() == Sha256::hex(contents));
                return std::variant<std::vector<std::string>, ds::DatasetError>(lines);
            } else {
                return std::variant<std::vector<std::string>, ds::DatasetError>(std::get<ds::DatasetError>(next));
            }
        }
    };
    const std::string big(200'000, 'x');
    auto whole = lines_of("a\n\nbc\n" + big + "\n", big.size());
    REQUIRE(std::get<std::vector<std::string>>(whole) == std::vector<std::string>({"a", "", "bc", big}));
    REQUIRE(std::get<ds::DatasetError>(lines_of("a\n" + big + "\n", 1000)).code ==
        ds::DatasetErrorCode::LineTooLong);
    REQUIRE(std::get<ds::DatasetError>(lines_of("a\nb", 1000)).code == ds::DatasetErrorCode::MissingFinalNewline);
    REQUIRE(std::get<std::vector<std::string>>(lines_of("", 10)).empty());
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"every event variant round trips exactly", test_every_event_variant_round_trips_exactly},
        {"the record format is pinned", test_the_record_format_is_pinned},
        {"boundary values round trip", test_boundary_values_round_trip},
        {"invalid records are rejected", test_invalid_records_are_rejected},
        {"text JSON cannot hold is refused", test_text_json_cannot_hold_is_refused},
        {"a dataset round trips and verifies", test_a_dataset_round_trips_and_verifies},
        {"conversion is deterministic and identity tracks meaning",
         test_conversion_is_deterministic_and_identity_tracks_meaning},
        {"invalid input publishes nothing", test_invalid_input_publishes_nothing},
        {"an abandoned writer leaves nothing", test_an_abandoned_writer_leaves_nothing},
        {"existing destinations are never replaced", test_existing_destinations_are_never_replaced},
        {"damaged records are detected where they are", test_damaged_records_are_detected_where_they_are},
        {"truncation and integrity faults fail closed", test_truncation_and_integrity_faults_fail_closed},
        {"manifests are parsed strictly", test_manifests_are_parsed_strictly},
        {"the catalog lists datasets and reports everything else",
         test_the_catalog_lists_datasets_and_reports_everything_else},
        {"replay of a dataset matches replay of its events",
         test_replay_of_a_dataset_matches_replay_of_its_events},
        {"the paced replay source plays a dataset", test_the_paced_replay_source_plays_a_dataset},
        {"line reader bounds and truncation", test_line_reader_bounds_and_truncation},
        {"the quality report format is pinned", test_the_quality_report_format_is_pinned},
        {"the quality report counts what it observes", test_the_quality_report_counts_what_it_observes},
    });
}
