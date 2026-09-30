#pragma once

#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"
#include "chronos/databento/recording_manifest.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <variant>

namespace chronos::databento_adapter {

// The mapping from EQUS.MINI MBP-1 records to Chronos events, as the
// normalizer implements it. A change to that mapping must change this
// version, and with it every dataset ID derived from a recording.
inline constexpr std::string_view kDatasetNormalizerName = "chronos.databento.equs_mini_mbp1";
inline constexpr std::string_view kDatasetNormalizerVersion = "1";

// Exactly one of the two is present: the recording could not be read, or
// the dataset could not be written.
struct RecordingConversionError {
    std::optional<EquitiesMiniMbp1FileError> recording;
    std::optional<datasets::DatasetError> dataset;
};

using RecordingConversionResult = std::variant<
    datasets::PublishedDataset,
    RecordingConversionError
>;

// The provider-independent provenance of a completed recording. The query
// window becomes the dataset's coverage when both ends are representable
// Chronos timestamps; otherwise the dataset records no coverage.
[[nodiscard]] datasets::DatasetSource dataset_source(const RecordingManifest& manifest);

// Streams a local EQUS.MINI MBP-1 recording into a new normalized dataset in
// catalog, an existing directory. Reads no network and needs no API key.
// Publishes nothing unless the whole recording decodes, validates, and
// verifies.
[[nodiscard]] RecordingConversionResult convert_recording_to_dataset(
    const std::filesystem::path& recording,
    MarketDataStreamId stream,
    const std::filesystem::path& catalog
);

}  // namespace chronos::databento_adapter
