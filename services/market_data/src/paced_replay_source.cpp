#include "chronos/market_data/paced_replay_source.hpp"

#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos::market_data {
namespace {

__extension__ using WideInteger = __int128;

// Adds two durations, or returns nullopt when the sum does not fit.
std::optional<MonotonicClock::duration> checked_sum(
    MonotonicClock::duration left,
    MonotonicClock::duration right
) {
    const WideInteger sum =
        static_cast<WideInteger>(left.count()) + right.count();
    if (sum > std::numeric_limits<MonotonicClock::duration::rep>::max()) {
        return std::nullopt;
    }
    return MonotonicClock::duration(
        static_cast<MonotonicClock::duration::rep>(sum)
    );
}

Timestamp receive_time_of(const ValidatedMarketEvent& event) {
    return market_event_header(event.value()).receive_timestamp;
}

}  // namespace

PacedReplaySource::PacedReplaySource(
    std::unique_ptr<MarketEventReader> reader,
    MarketStateFeed& feed,
    const MonotonicClock& clock,
    MarketDataSourceDescription description,
    PacedReplayOptions options,
    DiagnosticLog log
)
    : reader_(std::move(reader)),
      feed_(feed),
      clock_(clock),
      description_(std::move(description)),
      options_(options),
      log_(std::move(log)),
      state_(
          options.start_immediately ? MarketDataSourceState::Running
                                    : MarketDataSourceState::Ready
      ),
      schedule_(options.speed),
      worker_([this] { run(); }) {}

PacedReplaySource::~PacedReplaySource() { stop(); }

MarketDataSourceStatus PacedReplaySource::status() const {
    const std::lock_guard lock(mutex_);
    return status_locked();
}

MarketDataSourceStatus PacedReplaySource::status_locked() const {
    return MarketDataSourceStatus{
        .description = description_,
        .state = state_,
        .events_applied = feed_.events_applied(),
        .replay_speed = options_.speed,
        .diagnostic = diagnostic_,
        .recording_sha256 = recording_sha256_,
    };
}

ReplayCommandResult PacedReplaySource::control_replay(ReplayCommand command) {
    MarketDataSourceStatus updated;
    {
        const std::lock_guard lock(mutex_);
        if (auto rejected = replay_transition_error(command, state_)) {
            return *rejected;
        }
        switch (command) {
            case ReplayCommand::Start:
            case ReplayCommand::Resume:
                if (paused_since_) {
                    const auto paused = checked_sum(
                        unapplied_pause_,
                        clock_.now() - *paused_since_
                    );
                    // A pause longer than the clock range cannot happen;
                    // resuming then fails the replay instead of skipping.
                    if (!paused) {
                        feed_.market_state().suspend();
                        state_ = MarketDataSourceState::Failed;
                        diagnostic_ = "replay pause time cannot be represented";
                        break;
                    }
                    unapplied_pause_ = *paused;
                    paused_since_.reset();
                }
                state_ = MarketDataSourceState::Running;
                break;
            case ReplayCommand::Pause:
                paused_since_ = clock_.now();
                state_ = MarketDataSourceState::Paused;
                break;
        }
        updated = status_locked();
    }
    changed_.notify_all();
    feed_.notifier().publish();
    return updated;
}

void PacedReplaySource::stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    changed_.notify_all();
    // Safe from several threads at once: later callers wait for the join.
    std::call_once(joined_, [this] {
        if (worker_.joinable()) {
            worker_.join();
        }
    });
}

void PacedReplaySource::run() noexcept {
    try {
        play();
    } catch (const std::exception& error) {
        finish(
            MarketDataSourceState::Failed,
            std::string("replay stopped unexpectedly: ") + error.what()
        );
    } catch (...) {
        finish(MarketDataSourceState::Failed, "replay stopped unexpectedly");
    }
}

void PacedReplaySource::play() {
    if (!wait_for_start()) {
        finish(MarketDataSourceState::Stopped, std::nullopt);
        return;
    }
    while (true) {
        MarketEventRead read = reader_->read_next();
        if (auto* end = std::get_if<EndOfMarketEvents>(&read)) {
            finish(
                MarketDataSourceState::Completed,
                std::nullopt,
                std::move(end->source_sha256)
            );
            return;
        }
        if (const auto* error = std::get_if<MarketEventReadError>(&read)) {
            finish(
                MarketDataSourceState::Failed,
                "recording rejected: " + error->message
            );
            return;
        }
        const auto& event = std::get<ValidatedMarketEvent>(read);
        switch (wait_until_due(receive_time_of(event))) {
            case Pacing::Deliver:
                break;
            case Pacing::Stopped:
                finish(MarketDataSourceState::Stopped, std::nullopt);
                return;
            case Pacing::TimingOverflow:
                finish(
                    MarketDataSourceState::Failed,
                    "replay timing exceeds the steady-clock range"
                );
                return;
        }
        if (auto rejected = feed_.apply(event)) {
            finish(MarketDataSourceState::Failed, std::move(*rejected));
            return;
        }
    }
}

bool PacedReplaySource::wait_for_start() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] {
        return stop_requested_ || state_ != MarketDataSourceState::Ready;
    });
    return !stop_requested_;
}

PacedReplaySource::Pacing PacedReplaySource::wait_until_due(
    Timestamp receive_time
) {
    std::unique_lock lock(mutex_);
    while (true) {
        if (stop_requested_) {
            return Pacing::Stopped;
        }
        if (state_ == MarketDataSourceState::Paused) {
            changed_.wait(lock, [this] {
                return stop_requested_ ||
                    state_ != MarketDataSourceState::Paused;
            });
            continue;
        }
        if (state_ == MarketDataSourceState::Failed) {
            return Pacing::TimingOverflow;
        }
        if (!schedule_.postpone(std::exchange(unapplied_pause_, {}))) {
            return Pacing::TimingOverflow;
        }
        const MonotonicClock::time_point now = clock_.now();
        const ReplayDeadline deadline = schedule_.due_time(receive_time, now);
        if (std::holds_alternative<ReplayScheduleOverflow>(deadline)) {
            return Pacing::TimingOverflow;
        }
        const auto& due = std::get<std::optional<MonotonicClock::time_point>>(
            deadline
        );
        if (!due || now >= *due) {
            return Pacing::Deliver;
        }
        clock_.wait_until(lock, changed_, *due, [this] {
            return stop_requested_ || state_ != MarketDataSourceState::Running;
        });
    }
}

void PacedReplaySource::finish(
    MarketDataSourceState state,
    std::optional<std::string> diagnostic,
    std::optional<std::string> recording_sha256
) {
    // A failed or externally stopped source can no longer establish that its
    // last accepted quote is current. Completed recordings are different:
    // reaching EOF proves the final market state is complete and usable.
    if (state == MarketDataSourceState::Failed ||
        state == MarketDataSourceState::Stopped) {
        feed_.market_state().suspend();
    }
    std::optional<std::string> line;
    {
        const std::lock_guard lock(mutex_);
        state_ = state;
        diagnostic_ = std::move(diagnostic);
        recording_sha256_ = std::move(recording_sha256);
        const std::string events = std::to_string(feed_.events_applied());
        if (state == MarketDataSourceState::Completed) {
            line = "replay completed after " + events + " events" +
                (recording_sha256_
                     ? "; recording sha256 " + *recording_sha256_
                     : std::string());
        } else if (state == MarketDataSourceState::Failed) {
            line = "replay failed after " + events + " events: " +
                diagnostic_.value_or("unknown failure");
        }
    }
    changed_.notify_all();
    feed_.notifier().publish();
    if (line && log_) {
        log_(*line);
    }
}

}  // namespace chronos::market_data
