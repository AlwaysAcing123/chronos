#pragma once

#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_event_reader.hpp"
#include "chronos/replay_schedule.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace chronos::market_data {

struct PacedReplayOptions {
    ReplaySpeed speed{ReplaySpeed::RealTime};
    // Begin playing without waiting for a Start command.
    bool start_immediately{false};
};

// Plays a recorded stream into the market state at a chosen multiple of
// real time, on its own thread.
//
// The reader is pulled one event at a time, so a recording is streamed, not
// loaded. Playback begins in Ready (unless started immediately); the first
// event is delivered as soon as it starts, and each later event when its
// receive time is due under ReplaySchedule. Events keep the reader's order
// and ingest sequence exactly, and equal receive times need no wait.
// Pausing freezes playback; the paused wall time is added to the schedule,
// so resuming continues where it stopped rather than catching up. Stop
// interrupts any wait. The end of the recording is Completed; a reader
// error, a market-state rejection, or a timing overflow is Failed.
class PacedReplaySource final : public MarketDataSource {
public:
    using DiagnosticLog = std::function<void(std::string_view line)>;

    PacedReplaySource(
        std::unique_ptr<MarketEventReader> reader,
        MarketStateFeed& feed,
        const MonotonicClock& clock,
        MarketDataSourceDescription description,
        PacedReplayOptions options,
        DiagnosticLog log = {}
    );
    ~PacedReplaySource() override;

    PacedReplaySource(const PacedReplaySource&) = delete;
    PacedReplaySource& operator=(const PacedReplaySource&) = delete;
    PacedReplaySource(PacedReplaySource&&) = delete;
    PacedReplaySource& operator=(PacedReplaySource&&) = delete;

    [[nodiscard]] MarketDataSourceStatus status() const override;
    [[nodiscard]] ReplayCommandResult control_replay(
        ReplayCommand command
    ) override;
    void stop() noexcept override;

private:
    enum class Pacing {
        Deliver,
        Stopped,
        TimingOverflow,
    };

    void run() noexcept;
    void play();
    [[nodiscard]] bool wait_for_start();
    [[nodiscard]] Pacing wait_until_due(Timestamp receive_time);
    void finish(
        MarketDataSourceState state,
        std::optional<std::string> diagnostic,
        std::optional<std::string> recording_sha256 = std::nullopt
    );
    [[nodiscard]] MarketDataSourceStatus status_locked() const;

    std::unique_ptr<MarketEventReader> reader_;
    MarketStateFeed& feed_;
    const MonotonicClock& clock_;
    const MarketDataSourceDescription description_;
    const PacedReplayOptions options_;
    DiagnosticLog log_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    MarketDataSourceState state_;
    bool stop_requested_{false};
    std::optional<std::string> diagnostic_;
    std::optional<std::string> recording_sha256_;
    // Pause bookkeeping: the start of the current pause, and paused time
    // not yet added to the schedule.
    std::optional<MonotonicClock::time_point> paused_since_;
    MonotonicClock::duration unapplied_pause_{};
    ReplaySchedule schedule_;

    std::once_flag joined_;
    // Started last, after every member it uses exists.
    std::thread worker_;
};

}  // namespace chronos::market_data
