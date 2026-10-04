#include "chronos/canonical_json.hpp"
#include "chronos/strategy/strategy_run_artifact.hpp"
#include "chronos/strategy/strategy_simulation.hpp"
#include "dataset_cli_support.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// `chronos_strategy run|list|inspect`: deterministic historical evaluation
// of an inference schedule. See docs/strategy-evaluation.md.
namespace {

namespace cli = chronos::dataset_cli;
namespace json = chronos::canonical_json;
namespace st = chronos::strategy;

std::string usage() {
    return
        "Usage:\n"
        "  chronos_strategy run --catalog <DIR> --schedules <DIR> --schedule <ID> --output <DIR>\n"
        "      [--threshold-ppb 500000000] [--target-quantity 1] [--latency-ns 1000000]\n"
        "      [--decision-interval-ns 1000000000] [--starting-cash 10000] [--fee-per-fill 0.01]\n"
        "      [--maximum-quote-age-ns 5000000000] [--maximum-order-notional 1000000]\n"
        "  chronos_strategy list --output <DIR>\n"
        "  chronos_strategy inspect --output <DIR> --artifact <ID> [--catalog <DIR> --schedules <DIR>]\n"
        "\n"
        "Success prints JSON on stdout and exits 0; a refused input prints {\"error\": ...} on\n"
        "stderr and exits 1; malformed usage exits 2. Every option is given at most once.\n";
}

template <typename Decimal>
Decimal decimal_option(const cli::Options& options, const std::string& option, Decimal fallback, bool allow_zero) {
    const auto text = options.optional(option);
    if (!text) {
        return fallback;
    }
    try {
        const Decimal value = Decimal::parse(*text);
        if (value.to_string() == *text && (value.raw() > 0 || (allow_zero && value.raw() == 0))) {
            return value;
        }
    } catch (const std::exception&) {
    }
    throw cli::UsageError(
        option + " must be a " + (allow_zero ? "nonnegative" : "positive") +
        " decimal written canonically, without a sign, exponent, trailing zeros, or unit"
    );
}

std::int64_t nanoseconds_option(const cli::Options& options, const std::string& option, std::int64_t fallback) {
    const auto text = options.optional(option);
    if (!text) {
        return fallback;
    }
    return static_cast<std::int64_t>(cli::positive_integer(
        *text, option, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
    ));
}

st::StrategyConfiguration configuration(const cli::Options& options) {
    const st::StrategyConfiguration defaults;
    st::StrategyConfiguration result = defaults;
    // Read one option per statement, so a missing or invalid one is always
    // reported the same way on every compiler.
    if (const auto threshold = options.optional("--threshold-ppb")) {
        const auto value = json::parse_canonical_unsigned(*threshold);
        if (!value || *value > st::kConfidenceScale) {
            throw cli::UsageError("--threshold-ppb must be a whole number from 0 to 1000000000");
        }
        result.threshold_ppb = *value;
    }
    result.target_quantity = decimal_option(options, "--target-quantity", defaults.target_quantity, false);
    result.latency_ns = nanoseconds_option(options, "--latency-ns", defaults.latency_ns);
    result.decision_interval_ns = nanoseconds_option(options, "--decision-interval-ns", defaults.decision_interval_ns);
    result.starting_cash = decimal_option(options, "--starting-cash", defaults.starting_cash, false);
    result.fee_per_fill = decimal_option(options, "--fee-per-fill", defaults.fee_per_fill, true);
    result.maximum_quote_age_ns = nanoseconds_option(options, "--maximum-quote-age-ns", defaults.maximum_quote_age_ns);
    result.maximum_order_notional =
        decimal_option(options, "--maximum-order-notional", defaults.maximum_order_notional, false);
    return result;
}

json::Value summary_json(const st::StrategyRunManifest& manifest, const std::filesystem::path& directory) {
    json::Object summary = *manifest.document.if_object();
    summary.emplace_back("directory", cli::printable_path(directory));
    return json::Value(std::move(summary));
}

int run_command(const cli::Options& options) {
    const std::filesystem::path catalog = options.required("--catalog");
    const std::filesystem::path schedule = cli::id_path(options, "--schedules", "--schedule");
    const std::filesystem::path output = options.required("--output");
    const st::StrategyConfiguration chosen = configuration(options);
    if (auto error = cli::ensure_directory(output, "output")) {
        return cli::fail(*error);
    }
    auto published = st::run_strategy(catalog, schedule, output, chosen);
    if (auto* error = std::get_if<chronos::datasets::DatasetError>(&published)) {
        return cli::fail(*error);
    }
    const auto& run = std::get<st::PublishedStrategyRun>(published);
    return cli::print(json::Value(json::Object{
        {"artifact", summary_json(run.manifest, run.directory)},
        {"outcome", cli::outcome_name(run.outcome)},
        {"warning", cli::warning_json(run.warning)},
    }));
}

int list_command(const cli::Options& options) {
    const std::filesystem::path output = options.required("--output");
    auto listed = st::list_strategy_runs(output);
    if (auto* error = std::get_if<chronos::datasets::DatasetError>(&listed)) {
        return cli::fail(*error);
    }
    return cli::print(cli::artifact_list_json(output, std::get<std::vector<st::StrategyRunEntry>>(listed), summary_json));
}

int inspect_command(const cli::Options& options) {
    const std::filesystem::path directory = cli::id_path(options, "--output", "--artifact");
    const auto catalog = options.optional("--catalog");
    const auto schedules = options.optional("--schedules");
    if (catalog.has_value() != schedules.has_value()) {
        throw cli::UsageError("--catalog and --schedules are given together, to rerun the schedule");
    }
    auto verified = st::verify_strategy_run(directory);
    if (auto* error = std::get_if<chronos::datasets::DatasetError>(&verified)) {
        return cli::fail(*error);
    }
    const auto& manifest = std::get<st::StrategyRunManifest>(verified);
    std::string reproduced = "not checked: pass --catalog and --schedules to run the schedule again";
    if (catalog) {
        if (auto error = st::reproduce_strategy_run(manifest, *catalog, *schedules)) {
            return cli::fail(*error);
        }
        reproduced = "ran the schedule again over its verified dataset: identical journal and results";
    }
    return cli::print(json::Value(json::Object{
        {"artifact", summary_json(manifest, directory)},
        {"reproduced", reproduced},
        {"verified", "the file set, the manifest's form and ID, and every journal entry's form, count, and checksum"},
    }));
}

int run(int argc, char* argv[]) {
    if (argc < 2) {
        throw cli::UsageError("a command is required");
    }
    const std::string_view command = argv[1];
    if (command == "run") {
        return run_command(cli::Options(argc, argv, 2, {
            "--catalog", "--schedules", "--schedule", "--output", "--threshold-ppb", "--target-quantity",
            "--latency-ns", "--decision-interval-ns", "--starting-cash", "--fee-per-fill", "--maximum-quote-age-ns",
            "--maximum-order-notional",
        }));
    }
    if (command == "list") {
        return list_command(cli::Options(argc, argv, 2, {"--output"}));
    }
    if (command == "inspect") {
        return inspect_command(cli::Options(argc, argv, 2, {"--output", "--artifact", "--catalog", "--schedules"}));
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
