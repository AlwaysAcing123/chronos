#include "chronos/databento/dataset_conversion.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos::databento_adapter {
namespace {

std::optional<Timestamp> representable(std::uint64_t nanoseconds) {
    if (nanoseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return Timestamp::from_nanoseconds(static_cast<std::int64_t>(nanoseconds));
}

RecordingConversionError dataset_failure(datasets::DatasetError error) {
    return RecordingConversionError{.recording = std::nullopt, .dataset = std::move(error)};
}

}  // namespace

datasets::DatasetSource dataset_source(const RecordingManifest& manifest) {
    const auto start = representable(manifest.query_start_nanoseconds());
    const auto end = representable(manifest.query_end_nanoseconds());
    std::optional<datasets::DatasetCoverage> coverage;
    if (start && end && *start <= *end) {
        coverage = datasets::DatasetCoverage{
            .start = *start,
            .end = *end,
            // Databento filters an MBP-1 request on ts_recv, its index
            // timestamp (the pinned SDK's Mbp1Msg::IndexTs), not ts_event.
            .semantics = "Databento query window by receive timestamp (ts_recv); "
                         "start inclusive, end exclusive",
        };
    }
    return datasets::DatasetSource{
        .provider = "databento",
        .provider_dataset = std::string(manifest.dataset()),
        .provider_schema = std::string(manifest.schema()),
        .description = "Databento " + std::string(manifest.dataset()) + " " +
            std::string(manifest.schema()) + " historical recording",
        .recording_sha256 = std::string(manifest.source_sha256()),
        .coverage = std::move(coverage),
        .normalizer = std::string(kDatasetNormalizerName),
        .normalizer_version = std::string(kDatasetNormalizerVersion),
    };
}

RecordingConversionResult convert_recording_to_dataset(
    const std::filesystem::path& recording,
    MarketDataStreamId stream,
    const std::filesystem::path& catalog
) {
    auto created = datasets::NormalizedMarketDatasetWriter::create(catalog, stream);
    if (auto* error = std::get_if<datasets::DatasetError>(&created)) {
        return dataset_failure(std::move(*error));
    }
    auto& writer = std::get<std::unique_ptr<datasets::NormalizedMarketDatasetWriter>>(created);

    auto opened = EquitiesMiniMbp1FileReader::open(recording, std::move(stream));
    if (auto* error = std::get_if<EquitiesMiniMbp1FileError>(&opened)) {
        return RecordingConversionError{.recording = std::move(*error), .dataset = std::nullopt};
    }
    auto& reader = std::get<std::unique_ptr<EquitiesMiniMbp1FileReader>>(opened);
    while (true) {
        auto next = reader->read_next();
        if (auto* error = std::get_if<EquitiesMiniMbp1FileError>(&next)) {
            return RecordingConversionError{.recording = std::move(*error), .dataset = std::nullopt};
        }
        if (std::holds_alternative<EndOfRecording>(next)) {
            break;
        }
        if (auto error = writer->append(std::get<ValidatedMarketEvent>(next))) {
            return dataset_failure(std::move(*error));
        }
    }
    const auto& manifest = reader->manifest();
    if (!manifest) {
        return dataset_failure(datasets::dataset_error(
            datasets::DatasetErrorCode::InvalidArgument,
            "recording",
            "the completed recording produced no manifest"
        ));
    }
    auto finished = writer->finish(dataset_source(*manifest));
    if (auto* error = std::get_if<datasets::DatasetError>(&finished)) {
        return dataset_failure(std::move(*error));
    }
    return std::get<datasets::PublishedDataset>(std::move(finished));
}

}  // namespace chronos::databento_adapter
