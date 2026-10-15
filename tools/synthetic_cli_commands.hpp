#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/event_table.hpp"
#include "chronos/sha256.hpp"
#include "dataset_cli_support.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

// `chronos_dataset synthetic write`: the production producer of synthetic
// datasets (ADR 0025). It writes a named, versioned scenario's event table
// with explicit synthetic provenance, so a replay, a collection, and every
// view can say the prices are invented. It reads no market data.
namespace chronos::synthetic_cli {

namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;

// A scenario's event table is small; anything larger is not one.
inline constexpr std::size_t kMaximumTableBytes = 256U << 20U;

// The table is read once, so the bytes hashed are the bytes converted.
inline std::string read_table(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw cli::UsageError("--events cannot be read");
    }
    std::string bytes;
    char buffer[1 << 16];
    while (file.read(buffer, sizeof buffer) || file.gcount() > 0) {
        bytes.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (bytes.size() > kMaximumTableBytes) {
            throw cli::UsageError("--events is larger than an event table may be (256 MiB)");
        }
    }
    if (file.bad()) {
        throw cli::UsageError("--events cannot be read");
    }
    return bytes;
}

inline int write(const cli::Options& options) {
    const std::filesystem::path events = options.required("--events");
    const std::filesystem::path catalog = options.required("--catalog");
    const std::string stream = options.required("--stream-id");
    const std::string scenario = options.required("--scenario");
    const std::string version = options.required("--scenario-version");
    if (!std::regex_match(scenario, std::regex("[a-z0-9][a-z0-9._-]{0,63}"))) {
        throw cli::UsageError("--scenario must be 1 to 64 lowercase letters, digits, '.', '_', or '-'");
    }
    if (!std::regex_match(version, std::regex("(0|[1-9][0-9]{0,5})(\\.(0|[1-9][0-9]{0,5})){0,2}"))) {
        throw cli::UsageError("--scenario-version must be a version such as 1 or 1.2");
    }
    const std::string table = read_table(events);
    if (auto error = cli::ensure_directory(catalog, "catalog")) {
        return cli::fail(*error);
    }
    std::istringstream rows(table);
    auto written = datasets::write_event_table_dataset(
        rows,
        catalog,
        MarketDataStreamId(stream),
        datasets::synthetic_scenario_source(scenario, version, Sha256::hex(table))
    );
    if (auto* error = std::get_if<datasets::DatasetError>(&written)) {
        return cli::fail(*error);
    }
    const auto& dataset = std::get<datasets::PublishedDataset>(written);
    return cli::print(json::Value(json::Object{
        {"dataset", cli::dataset_summary_json(dataset.manifest, dataset.directory)},
        {"outcome", cli::outcome_name(dataset.outcome)},
        {"provenance", json::Object{
            {"description", dataset.manifest.source.description},
            {"normalizer", dataset.manifest.source.normalizer + "/" + dataset.manifest.source.normalizer_version},
            {"provider", dataset.manifest.source.provider},
            {"table_sha256", dataset.manifest.source.recording_sha256},
        }},
        {"warning", cli::warning_json(dataset.warning)},
    }));
}

inline int run(int argc, char* argv[]) {
    if (argc < 3 || std::string_view(argv[2]) != "write") {
        throw cli::UsageError("synthetic needs a subcommand: write");
    }
    return write(cli::Options(
        argc, argv, 3, {"--events", "--catalog", "--stream-id", "--scenario", "--scenario-version"}
    ));
}

}  // namespace chronos::synthetic_cli
