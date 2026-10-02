#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_catalog.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/sha256.hpp"
#include "dataset_cli_support.hpp"
#include "feature_cli_commands.hpp"
#include "label_cli_commands.hpp"
#include "split_cli_commands.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace cli = chronos::dataset_cli;
namespace ds = chronos::datasets;
namespace json = chronos::canonical_json;

std::string usage() {
    return
        "Usage:\n"
        "  chronos_dataset list --catalog <DIR> [--verify]\n"
        "  chronos_dataset inspect --catalog <DIR> --dataset <ID>\n"
        "  chronos_dataset features extract --catalog <DIR> --dataset <ID> --output <DIR>\n"
        "      [--feature-set top_of_book_v1]\n"
        "  chronos_dataset features list --output <DIR>\n"
        "  chronos_dataset features inspect --output <DIR> --artifact <ID> [--catalog <DIR>]\n"
        "  chronos_dataset labels extract --features <DIR> --feature-artifact <ID> --output <DIR>\n"
        "      [--horizon-ns 1000000000] [--label-set future_midpoint_return_v1]\n"
        "  chronos_dataset labels list --output <DIR>\n"
        "  chronos_dataset labels inspect --output <DIR> --artifact <ID> [--features <DIR>]\n"
        "  chronos_dataset splits create --labels <DIR> --label-artifact <ID> --output <DIR>\n"
        "      [--train-percent 60] [--validation-percent 20] [--test-percent 20]\n"
        "      [--split-set chronological_purged_v1]\n"
        "  chronos_dataset splits list --output <DIR>\n"
        "  chronos_dataset splits inspect --output <DIR> --artifact <ID> [--labels <DIR>]\n\n"
        "Lists, verifies, and inspects normalized market datasets, extracts\n"
        "point-in-time features from them, labels features with future returns,\n"
        "and splits labels chronologically with purging. Every artifact is\n"
        "immutable; nothing is ever overwritten.\n";
}

// --catalog/--dataset, read in that order.
std::filesystem::path dataset_directory(const cli::Options& options) {
    const std::filesystem::path catalog = options.required("--catalog");
    const std::string id = options.required("--dataset");
    if (!chronos::is_sha256_hex(id)) {
        throw cli::UsageError("--dataset must be a 64-digit lowercase dataset ID");
    }
    return catalog / id;
}

int list(const cli::Options& options) {
    const std::filesystem::path catalog = options.required("--catalog");
    const bool verify = options.flag("--verify");
    auto listed = ds::list_catalog(catalog, verify ? ds::CatalogCheck::Events : ds::CatalogCheck::Manifests);
    if (auto* error = std::get_if<ds::DatasetError>(&listed)) {
        return cli::fail(*error);
    }
    json::Array datasets;
    json::Array invalid;
    for (const auto& entry : std::get<std::vector<ds::CatalogEntry>>(listed)) {
        if (const auto* manifest = std::get_if<ds::DatasetManifest>(&entry.status)) {
            datasets.push_back(cli::dataset_summary_json(*manifest, catalog / entry.name));
        } else {
            invalid.push_back(json::Value(json::Object{
                {"error", cli::error_json(std::get<ds::DatasetError>(entry.status))},
                {"name", cli::printable(entry.name)},
            }));
        }
    }
    return cli::print(json::Value(json::Object{
        {"catalog", cli::printable_path(catalog)},
        {"checked", verify ? "every event" : "structure, manifest, and quality file"},
        {"datasets", std::move(datasets)},
        {"invalid_entries", std::move(invalid)},
    }));
}

int inspect(const cli::Options& options) {
    const std::filesystem::path directory = dataset_directory(options);
    auto verified = ds::verify_dataset(directory);
    if (auto* error = std::get_if<ds::DatasetError>(&verified)) {
        return cli::fail(*error);
    }
    auto opened = ds::open_dataset(directory);
    if (auto* error = std::get_if<ds::DatasetError>(&opened)) {
        return cli::fail(*error);
    }
    const auto& dataset = std::get<ds::OpenedDataset>(opened);
    const std::string& quality = dataset.quality_report;
    return cli::print(json::Value(json::Object{
        {"dataset", cli::dataset_summary_json(dataset.manifest, directory)},
        {"manifest", ds::manifest_json(dataset.manifest)},
        {"quality", std::get<json::Value>(json::parse(std::string_view(quality).substr(0, quality.size() - 1)))},
        {"verified", "every event, the checksums, the summary, and the quality report"},
    }));
}

int run(int argc, char* argv[]) {
    if (argc < 2) {
        throw cli::UsageError("a command is required");
    }
    const std::string_view command = argv[1];
    if (command == "list") {
        return list(cli::Options(argc, argv, 2, {"--catalog"}, {"--verify"}));
    }
    if (command == "inspect") {
        return inspect(cli::Options(argc, argv, 2, {"--catalog", "--dataset"}));
    }
    if (command == "features") {
        return chronos::feature_cli::run(argc, argv);
    }
    if (command == "labels") {
        return chronos::label_cli::run(argc, argv);
    }
    if (command == "splits") {
        return chronos::split_cli::run(argc, argv);
    }
    if (command == "--help" || command == "-h") {
        throw cli::UsageError("");
    }
    throw cli::UsageError("unknown command: " + std::string(command));
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(argc, argv);
    } catch (const cli::UsageError& error) {
        if (*error.what() != '\0') {
            std::cerr << "Error: " << error.what() << "\n\n";
        }
        std::cerr << usage();
        return 2;
    } catch (const std::exception& exception) {
        return cli::fail_unexpectedly(exception.what());
    }
}
