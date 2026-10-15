#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/market_data/dataset_replay.hpp"
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data/paced_replay_source.hpp"
#include "chronos/market_replay.hpp"
#include "chronos/market_state.hpp"
#include "dataset_test_support.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// A normalized dataset replayed by the paper-trading server (ADR 0025):
// verified end to end before anything is exposed, matched to the configured
// stream and instruments, and labelled from its provenance.
namespace {

namespace ds = chronos::datasets;
namespace md = chronos::market_data;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::test::TemporaryDirectory;

const MarketDataStreamId kStream{std::string(chronos::test::kDatasetStream)};
const std::vector<InstrumentId> kInstruments{InstrumentId("AAPL"), InstrumentId("SPY")};

ds::DatasetSource synthetic_source() {
    auto source = chronos::test::sample_source();
    source.provider = std::string(ds::kSyntheticProvider);
    source.description = "invented events from a test scenario";
    source.coverage = std::nullopt;
    return source;
}

ds::PublishedDataset write(const TemporaryDirectory& catalog, ds::DatasetSource source) {
    return chronos::test::published(
        chronos::test::write_dataset(catalog.path(), chronos::test::sample_session(), std::move(source))
    );
}

std::string refusal(
    const std::filesystem::path& directory,
    const MarketDataStreamId& stream = kStream,
    const std::vector<InstrumentId>& instruments = kInstruments
) {
    auto opened = md::open_dataset_replay(directory, stream, instruments);
    REQUIRE(std::holds_alternative<md::DatasetReplayRefusal>(opened));
    return std::get<md::DatasetReplayRefusal>(opened).message;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Replaces the first occurrence of `from` in the events file with `to`, of
// the same length, as a corruption that metadata and sizes cannot see.
void corrupt_events(const std::filesystem::path& directory, std::string_view from, std::string_view to) {
    REQUIRE(from.size() == to.size());
    const auto events = directory / std::string(ds::kEventsFileName);
    std::filesystem::permissions(events, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    std::string contents = chronos::test::read_file(events);
    const auto at = contents.find(from);
    REQUIRE(at != std::string::npos);
    contents.replace(at, from.size(), to);
    chronos::test::write_file(events, contents);
}

void test_a_verified_synthetic_dataset_waits_ready_then_plays_every_event() {
    const TemporaryDirectory catalog;
    const auto dataset = write(catalog, synthetic_source());
    auto opened = md::open_dataset_replay(dataset.directory, kStream, {InstrumentId("SPY"), InstrumentId("AAPL")});
    REQUIRE(std::holds_alternative<md::DatasetReplay>(opened));
    auto replay = std::get<md::DatasetReplay>(std::move(opened));
    REQUIRE(replay.dataset_id == dataset.manifest.dataset_id);
    REQUIRE(replay.description.synthetic);

    const md::SteadyClock clock;
    chronos::MarketState market{kStream};
    md::MarketDataNotifier notifier(clock, md::MarketDataNotifier::Options{});
    md::MarketStateFeed feed(market, notifier);
    md::PacedReplaySource source(
        std::move(replay.reader),
        feed,
        clock,
        std::move(replay.description),
        md::PacedReplayOptions{.speed = chronos::ReplaySpeed::Maximum, .start_immediately = false}
    );
    // Nothing is exposed until Start.
    REQUIRE(source.status().state == md::MarketDataSourceState::Ready);
    REQUIRE(source.status().events_applied == 0U);
    REQUIRE(!market.status().last_ingest_sequence.has_value());
    REQUIRE(std::holds_alternative<md::MarketDataSourceStatus>(source.control_replay(md::ReplayCommand::Start)));
    REQUIRE(chronos::test::eventually([&] {
        return source.status().state == md::MarketDataSourceState::Completed;
    }));
    const auto events = chronos::test::sample_session();
    REQUIRE(source.status().events_applied == events.size());
    REQUIRE(source.status().recording_sha256 == dataset.manifest.events.sha256);

    // The events are the dataset's own: same sequences, times, and books.
    chronos::MarketReplay direct{kStream};
    for (const auto& event : events) {
        REQUIRE(std::holds_alternative<chronos::MarketReplayStep>(direct.accept(event)));
    }
    REQUIRE(market.status().last_ingest_sequence == events.size() - 1);
    for (const auto& instrument : kInstruments) {
        auto quote = market.execution_quote(instrument);
        REQUIRE(std::holds_alternative<chronos::ExecutionQuote>(quote));
        REQUIRE(std::get<chronos::ExecutionQuote>(quote).top_of_book ==
            direct.market_view().find(instrument)->top_of_book);
    }
}

void test_a_synthetic_dataset_says_its_prices_are_invented() {
    const TemporaryDirectory catalog;
    const auto dataset = write(catalog, synthetic_source());
    const auto description = md::dataset_replay_description(dataset.manifest, kInstruments);
    REQUIRE(description.mode == md::MarketDataMode::HistoricalReplay);
    REQUIRE(description.synthetic);
    REQUIRE(description.provider == "synthetic");
    REQUIRE(description.label == "Synthetic replay: invented prices, not market data");
    REQUIRE(description.coverage_notice.has_value());
    REQUIRE(contains(*description.coverage_notice, "invented events from a test scenario"));
    REQUIRE(contains(*description.coverage_notice, "not observed on any exchange"));
    REQUIRE(description.coverage_label == "Synthetic, not market data");
    REQUIRE(description.instruments == kInstruments);
}

void test_a_recorded_dataset_keeps_its_provider_and_coverage() {
    const TemporaryDirectory catalog;
    const auto dataset = write(catalog, chronos::test::sample_source());
    const auto description = md::dataset_replay_description(dataset.manifest, kInstruments);
    REQUIRE(!description.synthetic);
    REQUIRE(description.provider == "test");
    REQUIRE(description.feed == "SYNTHETIC top-of-book");
    REQUIRE(description.label == "test SYNTHETIC top-of-book recording (normalized dataset replay)");
    REQUIRE(contains(*description.coverage_notice, "synthetic dataset for tests"));
    REQUIRE(contains(*description.coverage_notice, "chronos.test version 1"));
    REQUIRE(contains(*description.coverage_notice, "test window; start inclusive, end exclusive"));

    auto without_window = chronos::test::sample_source(std::string(64, 'b'));
    without_window.coverage = std::nullopt;
    const TemporaryDirectory other;
    const auto unwindowed = md::dataset_replay_description(write(other, without_window).manifest, kInstruments);
    REQUIRE(contains(*unwindowed.coverage_notice, "No provider query window was recorded"));
}

// Fail-first regression: open_dataset checks sizes and metadata only, so it
// accepts a same-size corruption; the replay must refuse it before any event
// is exposed. One edit makes an invalid book; the other is a valid event
// that only the events' checksum, checked at the end, can tell apart.
void test_a_same_size_corruption_is_refused_before_anything_plays() {
    for (const auto& [from, to, reason] : {
             std::tuple{"\"100000000000\"", "\"200000000000\"", "invalid_event"},
             std::tuple{"\"300000000\"", "\"400000000\"", "checksum_mismatch"},
         }) {
        const TemporaryDirectory catalog;
        const auto dataset = write(catalog, synthetic_source());
        corrupt_events(dataset.directory, from, to);
        REQUIRE(std::holds_alternative<ds::OpenedDataset>(ds::open_dataset(dataset.directory)));
        const std::string message = refusal(dataset.directory);
        REQUIRE(contains(message, std::string("the dataset does not verify: ") + reason));
        REQUIRE(message.find(from) == std::string::npos);
        REQUIRE(message.find(to) == std::string::npos);
    }
}

void test_truncated_missing_and_foreign_directories_are_refused() {
    const TemporaryDirectory catalog;
    const auto dataset = write(catalog, synthetic_source());
    const auto events = dataset.directory / std::string(ds::kEventsFileName);
    std::filesystem::permissions(events, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    const std::string contents = chronos::test::read_file(events);
    chronos::test::write_file(events, contents.substr(0, contents.size() / 2));
    REQUIRE(contains(refusal(dataset.directory), "the dataset does not verify: size_mismatch"));
    REQUIRE(contains(refusal(catalog.path() / std::string(64, 'c')), "the dataset does not verify: "));
    REQUIRE(contains(refusal(catalog.path()), "the dataset does not verify: "));
}

void test_the_stream_must_be_the_datasets() {
    const TemporaryDirectory catalog;
    const auto dataset = write(catalog, synthetic_source());
    REQUIRE(refusal(dataset.directory, MarketDataStreamId("another-stream")) ==
        "--stream-id another-stream is not the dataset's stream, dataset-test-stream");
}

// Every event enters the market state, so the instruments must be exactly
// the dataset's: neither a subset nor an extra one.
void test_the_instruments_must_be_exactly_the_datasets() {
    const TemporaryDirectory catalog;
    const auto dataset = write(catalog, synthetic_source());
    REQUIRE(refusal(dataset.directory, kStream, {InstrumentId("AAPL")}) ==
        "--instrument must list exactly the dataset's instruments (AAPL, SPY), not AAPL: every event of the "
        "dataset is replayed");
    REQUIRE(contains(
        refusal(dataset.directory, kStream, {InstrumentId("AAPL"), InstrumentId("SPY"), InstrumentId("MSFT")}),
        "not AAPL, MSFT, SPY"
    ));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "a verified synthetic dataset waits ready, then plays every event",
            test_a_verified_synthetic_dataset_waits_ready_then_plays_every_event,
        },
        {"a synthetic dataset says its prices are invented", test_a_synthetic_dataset_says_its_prices_are_invented},
        {"a recorded dataset keeps its provider and coverage", test_a_recorded_dataset_keeps_its_provider_and_coverage},
        {
            "a same-size corruption is refused before anything plays",
            test_a_same_size_corruption_is_refused_before_anything_plays,
        },
        {
            "truncated, missing, and foreign directories are refused",
            test_truncated_missing_and_foreign_directories_are_refused,
        },
        {"the stream must be the dataset's", test_the_stream_must_be_the_datasets},
        {"the instruments must be exactly the dataset's", test_the_instruments_must_be_exactly_the_datasets},
    });
}
