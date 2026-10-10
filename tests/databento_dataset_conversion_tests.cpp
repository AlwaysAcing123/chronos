#include "chronos/databento/dataset_conversion.hpp"
#include "chronos/databento/equities_mini_mbp1_event_reader.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/market_replay.hpp"
#include "chronos/sha256.hpp"
#include "databento_test_support.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace adapter = chronos::databento_adapter;
namespace ds = chronos::datasets;
using namespace chronos::test::databento_fixtures;
using chronos::MarketDataStreamId;
using chronos::ValidatedMarketEvent;
using chronos::test::read_file;
using chronos::test::TemporaryDirectory;

constexpr std::string_view kStream = "databento-conversion-test";

ds::PublishedDataset converted(const std::filesystem::path& recording, const std::filesystem::path& catalog) {
    auto result = adapter::convert_recording_to_dataset(recording, MarketDataStreamId(std::string(kStream)), catalog);
    if (auto* failure = std::get_if<adapter::RecordingConversionError>(&result)) {
        throw chronos::test::TestFailure(
            "conversion failed: " + (failure->recording ? failure->recording->message : failure->dataset->message)
        );
    }
    return std::get<ds::PublishedDataset>(std::move(result));
}

std::vector<ValidatedMarketEvent> recording_events(const std::filesystem::path& recording) {
    auto opened = adapter::EquitiesMiniMbp1EventReader::open(recording, MarketDataStreamId(std::string(kStream)));
    auto& reader = std::get<std::unique_ptr<adapter::EquitiesMiniMbp1EventReader>>(opened);
    std::vector<ValidatedMarketEvent> events;
    while (true) {
        auto read = reader->read_next();
        if (auto* event = std::get_if<ValidatedMarketEvent>(&read)) {
            events.push_back(std::move(*event));
        } else {
            REQUIRE(std::holds_alternative<chronos::EndOfMarketEvents>(read));
            return events;
        }
    }
}

// The normalized dataset holds exactly the recording's validated events,
// with provider-independent provenance.
void test_a_recording_becomes_an_equivalent_dataset() {
    TemporaryDbnFile recording;
    recording.write(valid_metadata(), valid_records());
    const TemporaryDirectory catalog;
    const auto dataset = converted(recording.path(), catalog.path());
    REQUIRE(dataset.outcome == ds::PublicationOutcome::Published);

    const auto outcome = chronos::test::read_dataset(dataset.directory);
    REQUIRE(!outcome.error);
    REQUIRE(outcome.events == recording_events(recording.path()));
    REQUIRE(outcome.end_sha256 == dataset.manifest.events.sha256);

    const ds::DatasetSource& source = dataset.manifest.source;
    REQUIRE(source.provider == "databento");
    REQUIRE(source.provider_dataset == "EQUS.MINI");
    REQUIRE(source.provider_schema == "mbp-1");
    REQUIRE(source.normalizer == adapter::kDatasetNormalizerName);
    REQUIRE(source.coverage.has_value());
    REQUIRE(source.coverage->start.nanoseconds() == static_cast<std::int64_t>(recording_day_nanoseconds()));
    REQUIRE(source.coverage->end.nanoseconds() ==
        static_cast<std::int64_t>(recording_day_nanoseconds() + 1'000'000'000));
    // Databento filters an MBP-1 request on ts_recv, its index timestamp,
    // not on ts_event.
    REQUIRE(source.coverage->semantics ==
        "Databento query window by receive timestamp (ts_recv); start inclusive, end exclusive");
    // The recording's checksum came from OpenSSL in the adapter; Chronos's
    // own SHA-256 of the same bytes must agree.
    REQUIRE(source.recording_sha256 == chronos::Sha256::hex(read_file(recording.path())));
    REQUIRE(dataset.manifest.stream.value() == kStream);
    REQUIRE(dataset.manifest.summary.events == 3);
    REQUIRE(dataset.manifest.summary.by_instrument.at("SPY") == 2);
}

void test_conversion_is_reproducible_and_never_overwrites() {
    TemporaryDbnFile recording;
    recording.write(valid_metadata(), valid_records());
    const TemporaryDirectory first;
    const TemporaryDirectory second;
    const auto one = converted(recording.path(), first.path());
    const auto two = converted(recording.path(), second.path());
    REQUIRE(one.manifest.dataset_id == two.manifest.dataset_id);
    for (const std::string_view file : {ds::kEventsFileName, ds::kManifestFileName, ds::kQualityFileName}) {
        REQUIRE(read_file(one.directory / file) == read_file(two.directory / file));
    }
    const auto again = converted(recording.path(), first.path());
    REQUIRE(again.outcome == ds::PublicationOutcome::AlreadyPresent);
    REQUIRE(again.manifest == one.manifest);

    // The compressed form of the same records is a different recording, so
    // its provenance differs, but its events are identical.
    TemporaryDbnFile compressed;
    compressed.write_compressed(valid_metadata(), valid_records());
    const auto zstd = converted(compressed.path(), first.path());
    REQUIRE(zstd.manifest.dataset_id != one.manifest.dataset_id);
    REQUIRE(read_file(zstd.directory / ds::kEventsFileName) == read_file(one.directory / ds::kEventsFileName));
}

bool catalog_is_empty(const std::filesystem::path& directory) {
    return std::filesystem::directory_iterator(directory) == std::filesystem::directory_iterator();
}

void test_a_failed_conversion_publishes_nothing() {
    const TemporaryDirectory catalog;
    auto records = valid_records();
    records.push_back(mbp1_record(9'999, 3'000));
    TemporaryDbnFile unmapped;
    unmapped.write(valid_metadata(), records);
    auto result = adapter::convert_recording_to_dataset(
        unmapped.path(), MarketDataStreamId(std::string(kStream)), catalog.path()
    );
    REQUIRE(std::holds_alternative<adapter::RecordingConversionError>(result));
    REQUIRE(std::get<adapter::RecordingConversionError>(result).recording.has_value());
    REQUIRE(catalog_is_empty(catalog.path()));

    TemporaryDbnFile empty;
    empty.write(valid_metadata(), {});
    result = adapter::convert_recording_to_dataset(empty.path(), MarketDataStreamId(std::string(kStream)), catalog.path());
    REQUIRE(std::holds_alternative<adapter::RecordingConversionError>(result));
    REQUIRE(std::get<adapter::RecordingConversionError>(result).dataset->code == ds::DatasetErrorCode::EmptyDataset);
    REQUIRE(catalog_is_empty(catalog.path()));

    result = adapter::convert_recording_to_dataset(
        catalog.path() / "missing.dbn", MarketDataStreamId(std::string(kStream)), catalog.path()
    );
    REQUIRE(std::get<adapter::RecordingConversionError>(result).recording.has_value());
    REQUIRE(catalog_is_empty(catalog.path()));
}

void test_replaying_the_dataset_equals_replaying_the_recording() {
    TemporaryDbnFile recording;
    recording.write(valid_metadata(), valid_records());
    const TemporaryDirectory catalog;
    const auto dataset = converted(recording.path(), catalog.path());
    const auto replay = [](const std::vector<ValidatedMarketEvent>& events) {
        chronos::MarketReplay market{MarketDataStreamId(std::string(kStream))};
        for (const auto& event : events) {
            REQUIRE(std::holds_alternative<chronos::MarketReplayStep>(market.accept(event)));
        }
        return market.market_view();
    };
    REQUIRE(replay(chronos::test::read_dataset(dataset.directory).events) ==
        replay(recording_events(recording.path())));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"a recording becomes an equivalent dataset", test_a_recording_becomes_an_equivalent_dataset},
        {"conversion is reproducible and never overwrites", test_conversion_is_reproducible_and_never_overwrites},
        {"a failed conversion publishes nothing", test_a_failed_conversion_publishes_nothing},
        {"replaying the dataset equals replaying the recording",
         test_replaying_the_dataset_equals_replaying_the_recording},
    });
}
