#pragma once

#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_event_reader.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace chronos::market_data {

// A normalized dataset, verified end to end, opened again for playback.
struct DatasetReplay {
    std::unique_ptr<MarketEventReader> reader;
    MarketDataSourceDescription description;
    std::string dataset_id;
};

// Why a dataset cannot be replayed: a safe message, never event bytes.
struct DatasetReplayRefusal {
    std::string message;
};

// Opens a normalized dataset (ADR 0012) for a paced replay (ADR 0025).
//
// Before anything is exposed, every event is read and the count, checksum,
// summary, and quality report are proved equal to the manifest's
// (datasets::verify_dataset); the directory's metadata and file sizes alone
// prove nothing about its events. The manifest's stream must be `stream`
// and its instruments exactly `instruments`: a subset is refused, because
// every event of the dataset enters the market state. The playback reader
// is opened afterwards and must name the same dataset; it checks the
// checksum again at the end. Nothing is relabelled or rewritten.
//
// Verification and playback read the files twice, so they assume the
// directory is immutable and private to the operator, as a published,
// read-only dataset is. A file changed in between fails the replay at its
// first invalid event or at the end, after any valid prefix was played.
[[nodiscard]] std::variant<DatasetReplay, DatasetReplayRefusal> open_dataset_replay(
    const std::filesystem::path& directory,
    const MarketDataStreamId& stream,
    const std::vector<InstrumentId>& instruments
);

// How a dataset's replay is labelled, from its provenance alone: a
// synthetic dataset says that its prices are invented, and a recording
// keeps its provider, dataset, schema, normalizer, and query window.
[[nodiscard]] MarketDataSourceDescription dataset_replay_description(
    const datasets::DatasetManifest& manifest,
    std::vector<InstrumentId> instruments
);

}  // namespace chronos::market_data
