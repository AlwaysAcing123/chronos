#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/labels/future_midpoint_return.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Immutable label artifacts derived from a feature artifact, in their own
// root so a feature artifact is never modified:
//
//     <label-root>/<artifact-id>/manifest.json
//     <label-root>/<artifact-id>/labels.ndjson
//
// There is one label row per feature row, in the same order, identified by
// the same stream, instrument, and ingest sequence. The artifact ID is the
// SHA-256 of the canonical manifest without the ID; the manifest names the
// source feature artifact and its checksum, the label definition and its
// horizon, the generator, the counts, and the rows' checksum.
namespace chronos::labels {

inline constexpr std::string_view kLabelArtifactSchemaName = "chronos.label_artifact";
inline constexpr std::string_view kLabelArtifactSchemaVersion = "1";
inline constexpr std::string_view kLabelRowRecordName = "chronos.label_row";
inline constexpr std::string_view kLabelRowRecordVersion = "1";
// The code that computes the rows. Its version changes whenever the same
// source and definition would produce different bytes.
inline constexpr std::string_view kLabelGeneratorName = "chronos.labels.future_midpoint_return";
inline constexpr std::string_view kLabelGeneratorVersion = "1";
inline constexpr std::string_view kLabelRowsFileName = "labels.ndjson";
inline constexpr datasets::DerivedArtifactLayout kLabelArtifactLayout{
    .kind = "label artifact",
    .rows_file = kLabelRowsFileName,
};

// The row's canonical record, without its newline.
[[nodiscard]] std::string encode_label_row(const FutureMidpointReturnRow& row);
[[nodiscard]] std::variant<FutureMidpointReturnRow, datasets::DatasetError> decode_label_row(
    std::string_view record,
    std::optional<std::uint64_t> line = std::nullopt
);

struct DirectionCounts {
    std::uint64_t down{0};
    std::uint64_t flat{0};
    std::uint64_t up{0};

    friend bool operator==(const DirectionCounts&, const DirectionCounts&) = default;
};

struct LabelArtifactManifest {
    std::string artifact_id;
    // The source feature artifact and, through it, the dataset.
    std::string dataset_id;
    std::string feature_artifact_id;
    std::string features_sha256;
    MarketDataStreamId stream;
    std::int64_t horizon_ns{0};
    std::uint64_t rows{0};
    std::map<std::string, std::uint64_t> rows_by_instrument;
    // Rows with a label, by direction.
    DirectionCounts directions;
    // Rows without one, by reason.
    std::map<std::string, std::uint64_t> unavailable;
    datasets::FileDigest labels;

    friend bool operator==(const LabelArtifactManifest&, const LabelArtifactManifest&) = default;
};

[[nodiscard]] std::uint64_t usable_labels(const LabelArtifactManifest& manifest) noexcept;

[[nodiscard]] std::string label_artifact_identity(const LabelArtifactManifest& manifest);
[[nodiscard]] std::string serialize_label_manifest(const LabelArtifactManifest& manifest);
[[nodiscard]] std::variant<LabelArtifactManifest, datasets::DatasetError> parse_label_manifest(
    std::string_view bytes
);

using PublishedLabelArtifact = datasets::PublishedArtifact<LabelArtifactManifest>;

// Reads and verifies a feature artifact completely, labels every row, and
// publishes the label artifact atomically into output_root, an existing
// directory. Nothing is published unless the whole source verifies.
[[nodiscard]] std::variant<PublishedLabelArtifact, datasets::DatasetError>
extract_future_midpoint_return_labels(
    const std::filesystem::path& feature_artifact_directory,
    const std::filesystem::path& output_root,
    std::int64_t horizon_ns = kDefaultHorizonNanoseconds
);

// Reads a label artifact's rows in order, checking each as it goes: every
// row is canonical, on the manifest's stream and horizon, after the
// previous one in ingest order, with a future observation that is later in
// ingest order, at least one horizon away, and correctly timed, and with the
// label its own midpoints and future observation determine. The end is
// reported only once the count, checksum, and summary match the manifest
// and a second pass has shown that each row's future observation is the one
// the definition selects from the artifact's own rows. A consumer must not
// act on rows before it sees EndOfRows.
class LabelArtifactReader final {
public:
    [[nodiscard]] static std::variant<std::unique_ptr<LabelArtifactReader>, datasets::DatasetError> open(
        const std::filesystem::path& directory
    );

    ~LabelArtifactReader();
    LabelArtifactReader(const LabelArtifactReader&) = delete;
    LabelArtifactReader& operator=(const LabelArtifactReader&) = delete;

    [[nodiscard]] const LabelArtifactManifest& manifest() const noexcept;
    // Where the rows were read from, without a trailing slash.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] std::variant<FutureMidpointReturnRow, datasets::EndOfRows, datasets::DatasetError> next();

private:
    class State;
    explicit LabelArtifactReader(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

// Reads every row with LabelArtifactReader and returns the manifest once the
// whole artifact has verified.
[[nodiscard]] std::variant<LabelArtifactManifest, datasets::DatasetError> verify_label_artifact(
    const std::filesystem::path& directory
);

// Recomputes the rows from the source feature artifact and compares their
// checksum with the artifact's: proof the artifact is exactly what that
// feature artifact and this definition produce.
[[nodiscard]] std::optional<datasets::DatasetError> reproduce_label_artifact(
    const LabelArtifactManifest& manifest,
    const std::filesystem::path& feature_artifact_directory
);

using LabelArtifactEntry = datasets::DerivedArtifactEntry<LabelArtifactManifest>;

// Every entry of a label root in byte order, with manifests checked.
[[nodiscard]] std::variant<std::vector<LabelArtifactEntry>, datasets::DatasetError> list_label_artifacts(
    const std::filesystem::path& root
);

}  // namespace chronos::labels
