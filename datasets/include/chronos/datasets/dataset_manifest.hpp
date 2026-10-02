#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/market_data_validation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::datasets {

// Version 1 of a normalized market dataset. A dataset is a directory named
// by its dataset ID that holds exactly these three files; see
// docs/normalized-market-datasets.md and ADR 0012.
inline constexpr std::string_view kDatasetSchemaName = "chronos.normalized_market_dataset";
inline constexpr std::string_view kDatasetSchemaVersion = "1";
inline constexpr std::string_view kEventEncodingName = "canonical_ndjson";
inline constexpr std::string_view kEventEncodingVersion = "1";
inline constexpr std::string_view kMarketEventRecordName = "chronos.market_event";
inline constexpr std::string_view kManifestFileName = "manifest.json";
inline constexpr std::string_view kEventsFileName = "events.ndjson";
inline constexpr std::string_view kQualityFileName = "quality.json";
inline constexpr std::string_view kTimestampUnit = "nanoseconds_since_unix_epoch_utc";

// Format limits that every writer respects and every reader enforces, so
// nothing can be published that its reader would refuse.
inline constexpr std::size_t kMaximumRecordBytes = 64U << 10U;
inline constexpr std::uint64_t kMaximumManifestBytes = 16U << 20U;
inline constexpr std::uint64_t kMaximumQualityBytes = 64U << 20U;

// The window a provider was asked for, which may be wider than the events
// it returned.
struct DatasetCoverage {
    Timestamp start;
    Timestamp end;
    // How to read the window, such as "provider query window by exchange
    // timestamp; start inclusive, end exclusive".
    std::string semantics;

    friend bool operator==(const DatasetCoverage&, const DatasetCoverage&) = default;
};

// Where the events came from. Every field is part of the dataset's identity;
// none may name a machine, a path, or a time of conversion.
struct DatasetSource {
    std::string provider;
    std::string provider_dataset;
    std::string provider_schema;
    std::string description;
    // SHA-256 of the provider's original recording.
    std::string recording_sha256;
    std::optional<DatasetCoverage> coverage;
    // The mapping from provider records to Chronos events, such as
    // "chronos.databento.equs_mini_mbp1" version "1".
    std::string normalizer;
    std::string normalizer_version;

    friend bool operator==(const DatasetSource&, const DatasetSource&) = default;
};

struct EventTypeCounts {
    std::uint64_t trade{0};
    std::uint64_t top_of_book{0};
    std::uint64_t market_status{0};
    std::uint64_t instrument_definition{0};

    friend bool operator==(const EventTypeCounts&, const EventTypeCounts&) = default;
};

struct DatasetSummary {
    std::uint64_t events{0};
    EventTypeCounts by_type;
    std::map<std::string, std::uint64_t> by_instrument;
    std::uint64_t first_ingest_sequence{0};
    std::uint64_t last_ingest_sequence{0};
    Timestamp earliest_event_timestamp;
    Timestamp latest_event_timestamp;
    Timestamp earliest_receive_timestamp;
    Timestamp latest_receive_timestamp;

    friend bool operator==(const DatasetSummary&, const DatasetSummary&) = default;
};

// Accumulates the summary of a stream of events in bounded memory: one
// counter per instrument.
class DatasetSummaryBuilder final {
public:
    [[nodiscard]] std::optional<DatasetError> add(const ValidatedMarketEvent& event);
    // Nullopt before the first event.
    [[nodiscard]] const std::optional<DatasetSummary>& summary() const noexcept {
        return summary_;
    }

private:
    std::optional<DatasetSummary> summary_;
};

struct FileDigest {
    std::string sha256;
    std::uint64_t size_bytes{0};

    friend bool operator==(const FileDigest&, const FileDigest&) = default;
};

// {"name", "sha256", "size_bytes"}: how every manifest names one of its files.
[[nodiscard]] canonical_json::Value file_digest_json(std::string_view name, const FileDigest& digest);
[[nodiscard]] FileDigest parse_file_digest(ObjectFields& parent, std::string_view key, std::string_view name);

struct DatasetManifest {
    // SHA-256 of this manifest's canonical form without the dataset ID.
    std::string dataset_id;
    MarketDataStreamId stream;
    DatasetSource source;
    DatasetSummary summary;
    FileDigest events;
    FileDigest quality;

    friend bool operator==(const DatasetManifest&, const DatasetManifest&) = default;
};

// The dataset ID the manifest's content defines.
[[nodiscard]] std::string dataset_identity(const DatasetManifest& manifest);

[[nodiscard]] canonical_json::Value manifest_json(const DatasetManifest& manifest);
// The exact bytes of manifest.json.
[[nodiscard]] std::string serialize_manifest(const DatasetManifest& manifest);

// Parses manifest.json strictly: canonical bytes, known fields only, the
// supported schema and scales, and internally consistent counts, sequences,
// instruments, and identity.
[[nodiscard]] std::variant<DatasetManifest, DatasetError> parse_manifest(
    std::string_view bytes
);

[[nodiscard]] canonical_json::Value event_type_counts_json(const EventTypeCounts& counts);

}  // namespace chronos::datasets
