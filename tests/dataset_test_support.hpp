#pragma once

#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_quality.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/sha256.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace chronos::test {

inline constexpr std::string_view kDatasetStream = "dataset-test-stream";

// A scratch directory removed with everything in it, including read-only
// artifacts, when the test ends.
class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<int> counter{0};
        std::random_device device;
        path_ = std::filesystem::temp_directory_path() /
            ("chronos-test-" + std::to_string(::getpid()) + "-" +
             std::to_string(device()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        for (auto entry = std::filesystem::recursive_directory_iterator(path_, ignored);
             entry != std::filesystem::recursive_directory_iterator();
             entry.increment(ignored)) {
            if (!entry->is_symlink(ignored)) {
                std::filesystem::permissions(entry->path(), std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::add, ignored);
            }
        }
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

inline void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    REQUIRE(stream.good());
}

inline ValidatedMarketEvent valid(MarketEvent event) {
    auto result = validate_market_event(std::move(event));
    REQUIRE(std::holds_alternative<ValidatedMarketEvent>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

inline MarketEventHeader header(
    std::string instrument,
    std::uint64_t sequence,
    std::int64_t event_ns,
    std::int64_t receive_ns,
    MarketDataQuality quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay)
) {
    return MarketEventHeader{
        .instrument = InstrumentId(std::move(instrument)),
        .stream = MarketDataStreamId(std::string(kDatasetStream)),
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(event_ns),
        .receive_timestamp = Timestamp::from_nanoseconds(receive_ns),
        .ingest_sequence = sequence,
        .venue_sequence = std::nullopt,
        .quality = quality,
    };
}

inline std::optional<BookLevel> level(std::string_view price, std::string_view quantity) {
    return BookLevel{.price = Price::parse(price), .quantity = Quantity::parse(quantity)};
}

inline ValidatedMarketEvent quote(
    MarketEventHeader event_header,
    std::optional<BookLevel> bid,
    std::optional<BookLevel> ask
) {
    return valid(TopOfBookEvent{.header = std::move(event_header), .bid = bid, .ask = ask});
}

inline ValidatedMarketEvent trade(MarketEventHeader event_header, std::string_view price, std::string_view quantity) {
    return valid(TradeEvent{
        .header = std::move(event_header),
        .price = Price::parse(price),
        .quantity = Quantity::parse(quantity),
        .aggressor_side = AggressorSide::Buy,
    });
}

inline ValidatedMarketEvent status(MarketEventHeader event_header, TradingStatus trading_status) {
    return valid(MarketStatusEvent{
        .header = std::move(event_header),
        .status = trading_status,
        .reason = "scheduled",
    });
}

// A small multi-instrument session: two instruments, equal timestamps,
// trades, quotes, and statuses, with contiguous ingest sequences from 0.
inline std::vector<ValidatedMarketEvent> sample_session() {
    return {
        status(header("AAPL", 0, 1'000, 1'100), TradingStatus::Open),
        quote(header("AAPL", 1, 1'000, 1'100), level("100", "3"), level("100.02", "1")),
        quote(header("SPY", 2, 1'000, 1'100), level("500", "10"), level("500.01", "30")),
        trade(header("AAPL", 3, 1'050, 1'200), "100.02", "1"),
        quote(header("AAPL", 4, 1'060, 1'200), level("100.01", "2"), std::nullopt),
        quote(header("SPY", 5, 990, 1'250), level("500", "5"), level("500.02", "5")),
    };
}

inline datasets::DatasetSource sample_source(std::string recording_sha256 = std::string(64, 'a')) {
    return datasets::DatasetSource{
        .provider = "test",
        .provider_dataset = "SYNTHETIC",
        .provider_schema = "top-of-book",
        .description = "synthetic dataset for tests",
        .recording_sha256 = std::move(recording_sha256),
        .coverage = datasets::DatasetCoverage{
            .start = Timestamp::from_nanoseconds(0),
            .end = Timestamp::from_nanoseconds(10'000),
            .semantics = "test window; start inclusive, end exclusive",
        },
        .normalizer = "chronos.test",
        .normalizer_version = "1",
    };
}

// Writes events into a catalog and returns what finish() returned.
inline std::variant<datasets::PublishedDataset, datasets::DatasetError> write_dataset(
    const std::filesystem::path& catalog,
    const std::vector<ValidatedMarketEvent>& events,
    datasets::DatasetSource source = sample_source(),
    std::string_view stream = kDatasetStream
) {
    auto created = datasets::NormalizedMarketDatasetWriter::create(
        catalog,
        MarketDataStreamId(std::string(stream))
    );
    REQUIRE(std::holds_alternative<std::unique_ptr<datasets::NormalizedMarketDatasetWriter>>(created));
    auto& writer = std::get<std::unique_ptr<datasets::NormalizedMarketDatasetWriter>>(created);
    for (const auto& event : events) {
        if (auto error = writer->append(event)) {
            return *error;
        }
    }
    return writer->finish(std::move(source));
}

inline datasets::PublishedDataset published(
    std::variant<datasets::PublishedDataset, datasets::DatasetError> result
) {
    if (auto* error = std::get_if<datasets::DatasetError>(&result)) {
        throw TestFailure("dataset was not published: " + error->field + ": " + error->message);
    }
    return std::get<datasets::PublishedDataset>(std::move(result));
}

inline std::string encoded(const ValidatedMarketEvent& event) {
    auto record = datasets::encode_market_event(event);
    REQUIRE(std::holds_alternative<std::string>(record));
    return std::get<std::string>(std::move(record));
}

// Writes a structurally valid dataset directory with arbitrary content:
// the given events file, a manifest built from `manifest` with the events'
// real digest (unless the caller overrides it), and the given quality file.
// The reader must then find any fault by itself.
inline std::filesystem::path forge_dataset(
    const std::filesystem::path& catalog,
    datasets::DatasetManifest manifest,
    std::string_view events,
    std::string_view quality,
    bool keep_events_digest = false
) {
    if (!keep_events_digest) {
        manifest.events = datasets::FileDigest{
            .sha256 = Sha256::hex(events),
            .size_bytes = events.size(),
        };
    }
    manifest.quality = datasets::FileDigest{
        .sha256 = Sha256::hex(quality),
        .size_bytes = quality.size(),
    };
    manifest.dataset_id = datasets::dataset_identity(manifest);
    const std::filesystem::path directory = catalog / manifest.dataset_id;
    std::filesystem::create_directory(directory);
    write_file(directory / datasets::kEventsFileName, events);
    write_file(directory / datasets::kQualityFileName, quality);
    write_file(directory / datasets::kManifestFileName, datasets::serialize_manifest(manifest));
    return directory;
}

// Reads a dataset to the end: its events, or the reader's typed error.
struct ReadOutcome {
    std::vector<ValidatedMarketEvent> events;
    std::optional<std::string> end_sha256;
    std::optional<datasets::DatasetError> error;
};

inline ReadOutcome read_dataset(const std::filesystem::path& directory) {
    ReadOutcome outcome;
    auto opened = datasets::NormalizedMarketEventReader::open(directory);
    if (auto* error = std::get_if<datasets::DatasetError>(&opened)) {
        outcome.error = *error;
        return outcome;
    }
    auto& reader = std::get<std::unique_ptr<datasets::NormalizedMarketEventReader>>(opened);
    while (true) {
        MarketEventRead read = reader->read_next();
        if (auto* event = std::get_if<ValidatedMarketEvent>(&read)) {
            outcome.events.push_back(std::move(*event));
        } else if (auto* end = std::get_if<EndOfMarketEvents>(&read)) {
            outcome.end_sha256 = end->source_sha256;
            return outcome;
        } else {
            outcome.error = reader->error();
            REQUIRE(outcome.error.has_value());
            return outcome;
        }
    }
}

}  // namespace chronos::test
