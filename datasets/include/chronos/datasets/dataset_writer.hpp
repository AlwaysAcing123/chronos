#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/market_data_validation.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <variant>

namespace chronos::datasets {

enum class PublicationOutcome {
    Published,
    // An identical, valid dataset already had this ID; nothing was written.
    AlreadyPresent,
};

struct PublishedDataset {
    DatasetManifest manifest;
    std::filesystem::path directory;
    PublicationOutcome outcome;
    // The dataset is published, but a step after the atomic rename failed:
    // it may not be read-only or durable yet.
    std::optional<DatasetError> warning;
};

// Writes validated events into a new normalized dataset in a catalog.
//
// Events are streamed to a staging directory and hashed as they are
// written, so memory is bounded by the number of instruments. finish()
// writes the quality report and manifest, derives the dataset ID, and
// publishes the directory atomically under that ID. Any failure, including
// abandoning the writer, removes only the staging directory; a published
// dataset is never replaced or modified.
class NormalizedMarketDatasetWriter final {
public:
    // The catalog must be an existing directory. Every event must belong to
    // stream, with ingest sequences contiguous from the first event and
    // receive time never decreasing.
    [[nodiscard]] static std::variant<std::unique_ptr<NormalizedMarketDatasetWriter>, DatasetError>
    create(const std::filesystem::path& catalog, MarketDataStreamId stream);

    ~NormalizedMarketDatasetWriter();

    NormalizedMarketDatasetWriter(const NormalizedMarketDatasetWriter&) = delete;
    NormalizedMarketDatasetWriter& operator=(const NormalizedMarketDatasetWriter&) = delete;

    // After an error the writer is failed: every later call returns that
    // error and nothing is published.
    [[nodiscard]] std::optional<DatasetError> append(const ValidatedMarketEvent& event);

    // Publishes the dataset. When a directory with its ID already exists it
    // must verify as exactly this dataset (AlreadyPresent); anything else
    // there is a Collision and is left untouched.
    [[nodiscard]] std::variant<PublishedDataset, DatasetError> finish(DatasetSource source);

private:
    class Impl;
    explicit NormalizedMarketDatasetWriter(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

}  // namespace chronos::datasets
