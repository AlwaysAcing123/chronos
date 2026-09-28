#include "chronos/market_state.hpp"
#include "chronos/paper_trading/paper_trading_http_api.hpp"
#include "chronos/paper_trading/paper_trading_http_contract.hpp"
#include "chronos/paper_trading/paper_trading_http_server.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"

#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"
#endif

#include <pthread.h>
#include <signal.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

namespace paper_trading = chronos::paper_trading;

constexpr std::string_view kDatabaseUrlVariable = "CHRONOS_POSTGRES_URL";

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Arguments {
    std::optional<std::string> stream_id;
    std::vector<chronos::InstrumentId> instruments;
    std::optional<std::filesystem::path> replay_path;
    paper_trading::HttpServerOptions http;
    chronos::PreTradeRiskLimits risk_limits{
        .maximum_order_quantity = chronos::Quantity::parse("1000"),
        .maximum_order_notional = chronos::Money::parse("1000000"),
        .maximum_position_quantity = chronos::Quantity::parse("10000"),
        .maximum_quote_age_nanoseconds = 5'000'000'000,
        .fee_per_fill = chronos::Money{},
    };
};

std::string usage() {
    return
        "Usage:\n"
        "  CHRONOS_POSTGRES_URL=<URL> chronos_paper_trading_server \\\n"
        "      --stream-id <ID> --instrument <SYMBOL> [--instrument ...]\n"
        "      [--replay-dbn <FILE.dbn.zst>] [--host 127.0.0.1] "
        "[--port 8080]\n"
        "      [--max-order-quantity 1000] [--max-order-notional 1000000]\n"
        "      [--max-position-quantity 10000] [--max-quote-age-ms 5000]\n"
        "      [--fee-per-fill 0]\n\n"
        "Serves the paper-trading API. Simulated trading only; no order is\n"
        "ever sent to a broker or exchange. The API has no authentication\n"
        "and is not production-secure; keep it on localhost.\n"
        "--replay-dbn is available when built with the Databento adapter.\n";
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
        throw UsageError(
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
        throw UsageError(std::string(option) + ": " + error.what());
    }
}

Arguments parse_arguments(int argc, char* argv[]) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--stream-id") {
            if (arguments.stream_id) {
                throw UsageError("--stream-id may be provided only once");
            }
            arguments.stream_id = require_value(argc, argv, index, option);
        } else if (option == "--instrument") {
            arguments.instruments.emplace_back(
                require_value(argc, argv, index, option)
            );
        } else if (option == "--replay-dbn") {
            if (arguments.replay_path) {
                throw UsageError("--replay-dbn may be provided only once");
            }
            arguments.replay_path = require_value(argc, argv, index, option);
        } else if (option == "--host") {
            arguments.http.host = require_value(argc, argv, index, option);
        } else if (option == "--port") {
            arguments.http.port = parse_integer<int>(
                require_value(argc, argv, index, option),
                option,
                1,
                65'535
            );
        } else if (option == "--max-order-quantity") {
            arguments.risk_limits.maximum_order_quantity =
                parse_decimal<chronos::Quantity>(
                    require_value(argc, argv, index, option),
                    option
                );
        } else if (option == "--max-order-notional") {
            arguments.risk_limits.maximum_order_notional =
                parse_decimal<chronos::Money>(
                    require_value(argc, argv, index, option),
                    option
                );
        } else if (option == "--max-position-quantity") {
            arguments.risk_limits.maximum_position_quantity =
                parse_decimal<chronos::Quantity>(
                    require_value(argc, argv, index, option),
                    option
                );
        } else if (option == "--max-quote-age-ms") {
            constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
            arguments.risk_limits.maximum_quote_age_nanoseconds =
                parse_integer<std::int64_t>(
                    require_value(argc, argv, index, option),
                    option,
                    0,
                    std::numeric_limits<std::int64_t>::max() /
                        kNanosecondsPerMillisecond
                ) *
                kNanosecondsPerMillisecond;
        } else if (option == "--fee-per-fill") {
            arguments.risk_limits.fee_per_fill = parse_decimal<chronos::Money>(
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--help" || option == "-h") {
            throw UsageError("");
        } else {
            throw UsageError("unknown option: " + std::string(option));
        }
    }
    if (!arguments.stream_id || arguments.instruments.empty()) {
        throw UsageError(
            "--stream-id and at least one --instrument are required"
        );
    }
#if !defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
    if (arguments.replay_path) {
        throw UsageError(
            "--replay-dbn requires a build with "
            "CHRONOS_ENABLE_DATABENTO_ADAPTER=ON"
        );
    }
#endif
    return arguments;
}

#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
// Applies a complete local recording before serving, so the server starts
// with the recording's final validated market. The replay is not paced.
std::uint64_t replay_recording(
    const std::filesystem::path& path,
    chronos::MarketState& market_state
) {
    namespace adapter = chronos::databento_adapter;
    auto opened = adapter::EquitiesMiniMbp1FileReader::open(
        path,
        market_state.status().stream
    );
    if (const auto* error = std::get_if<adapter::EquitiesMiniMbp1FileError>(
            &opened
        )) {
        throw std::runtime_error("could not open recording: " + error->message);
    }
    auto& reader = *std::get<
        std::unique_ptr<adapter::EquitiesMiniMbp1FileReader>
    >(opened);

    std::uint64_t applied_events = 0;
    while (true) {
        auto next = reader.read_next();
        if (std::holds_alternative<adapter::EndOfRecording>(next)) {
            if (reader.manifest()) {
                std::cerr << "recording sha256: "
                          << reader.manifest()->source_sha256() << '\n';
            }
            return applied_events;
        }
        if (const auto* error =
                std::get_if<adapter::EquitiesMiniMbp1FileError>(&next)) {
            throw std::runtime_error("recording rejected: " + error->message);
        }
        const auto update = market_state.apply(
            std::get<chronos::ValidatedMarketEvent>(next)
        );
        if (const auto* rejected =
                std::get_if<chronos::MarketStateUpdateError>(&update)) {
            throw std::runtime_error(
                "market state rejected the recording: " + rejected->message
            );
        }
        ++applied_events;
    }
}
#endif

void log_line(std::string_view line) { std::cerr << line << '\n'; }

int run(const Arguments& arguments) {
    const char* database_url = std::getenv(kDatabaseUrlVariable.data());
    if (database_url == nullptr || std::string_view(database_url).empty()) {
        throw UsageError(std::string(kDatabaseUrlVariable) + " must be set");
    }

    // Block termination signals before any thread starts, so every server
    // thread inherits the mask and only the waiter below receives them.
    sigset_t termination_signals;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &termination_signals, nullptr);

    chronos::MarketState market_state{
        chronos::MarketDataStreamId(*arguments.stream_id),
    };
#if defined(CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY)
    if (arguments.replay_path) {
        const std::uint64_t events = replay_recording(
            *arguments.replay_path,
            market_state
        );
        std::cerr << "replayed " << events << " market events\n";
    }
#endif
    const chronos::MarketStateStatus market = market_state.status();
    std::cerr << "market stream " << market.stream.value() << ", market time "
              << (market.market_time
                      ? paper_trading::format_timestamp(*market.market_time)
                      : std::string("none (no events)"))
              << '\n';

    paper_trading::PaperTradingService service(
        std::make_unique<chronos::PostgresPaperOrderRepository>(database_url),
        market_state,
        paper_trading::PaperTradingServiceConfig{
            .tradeable_instruments = arguments.instruments,
            .account_risk_limits = arguments.risk_limits,
        }
    );
    paper_trading::PaperTradingHttpApi api(service, log_line);
    paper_trading::PaperTradingHttpServer server(api, arguments.http, log_line);
    const int port = server.bind();
    std::cerr << "PAPER TRADING ONLY: listening on http://"
              << arguments.http.host << ":" << port
              << " (no authentication; not production-secure)\n";

    std::thread signal_waiter([&] {
        int received = 0;
        sigwait(&termination_signals, &received);
        server.stop();
    });
    std::exception_ptr listen_failure;
    try {
        server.listen();
    } catch (...) {
        listen_failure = std::current_exception();
    }
    // Wakes the waiter when listening ended for a reason other than a signal;
    // the thread must be joined before it is destroyed.
    pthread_kill(signal_waiter.native_handle(), SIGTERM);
    signal_waiter.join();
    if (listen_failure) {
        std::rethrow_exception(listen_failure);
    }
    std::cerr << "stopped\n";
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const UsageError& error) {
        if (error.what()[0] != '\0') {
            std::cerr << "error: " << error.what() << "\n\n";
        }
        std::cerr << usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
