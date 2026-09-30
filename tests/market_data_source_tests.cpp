#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_event_observer.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_utc_clock.hpp"
#include "chronos/market_data/paced_replay_source.hpp"
#include "chronos/market_data/reconnect_backoff.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::EndOfMarketEvents;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::MarketEventRead;
using chronos::MarketEventReadError;
using chronos::MarketState;
using chronos::MarketStateUnavailable;
using chronos::MarketStateUnavailableCode;
using chronos::ReplaySpeed;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::ValidatedMarketEvent;
using chronos::market_data::MarketDataChanged;
using chronos::market_data::MarketDataHeartbeat;
using chronos::market_data::MarketDataMode;
using chronos::market_data::MarketDataNotice;
using chronos::market_data::MarketDataNotifier;
using chronos::market_data::MarketDataNotifierClosed;
using chronos::market_data::MarketDataSourceDescription;
using chronos::market_data::MarketDataSourceState;
using chronos::market_data::MarketDataSubscription;
using chronos::market_data::MarketEventGate;
using chronos::market_data::MarketEventObserver;
using chronos::market_data::MarketStateFeed;
using chronos::market_data::MonotonicClock;
using chronos::market_data::MonotonicUtcClock;
using chronos::market_data::NoMarketDataSource;
using chronos::market_data::PacedReplayOptions;
using chronos::market_data::PacedReplaySource;
using chronos::market_data::ReconnectBackoff;
using chronos::market_data::ReconnectBackoffPolicy;
using chronos::market_data::ReplayCommand;
using chronos::market_data::ReplayCommandError;
using chronos::market_data::ReplayCommandErrorCode;
using chronos::test::eventually;
using chronos::test::kSourceTestStream;
using chronos::test::ManualClock;
using chronos::test::recorded_quote;
using chronos::test::ScriptedReader;
using namespace std::chrono_literals;

MarketDataSourceDescription replay_description() {
    return MarketDataSourceDescription{
        .mode = MarketDataMode::HistoricalReplay,
        .provider = "Test",
        .feed = "Synthetic",
        .label = "Synthetic replay",
        .coverage_notice = std::nullopt,
        .coverage_label = std::nullopt,
        .instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
    };
}

// Receive offsets for a script of contiguous quotes.
std::deque<MarketEventRead> quotes_at(std::initializer_list<std::chrono::nanoseconds> offsets) {
    std::deque<MarketEventRead> reads;
    std::uint64_t sequence = 0;
    for (const auto offset : offsets) {
        reads.emplace_back(recorded_quote(sequence++, offset));
    }
    return reads;
}

// One source with its market state, notifier, and clock.
struct Replay {
    explicit Replay(
        std::deque<MarketEventRead> reads,
        ReplaySpeed speed = ReplaySpeed::RealTime,
        ManualClock::Mode mode = ManualClock::Mode::Advance,
        bool start_immediately = false,
        std::uint64_t events_already_applied = 0
    )
        : clock(mode),
          market(MarketDataStreamId(std::string(kSourceTestStream))),
          notifier(clock, MarketDataNotifier::Options{}),
          feed(market, notifier, events_already_applied),
          start(clock.now()),
          source(std::make_unique<PacedReplaySource>(
              std::make_unique<ScriptedReader>(std::move(reads)),
              feed,
              clock,
              replay_description(),
              PacedReplayOptions{.speed = speed, .start_immediately = start_immediately},
              [this](std::string_view line) {
                  const std::lock_guard lock(log_mutex);
                  log.emplace_back(line);
              }
          )) {}

    [[nodiscard]] MarketDataSourceState state() const { return source->status().state; }

    [[nodiscard]] bool reaches(MarketDataSourceState expected) const {
        return eventually([&] { return state() == expected; });
    }

    [[nodiscard]] std::uint64_t events() const { return feed.events_applied(); }

    void command(ReplayCommand command) {
        REQUIRE(std::holds_alternative<chronos::market_data::MarketDataSourceStatus>(
            source->control_replay(command)
        ));
    }

    ManualClock clock;
    MarketState market;
    MarketDataNotifier notifier;
    MarketStateFeed feed;
    ManualClock::time_point start;
    std::mutex log_mutex;
    std::vector<std::string> log;
    std::unique_ptr<PacedReplaySource> source;
};

// ---------------------------------------------------------------------------
// Observation

// Records what the market and the notifier looked like when each event was
// offered.
struct RecordingObserver final : MarketEventObserver {
    RecordingObserver(const MarketState& market, const MarketDataNotifier& notifier)
        : market(market), notifier(notifier) {}

    void observe(const ValidatedMarketEvent& event) override {
        observed.push_back(std::get<TopOfBookEvent>(event.value()).header.ingest_sequence);
        applied_when_observed.push_back(market.status().last_ingest_sequence);
        revision_when_observed.push_back(notifier.revision());
    }

    const MarketState& market;
    const MarketDataNotifier& notifier;
    std::vector<std::uint64_t> observed;
    std::vector<std::optional<std::uint64_t>> applied_when_observed;
    std::vector<std::uint64_t> revision_when_observed;
};

void test_the_feed_observes_each_event_after_applying_it_and_before_telling_subscribers() {
    ManualClock clock(ManualClock::Mode::Advance);
    MarketState market{MarketDataStreamId(std::string(kSourceTestStream))};
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    RecordingObserver observer(market, notifier);
    MarketEventGate gate;
    MarketStateFeed feed(market, notifier, 0, &observer, &gate);
    for (std::uint64_t sequence = 0; sequence < 3; ++sequence) {
        const std::uint64_t before = notifier.revision();
        REQUIRE(!feed.apply(recorded_quote(sequence, std::chrono::milliseconds(sequence))));
        REQUIRE(notifier.revision() > before);
        REQUIRE(observer.revision_when_observed.back() == before);
    }
    REQUIRE(observer.observed == std::vector<std::uint64_t>({0, 1, 2}));
    REQUIRE(observer.applied_when_observed ==
            std::vector<std::optional<std::uint64_t>>({0U, 1U, 2U}));
    // A rejected event is never offered.
    REQUIRE(feed.apply(recorded_quote(9, 9ms)).has_value());
    REQUIRE(observer.observed.size() == 3);
}

// An observer that fails leaves the event applied but not fully observed.
// Whoever gets the gate next, such as a submission that was waiting for
// it, must already find the market unusable.
struct FailingObserver final : MarketEventObserver {
    FailingObserver(MarketEventGate& gate, const MarketState& market)
        : gate(gate), market(market) {}

    void observe(const ValidatedMarketEvent&) override {
        waiter = std::thread([this] {
            auto held = gate.hold_market(std::chrono::steady_clock::now() + 10s);
            if (held) {
                quote_after_gate = market.execution_quote(InstrumentId("AAPL"));
            }
        });
        throw std::runtime_error("database refused the query");
    }

    MarketEventGate& gate;
    const MarketState& market;
    std::thread waiter;
    std::optional<chronos::ExecutionQuoteResult> quote_after_gate;
};

void test_a_failing_observer_suspends_the_market_before_releasing_the_gate() {
    ManualClock clock(ManualClock::Mode::Advance);
    MarketState market{MarketDataStreamId(std::string(kSourceTestStream))};
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    MarketEventGate gate;
    FailingObserver observer(gate, market);
    MarketStateFeed feed(market, notifier, 0, &observer, &gate);
    const auto failure = feed.apply(recorded_quote(0, 0ms));
    observer.waiter.join();
    REQUIRE(failure == "market event observer failed: database refused the query");
    REQUIRE(feed.events_applied() == 1);
    REQUIRE(observer.quote_after_gate.has_value());
    REQUIRE(std::get<MarketStateUnavailable>(*observer.quote_after_gate).code ==
        MarketStateUnavailableCode::SourceUnavailable);
}

// ---------------------------------------------------------------------------
// Pacing

void test_replay_waits_in_ready_until_started() {
    Replay replay(quotes_at({0s, 1s}));
    REQUIRE(replay.state() == MarketDataSourceState::Ready);
    REQUIRE(replay.source->status().replay_speed == ReplaySpeed::RealTime);
    REQUIRE(replay.events() == 0);
    REQUIRE(replay.clock.deadlines().empty());
    replay.source->stop();
    REQUIRE(replay.state() == MarketDataSourceState::Stopped);
    REQUIRE(replay.events() == 0);
}

void test_first_event_is_immediate_and_later_ones_follow_receive_time() {
    // Two events share a receive time; the second needs no wait.
    Replay replay(quotes_at({0s, 1s, 1s, 2500ms}));
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    REQUIRE(replay.events() == 4);
    REQUIRE((replay.clock.deadlines() == std::vector{replay.start + 1s, replay.start + 2500ms}));
    REQUIRE(replay.market.status().last_ingest_sequence == 3U);
}

void test_faster_speeds_scale_every_wait() {
    Replay ten(quotes_at({0s, 1s, 2500ms}), ReplaySpeed::TenTimes);
    ten.command(ReplayCommand::Start);
    REQUIRE(ten.reaches(MarketDataSourceState::Completed));
    REQUIRE((ten.clock.deadlines() == std::vector{ten.start + 100ms, ten.start + 250ms}));

    Replay hundred(quotes_at({0s, 1s, 2500ms}), ReplaySpeed::HundredTimes);
    hundred.command(ReplayCommand::Start);
    REQUIRE(hundred.reaches(MarketDataSourceState::Completed));
    REQUIRE((hundred.clock.deadlines() == std::vector{hundred.start + 10ms, hundred.start + 25ms}));
}

void test_maximum_speed_never_waits() {
    Replay replay(quotes_at({0s, 1s, 60s, 3600s}), ReplaySpeed::Maximum);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    REQUIRE(replay.events() == 4);
    REQUIRE(replay.clock.deadlines().empty());
}

void test_late_events_catch_up_without_waiting() {
    // The clock is already past both deadlines when they are scheduled.
    Replay replay(quotes_at({0s, 1s, 2s}), ReplaySpeed::RealTime, ManualClock::Mode::Hold);
    replay.clock.advance_to(replay.start);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.clock.await_waits(1));
    replay.clock.advance_to(replay.start + 5s);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    // Only the first gap was waited for; the second was already late.
    REQUIRE((replay.clock.deadlines() == std::vector{replay.start + 1s}));
}

void test_equal_receive_times_keep_ingest_order() {
    std::deque<MarketEventRead> reads;
    reads.emplace_back(recorded_quote(0, 0s, "AAPL"));
    reads.emplace_back(recorded_quote(1, 0s, "SPY"));
    reads.emplace_back(recorded_quote(2, 0s, "AAPL"));
    Replay replay(std::move(reads));
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    REQUIRE(replay.clock.deadlines().empty());
    REQUIRE(replay.market.status().last_ingest_sequence == 2U);
    const auto quote = replay.market.execution_quote(InstrumentId("AAPL"));
    REQUIRE(std::get<chronos::ExecutionQuote>(quote).top_of_book.header.ingest_sequence == 2U);
}

void test_receive_time_regression_fails_closed() {
    Replay replay(quotes_at({0s, 2s, 1s}));
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Failed));
    REQUIRE(replay.events() == 2);
    const auto status = replay.source->status();
    REQUIRE(status.diagnostic->find("receive time") != std::string::npos);
    REQUIRE(replay.market.status().interruption.has_value());
}

// ---------------------------------------------------------------------------
// Pause, resume, and stop

void test_pause_interrupts_a_pending_wait() {
    Replay replay(quotes_at({0s, 10s}), ReplaySpeed::RealTime, ManualClock::Mode::Hold);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.clock.await_waits(1));
    replay.command(ReplayCommand::Pause);
    REQUIRE(replay.state() == MarketDataSourceState::Paused);
    // Time passing while paused delivers nothing.
    replay.clock.advance_to(replay.start + 60s);
    replay.source->stop();
    REQUIRE(replay.events() == 1);
    REQUIRE(replay.state() == MarketDataSourceState::Stopped);
}

void test_resume_shifts_the_schedule_instead_of_bursting() {
    Replay replay(quotes_at({0s, 10s, 11s}), ReplaySpeed::RealTime, ManualClock::Mode::Hold);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.clock.await_waits(1));

    replay.clock.advance_to(replay.start + 4s);
    replay.command(ReplayCommand::Pause);
    replay.clock.advance_to(replay.start + 104s);
    replay.command(ReplayCommand::Resume);
    // The event 6 s of playback away is due 6 s after resuming, not at once.
    REQUIRE(replay.clock.await_waits(2));
    REQUIRE((replay.clock.deadlines() == std::vector{replay.start + 10s, replay.start + 110s}));
    REQUIRE(replay.events() == 1);

    replay.clock.advance_to(replay.start + 110s);
    REQUIRE(eventually([&] { return replay.events() == 2; }));
    REQUIRE(replay.clock.await_waits(3));
    REQUIRE(replay.clock.deadlines().back() == replay.start + 111s);
    replay.clock.advance_to(replay.start + 111s);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
}

void test_stop_interrupts_a_pending_wait() {
    Replay replay(quotes_at({0s, 3600s}), ReplaySpeed::RealTime, ManualClock::Mode::Hold);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.clock.await_waits(1));
    replay.source->stop();
    REQUIRE(replay.state() == MarketDataSourceState::Stopped);
    REQUIRE(replay.events() == 1);
    replay.source->stop();  // idempotent
}

void test_stop_can_race_from_several_threads() {
    Replay replay(quotes_at({0s, 3600s}), ReplaySpeed::RealTime, ManualClock::Mode::Hold);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.clock.await_waits(1));
    std::thread first([&] { replay.source->stop(); });
    std::thread second([&] { replay.source->stop(); });
    first.join();
    second.join();
    REQUIRE(replay.state() == MarketDataSourceState::Stopped);
}

void test_invalid_transitions_are_typed() {
    Replay replay(quotes_at({0s, 3600s}), ReplaySpeed::RealTime, ManualClock::Mode::Hold);
    const auto rejected = [&](ReplayCommand command) {
        auto result = replay.source->control_replay(command);
        REQUIRE(std::holds_alternative<ReplayCommandError>(result));
        return std::get<ReplayCommandError>(result);
    };
    REQUIRE(rejected(ReplayCommand::Pause).state == MarketDataSourceState::Ready);
    REQUIRE(rejected(ReplayCommand::Resume).code == ReplayCommandErrorCode::InvalidTransition);
    replay.command(ReplayCommand::Start);
    REQUIRE(rejected(ReplayCommand::Start).state == MarketDataSourceState::Running);
    REQUIRE(rejected(ReplayCommand::Resume).message == "cannot resume a replay that is running");
    replay.command(ReplayCommand::Pause);
    REQUIRE(rejected(ReplayCommand::Pause).state == MarketDataSourceState::Paused);
    REQUIRE(rejected(ReplayCommand::Start).state == MarketDataSourceState::Paused);
    replay.source->stop();
    REQUIRE(rejected(ReplayCommand::Resume).state == MarketDataSourceState::Stopped);
}

void test_completed_replay_cannot_restart() {
    Replay replay(quotes_at({0s}));
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    auto result = replay.source->control_replay(ReplayCommand::Start);
    REQUIRE(std::get<ReplayCommandError>(result).state == MarketDataSourceState::Completed);
    // Stopping the server later does not rewrite the outcome.
    replay.source->stop();
    REQUIRE(replay.state() == MarketDataSourceState::Completed);
}

void test_autostart_plays_without_a_command() {
    Replay replay(quotes_at({0s, 1s}), ReplaySpeed::Maximum, ManualClock::Mode::Advance, true);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    REQUIRE(replay.events() == 2);
}

void test_no_source_rejects_replay_commands() {
    NoMarketDataSource source({InstrumentId("AAPL")});
    REQUIRE(source.status().state == MarketDataSourceState::None);
    REQUIRE(source.status().description.mode == MarketDataMode::None);
    const auto result = source.control_replay(ReplayCommand::Start);
    REQUIRE(std::get<ReplayCommandError>(result).code == ReplayCommandErrorCode::NotReplay);
}

// ---------------------------------------------------------------------------
// Outcomes

void test_completion_records_the_recording_checksum() {
    std::deque<MarketEventRead> reads = quotes_at({0s});
    reads.emplace_back(EndOfMarketEvents{.source_sha256 = std::string(64, 'a')});
    Replay replay(std::move(reads));
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    REQUIRE(replay.source->status().recording_sha256 == std::string(64, 'a'));
    REQUIRE(!replay.source->status().diagnostic.has_value());
    REQUIRE(std::holds_alternative<chronos::ExecutionQuote>(
        replay.market.execution_quote(InstrumentId("AAPL"))
    ));
    const std::lock_guard lock(replay.log_mutex);
    REQUIRE(replay.log.size() == 1);
    REQUIRE(replay.log.front() == "replay completed after 1 events; recording sha256 " + std::string(64, 'a'));
}

void test_reader_errors_fail_the_replay() {
    std::deque<MarketEventRead> reads = quotes_at({0s});
    reads.emplace_back(MarketEventReadError{.message = "record 2: truncated record"});
    Replay replay(std::move(reads));
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Failed));
    REQUIRE(replay.source->status().diagnostic == "recording rejected: record 2: truncated record");
    REQUIRE(replay.events() == 1);
    const auto quote = replay.market.execution_quote(InstrumentId("AAPL"));
    REQUIRE(std::get<MarketStateUnavailable>(quote).code ==
        MarketStateUnavailableCode::SourceUnavailable);
}

void test_market_state_rejections_fail_the_replay() {
    std::deque<MarketEventRead> reads = quotes_at({0s});
    reads.emplace_back(recorded_quote(1, 1s, "AAPL", "another-stream"));
    Replay replay(std::move(reads), ReplaySpeed::Maximum);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Failed));
    REQUIRE(replay.source->status().diagnostic->starts_with("market state rejected an event:"));
    REQUIRE(replay.events() == 1);
}

void test_timing_overflow_fails_the_replay() {
    Replay replay(
        quotes_at({0s, 2s}),
        ReplaySpeed::RealTime,
        ManualClock::Mode::Advance
    );
    replay.clock.advance_to(ManualClock::time_point::max() - 1s);
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Failed));
    REQUIRE(replay.source->status().diagnostic == "replay timing exceeds the steady-clock range");
    REQUIRE(replay.events() == 1);
    const auto quote = replay.market.execution_quote(InstrumentId("AAPL"));
    REQUIRE(std::get<MarketStateUnavailable>(quote).code ==
        MarketStateUnavailableCode::SourceUnavailable);
}

void test_event_count_overflow_fails_the_replay() {
    Replay replay(
        quotes_at({0s}),
        ReplaySpeed::Maximum,
        ManualClock::Mode::Advance,
        false,
        std::numeric_limits<std::uint64_t>::max()
    );
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Failed));
    REQUIRE(replay.source->status().diagnostic == "the applied-event count cannot be represented");
    // The uncountable event was not applied.
    REQUIRE(!replay.market.status().last_ingest_sequence.has_value());
}

void test_status_is_consistent_under_concurrent_reads() {
    std::deque<MarketEventRead> reads;
    for (std::uint64_t sequence = 0; sequence < 5'000; ++sequence) {
        reads.emplace_back(recorded_quote(sequence, std::chrono::microseconds(sequence)));
    }
    Replay replay(std::move(reads), ReplaySpeed::Maximum);
    std::atomic<bool> regressed{false};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) {
        readers.emplace_back([&] {
            std::uint64_t previous = 0;
            while (true) {
                const auto status = replay.source->status();
                if (status.events_applied < previous) {
                    regressed = true;
                }
                previous = status.events_applied;
                if (status.state == MarketDataSourceState::Completed) {
                    return;
                }
                std::this_thread::yield();
            }
        });
    }
    replay.command(ReplayCommand::Start);
    for (auto& reader : readers) {
        reader.join();
    }
    REQUIRE(!regressed);
    REQUIRE(replay.events() == 5'000);
}

// ---------------------------------------------------------------------------
// Notifications

void test_first_notice_reports_the_current_revision() {
    ManualClock clock;
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    notifier.publish();
    notifier.publish();
    auto subscription = std::get<MarketDataSubscription>(notifier.subscribe());
    const MarketDataNotice notice = subscription.next();
    REQUIRE(std::get<MarketDataChanged>(notice).revision == 2U);
}

void test_rapid_changes_coalesce_into_throttled_notices() {
    ManualClock clock;
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    auto subscription = std::get<MarketDataSubscription>(notifier.subscribe());
    const auto first_at = clock.now();
    REQUIRE(std::get<MarketDataChanged>(subscription.next()).revision == 0U);
    for (int change = 0; change < 1'000; ++change) {
        notifier.publish();
    }
    // One notice for all thousand changes, no sooner than the interval.
    REQUIRE(std::get<MarketDataChanged>(subscription.next()).revision == 1'000U);
    REQUIRE(clock.now() == first_at + 250ms);
    REQUIRE(notifier.revision() == 1'000U);
}

void test_idle_subscribers_receive_heartbeats() {
    ManualClock clock;
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    auto subscription = std::get<MarketDataSubscription>(notifier.subscribe());
    const auto start = clock.now();
    static_cast<void>(subscription.next());
    REQUIRE(std::holds_alternative<MarketDataHeartbeat>(subscription.next()));
    REQUIRE(clock.now() == start + 10s);
}

void test_close_ends_a_waiting_subscription() {
    ManualClock clock(ManualClock::Mode::Hold);
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    auto subscription = std::get<MarketDataSubscription>(notifier.subscribe());
    static_cast<void>(subscription.next());
    std::thread closer([&] {
        REQUIRE(clock.await_waits(1));
        notifier.close();
    });
    REQUIRE(std::holds_alternative<MarketDataNotifierClosed>(subscription.next()));
    closer.join();
    REQUIRE(std::get<MarketDataNotifier::SubscribeError>(notifier.subscribe()) ==
            MarketDataNotifier::SubscribeError::Closed);
}

void test_subscriber_capacity_is_bounded() {
    ManualClock clock;
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{.maximum_subscribers = 2});
    auto first = notifier.subscribe();
    auto second = notifier.subscribe();
    REQUIRE(std::holds_alternative<MarketDataSubscription>(first));
    REQUIRE(std::holds_alternative<MarketDataSubscription>(second));
    REQUIRE(std::get<MarketDataNotifier::SubscribeError>(notifier.subscribe()) ==
            MarketDataNotifier::SubscribeError::TooManySubscribers);
    {
        auto released = std::move(first);
    }
    REQUIRE(std::holds_alternative<MarketDataSubscription>(notifier.subscribe()));
}

void test_coalescing_never_skips_market_events() {
    std::deque<MarketEventRead> reads;
    for (std::uint64_t sequence = 0; sequence < 2'000; ++sequence) {
        reads.emplace_back(recorded_quote(sequence, std::chrono::milliseconds(sequence)));
    }
    Replay replay(std::move(reads), ReplaySpeed::Maximum);
    auto subscription = std::get<MarketDataSubscription>(replay.notifier.subscribe());
    replay.command(ReplayCommand::Start);
    REQUIRE(replay.reaches(MarketDataSourceState::Completed));
    // Every event reached the market state, in order.
    REQUIRE(replay.events() == 2'000);
    REQUIRE(replay.market.status().last_ingest_sequence == 1'999U);
    // A subscriber that was not reading sees one notice with the final revision.
    const auto notice = std::get<MarketDataChanged>(subscription.next());
    REQUIRE(notice.revision == replay.notifier.revision());
    REQUIRE(notice.revision >= 2'000U);
}

void test_receive_clock_advances_from_its_utc_anchor() {
    ManualClock clock;
    const Timestamp anchor = Timestamp::from_nanoseconds(1'790'343'000'000'000'000);
    const MonotonicUtcClock utc(clock, anchor);
    REQUIRE(utc.now() == anchor);
    clock.advance_to(clock.now() + 1'500ms + 7ns);
    REQUIRE(utc.now().nanoseconds() == anchor.nanoseconds() + 1'500'000'007);
    REQUIRE(utc.now() == utc.now());

    const MonotonicUtcClock near_limit(
        clock,
        Timestamp::from_nanoseconds(std::numeric_limits<std::int64_t>::max() - 5)
    );
    clock.advance_to(clock.now() + 6ns);
    chronos::test::require_throws<std::overflow_error>([&] {
        static_cast<void>(near_limit.now());
    }, __LINE__);

    // The system clock anchor is a plausible UTC time after 2020.
    REQUIRE(chronos::market_data::system_utc_now().nanoseconds() >
        1'577'836'800'000'000'000);
}

void test_backoff_doubles_to_its_bound_with_equal_jitter() {
    std::vector<MonotonicClock::duration> bounds;
    ReconnectBackoff backoff(
        ReconnectBackoffPolicy{.initial = 1s, .maximum = 10s},
        [&bounds](MonotonicClock::duration bound) {
            bounds.push_back(bound);
            return bound;
        }
    );
    REQUIRE(backoff.next_delay() == 1s);
    REQUIRE(backoff.next_delay() == 2s);
    REQUIRE(backoff.next_delay() == 4s);
    REQUIRE(backoff.next_delay() == 8s);
    REQUIRE(backoff.next_delay() == 10s);
    REQUIRE(backoff.next_delay() == 10s);
    REQUIRE(backoff.failures() == 6U);
    REQUIRE((bounds == std::vector<MonotonicClock::duration>{
        500ms, 1s, 2s, 4s, 5s, 5s,
    }));

    backoff.reset();
    REQUIRE(backoff.failures() == 0U);
    REQUIRE(backoff.next_delay() == 1s);
}

void test_backoff_clamps_a_faulty_jitter_source() {
    ReconnectBackoff low(
        ReconnectBackoffPolicy{.initial = 2s, .maximum = 2s},
        [](MonotonicClock::duration) { return -5s; }
    );
    REQUIRE(low.next_delay() == 1s);
    ReconnectBackoff high(
        ReconnectBackoffPolicy{.initial = 2s, .maximum = 2s},
        [](MonotonicClock::duration) { return 1h; }
    );
    REQUIRE(high.next_delay() == 2s);

    // Many failures neither overflow nor exceed the maximum.
    ReconnectBackoff many(
        ReconnectBackoffPolicy{.initial = 1ns, .maximum = 30s},
        [](MonotonicClock::duration bound) { return bound; }
    );
    for (int failure = 0; failure < 200; ++failure) {
        REQUIRE(many.next_delay() <= 30s);
    }
    REQUIRE(many.next_delay() == 30s);

    chronos::test::require_throws<std::invalid_argument>([] {
        ReconnectBackoff invalid(
            ReconnectBackoffPolicy{.initial = 0s, .maximum = 1s},
            [](MonotonicClock::duration bound) { return bound; }
        );
    }, __LINE__);
    chronos::test::require_throws<std::invalid_argument>([] {
        ReconnectBackoff invalid(
            ReconnectBackoffPolicy{.initial = 2s, .maximum = 1s},
            [](MonotonicClock::duration bound) { return bound; }
        );
    }, __LINE__);

    const auto jitter = chronos::market_data::random_backoff_jitter();
    for (int draw = 0; draw < 100; ++draw) {
        const auto value = jitter(3ms);
        REQUIRE(value >= 0ms);
        REQUIRE(value <= 3ms);
    }
    REQUIRE(jitter(0ns) == 0ns);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"replay waits in ready until started", test_replay_waits_in_ready_until_started},
        {"first event is immediate, later ones follow receive time",
         test_first_event_is_immediate_and_later_ones_follow_receive_time},
        {"faster speeds scale every wait", test_faster_speeds_scale_every_wait},
        {"maximum speed never waits", test_maximum_speed_never_waits},
        {"late events catch up without waiting", test_late_events_catch_up_without_waiting},
        {"equal receive times keep ingest order", test_equal_receive_times_keep_ingest_order},
        {"receive-time regression fails closed", test_receive_time_regression_fails_closed},
        {"pause interrupts a pending wait", test_pause_interrupts_a_pending_wait},
        {"resume shifts the schedule instead of bursting",
         test_resume_shifts_the_schedule_instead_of_bursting},
        {"stop interrupts a pending wait", test_stop_interrupts_a_pending_wait},
        {"stop can race from several threads", test_stop_can_race_from_several_threads},
        {"invalid transitions are typed", test_invalid_transitions_are_typed},
        {"completed replay cannot restart", test_completed_replay_cannot_restart},
        {"autostart plays without a command", test_autostart_plays_without_a_command},
        {"no source rejects replay commands", test_no_source_rejects_replay_commands},
        {"completion records the recording checksum", test_completion_records_the_recording_checksum},
        {"reader errors fail the replay", test_reader_errors_fail_the_replay},
        {"market-state rejections fail the replay", test_market_state_rejections_fail_the_replay},
        {"timing overflow fails the replay", test_timing_overflow_fails_the_replay},
        {"the feed observes each event before telling subscribers",
         test_the_feed_observes_each_event_after_applying_it_and_before_telling_subscribers},
        {"a failing observer suspends the market before releasing the gate",
         test_a_failing_observer_suspends_the_market_before_releasing_the_gate},
        {"event count overflow fails the replay", test_event_count_overflow_fails_the_replay},
        {"status is consistent under concurrent reads", test_status_is_consistent_under_concurrent_reads},
        {"first notice reports the current revision", test_first_notice_reports_the_current_revision},
        {"rapid changes coalesce into throttled notices",
         test_rapid_changes_coalesce_into_throttled_notices},
        {"idle subscribers receive heartbeats", test_idle_subscribers_receive_heartbeats},
        {"close ends a waiting subscription", test_close_ends_a_waiting_subscription},
        {"subscriber capacity is bounded", test_subscriber_capacity_is_bounded},
        {"coalescing never skips market events", test_coalescing_never_skips_market_events},
        {"receive clock advances from its UTC anchor", test_receive_clock_advances_from_its_utc_anchor},
        {"backoff doubles to its bound with equal jitter",
         test_backoff_doubles_to_its_bound_with_equal_jitter},
        {"backoff clamps a faulty jitter source", test_backoff_clamps_a_faulty_jitter_source},
    });
}
