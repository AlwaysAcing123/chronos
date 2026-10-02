#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/splits/split_artifact.hpp"
#include "dataset_cli_support.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// `chronos_dataset splits ...`: create, list, and inspect chronological
// split artifacts.
namespace chronos::split_cli {

namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;

inline constexpr std::string_view kChronologicalSplitSet = "chronological_purged_v1";

// The whole manifest, which consumers such as the research package read
// instead of the file, and where it is.
inline json::Value artifact_summary_json(
    const splits::SplitArtifactManifest& manifest,
    const std::filesystem::path& directory
) {
    json::Object summary = *splits::split_manifest_json(manifest).if_object();
    summary.emplace_back("directory", cli::printable_path(directory));
    return json::Value(std::move(summary));
}

inline int create(const cli::Options& options) {
    const std::string split_set = options.optional("--split-set").value_or(std::string(kChronologicalSplitSet));
    if (split_set != kChronologicalSplitSet) {
        throw cli::UsageError("the only split set is " + std::string(kChronologicalSplitSet));
    }
    const auto percent = [&](const std::string& option, std::uint64_t fallback) {
        return cli::positive_integer(options.optional(option).value_or(std::to_string(fallback)), option, 98);
    };
    const splits::SplitPercentages percentages{
        .train = percent("--train-percent", splits::kDefaultSplitPercentages.train),
        .validation = percent("--validation-percent", splits::kDefaultSplitPercentages.validation),
        .test = percent("--test-percent", splits::kDefaultSplitPercentages.test),
    };
    if (percentages.train + percentages.validation + percentages.test != 100) {
        throw cli::UsageError("--train-percent, --validation-percent, and --test-percent must add up to exactly 100");
    }
    const std::filesystem::path labels =
        cli::id_path(options, "--labels", "--label-artifact");
    const std::filesystem::path output = options.required("--output");
    if (auto error = cli::ensure_directory(output, "output")) {
        return cli::fail(*error);
    }
    auto created = splits::create_chronological_split(labels, output, percentages);
    if (auto* error = std::get_if<datasets::DatasetError>(&created)) {
        return cli::fail(*error);
    }
    const auto& artifact = std::get<splits::PublishedSplitArtifact>(created);
    return cli::print(json::Value(json::Object{
        {"artifact", artifact_summary_json(artifact.manifest, artifact.directory)},
        {"outcome", cli::outcome_name(artifact.outcome)},
        {"warning", cli::warning_json(artifact.warning)},
    }));
}

inline int list(const cli::Options& options) {
    const std::filesystem::path output = options.required("--output");
    auto listed = splits::list_split_artifacts(output);
    if (auto* error = std::get_if<datasets::DatasetError>(&listed)) {
        return cli::fail(*error);
    }
    return cli::print(cli::artifact_list_json(
        output,
        std::get<std::vector<splits::SplitArtifactEntry>>(listed),
        artifact_summary_json
    ));
}

inline int inspect(const cli::Options& options) {
    const std::filesystem::path directory =
        cli::id_path(options, "--output", "--artifact");
    auto verified = splits::verify_split_artifact(directory);
    if (auto* error = std::get_if<datasets::DatasetError>(&verified)) {
        return cli::fail(*error);
    }
    const auto& manifest = std::get<splits::SplitArtifactManifest>(verified);
    std::string reproduced = "not checked: pass --labels to recompute the split from its label artifact";
    if (const auto labels = options.optional("--labels")) {
        const std::filesystem::path source = std::filesystem::path(*labels) / manifest.label_artifact_id;
        if (auto error = splits::reproduce_split_artifact(manifest, source)) {
            return cli::fail(*error);
        }
        reproduced = "recomputed from the source label artifact: identical rows";
    }
    return cli::print(json::Value(json::Object{
        {"artifact", artifact_summary_json(manifest, directory)},
        {"reproduced", reproduced},
        {"verified",
         "every row's period, exclusion, and label timing, the no-leakage inequalities, the boundaries, the "
         "summaries, the checksum, and the artifact ID"},
    }));
}

inline int run(int argc, char* argv[]) {
    if (argc < 3) {
        throw cli::UsageError("splits needs a command: create, list, or inspect");
    }
    const std::string_view command = argv[2];
    if (command == "create") {
        return create(cli::Options(argc, argv, 3, {
            "--labels", "--label-artifact", "--output", "--train-percent", "--validation-percent", "--test-percent",
            "--split-set",
        }));
    }
    if (command == "list") {
        return list(cli::Options(argc, argv, 3, {"--output"}));
    }
    if (command == "inspect") {
        return inspect(cli::Options(argc, argv, 3, {"--output", "--artifact", "--labels"}));
    }
    throw cli::UsageError("unknown splits command: " + std::string(command));
}

}  // namespace chronos::split_cli
