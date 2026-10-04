#include "chronos/paper_trading/server_arguments.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace chronos::paper_trading {
namespace {

// Walks the command line, handing out option values and refusing repeats.
class OptionCursor final {
public:
    explicit OptionCursor(std::span<const std::string_view> arguments)
        : arguments_(arguments) {}

    [[nodiscard]] bool done() const { return index_ >= arguments_.size(); }

    [[nodiscard]] std::string_view next_option() {
        return arguments_[index_++];
    }

    [[nodiscard]] std::string value(std::string_view option) {
        if (done()) {
            throw ServerUsageError(std::string(option) + " requires a value");
        }
        return std::string(arguments_[index_++]);
    }

    void claim_once(std::string_view option) {
        if (!seen_.insert(std::string(option)).second) {
            throw ServerUsageError(
                std::string(option) + " may be provided only once"
            );
        }
    }

private:
    std::span<const std::string_view> arguments_;
    std::size_t index_{0};
    std::set<std::string> seen_;
};

template <typename Integer>
Integer parse_integer(
    std::string_view text,
    std::string_view option,
    Integer minimum,
    Integer maximum
) {
    Integer value{};
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value < minimum || value > maximum) {
        throw ServerUsageError(
            std::string(option) + " must be an integer from " +
            std::to_string(minimum) + " to " + std::to_string(maximum)
        );
    }
    return value;
}

template <typename Decimal>
Decimal parse_decimal(std::string_view text, std::string_view option) {
    try {
        return Decimal::parse(text);
    } catch (const std::exception& error) {
        throw ServerUsageError(std::string(option) + ": " + error.what());
    }
}

PreTradeRiskLimits default_risk_limits() {
    return PreTradeRiskLimits{
        .maximum_order_quantity = Quantity::parse("1000"),
        .maximum_order_notional = Money::parse("1000000"),
        .maximum_position_quantity = Quantity::parse("10000"),
        .maximum_quote_age_nanoseconds = 5'000'000'000,
        .fee_per_fill = Money{},
    };
}

}  // namespace

ServerArguments parse_server_arguments(
    std::span<const std::string_view> arguments,
    ServerCapabilities capabilities
) {
    ServerArguments parsed{
        .stream_id = {},
        .instruments = {},
        .replay = std::nullopt,
        .alpaca_iex = false,
        .http = HttpServerOptions{},
        .risk_limits = default_risk_limits(),
        .deployments = std::nullopt,
    };
    bool model_deployments = false;
    std::optional<std::string> research_python;
    std::optional<std::string> research_python_root;
    std::optional<std::string> research_artifacts;
    std::optional<std::string> stream_id;
    std::optional<std::string> recording;
    std::optional<ReplaySpeed> replay_speed;
    bool replay_autostart = false;

    OptionCursor cursor(arguments);
    while (!cursor.done()) {
        const std::string_view option = cursor.next_option();
        if (option == "--help" || option == "-h") {
            throw ServerUsageError("");
        }
        if (option != "--instrument") {
            cursor.claim_once(option);
        }
        if (option == "--stream-id") {
            stream_id = cursor.value(option);
        } else if (option == "--instrument") {
            const std::string symbol = cursor.value(option);
            if (symbol.empty()) {
                throw ServerUsageError("--instrument requires a symbol");
            }
            InstrumentId instrument(symbol);
            if (std::ranges::find(parsed.instruments, instrument) !=
                parsed.instruments.end()) {
                throw ServerUsageError(
                    "--instrument " + instrument.value() +
                    " is listed more than once"
                );
            }
            parsed.instruments.push_back(std::move(instrument));
        } else if (option == "--replay-dbn") {
            recording = cursor.value(option);
        } else if (option == "--replay-speed") {
            const std::string name = cursor.value(option);
            replay_speed = parse_replay_speed(name);
            if (!replay_speed) {
                throw ServerUsageError(
                    "--replay-speed must be one of 1x, 10x, 100x, max"
                );
            }
        } else if (option == "--replay-autostart") {
            replay_autostart = true;
        } else if (option == "--alpaca-iex") {
            parsed.alpaca_iex = true;
        } else if (option == "--host") {
            parsed.http.host = cursor.value(option);
        } else if (option == "--port") {
            parsed.http.port = parse_integer<int>(
                cursor.value(option),
                option,
                1,
                65'535
            );
        } else if (option == "--max-order-quantity") {
            parsed.risk_limits.maximum_order_quantity =
                parse_decimal<Quantity>(cursor.value(option), option);
        } else if (option == "--max-order-notional") {
            parsed.risk_limits.maximum_order_notional =
                parse_decimal<Money>(cursor.value(option), option);
        } else if (option == "--max-position-quantity") {
            parsed.risk_limits.maximum_position_quantity =
                parse_decimal<Quantity>(cursor.value(option), option);
        } else if (option == "--max-quote-age-ms") {
            constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
            parsed.risk_limits.maximum_quote_age_nanoseconds =
                parse_integer<std::int64_t>(
                    cursor.value(option),
                    option,
                    0,
                    std::numeric_limits<std::int64_t>::max() /
                        kNanosecondsPerMillisecond
                ) *
                kNanosecondsPerMillisecond;
        } else if (option == "--fee-per-fill") {
            parsed.risk_limits.fee_per_fill =
                parse_decimal<Money>(cursor.value(option), option);
        } else if (option == "--model-deployments") {
            model_deployments = true;
        } else if (option == "--research-python") {
            research_python = cursor.value(option);
        } else if (option == "--research-python-root") {
            research_python_root = cursor.value(option);
        } else if (option == "--research-artifacts") {
            research_artifacts = cursor.value(option);
        } else {
            throw ServerUsageError("unknown option: " + std::string(option));
        }
    }

    if (!stream_id || parsed.instruments.empty()) {
        throw ServerUsageError(
            "--stream-id and at least one --instrument are required"
        );
    }
    parsed.stream_id = std::move(*stream_id);
    const bool any_research = research_python || research_python_root || research_artifacts;
    if (model_deployments) {
        if (!research_python || !research_python_root || !research_artifacts ||
            research_python->empty() || research_python_root->empty() || research_artifacts->empty()) {
            throw ServerUsageError(
                "--model-deployments requires --research-python, "
                "--research-python-root, and --research-artifacts"
            );
        }
        parsed.deployments = DeploymentArguments{
            .python = std::move(*research_python),
            .python_root = std::move(*research_python_root),
            .artifacts = std::move(*research_artifacts),
        };
    } else if (any_research) {
        throw ServerUsageError(
            "--research-python, --research-python-root, and "
            "--research-artifacts require --model-deployments"
        );
    }
    if (parsed.alpaca_iex) {
        if (recording) {
            throw ServerUsageError(
                "choose one market-data source: --replay-dbn or --alpaca-iex"
            );
        }
        if (!capabilities.alpaca_live) {
            throw ServerUsageError(
                "--alpaca-iex requires a build with "
                "CHRONOS_ENABLE_ALPACA_ADAPTER=ON"
            );
        }
        if (parsed.instruments.size() > kAlpacaIexInstrumentLimit) {
            throw ServerUsageError(
                "--alpaca-iex supports at most " +
                std::to_string(kAlpacaIexInstrumentLimit) +
                " instruments, the limit of Alpaca's Basic plan"
            );
        }
    }
    if (!recording) {
        if (replay_speed) {
            throw ServerUsageError("--replay-speed requires --replay-dbn");
        }
        if (replay_autostart) {
            throw ServerUsageError("--replay-autostart requires --replay-dbn");
        }
        return parsed;
    }
    if (!capabilities.databento_replay) {
        throw ServerUsageError(
            "--replay-dbn requires a build with "
            "CHRONOS_ENABLE_DATABENTO_ADAPTER=ON"
        );
    }
    parsed.replay = ReplayArguments{
        .recording = std::move(*recording),
        .speed = replay_speed.value_or(ReplaySpeed::RealTime),
        .start_immediately = replay_autostart,
    };
    return parsed;
}

std::string server_usage() {
    return
        "Usage:\n"
        "  CHRONOS_POSTGRES_URL=<URL> chronos_paper_trading_server \\\n"
        "      --stream-id <ID> --instrument <SYMBOL> [--instrument ...]\n"
        "      [--host 127.0.0.1] [--port 8080]\n"
        "      [--max-order-quantity 1000] [--max-order-notional 1000000]\n"
        "      [--max-position-quantity 10000] [--max-quote-age-ms 5000]\n"
        "      [--fee-per-fill 0]\n"
        "      [market-data source] [model deployments]\n\n"
        "Market-data source (at most one; without one the market is empty):\n"
        "  --replay-dbn <FILE.dbn.zst>  Replay a local Databento EQUS.MINI\n"
        "                               MBP-1 recording (Databento builds).\n"
        "    --replay-speed <SPEED>     1x (default), 10x, 100x, or max.\n"
        "    --replay-autostart         Start playing at once instead of\n"
        "                               waiting for a Start command.\n"
        "  --alpaca-iex                 Stream live Alpaca IEX trades and\n"
        "                               quotes (Alpaca builds; at most 30\n"
        "                               instruments). IEX is one exchange,\n"
        "                               not SIP or NBBO data. Credentials are\n"
        "                               read only from APCA_API_KEY_ID and\n"
        "                               APCA_API_SECRET_KEY.\n\n"
        "Model deployments (off unless asked for; see docs/model-deployments.md):\n"
        "  --model-deployments          Allow verified research models to\n"
        "                               trade dedicated paper accounts, each\n"
        "                               only after an explicit Start.\n"
        "    --research-python <PATH>   The Python with chronos_research's\n"
        "                               dependencies, such as python/.venv/bin/python.\n"
        "    --research-python-root <DIR>  The repository's python directory.\n"
        "    --research-artifacts <DIR> The research artifacts directory,\n"
        "                               holding experiments/.\n\n"
        "Serves the paper-trading API. Simulated trading only; no order is\n"
        "ever sent to a broker or exchange. The API has no authentication\n"
        "and is not production-secure; keep it on localhost.\n";
}

}  // namespace chronos::paper_trading
