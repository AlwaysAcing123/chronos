#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/market_event_reader.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace chronos::datasets {

// A dataset directory whose structure, manifest, identity, and quality
// file have been checked; its events are verified only as they are read.
struct OpenedDataset {
    DatasetManifest manifest;
    std::string quality_report;
};

// Checks everything but the events: the directory holds exactly the
// dataset's three regular files, the manifest is canonical, consistent,
// and names this directory, and quality.json and events.ndjson have the
// sizes, and quality.json the checksum, the manifest records.
[[nodiscard]] std::variant<OpenedDataset, DatasetError> open_dataset(
    const std::filesystem::path& directory
);

// Streams a normalized dataset as validated market events. Each record must
// be canonical and pass Chronos validation and the dataset's stream
// contract. Reaching the end also proves that the event count, checksum,
// summary, and quality report are exactly those in the manifest; only then
// is EndOfMarketEvents returned, carrying the events' SHA-256.
class NormalizedMarketEventReader final : public MarketEventReader {
public:
    [[nodiscard]] static std::variant<std::unique_ptr<NormalizedMarketEventReader>, DatasetError>
    open(const std::filesystem::path& directory);

    ~NormalizedMarketEventReader() override;

    NormalizedMarketEventReader(const NormalizedMarketEventReader&) = delete;
    NormalizedMarketEventReader& operator=(const NormalizedMarketEventReader&) = delete;

    [[nodiscard]] MarketEventRead read_next() override;

    [[nodiscard]] const DatasetManifest& manifest() const noexcept;
    // The typed reason for the last MarketEventReadError.
    [[nodiscard]] const std::optional<DatasetError>& error() const noexcept;

private:
    class Impl;
    explicit NormalizedMarketEventReader(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

// Reads every event of a dataset and returns its manifest when all of it
// verifies.
[[nodiscard]] std::variant<DatasetManifest, DatasetError> verify_dataset(
    const std::filesystem::path& directory
);

}  // namespace chronos::datasets
