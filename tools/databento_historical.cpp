#include "chronos/databento/historical_recording_acquisition.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace adapter = chronos::databento_adapter;

enum class Command {
    Quote,
    Download,
};

struct Arguments {
    Command command;
    adapter::HistoricalRecordingQuery query;
    std::optional<std::filesystem::path> output;
    std::optional<std::string> stream_id;
    std::optional<double> maximum_cost_usd;
};

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string usage() {
    return
        "Usage:\n"
        "  chronos_databento_historical quote --start <ISO-8601> "
        "--end <ISO-8601> --symbol <SYMBOL> [--symbol <SYMBOL> ...]\n"
        "  chronos_databento_historical download --start <ISO-8601> "
        "--end <ISO-8601> --symbol <SYMBOL> [--symbol <SYMBOL> ...] "
        "--output <FILE.dbn.zst> --stream-id <ID> "
        "--max-cost-usd <USD>\n\n"
        "The API key is read only from DATABENTO_API_KEY. Quote is metadata-only. "
        "Download always quotes first and refuses costs above --max-cost-usd.\n";
}

double parse_nonnegative_number(std::string_view text) {
    if (text.empty()) {
        throw UsageError("--max-cost-usd requires a number");
    }
    errno = 0;
    char* end = nullptr;
    const std::string owned(text);
    const double value = std::strtod(owned.c_str(), &end);
    if (errno == ERANGE || end == owned.c_str() || *end != '\0' ||
        !std::isfinite(value) || value < 0.0) {
        throw UsageError(
            "--max-cost-usd must be a finite nonnegative number"
        );
    }
    return value;
}

std::string require_value(
    int argc,
    char* argv[],
    int& index,
    std::string_view option
) {
    if (index + 1 >= argc) {
        throw UsageError(std::string(option) + " requires a value");
    }
    ++index;
    return argv[index];
}

void assign_once(
    std::optional<std::string>& destination,
    std::string value,
    std::string_view option
) {
    if (destination) {
        throw UsageError(std::string(option) + " may be provided only once");
    }
    destination = std::move(value);
}

Arguments parse_arguments(int argc, char* argv[]) {
    if (argc < 2) {
        throw UsageError("a command is required");
    }

    Command command;
    const std::string_view command_text = argv[1];
    if (command_text == "quote") {
        command = Command::Quote;
    } else if (command_text == "download") {
        command = Command::Download;
    } else {
        throw UsageError("command must be quote or download");
    }

    std::optional<std::string> start;
    std::optional<std::string> end;
    std::optional<std::string> output;
    std::optional<std::string> stream_id;
    std::optional<double> maximum_cost_usd;
    std::vector<std::string> symbols;

    for (int index = 2; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--start") {
            assign_once(
                start,
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--end") {
            assign_once(
                end,
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--symbol") {
            symbols.push_back(require_value(argc, argv, index, option));
        } else if (option == "--output") {
            assign_once(
                output,
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--stream-id") {
            assign_once(
                stream_id,
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--max-cost-usd") {
            if (maximum_cost_usd) {
                throw UsageError(
                    "--max-cost-usd may be provided only once"
                );
            }
            maximum_cost_usd = parse_nonnegative_number(
                require_value(argc, argv, index, option)
            );
        } else if (option == "--help" || option == "-h") {
            throw UsageError("");
        } else {
            throw UsageError("unknown option: " + std::string(option));
        }
    }

    if (!start || !end || symbols.empty()) {
        throw UsageError(
            "--start, --end, and at least one --symbol are required"
        );
    }
    if (command == Command::Quote &&
        (output || stream_id || maximum_cost_usd)) {
        throw UsageError(
            "quote does not accept --output, --stream-id, or --max-cost-usd"
        );
    }
    if (command == Command::Download &&
        (!output || !stream_id || !maximum_cost_usd)) {
        throw UsageError(
            "download requires --output, --stream-id, and --max-cost-usd"
        );
    }

    return Arguments{
        .command = command,
        .query = adapter::HistoricalRecordingQuery{
            .start = std::move(*start),
            .end = std::move(*end),
            .symbols = std::move(symbols),
        },
        .output = output
            ? std::optional<std::filesystem::path>(*output)
            : std::nullopt,
        .stream_id = std::move(stream_id),
        .maximum_cost_usd = maximum_cost_usd,
    };
}

nlohmann::json query_json(
    const adapter::HistoricalRecordingQuery& query
) {
    return {
        {"dataset", "EQUS.MINI"},
        {"end", query.end},
        {"input_symbology", "raw_symbol"},
        {"output_symbology", "instrument_id"},
        {"schema", "mbp-1"},
        {"start", query.start},
        {"symbols", query.symbols},
    };
}

nlohmann::json quote_json(
    const adapter::HistoricalRecordingQuote& quote
) {
    return {
        {"estimated_billable_size_bytes",
         quote.estimated_billable_size_bytes},
        {"estimated_cost_usd", quote.estimated_cost_usd},
        {"estimated_record_count", quote.estimated_record_count},
    };
}

int report_error(const adapter::HistoricalRecordingError& error) {
    const nlohmann::json document = {
        {"error",
         {
             {"code", adapter::to_string(error.code)},
             {"field", error.field},
             {"message", error.message},
         }},
    };
    std::cerr << document.dump(2) << '\n';
    return 1;
}

int run(const Arguments& arguments) {
    auto connection =
        adapter::DatabentoHistoricalRecordingProvider::
            connect_from_environment();
    if (std::holds_alternative<adapter::HistoricalRecordingError>(
            connection
        )) {
        return report_error(
            std::get<adapter::HistoricalRecordingError>(connection)
        );
    }
    auto provider = std::get<std::unique_ptr<
        adapter::DatabentoHistoricalRecordingProvider
    >>(std::move(connection));

    if (arguments.command == Command::Quote) {
        auto result = adapter::quote_historical_recording(
            *provider,
            arguments.query
        );
        if (std::holds_alternative<adapter::HistoricalRecordingError>(
                result
            )) {
            return report_error(
                std::get<adapter::HistoricalRecordingError>(result)
            );
        }
        const auto& quote = std::get<adapter::HistoricalRecordingQuote>(
            result
        );
        std::cout << nlohmann::json{
            {"query", query_json(arguments.query)},
            {"quote", quote_json(quote)},
        }.dump(2) << '\n';
        return 0;
    }

    auto result = adapter::acquire_historical_recording(
        *provider,
        adapter::HistoricalRecordingAcquisitionRequest{
            .query = arguments.query,
            .destination = *arguments.output,
            .stream = chronos::MarketDataStreamId(*arguments.stream_id),
            .maximum_cost_usd = *arguments.maximum_cost_usd,
        }
    );
    if (std::holds_alternative<adapter::HistoricalRecordingError>(result)) {
        return report_error(
            std::get<adapter::HistoricalRecordingError>(result)
        );
    }

    const auto& success =
        std::get<adapter::HistoricalRecordingAcquisitionSuccess>(result);
    std::cout << nlohmann::json{
        {"manifest", nlohmann::json::parse(success.manifest.to_json())},
        {"manifest_path", success.manifest_path.string()},
        {"query", query_json(arguments.query)},
        {"quote", quote_json(success.quote)},
        {"recording_path", success.recording_path.string()},
    }.dump(2) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const UsageError& error) {
        if (*error.what() != '\0') {
            std::cerr << "Error: " << error.what() << "\n\n";
        }
        std::cerr << usage();
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected error: " << exception.what() << '\n';
        return 1;
    }
}
