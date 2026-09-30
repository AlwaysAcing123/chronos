#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/features/feature_artifact.hpp"
#include "chronos/sha256.hpp"
#include "dataset_cli_support.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// `chronos_dataset features ...`: extract, list, and inspect feature
// artifacts.
namespace chronos::feature_cli {

namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;

inline constexpr std::string_view kTopOfBookFeatureSet = "top_of_book_v1";

inline json::Value artifact_summary_json(
    const features::FeatureArtifactManifest& manifest,
    const std::filesystem::path& directory
) {
    json::Object by_instrument;
    for (const auto& [instrument, count] : manifest.rows_by_instrument) {
        by_instrument.emplace_back(instrument, cli::decimal(count));
    }
    json::Object unavailable;
    for (const auto& [feature, reasons] : manifest.unavailable) {
        json::Object counts;
        for (const auto& [reason, count] : reasons) {
            counts.emplace_back(reason, cli::decimal(count));
        }
        unavailable.emplace_back(feature, std::move(counts));
    }
    return json::Value(json::Object{
        {"artifact_id", manifest.artifact_id},
        {"directory", cli::printable_path(directory)},
        {"feature_set", kTopOfBookFeatureSet},
        {"features_sha256", manifest.features.sha256},
        {"rows", cli::decimal(manifest.rows)},
        {"rows_by_instrument", std::move(by_instrument)},
        {"size_bytes", cli::decimal(manifest.features.size_bytes)},
        {"source", json::Object{
            {"dataset_id", manifest.dataset_id},
            {"events_sha256", manifest.dataset_events_sha256},
            {"stream", manifest.stream.value()},
        }},
        {"unavailable", std::move(unavailable)},
    });
}

inline std::filesystem::path id_path(const std::filesystem::path& root, const std::string& id, std::string_view option) {
    if (!is_sha256_hex(id)) {
        throw cli::UsageError(std::string(option) + " must be a 64-digit lowercase ID");
    }
    return root / id;
}

inline int extract(const cli::Options& options) {
    const std::string feature_set = options.optional("--feature-set").value_or(std::string(kTopOfBookFeatureSet));
    if (feature_set != kTopOfBookFeatureSet) {
        throw cli::UsageError("the only feature set is " + std::string(kTopOfBookFeatureSet));
    }
    const std::filesystem::path dataset =
        id_path(options.required("--catalog"), options.required("--dataset"), "--dataset");
    const std::filesystem::path output = options.required("--output");
    if (auto error = cli::ensure_directory(output, "output")) {
        return cli::fail(*error);
    }
    auto extracted = features::extract_top_of_book_features(dataset, output);
    if (auto* error = std::get_if<datasets::DatasetError>(&extracted)) {
        return cli::fail(*error);
    }
    const auto& artifact = std::get<features::PublishedFeatureArtifact>(extracted);
    return cli::print(json::Value(json::Object{
        {"artifact", artifact_summary_json(artifact.manifest, artifact.directory)},
        {"outcome", cli::outcome_name(artifact.outcome)},
        {"warning", cli::warning_json(artifact.warning)},
    }));
}

inline int list(const cli::Options& options) {
    const std::filesystem::path output = options.required("--output");
    auto listed = features::list_feature_artifacts(output);
    if (auto* error = std::get_if<datasets::DatasetError>(&listed)) {
        return cli::fail(*error);
    }
    json::Array artifacts;
    json::Array invalid;
    for (const auto& entry : std::get<std::vector<features::FeatureArtifactEntry>>(listed)) {
        if (const auto* manifest = std::get_if<features::FeatureArtifactManifest>(&entry.status)) {
            artifacts.push_back(artifact_summary_json(*manifest, output / entry.name));
        } else {
            invalid.push_back(json::Value(json::Object{
                {"error", cli::error_json(std::get<datasets::DatasetError>(entry.status))},
                {"name", cli::printable(entry.name)},
            }));
        }
    }
    return cli::print(json::Value(json::Object{
        {"artifacts", std::move(artifacts)},
        {"invalid_entries", std::move(invalid)},
        {"output", cli::printable_path(output)},
    }));
}

inline int inspect(const cli::Options& options) {
    const std::filesystem::path directory =
        id_path(options.required("--output"), options.required("--artifact"), "--artifact");
    auto verified = features::verify_feature_artifact(directory);
    if (auto* error = std::get_if<datasets::DatasetError>(&verified)) {
        return cli::fail(*error);
    }
    const auto& manifest = std::get<features::FeatureArtifactManifest>(verified);
    std::string reproduced = "not checked: pass --catalog to recompute the rows from their dataset";
    if (const auto catalog = options.optional("--catalog")) {
        if (auto error = features::reproduce_feature_artifact(manifest, std::filesystem::path(*catalog) / manifest.dataset_id)) {
            return cli::fail(*error);
        }
        reproduced = "recomputed from the source dataset: identical rows";
    }
    return cli::print(json::Value(json::Object{
        {"artifact", artifact_summary_json(manifest, directory)},
        {"reproduced", reproduced},
        {"verified", "every row, the counts, and the checksum"},
    }));
}

inline int run(int argc, char* argv[]) {
    if (argc < 3) {
        throw cli::UsageError("features needs a command: extract, list, or inspect");
    }
    const std::string_view command = argv[2];
    if (command == "extract") {
        return extract(cli::Options(argc, argv, 3, {"--catalog", "--dataset", "--output", "--feature-set"}));
    }
    if (command == "list") {
        return list(cli::Options(argc, argv, 3, {"--output"}));
    }
    if (command == "inspect") {
        return inspect(cli::Options(argc, argv, 3, {"--output", "--artifact", "--catalog"}));
    }
    throw cli::UsageError("unknown features command: " + std::string(command));
}

}  // namespace chronos::feature_cli
