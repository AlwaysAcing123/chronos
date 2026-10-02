#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/labels/label_artifact.hpp"
#include "dataset_cli_support.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// `chronos_dataset labels ...`: extract, list, and inspect label artifacts.
namespace chronos::label_cli {

namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;

inline constexpr std::string_view kFutureMidpointReturnLabelSet = "future_midpoint_return_v1";

inline json::Value artifact_summary_json(
    const labels::LabelArtifactManifest& manifest,
    const std::filesystem::path& directory
) {
    return json::Value(json::Object{
        {"artifact_id", manifest.artifact_id},
        {"directions", json::Object{
            {"down", cli::decimal(manifest.directions.down)},
            {"flat", cli::decimal(manifest.directions.flat)},
            {"up", cli::decimal(manifest.directions.up)},
        }},
        {"directory", cli::printable_path(directory)},
        {"horizon_ns", std::to_string(manifest.horizon_ns)},
        {"label_set", kFutureMidpointReturnLabelSet},
        {"labels_sha256", manifest.labels.sha256},
        {"rows", cli::decimal(manifest.rows)},
        {"rows_by_instrument", datasets::count_map_json(manifest.rows_by_instrument)},
        {"size_bytes", cli::decimal(manifest.labels.size_bytes)},
        {"source", json::Object{
            {"dataset_id", manifest.dataset_id},
            {"feature_artifact_id", manifest.feature_artifact_id},
            {"features_sha256", manifest.features_sha256},
            {"stream", manifest.stream.value()},
        }},
        {"unavailable", datasets::count_map_json(manifest.unavailable)},
        {"usable", cli::decimal(labels::usable_labels(manifest))},
    });
}

inline int extract(const cli::Options& options) {
    const std::string label_set =
        options.optional("--label-set").value_or(std::string(kFutureMidpointReturnLabelSet));
    if (label_set != kFutureMidpointReturnLabelSet) {
        throw cli::UsageError("the only label set is " + std::string(kFutureMidpointReturnLabelSet));
    }
    const std::int64_t horizon_ns = static_cast<std::int64_t>(cli::positive_integer(
        options.optional("--horizon-ns").value_or(std::to_string(labels::kDefaultHorizonNanoseconds)),
        "--horizon-ns",
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
    ));
    const std::filesystem::path features =
        cli::id_path(options, "--features", "--feature-artifact");
    const std::filesystem::path output = options.required("--output");
    if (auto error = cli::ensure_directory(output, "output")) {
        return cli::fail(*error);
    }
    auto extracted = labels::extract_future_midpoint_return_labels(features, output, horizon_ns);
    if (auto* error = std::get_if<datasets::DatasetError>(&extracted)) {
        return cli::fail(*error);
    }
    const auto& artifact = std::get<labels::PublishedLabelArtifact>(extracted);
    return cli::print(json::Value(json::Object{
        {"artifact", artifact_summary_json(artifact.manifest, artifact.directory)},
        {"outcome", cli::outcome_name(artifact.outcome)},
        {"warning", cli::warning_json(artifact.warning)},
    }));
}

inline int list(const cli::Options& options) {
    const std::filesystem::path output = options.required("--output");
    auto listed = labels::list_label_artifacts(output);
    if (auto* error = std::get_if<datasets::DatasetError>(&listed)) {
        return cli::fail(*error);
    }
    return cli::print(cli::artifact_list_json(
        output,
        std::get<std::vector<labels::LabelArtifactEntry>>(listed),
        artifact_summary_json
    ));
}

inline int inspect(const cli::Options& options) {
    const std::filesystem::path directory =
        cli::id_path(options, "--output", "--artifact");
    auto verified = labels::verify_label_artifact(directory);
    if (auto* error = std::get_if<datasets::DatasetError>(&verified)) {
        return cli::fail(*error);
    }
    const auto& manifest = std::get<labels::LabelArtifactManifest>(verified);
    std::string reproduced = "not checked: pass --features to recompute the labels from their feature artifact";
    if (const auto features = options.optional("--features")) {
        const std::filesystem::path source = std::filesystem::path(*features) / manifest.feature_artifact_id;
        if (auto error = labels::reproduce_label_artifact(manifest, source)) {
            return cli::fail(*error);
        }
        reproduced = "recomputed from the source feature artifact: identical rows";
    }
    return cli::print(json::Value(json::Object{
        {"artifact", artifact_summary_json(manifest, directory)},
        {"reproduced", reproduced},
        {"verified",
         "every row, its future observation and label, the counts, the checksum, and the artifact ID"},
    }));
}

inline int run(int argc, char* argv[]) {
    if (argc < 3) {
        throw cli::UsageError("labels needs a command: extract, list, or inspect");
    }
    const std::string_view command = argv[2];
    if (command == "extract") {
        return extract(cli::Options(
            argc, argv, 3, {"--features", "--feature-artifact", "--output", "--horizon-ns", "--label-set"}
        ));
    }
    if (command == "list") {
        return list(cli::Options(argc, argv, 3, {"--output"}));
    }
    if (command == "inspect") {
        return inspect(cli::Options(argc, argv, 3, {"--output", "--artifact", "--features"}));
    }
    throw cli::UsageError("unknown labels command: " + std::string(command));
}

}  // namespace chronos::label_cli
