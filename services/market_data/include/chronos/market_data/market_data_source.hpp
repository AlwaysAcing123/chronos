#pragma once

#include "chronos/market_data_types.hpp"
#include "chronos/replay_schedule.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::market_data {

enum class MarketDataMode {
    None,
    HistoricalReplay,
    Live,
};

// One lifecycle vocabulary for every source. A replay moves through
// Ready, Running, Paused, and Completed; a live feed through Connecting,
// Authenticating, Subscribing, Live, and Reconnecting. Failed is permanent
// until the server restarts; Stopped means the server is shutting down.
enum class MarketDataSourceState {
    None,
    Ready,
    Running,
    Paused,
    Completed,
    Connecting,
    Authenticating,
    Subscribing,
    Live,
    Reconnecting,
    Failed,
    Stopped,
};

// What a source is, fixed when it is configured. Provider names are
// descriptive data for people; no component branches on them.
struct MarketDataSourceDescription {
    MarketDataMode mode{MarketDataMode::None};
    // The events were invented by a generator, not observed on a market:
    // a replay of a dataset whose provenance says it is synthetic.
    bool synthetic{false};
    // "Databento" or "Alpaca"; empty without a source.
    std::string provider;
    // The dataset or feed, such as "EQUS.MINI MBP-1" or "IEX".
    std::string feed;
    // How the interface names the source.
    std::string label;
    // A coverage limitation every view must show, such as a single-venue
    // live feed that is not consolidated data.
    std::optional<std::string> coverage_notice;
    // The same limitation in a few words, for compact displays.
    std::optional<std::string> coverage_label;
    std::vector<InstrumentId> instruments;
};

// A consistent snapshot of a source, safe to take from any thread.
struct MarketDataSourceStatus {
    MarketDataSourceDescription description;
    MarketDataSourceState state{MarketDataSourceState::None};
    // Events the source applied to the market state, exactly.
    std::uint64_t events_applied{0};
    std::optional<ReplaySpeed> replay_speed;
    // The most recent safe, non-secret explanation, such as why a source
    // failed or is reconnecting.
    std::optional<std::string> diagnostic;
    // The verified SHA-256 of a completed recording.
    std::optional<std::string> recording_sha256;
};

enum class ReplayCommand {
    Start,
    Pause,
    Resume,
};

enum class ReplayCommandErrorCode {
    // The configured source is not a replay.
    NotReplay,
    // The command does not apply in the source's current state.
    InvalidTransition,
};

struct ReplayCommandError {
    ReplayCommandErrorCode code;
    MarketDataSourceState state;
    std::string message;
};

using ReplayCommandResult = std::variant<
    MarketDataSourceStatus,
    ReplayCommandError
>;

// The server's single market-data source. It feeds the server-owned
// MarketState on its own thread; HTTP threads read its status and send
// replay commands. All member functions are safe to call concurrently.
class MarketDataSource {
public:
    virtual ~MarketDataSource() = default;

    [[nodiscard]] virtual MarketDataSourceStatus status() const = 0;

    [[nodiscard]] virtual ReplayCommandResult control_replay(
        ReplayCommand command
    ) = 0;

    // Stops delivery and joins the source's thread. Idempotent.
    virtual void stop() noexcept = 0;
};

// The source of a server started without market data.
class NoMarketDataSource final : public MarketDataSource {
public:
    explicit NoMarketDataSource(std::vector<InstrumentId> instruments);

    [[nodiscard]] MarketDataSourceStatus status() const override;
    [[nodiscard]] ReplayCommandResult control_replay(
        ReplayCommand command
    ) override;
    void stop() noexcept override {}

private:
    MarketDataSourceStatus status_;
};

[[nodiscard]] std::string_view state_name(MarketDataSourceState state) noexcept;
[[nodiscard]] std::string_view mode_name(MarketDataMode mode) noexcept;
[[nodiscard]] std::string_view command_name(ReplayCommand command) noexcept;

// Why `command` cannot run in `state`, or nullopt when it can.
[[nodiscard]] std::optional<ReplayCommandError> replay_transition_error(
    ReplayCommand command,
    MarketDataSourceState state
);

}  // namespace chronos::market_data
