#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/strategy/strategy_simulation.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Immutable strategy-run artifacts: one schedule run through the strategy
// simulation over its verified dataset, with both benchmarks.
//
//     <strategy-root>/<run-id>/manifest.json
//     <strategy-root>/<run-id>/journal.ndjson   decisions, orders, executions,
//                                               expiries, and valuations
//
// The run ID is the SHA-256 of the canonical manifest without the ID. The
// manifest names the dataset, schedule, and model it came from,
// the configuration, every assumption, the runner's version, the build
// environment, and the results, so the same inputs and build give the same
// ID and bytes. Nothing is published unless the whole dataset and the whole
// schedule verify.
namespace chronos::strategy {

inline constexpr std::string_view kStrategyRunSchemaName = "chronos.strategy_run";
// A version 1 run names the experiment model its version 1 schedule came
// from; a version 2 run the registered model version of a version 2
// schedule, and no experiment; a version 3 run the walk-forward fold model
// of a version 3 schedule, and no experiment, split, or labels. Each stays
// readable.
inline constexpr std::string_view kExperimentStrategyRunVersion = "1";
inline constexpr std::string_view kRegisteredStrategyRunVersion = "2";
inline constexpr std::string_view kFoldStrategyRunVersion = "3";
inline constexpr std::string_view kJournalFileName = "journal.ndjson";
inline constexpr datasets::DerivedArtifactLayout kStrategyRunLayout{
    .kind = "strategy run",
    .rows_file = kJournalFileName,
};

struct StrategyRunManifest {
    std::string artifact_id;
    std::string schedule_id;
    std::string dataset_id;
    StrategyConfiguration configuration;
    std::uint64_t journal_entries{0};
    datasets::FileDigest journal;
    // The whole manifest, for summaries.
    canonical_json::Value document;
};

[[nodiscard]] std::variant<StrategyRunManifest, datasets::DatasetError> parse_strategy_run_manifest(
    std::string_view bytes
);

using PublishedStrategyRun = datasets::PublishedArtifact<StrategyRunManifest>;

// Runs the schedule at schedule_directory over its dataset in catalog and
// publishes the run into output_root, an existing directory.
[[nodiscard]] std::variant<PublishedStrategyRun, datasets::DatasetError> run_strategy(
    const std::filesystem::path& catalog,
    const std::filesystem::path& schedule_directory,
    const std::filesystem::path& output_root,
    const StrategyConfiguration& configuration
);

// Checks the directory, the manifest's form and identity, and every journal
// record's form, count, and checksum.
[[nodiscard]] std::variant<StrategyRunManifest, datasets::DatasetError> verify_strategy_run(
    const std::filesystem::path& directory
);

// Runs the schedule again from its sources and requires the same journal
// and results: proof the artifact is exactly what they produce.
[[nodiscard]] std::optional<datasets::DatasetError> reproduce_strategy_run(
    const StrategyRunManifest& manifest,
    const std::filesystem::path& catalog,
    const std::filesystem::path& schedules_root
);

using StrategyRunEntry = datasets::DerivedArtifactEntry<StrategyRunManifest>;

[[nodiscard]] std::variant<std::vector<StrategyRunEntry>, datasets::DatasetError> list_strategy_runs(
    const std::filesystem::path& root
);

}  // namespace chronos::strategy
