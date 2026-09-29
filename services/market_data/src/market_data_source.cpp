#include "chronos/market_data/market_data_source.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chronos::market_data {

NoMarketDataSource::NoMarketDataSource(std::vector<InstrumentId> instruments)
    : status_{
          .description = MarketDataSourceDescription{
              .mode = MarketDataMode::None,
              .provider = "",
              .feed = "",
              .label = "No market-data source",
              .coverage_notice = std::nullopt,
              .coverage_label = std::nullopt,
              .instruments = std::move(instruments),
          },
          .state = MarketDataSourceState::None,
          .events_applied = 0,
          .replay_speed = std::nullopt,
          .diagnostic = std::nullopt,
          .recording_sha256 = std::nullopt,
      } {}

MarketDataSourceStatus NoMarketDataSource::status() const { return status_; }

ReplayCommandResult NoMarketDataSource::control_replay(ReplayCommand) {
    return ReplayCommandError{
        .code = ReplayCommandErrorCode::NotReplay,
        .state = MarketDataSourceState::None,
        .message = "the server has no market-data source to control",
    };
}

std::string_view state_name(MarketDataSourceState state) noexcept {
    switch (state) {
        case MarketDataSourceState::None:
            return "none";
        case MarketDataSourceState::Ready:
            return "ready";
        case MarketDataSourceState::Running:
            return "running";
        case MarketDataSourceState::Paused:
            return "paused";
        case MarketDataSourceState::Completed:
            return "completed";
        case MarketDataSourceState::Connecting:
            return "connecting";
        case MarketDataSourceState::Authenticating:
            return "authenticating";
        case MarketDataSourceState::Subscribing:
            return "subscribing";
        case MarketDataSourceState::Live:
            return "live";
        case MarketDataSourceState::Reconnecting:
            return "reconnecting";
        case MarketDataSourceState::Failed:
            return "failed";
        case MarketDataSourceState::Stopped:
            return "stopped";
    }
    return "unknown";
}

std::string_view mode_name(MarketDataMode mode) noexcept {
    switch (mode) {
        case MarketDataMode::None:
            return "none";
        case MarketDataMode::HistoricalReplay:
            return "historical_replay";
        case MarketDataMode::Live:
            return "live";
    }
    return "unknown";
}

std::string_view command_name(ReplayCommand command) noexcept {
    switch (command) {
        case ReplayCommand::Start:
            return "start";
        case ReplayCommand::Pause:
            return "pause";
        case ReplayCommand::Resume:
            return "resume";
    }
    return "unknown";
}

std::optional<ReplayCommandError> replay_transition_error(
    ReplayCommand command,
    MarketDataSourceState state
) {
    const MarketDataSourceState required = [command] {
        switch (command) {
            case ReplayCommand::Start:
                return MarketDataSourceState::Ready;
            case ReplayCommand::Pause:
                return MarketDataSourceState::Running;
            case ReplayCommand::Resume:
                return MarketDataSourceState::Paused;
        }
        return MarketDataSourceState::None;
    }();
    if (state == required) {
        return std::nullopt;
    }
    return ReplayCommandError{
        .code = ReplayCommandErrorCode::InvalidTransition,
        .state = state,
        .message = "cannot " + std::string(command_name(command)) +
            " a replay that is " + std::string(state_name(state)),
    };
}

}  // namespace chronos::market_data
