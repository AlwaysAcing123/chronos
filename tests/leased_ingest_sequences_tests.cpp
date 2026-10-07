// A live stream's ingest sequences under a durable, exclusive, expiring
// writer lease (ADR 0020), against an in-memory store with its own clock and
// injected faults: no PostgreSQL, no threads except where the keeper's own
// thread is the point. Every case is driven by explicit clock moves.
#include "chronos/market_data/leased_ingest_sequences.hpp"
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_state.hpp"
#include "live_sequence_test_support.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::MarketDataStreamId;
using chronos::market_data::BegunRun;
using chronos::market_data::IngestSequencesUnavailable;
using chronos::market_data::IssuedSequence;
using chronos::market_data::PublicationRun;
using chronos::market_data::LeasedIngestSequences;
using chronos::market_data::LeasedIngestSequencesOptions;
using chronos::market_data::MonotonicClock;
using chronos::market_data::StreamWriter;
using chronos::test::ManualClock;
using chronos::test::MemoryLeaseStore;
using namespace std::chrono_literals;

constexpr std::string_view kStream = "alpaca-iex-sequences";
constexpr std::uint64_t kLast = std::numeric_limits<std::uint64_t>::max();

LeasedIngestSequencesOptions small_blocks(std::uint64_t block_size = 8) {
    return LeasedIngestSequencesOptions{
        .lease = 15'000ms,
        .safety_margin = 3'000ms,
        .renew_every = 5'000ms,
        .retry_every = 1'000ms,
        .block_size = block_size,
        .adopt_history = false,
    };
}

std::uint64_t issued(IssuedSequence result) {
    if (const auto* unavailable = std::get_if<IngestSequencesUnavailable>(&result)) {
        throw chronos::test::TestFailure("no sequence was issued: " + unavailable->diagnostic);
    }
    return std::get<std::uint64_t>(result);
}

// A begun run's first sequence.
std::uint64_t issued(BegunRun result) {
    if (const auto* unavailable = std::get_if<IngestSequencesUnavailable>(&result)) {
        throw chronos::test::TestFailure("no run began: " + unavailable->diagnostic);
    }
    return std::get<PublicationRun>(result).first;
}

IngestSequencesUnavailable refused(IssuedSequence result) {
    REQUIRE(std::holds_alternative<IngestSequencesUnavailable>(result));
    return std::get<IngestSequencesUnavailable>(std::move(result));
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// One server process: its own steady clock, writer, and sequences, over the
// shared store. The database clock moves with every process's clock, so a
// test moves time once for all of them.
class Process final {
public:
    Process(MemoryLeaseStore& store, std::string writer, LeasedIngestSequencesOptions options = small_blocks())
        : name_(writer),
          sequences_(
              MarketDataStreamId(std::string(kStream)),
              store,
              StreamWriter{.id = writer, .label = "pid of " + writer},
              clock_,
              options
          ) {}

    LeasedIngestSequences& sequences() { return sequences_; }
    ManualClock& clock() { return clock_; }
    const std::string& name() const { return name_; }

    // As a source's session uses them: the run it began numbers and
    // publishes everything until it begins another.
    BegunRun begin_run() {
        BegunRun begun = sequences_.begin_run();
        if (const auto* run = std::get_if<PublicationRun>(&begun)) {
            run_ = run->id;
        }
        return begun;
    }
    IssuedSequence issue(std::uint64_t count) { return sequences_.issue(run_, count); }
    std::optional<IngestSequencesUnavailable> publication_refusal() const {
        return sequences_.publication_refusal(run_);
    }
    // The run this process's session began last; 0 before any.
    std::uint64_t run() const { return run_; }

private:
    std::string name_;
    std::uint64_t run_{0};
    ManualClock clock_{ManualClock::Mode::Hold};
    LeasedIngestSequences sequences_;
};

class World final {
public:
    World() : store_(database_) {}

    MemoryLeaseStore& store() { return store_; }

    // Moves real time: the database clock and every listed process's clock.
    void pass(MonotonicClock::duration elapsed, const std::vector<Process*>& processes) {
        database_.advance_to(database_.now() + elapsed);
        for (Process* process : processes) {
            process->clock().advance_to(process->clock().now() + elapsed);
        }
    }

private:
    ManualClock database_{ManualClock::Mode::Hold};
    MemoryLeaseStore store_;
};

void test_a_run_issues_contiguous_sequences_from_a_reserved_block() {
    World world;
    Process process(world.store(), "a");
    // Nothing before the lease is held.
    REQUIRE(process.sequences().readiness().has_value());
    refused(process.issue(1));
    static_cast<void>(process.sequences().maintain());
    REQUIRE(!process.sequences().readiness().has_value());
    // A new run must begin before anything is issued.
    REQUIRE(contains(refused(process.issue(1)).diagnostic, "new publication run"));
    REQUIRE(issued(process.begin_run()) == 0);
    REQUIRE(issued(process.issue(3)) == 0);
    REQUIRE(issued(process.issue(1)) == 3);
    REQUIRE(!process.publication_refusal().has_value());
    REQUIRE(world.store().row(std::string(kStream))->next == std::optional<std::uint64_t>(8));
}

void test_a_frame_is_issued_whole_and_blocks_refill_contiguously() {
    World world;
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.begin_run()) == 0);
    REQUIRE(issued(process.issue(5)) == 0);
    // Below half a block: the next step reserves another, which continues
    // the first, so the run goes on without a jump.
    static_cast<void>(process.sequences().maintain());
    REQUIRE(world.store().row(std::string(kStream))->next == std::optional<std::uint64_t>(16));
    REQUIRE(issued(process.issue(7)) == 5);
    REQUIRE(issued(process.issue(4)) == 12);
    // A frame larger than what remains is not split: nothing is issued.
    REQUIRE(!refused(process.issue(20)).permanent);
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.issue(1)) == 16);
}

void test_publication_stops_at_the_local_deadline_and_a_late_renewal_needs_a_new_run() {
    World world;
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.begin_run()) == 0);
    // The store is unavailable: renewals fail, and the deadline stays at
    // acquisition + 15 s − 3 s.
    world.store().fail_next(MemoryLeaseStore::Fault::Unavailable, 3);
    world.pass(5s, {&process});
    static_cast<void>(process.sequences().maintain());
    world.pass(6'999ms, {&process});
    REQUIRE(!process.publication_refusal().has_value());
    world.pass(1ms, {&process});
    const auto lapsed = process.publication_refusal();
    REQUIRE(lapsed.has_value());
    REQUIRE(contains(lapsed->diagnostic, "lapsed"));
    REQUIRE(contains(lapsed->diagnostic, "database unavailable"));
    refused(process.issue(1));
    REQUIRE(process.sequences().readiness().has_value());
    static_cast<void>(process.sequences().maintain());
    static_cast<void>(process.sequences().maintain());
    // Back: still this writer's, so its block is intact, but the run that
    // lapsed is over for good (Codex's review, finding 2). A new one
    // continues the block without a jump.
    static_cast<void>(process.sequences().maintain());
    REQUIRE(!process.sequences().readiness().has_value());
    const std::uint64_t lapsed_run = process.run();
    REQUIRE(process.publication_refusal().has_value());
    REQUIRE(contains(refused(process.issue(1)).diagnostic, "new publication run"));
    REQUIRE(issued(process.begin_run()) == 0);
    REQUIRE(process.run() > lapsed_run);
    REQUIRE(issued(process.issue(1)) == 0);
    REQUIRE(process.sequences().publication_refusal(lapsed_run).has_value());
}

// The exact boundary: publication is allowed up to the last nanosecond
// before the deadline, and a renewal that began in time keeps the run only
// if it is confirmed before the deadline. Confirmed at it or after, the run
// is over, although the lease is still this writer's.
void test_a_renewal_confirmed_at_the_deadline_ends_the_run() {
    for (const auto confirmed_after : {std::chrono::nanoseconds(6'999'999'999), std::chrono::nanoseconds(7s)}) {
        World world;
        Process process(world.store(), "a");
        static_cast<void>(process.sequences().maintain());  // deadline at 12 s
        REQUIRE(issued(process.begin_run()) == 0);
        const std::uint64_t run = process.run();
        world.pass(5s, {&process});
        world.store().during_next([&] { world.pass(confirmed_after, {&process}); });
        static_cast<void>(process.sequences().maintain());  // began at 5 s
        REQUIRE(!process.sequences().readiness().has_value());
        const bool in_time = confirmed_after < 7s;
        REQUIRE(process.publication_refusal().has_value() == !in_time);
        REQUIRE(process.sequences().current_run() == (in_time ? std::optional(run) : std::nullopt));
        if (in_time) {
            REQUIRE(issued(process.issue(1)) == 0);
            continue;
        }
        REQUIRE(contains(refused(process.issue(1)).diagnostic, "new publication run"));
        REQUIRE(issued(process.begin_run()) == 0);
        REQUIRE(issued(process.issue(1)) == 0);
        REQUIRE(process.sequences().publication_refusal(run).has_value());
    }
    // With no renewal at all, the last nanosecond before the deadline still
    // publishes; the deadline itself does not, and the run is over.
    World world;
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.begin_run()) == 0);
    world.pass(std::chrono::nanoseconds(12s) - 1ns, {&process});
    REQUIRE(!process.publication_refusal().has_value());
    REQUIRE(issued(process.issue(1)) == 0);
    world.pass(1ns, {&process});
    REQUIRE(process.publication_refusal().has_value());
    refused(process.issue(1));
    REQUIRE(!process.sequences().current_run().has_value());
}

// A crash, or a process alive behind a broken connection: the new writer
// waits for the lease to expire, and by then the old one has stopped.
void test_a_new_writer_takes_over_only_after_the_old_one_stopped() {
    World world;
    Process old_writer(world.store(), "old");
    Process new_writer(world.store(), "new");
    static_cast<void>(old_writer.sequences().maintain());
    REQUIRE(issued(old_writer.begin_run()) == 0);
    REQUIRE(issued(old_writer.issue(2)) == 0);

    // The old writer's keeper stops (it crashed, or cannot reach the
    // database); the new one keeps trying and is told who holds the lease.
    static_cast<void>(new_writer.sequences().maintain());
    const auto waiting = new_writer.sequences().readiness();
    REQUIRE(waiting.has_value());
    REQUIRE(contains(waiting->diagnostic, "pid of old"));
    REQUIRE(!waiting->permanent);
    for (int second = 1; second < 15; ++second) {
        world.pass(1s, {&old_writer, &new_writer});
        static_cast<void>(new_writer.sequences().maintain());
        REQUIRE(new_writer.sequences().readiness().has_value());
        // From 12 s the old writer refuses to publish: before any takeover.
        REQUIRE(old_writer.publication_refusal().has_value() == (second >= 12));
    }
    world.pass(1s, {&old_writer, &new_writer});
    static_cast<void>(new_writer.sequences().maintain());
    REQUIRE(!new_writer.sequences().readiness().has_value());
    REQUIRE(old_writer.publication_refusal().has_value());
    // After the old writer's block: never reused, never behind.
    REQUIRE(issued(new_writer.begin_run()) == 8);

    // The old process returns: its renewal finds the stream taken, and it
    // discards its block for good, then waits for the new writer's lease.
    static_cast<void>(old_writer.sequences().maintain());
    REQUIRE(contains(old_writer.sequences().readiness()->diagnostic, "lost"));
    refused(old_writer.issue(1));
    static_cast<void>(old_writer.sequences().maintain());
    REQUIRE(contains(old_writer.sequences().readiness()->diagnostic, "pid of new"));
}

void test_a_clean_release_lets_the_next_writer_start_at_once() {
    World world;
    {
        Process first(world.store(), "first");
        static_cast<void>(first.sequences().maintain());
        REQUIRE(issued(first.begin_run()) == 0);
        first.sequences().close();
        // Closed: nothing more is issued or published.
        refused(first.issue(1));
        REQUIRE(first.publication_refusal().has_value());
        first.sequences().close();
    }
    REQUIRE(!world.store().row(std::string(kStream))->writer.has_value());
    Process second(world.store(), "second");
    static_cast<void>(second.sequences().maintain());
    REQUIRE(issued(second.begin_run()) == 8);
}

void test_an_unanswered_acquisition_leaves_only_a_gap() {
    World world;
    Process process(world.store(), "a");
    world.store().fail_next(MemoryLeaseStore::Fault::CommitThenUnavailable);
    static_cast<void>(process.sequences().maintain());
    // It did commit, but the writer cannot know, so it holds nothing.
    REQUIRE(world.store().row(std::string(kStream))->writer == std::optional<std::string>("a"));
    REQUIRE(process.sequences().readiness().has_value());
    world.pass(1s, {&process});
    static_cast<void>(process.sequences().maintain());
    // The row is already its own: a new block, after the unknown one.
    REQUIRE(issued(process.begin_run()) == 8);
}

void test_an_unanswered_reservation_becomes_a_declared_jump() {
    World world;
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.begin_run()) == 0);
    REQUIRE(issued(process.issue(5)) == 0);
    // The refill commits [8, 15] but its answer is lost; the next one
    // reserves [16, 23], which does not continue [0, 7].
    world.store().fail_next(MemoryLeaseStore::Fault::CommitThenUnavailable);
    static_cast<void>(process.sequences().maintain());
    world.pass(1s, {&process});
    static_cast<void>(process.sequences().maintain());
    REQUIRE(world.store().row(std::string(kStream))->next == std::optional<std::uint64_t>(24));
    // The current block is used up first, then the run must end.
    REQUIRE(issued(process.issue(3)) == 5);
    REQUIRE(contains(refused(process.issue(1)).diagnostic, "new publication run"));
    // The next run begins after the gap, never inside it.
    REQUIRE(issued(process.begin_run()) == 16);
    REQUIRE(issued(process.issue(1)) == 16);
}

void test_an_unavailable_database_at_start_blocks_publication_until_it_returns() {
    World world;
    Process process(world.store(), "a");
    world.store().fail_next(MemoryLeaseStore::Fault::Unavailable, 2);
    static_cast<void>(process.sequences().maintain());
    const auto waiting = process.sequences().readiness();
    REQUIRE(waiting.has_value());
    REQUIRE(contains(waiting->diagnostic, "database unavailable"));
    REQUIRE(!waiting->permanent);
    REQUIRE(process.publication_refusal().has_value());
    world.pass(1s, {&process});
    static_cast<void>(process.sequences().maintain());
    REQUIRE(process.sequences().readiness().has_value());
    world.pass(1s, {&process});
    static_cast<void>(process.sequences().maintain());
    REQUIRE(!process.sequences().readiness().has_value());
}

void test_ownership_loss_discards_the_lost_block_for_good() {
    World world;
    Process a(world.store(), "a");
    Process b(world.store(), "b");
    static_cast<void>(a.sequences().maintain());
    REQUIRE(issued(a.begin_run()) == 0);
    // An operator cleared the lease (as if a's process were gone), and b
    // took the stream.
    auto stored_row = world.store().row(std::string(kStream));
    REQUIRE(stored_row.has_value());
    stored_row->writer.reset();
    world.store().set_row(std::string(kStream), std::move(*stored_row));
    static_cast<void>(b.sequences().maintain());
    REQUIRE(issued(b.begin_run()) == 8);
    REQUIRE(issued(b.issue(1)) == 8);
    world.pass(5s, {&a, &b});
    static_cast<void>(a.sequences().maintain());
    REQUIRE(a.sequences().readiness().has_value());
    refused(a.issue(1));
    REQUIRE(a.publication_refusal().has_value());
    // b releases; a acquires again, after everything b reserved.
    b.sequences().close();
    world.pass(1s, {&a, &b});
    static_cast<void>(a.sequences().maintain());
    REQUIRE(issued(a.begin_run()) == 16);
}

void test_sequences_end_at_the_largest_value_without_wrapping() {
    World world;
    world.store().set_row(std::string(kStream), MemoryLeaseStore::Row{.next = kLast - 2});
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.begin_run()) == kLast - 2);
    REQUIRE(issued(process.issue(2)) == kLast - 2);
    // The refill finds nothing left to reserve.
    static_cast<void>(process.sequences().maintain());
    REQUIRE(!world.store().row(std::string(kStream))->next.has_value());
    // Two cannot ever fit: one sequence is left, and no more can be reserved.
    REQUIRE(refused(process.issue(2)).permanent);
    REQUIRE(issued(process.issue(1)) == kLast);
    const auto exhausted = refused(process.issue(1));
    REQUIRE(exhausted.permanent);
    REQUIRE(contains(exhausted.diagnostic, "new stream ID"));
    REQUIRE(process.sequences().readiness()->permanent);

    // An exhausted stream cannot even be acquired.
    Process later(world.store(), "later");
    process.sequences().close();
    static_cast<void>(later.sequences().maintain());
    const auto refusal = later.sequences().readiness();
    REQUIRE(refusal.has_value());
    REQUIRE(refusal->permanent);
}

void test_a_frame_that_cannot_fit_in_the_last_sequences_is_refused_permanently() {
    World world;
    world.store().set_row(std::string(kStream), MemoryLeaseStore::Row{.next = kLast - 1});
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    REQUIRE(issued(process.begin_run()) == kLast - 1);
    static_cast<void>(process.sequences().maintain());  // finds the stream exhausted
    const auto too_large = refused(process.issue(3));
    REQUIRE(too_large.permanent);
    // Nothing was consumed by the refusal.
    REQUIRE(issued(process.issue(2)) == kLast - 1);
}

void test_legacy_history_needs_an_explicit_adoption() {
    World world;
    world.store().reference(std::string(kStream), 1087);
    {
        Process refusing(world.store(), "a");
        static_cast<void>(refusing.sequences().maintain());
        const auto refusal = refusing.sequences().readiness();
        REQUIRE(refusal.has_value());
        REQUIRE(refusal->permanent);
        REQUIRE(contains(refusal->diagnostic, "1087"));
        // Permanent: the keeper stops trying.
        const std::size_t calls = world.store().calls();
        static_cast<void>(refusing.sequences().maintain());
        REQUIRE(world.store().calls() == calls);
    }
    REQUIRE(!world.store().row(std::string(kStream)).has_value());
    auto options = small_blocks();
    options.adopt_history = true;
    Process adopting(world.store(), "b", options);
    static_cast<void>(adopting.sequences().maintain());
    REQUIRE(issued(adopting.begin_run()) == 1088);
    REQUIRE(world.store().row(std::string(kStream))->adopted_maximum == std::optional<std::uint64_t>(1087));
}

void test_history_beyond_the_reservations_is_refused() {
    World world;
    world.store().set_row(std::string(kStream), MemoryLeaseStore::Row{.next = 100});
    // Written by a server without durable sequencing, or a replay.
    world.store().reference(std::string(kStream), 100);
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    const auto refusal = process.sequences().readiness();
    REQUIRE(refusal.has_value());
    REQUIRE(refusal->permanent);
}

// The keeper's own thread blocks inside the store; publication still stops
// at the deadline, because the deadline is local.
void test_a_blocked_keeper_cannot_extend_publication() {
    World world;
    Process process(world.store(), "a");
    process.sequences().start();
    REQUIRE(issued(process.begin_run()) == 0);
    world.store().block_next();
    world.pass(5s, {&process});
    REQUIRE(world.store().await_blocked());
    world.pass(7s, {&process});
    REQUIRE(process.publication_refusal().has_value());
    refused(process.issue(1));
    world.store().unblock();
    // Renewed, but only after the deadline: the run is over.
    REQUIRE(chronos::test::eventually([&] { return !process.sequences().readiness().has_value(); }));
    REQUIRE(process.publication_refusal().has_value());
    REQUIRE(issued(process.begin_run()) == 0);
    REQUIRE(issued(process.issue(1)) == 0);
    process.sequences().close();
    REQUIRE(!world.store().row(std::string(kStream))->writer.has_value());
}

// The permit is asked under the gate, just before applying: a lapsed
// writer publishes nothing, and the market state is left as it was.
void test_a_refused_publication_changes_nothing() {
    World world;
    Process process(world.store(), "a");
    static_cast<void>(process.sequences().maintain());
    const std::uint64_t first = issued(process.begin_run());
    REQUIRE(issued(process.issue(2)) == first);

    ManualClock feed_clock;
    chronos::MarketState market{MarketDataStreamId(std::string(chronos::test::kSourceTestStream))};
    chronos::market_data::MarketDataNotifier notifier(feed_clock, chronos::market_data::MarketDataNotifier::Options{});
    chronos::market_data::MarketStateFeed feed(market, notifier);
    market.suspend();
    REQUIRE(!market.continue_at(first));
    market.resume();
    const auto published = feed.publish(chronos::test::recorded_quote(first, 0ns), process.sequences(), process.run());
    REQUIRE(!published.refused.has_value());
    REQUIRE(!published.failed.has_value());
    REQUIRE(feed.events_applied() == 1U);

    world.pass(12s, {&process});
    const auto refusal = feed.publish(chronos::test::recorded_quote(first + 1, 1ms), process.sequences(), process.run());
    REQUIRE(refusal.refused.has_value());
    REQUIRE(!refusal.failed.has_value());
    REQUIRE(feed.events_applied() == 1U);
    const auto status = market.status();
    REQUIRE(status.last_ingest_sequence == std::optional<std::uint64_t>(first));
    REQUIRE(!status.interruption.has_value());
}

// Codex's review, finding 1, with the real feed: a frame numbered under
// one acquisition, delayed while a successor takes over after a genuine
// expiry and publishes, and while this writer takes the stream back. The
// old frame is never published.
void test_an_obsolete_frame_is_never_published_after_reacquisition() {
    World world;
    Process old_writer(world.store(), "old");
    Process successor(world.store(), "new");
    static_cast<void>(old_writer.sequences().maintain());
    REQUIRE(issued(old_writer.begin_run()) == 0);
    const std::uint64_t stale = issued(old_writer.issue(1));

    ManualClock feed_clock;
    chronos::MarketState old_market{MarketDataStreamId(std::string(chronos::test::kSourceTestStream))};
    chronos::market_data::MarketDataNotifier notifier(feed_clock, chronos::market_data::MarketDataNotifier::Options{});
    chronos::market_data::MarketStateFeed old_feed(old_market, notifier);
    old_market.suspend();
    REQUIRE(!old_market.continue_at(stale));
    old_market.resume();

    world.pass(15s, {&old_writer, &successor});
    static_cast<void>(successor.sequences().maintain());
    REQUIRE(issued(successor.begin_run()) == 8);
    static_cast<void>(old_writer.sequences().maintain());  // finds the stream taken
    successor.sequences().close();
    static_cast<void>(old_writer.sequences().maintain());  // takes it back: a new block
    REQUIRE(!old_writer.sequences().readiness().has_value());

    const auto published =
        old_feed.publish(chronos::test::recorded_quote(stale, 0ns), old_writer.sequences(), old_writer.run());
    REQUIRE(published.refused.has_value());
    REQUIRE(contains(published.refused->diagnostic, "has ended"));
    REQUIRE(old_feed.events_applied() == 0U);
    // Nor once the old writer begins a new run: that run is a new session's.
    REQUIRE(issued(old_writer.begin_run()) == 16);
    const auto still = old_feed.publish(chronos::test::recorded_quote(stale, 0ns), old_writer.sequences(), 1);
    REQUIRE(still.refused.has_value());
    REQUIRE(old_feed.events_applied() == 0U);
}

// Many writers, crashes, releases, lost answers, and outages, in a fixed
// pseudo-random order: every sequence issued anywhere is unique, and each
// is greater than every one issued before it.
void test_no_sequence_is_issued_twice_or_out_of_order_across_writers() {
    World world;
    std::vector<std::unique_ptr<Process>> processes;
    std::mt19937 random(20261005);
    std::vector<std::uint64_t> history;
    std::set<std::uint64_t> seen;
    std::set<std::string> writers;
    int created = 0;
    const auto spawn = [&] {
        processes.push_back(std::make_unique<Process>(world.store(), "w" + std::to_string(created++)));
    };
    spawn();
    spawn();
    int crashes = 0;
    int closes = 0;
    for (int step = 0; step < 20'000; ++step) {
        Process& process = *processes[random() % processes.size()];
        const unsigned roll = random() % 100;
        if (roll < 3) {
            world.store().fail_next(
                random() % 2 == 0 ? MemoryLeaseStore::Fault::Unavailable
                                  : MemoryLeaseStore::Fault::CommitThenUnavailable
            );
        } else if (roll < 13) {
            std::vector<Process*> all;
            for (auto& each : processes) {
                all.push_back(each.get());
            }
            world.pass(std::chrono::milliseconds(random() % 2'000), all);
        } else if (roll < 14) {
            if (random() % 2 == 0) {
                // A crash: the process vanishes and its lease stays until it
                // expires (its release never reaches the store).
                world.store().fail_next(MemoryLeaseStore::Fault::Unavailable);
                processes.erase(processes.begin() + static_cast<std::ptrdiff_t>(random() % processes.size()));
                spawn();
                ++crashes;
            } else {
                process.sequences().close();
                ++closes;
            }
        } else if (roll < 34) {
            static_cast<void>(process.sequences().maintain());
        } else if (roll < 39) {
            static_cast<void>(process.begin_run());
        } else {
            const std::uint64_t count = 1 + random() % 5;
            const auto result = process.issue(count);
            if (const auto* first = std::get_if<std::uint64_t>(&result)) {
                writers.insert(process.name());
                for (std::uint64_t offset = 0; offset < count; ++offset) {
                    const std::uint64_t sequence = *first + offset;
                    REQUIRE(history.empty() || sequence > history.back());
                    REQUIRE(seen.insert(sequence).second);
                    history.push_back(sequence);
                }
            }
        }
        // Replace closed processes so the stream keeps moving.
        for (auto& each : processes) {
            const auto readiness = each->sequences().readiness();
            if (readiness && readiness->diagnostic.find("is closed") != std::string::npos) {
                each = std::make_unique<Process>(world.store(), "w" + std::to_string(created++));
            }
        }
    }
    std::cout << "issued " << history.size() << " sequences from " << writers.size() << " writers; "
              << crashes << " crashes, " << closes << " clean closes; last " << history.back() << '\n';
    // Enough happened for the property to mean something: takeovers after
    // crashes and closes, not one writer throughout.
    REQUIRE(history.size() > 1'000);
    REQUIRE(writers.size() >= 10);
    REQUIRE(crashes >= 10);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "a run issues contiguous sequences from a reserved block",
            test_a_run_issues_contiguous_sequences_from_a_reserved_block,
        },
        {
            "a frame is issued whole and blocks refill contiguously",
            test_a_frame_is_issued_whole_and_blocks_refill_contiguously,
        },
        {
            "publication stops at the local deadline and a late renewal needs a new run",
            test_publication_stops_at_the_local_deadline_and_a_late_renewal_needs_a_new_run,
        },
        {
            "a renewal confirmed at the deadline ends the run",
            test_a_renewal_confirmed_at_the_deadline_ends_the_run,
        },
        {
            "a new writer takes over only after the old one stopped",
            test_a_new_writer_takes_over_only_after_the_old_one_stopped,
        },
        {"a clean release lets the next writer start at once", test_a_clean_release_lets_the_next_writer_start_at_once},
        {"an unanswered acquisition leaves only a gap", test_an_unanswered_acquisition_leaves_only_a_gap},
        {"an unanswered reservation becomes a declared jump", test_an_unanswered_reservation_becomes_a_declared_jump},
        {
            "an unavailable database at start blocks publication until it returns",
            test_an_unavailable_database_at_start_blocks_publication_until_it_returns,
        },
        {"ownership loss discards the lost block for good", test_ownership_loss_discards_the_lost_block_for_good},
        {
            "sequences end at the largest value without wrapping",
            test_sequences_end_at_the_largest_value_without_wrapping,
        },
        {
            "a frame that cannot fit in the last sequences is refused permanently",
            test_a_frame_that_cannot_fit_in_the_last_sequences_is_refused_permanently,
        },
        {"legacy history needs an explicit adoption", test_legacy_history_needs_an_explicit_adoption},
        {"history beyond the reservations is refused", test_history_beyond_the_reservations_is_refused},
        {"a blocked keeper cannot extend publication", test_a_blocked_keeper_cannot_extend_publication},
        {"a refused publication changes nothing", test_a_refused_publication_changes_nothing},
        {
            "an obsolete frame is never published after reacquisition",
            test_an_obsolete_frame_is_never_published_after_reacquisition,
        },
        {
            "no sequence is issued twice or out of order across writers",
            test_no_sequence_is_issued_twice_or_out_of_order_across_writers,
        },
    });
}
