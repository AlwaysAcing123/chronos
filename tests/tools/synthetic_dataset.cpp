#include "chronos/canonical_json.hpp"
#include "chronos/datasets/event_table.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <variant>

// A test-only tool that writes a normalized dataset from a small CSV of
// synthetic quotes and statuses (the event-table format of
// chronos/datasets/event_table.hpp), so tests outside C++ (the Python
// research package) can build the whole pipeline from data that is neither
// licensed nor downloaded. Its provenance is fixed, so every test dataset
// keeps its identity; production synthetic data comes from
// `chronos_dataset synthetic write` instead.
namespace {

namespace json = chronos::canonical_json;
namespace ds = chronos::datasets;

int run(const std::string& input, const std::string& catalog, const std::string& stream) {
    std::ifstream file(input);
    if (!file) {
        std::cerr << "cannot read " << input << '\n';
        return 1;
    }
    auto finished = ds::write_event_table_dataset(file, catalog, chronos::MarketDataStreamId(stream), ds::DatasetSource{
        .provider = "synthetic",
        .provider_dataset = "SYNTHETIC",
        .provider_schema = "top-of-book",
        .description = "synthetic events for tests",
        .recording_sha256 = std::string(64, '0'),
        .coverage = std::nullopt,
        .normalizer = "chronos.test.synthetic_dataset",
        .normalizer_version = "1",
    });
    if (auto* error = std::get_if<ds::DatasetError>(&finished)) {
        if (error->line) {
            std::cerr << "line " << *error->line << ": ";
        }
        std::cerr << error->field << ": " << error->message << '\n';
        return 1;
    }
    const auto& dataset = std::get<ds::PublishedDataset>(finished);
    std::cout << json::serialize(json::Value(json::Object{
        {"dataset_id", dataset.manifest.dataset_id},
        {"directory", dataset.directory.string()},
        {"outcome", dataset.outcome == ds::PublicationOutcome::Published ? "published" : "already_present"},
    })) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: chronos_synthetic_dataset <events.csv> <catalog> <stream-id>\n";
        return 2;
    }
    return run(argv[1], argv[2], argv[3]);
}
