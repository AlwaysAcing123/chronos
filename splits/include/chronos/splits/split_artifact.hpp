#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/splits/chronological_split.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Immutable split artifacts derived from a label artifact, in their own root:
//
//     <split-root>/<artifact-id>/manifest.json
//     <split-root>/<artifact-id>/splits.ndjson
//
// There is one row per label row, in the same order, with the same identity,
// assigning it to train, validation, or test, or excluding it with a
// reason. The artifact ID is the SHA-256 of the canonical manifest without
// the ID; the manifest names the source label artifact and its checksum,
// the percentages, the resulting boundaries, per-period and per-split
// counts and times, and the rows' checksum.
namespace chronos::splits {

inline constexpr std::string_view kSplitArtifactSchemaName = "chronos.split_artifact";
inline constexpr std::string_view kSplitArtifactSchemaVersion = "1";
inline constexpr std::string_view kSplitRowRecordName = "chronos.split_row";
inline constexpr std::string_view kSplitRowRecordVersion = "1";
inline constexpr std::string_view kSplitGeneratorName = "chronos.splits.chronological_purged";
inline constexpr std::string_view kSplitGeneratorVersion = "1";
inline constexpr std::string_view kSplitRowsFileName = "splits.ndjson";
inline constexpr datasets::DerivedArtifactLayout kSplitArtifactLayout{
    .kind = "split artifact",
    .rows_file = kSplitRowsFileName,
};

// The row's canonical record, without its newline.
[[nodiscard]] std::string encode_split_row(const SplitRow& row);
[[nodiscard]] std::variant<SplitRow, datasets::DatasetError> decode_split_row(
    std::string_view record,
    std::optional<std::uint64_t> line = std::nullopt
);

// Every row of a period, usable or not.
struct PeriodSummary {
    std::uint64_t rows{0};
    Timestamp first_prediction_at;
    Timestamp last_prediction_at;

    friend bool operator==(const PeriodSummary&, const PeriodSummary&) = default;
};

// The usable rows of a split.
struct UsableSummary {
    std::uint64_t rows{0};
    Timestamp first_prediction_at;
    Timestamp last_prediction_at;
    Timestamp max_label_available_at;

    friend bool operator==(const UsableSummary&, const UsableSummary&) = default;
};

struct SplitArtifactManifest {
    std::string artifact_id;
    // The source label artifact and, through it, the features and dataset.
    std::string dataset_id;
    std::string feature_artifact_id;
    std::string label_artifact_id;
    std::string labels_sha256;
    MarketDataStreamId stream;
    std::int64_t horizon_ns{0};
    SplitPercentages percentages;
    SplitBoundaries boundaries;
    std::uint64_t rows{0};
    ByPeriod<PeriodSummary> periods;
    ByPeriod<UsableSummary> usable;
    // Rows in no split, by reason.
    std::map<std::string, std::uint64_t> excluded;
    datasets::FileDigest splits;

    friend bool operator==(const SplitArtifactManifest&, const SplitArtifactManifest&) = default;
};

[[nodiscard]] std::string split_artifact_identity(const SplitArtifactManifest& manifest);
// The manifest as a JSON object, artifact_id included.
[[nodiscard]] canonical_json::Value split_manifest_json(const SplitArtifactManifest& manifest);
[[nodiscard]] std::string serialize_split_manifest(const SplitArtifactManifest& manifest);
// Also refuses a manifest whose summaries break the split's invariants,
// above all a usable train or validation label known at or after the next
// period begins.
[[nodiscard]] std::variant<SplitArtifactManifest, datasets::DatasetError> parse_split_manifest(
    std::string_view bytes
);

using PublishedSplitArtifact = datasets::PublishedArtifact<SplitArtifactManifest>;

// Reads and verifies a label artifact completely, splits its rows, and
// publishes the split artifact atomically into output_root, an existing
// directory. Nothing is published unless the whole source verifies and
// every period and split is nonempty.
[[nodiscard]] std::variant<PublishedSplitArtifact, datasets::DatasetError> create_chronological_split(
    const std::filesystem::path& label_artifact_directory,
    const std::filesystem::path& output_root,
    const SplitPercentages& percentages = kDefaultSplitPercentages
);

// Reads every row and returns the manifest when the rows are canonical, on
// the manifest's stream, in ingest order with prediction times that never
// decrease; each row's period and exclusion follow from its position and
// label under the manifest's boundaries; no usable label crosses a
// boundary; the boundaries are the ones the percentages give for these
// prediction times; and the counts, summaries, and checksum match.
[[nodiscard]] std::variant<SplitArtifactManifest, datasets::DatasetError> verify_split_artifact(
    const std::filesystem::path& directory
);

// Recomputes the rows from the source label artifact and compares their
// checksum with the artifact's.
[[nodiscard]] std::optional<datasets::DatasetError> reproduce_split_artifact(
    const SplitArtifactManifest& manifest,
    const std::filesystem::path& label_artifact_directory
);

using SplitArtifactEntry = datasets::DerivedArtifactEntry<SplitArtifactManifest>;

// Every entry of a split root in byte order, with manifests checked.
[[nodiscard]] std::variant<std::vector<SplitArtifactEntry>, datasets::DatasetError> list_split_artifacts(
    const std::filesystem::path& root
);

}  // namespace chronos::splits
