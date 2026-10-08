#pragma once

#include "chronos/paper_trading/paper_trading_http_server.hpp"
#include "chronos/pre_trade_risk.hpp"
#include "chronos/replay_schedule.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chronos::paper_trading {

// Which optional market-data sources this server binary was built with.
struct ServerCapabilities {
    bool databento_replay{false};
    bool alpaca_live{false};
};

struct ReplayArguments {
    std::filesystem::path recording;
    ReplaySpeed speed{ReplaySpeed::RealTime};
    bool start_immediately{false};
};

// Model deployments (ADR 0018), enabled only by --model-deployments: the
// Python interpreter that runs the inference worker, the repository's
// python directory, and the research artifacts holding the experiments and
// registered models. --onnx-docker, the absolute path of a Docker CLI,
// lets registered imported models run in the ONNX sandbox (ADR 0021);
// without it they cannot be deployed.
struct DeploymentArguments {
    std::filesystem::path python;
    std::filesystem::path python_root;
    std::filesystem::path artifacts;
    std::optional<std::filesystem::path> onnx_docker{};
};

// A validated server command line. At most one market-data source is
// selected, a replay or Alpaca IEX; without one the market stays empty.
struct ServerArguments {
    std::string stream_id;
    std::vector<InstrumentId> instruments;
    std::optional<ReplayArguments> replay;
    // Live data from Alpaca's IEX stream. Its credentials come only from
    // the environment.
    bool alpaca_iex{false};
    // Continue a live stream whose durable history predates durable live
    // sequences (migration 006), after the largest sequence that history
    // references (ADR 0020). Only with --alpaca-iex.
    bool adopt_live_stream_history{false};
    HttpServerOptions http;
    PreTradeRiskLimits risk_limits;
    // Absent unless model deployments were explicitly enabled.
    std::optional<DeploymentArguments> deployments{};
};

// Alpaca's free Basic plan allows 30 streamed symbols.
inline constexpr std::size_t kAlpacaIexInstrumentLimit = 30;

// A command line that cannot start the server. An empty message asks for
// the help text only.
class ServerUsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Strict: every option except --instrument may appear once, an instrument
// may be listed once, and options for a source that is not selected or not
// built are rejected rather than ignored. Credentials are never accepted
// on the command line.
[[nodiscard]] ServerArguments parse_server_arguments(
    std::span<const std::string_view> arguments,
    ServerCapabilities capabilities
);

[[nodiscard]] std::string server_usage();

}  // namespace chronos::paper_trading
