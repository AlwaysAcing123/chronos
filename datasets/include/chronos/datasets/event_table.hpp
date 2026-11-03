#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"

#include <filesystem>
#include <istream>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::datasets {

// An event table is a small CSV of invented quotes and statuses, from which a
// synthetic dataset is written. Each line is one event, in ingest order:
//
//     quote,<instrument>,<receive_ns>,<bid>,<bid_size>,<ask>,<ask_size>
//     status,<instrument>,<receive_ns>,<open|halted|closed|pre_open|unknown>
//
// Prices and sizes are decimals, and an empty bid or ask pair is a missing
// side. Every event happens when it is received and is flagged as a replay.
// Blank lines and lines starting with '#' are ignored. Sequences are
// numbered from 0 in line order; the dataset writer still enforces the
// stream contract and validates every event.
[[nodiscard]] std::variant<PublishedDataset, DatasetError> write_event_table_dataset(
    std::istream& table,
    const std::filesystem::path& catalog,
    const MarketDataStreamId& stream,
    DatasetSource source
);

// How a scenario generator's event table is normalized (ADR 0025).
inline constexpr std::string_view kEventTableNormalizer = "chronos.synthetic.event_table";
inline constexpr std::string_view kEventTableNormalizerVersion = "1";

// The provenance of a named, versioned synthetic scenario's event table:
// provider `synthetic`, the scenario as the provider dataset, the table's
// real SHA-256 as the recording checksum, and no query window.
[[nodiscard]] DatasetSource synthetic_scenario_source(
    std::string_view scenario,
    std::string_view version,
    std::string table_sha256
);

}  // namespace chronos::datasets
