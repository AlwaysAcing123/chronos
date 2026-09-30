#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/features/top_of_book_features.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Immutable feature artifacts derived from a normalized dataset. An artifact
// is a directory named by its artifact ID in a feature root separate from
// the dataset catalog, so a dataset directory is never modified:
//
//     <feature-root>/<artifact-id>/manifest.json
//     <feature-root>/<artifact-id>/features.ndjson
//
// The artifact ID is the SHA-256 of the canonical manifest without the ID.
// The manifest names the source dataset ID and events checksum, the feature
// set and its version, the configuration that fixes the values' meaning,
// and the rows' checksum, so the same dataset and configuration always
// produce the same ID and bytes.
namespace chronos::features {

inline constexpr std::string_view kFeatureArtifactSchemaName = "chronos.feature_artifact";
inline constexpr std::string_view kFeatureArtifactSchemaVersion = "1";
inline constexpr std::string_view kFeatureRowRecordName = "chronos.feature_row";
inline constexpr std::string_view kFeatureRowRecordVersion = "1";
inline constexpr std::string_view kFeatureManifestFileName = "manifest.json";
inline constexpr std::string_view kFeatureRowsFileName = "features.ndjson";

// The row's canonical record, without its newline.
[[nodiscard]] std::string encode_feature_row(const TopOfBookFeatureRow& row);
[[nodiscard]] std::variant<TopOfBookFeatureRow, datasets::DatasetError> decode_feature_row(
    std::string_view record,
    std::optional<std::uint64_t> line = std::nullopt
);

struct FeatureArtifactManifest {
    std::string artifact_id;
    std::string dataset_id;
    std::string dataset_events_sha256;
    MarketDataStreamId stream;
    std::uint64_t rows{0};
    std::map<std::string, std::uint64_t> rows_by_instrument;
    // For each feature, how many rows gave each unavailability reason.
    std::map<std::string, std::map<std::string, std::uint64_t>> unavailable;
    datasets::FileDigest features;

    friend bool operator==(const FeatureArtifactManifest&, const FeatureArtifactManifest&) = default;
};

[[nodiscard]] std::string feature_artifact_identity(const FeatureArtifactManifest& manifest);
[[nodiscard]] std::string serialize_feature_manifest(const FeatureArtifactManifest& manifest);
[[nodiscard]] std::variant<FeatureArtifactManifest, datasets::DatasetError> parse_feature_manifest(
    std::string_view bytes
);

struct PublishedFeatureArtifact {
    FeatureArtifactManifest manifest;
    std::filesystem::path directory;
    datasets::PublicationOutcome outcome;
    // Published, but a step after the atomic rename failed.
    std::optional<datasets::DatasetError> warning;
};

// Reads and verifies a dataset, computes a row for every top-of-book event,
// and publishes the artifact atomically into output_root, an existing
// directory. Nothing is published unless the whole dataset verifies.
[[nodiscard]] std::variant<PublishedFeatureArtifact, datasets::DatasetError>
extract_top_of_book_features(
    const std::filesystem::path& dataset_directory,
    const std::filesystem::path& output_root
);

// Reads every row of an artifact and returns its manifest when the rows are
// canonical, in ingest order, and agree with the manifest's counts and
// checksum.
[[nodiscard]] std::variant<FeatureArtifactManifest, datasets::DatasetError> verify_feature_artifact(
    const std::filesystem::path& directory
);

// Recomputes the rows from the source dataset and compares their checksum
// with the artifact's: proof the artifact is exactly what the dataset and
// this feature code produce.
[[nodiscard]] std::optional<datasets::DatasetError> reproduce_feature_artifact(
    const FeatureArtifactManifest& manifest,
    const std::filesystem::path& dataset_directory
);

struct FeatureArtifactEntry {
    std::string name;
    std::variant<FeatureArtifactManifest, datasets::DatasetError> status;
};

// Every entry of a feature root in byte order, with manifests checked.
[[nodiscard]] std::variant<std::vector<FeatureArtifactEntry>, datasets::DatasetError>
list_feature_artifacts(const std::filesystem::path& root);

}  // namespace chronos::features
