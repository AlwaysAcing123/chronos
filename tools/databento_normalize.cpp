#include "chronos/canonical_json.hpp"
#include "chronos/databento/dataset_conversion.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "dataset_cli_support.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace {

namespace adapter = chronos::databento_adapter;
namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;

std::string usage() {
    return
        "Usage:\n"
        "  chronos_databento_normalize --input <FILE.dbn[.zst]> --stream-id <ID> --catalog <DIR>\n\n"
        "Converts a local EQUS.MINI MBP-1 recording into a normalized Chronos\n"
        "dataset. Reads no network and needs no API key; never overwrites.\n";
}

json::Value recording_error_json(const adapter::EquitiesMiniMbp1FileError& error) {
    json::Object document{
        {"code", "recording_read_failed"},
        {"field", cli::printable(error.field)},
        {"message", cli::printable(error.message)},
        {"source_record", error.source_record_number
            ? cli::decimal(*error.source_record_number)
            : json::Value(nullptr)},
    };
    return json::Value(std::move(document));
}

int run(const cli::Options& options) {
    const std::filesystem::path input = options.required("--input");
    const std::filesystem::path catalog = options.required("--catalog");
    const std::string stream = options.required("--stream-id");
    if (auto error = cli::ensure_directory(catalog, "catalog")) {
        return cli::fail(*error);
    }
    auto converted = adapter::convert_recording_to_dataset(
        input,
        chronos::MarketDataStreamId(stream),
        catalog
    );
    if (auto* failure = std::get_if<adapter::RecordingConversionError>(&converted)) {
        if (failure->recording) {
            return cli::fail(recording_error_json(*failure->recording));
        }
        return cli::fail(*failure->dataset);
    }
    // The dataset is published: from here on the command reports success,
    // with any problem as a warning.
    const auto& dataset = std::get<chronos::datasets::PublishedDataset>(converted);
    std::optional<chronos::datasets::DatasetError> warning = dataset.warning;
    json::Value quality = nullptr;
    auto opened = chronos::datasets::open_dataset(dataset.directory);
    if (auto* error = std::get_if<chronos::datasets::DatasetError>(&opened)) {
        warning = warning.value_or(*error);
    } else {
        auto parsed = json::parse(std::get<chronos::datasets::OpenedDataset>(opened).quality_report);
        if (auto* report = std::get_if<json::Value>(&parsed)) {
            quality = std::move(*report);
        }
    }
    return cli::print(json::Value(json::Object{
        {"dataset", cli::dataset_summary_json(dataset.manifest, dataset.directory)},
        {"outcome", cli::outcome_name(dataset.outcome)},
        {"quality", std::move(quality)},
        {"warning", cli::warning_json(warning)},
    }));
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(cli::Options(argc, argv, 1, {"--input", "--catalog", "--stream-id"}));
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
