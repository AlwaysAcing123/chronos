#pragma once

#include "chronos/market_data/live_ingest_sequences.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data_types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace chronos::market_data {

// One server process as a live stream's writer. The ID is random and never
// reused; the label says where the process runs, for diagnostics only.
struct StreamWriter {
    std::string id;
    std::string label;
};

// A reserved block of sequences, first to last inclusive, never empty.
struct SequenceBlock {
    std::uint64_t first{0};
    std::uint64_t last{0};

    friend bool operator==(const SequenceBlock&, const SequenceBlock&) = default;
};

// The writer now holds the stream's lease and has reserved `block`.
struct LeaseGranted {
    SequenceBlock block;
    // Set when this acquisition created the stream's record after durable
    // history from before durable sequencing: the largest sequence that
    // history references, which `block` follows.
    std::optional<std::uint64_t> adopted_maximum{};
};

// Another writer's lease has not expired.
struct LeaseHeld {
    std::string writer_label;
    std::string expires_at;
};

// Waiting cannot help: the stream is exhausted, or its durable history
// needs an operator's decision.
struct LeaseRefused {
    std::string reason;
};

using LeaseAcquisition = std::variant<LeaseGranted, LeaseHeld, LeaseRefused>;

struct LeaseRenewed {
    // The block reserved by this renewal, when one was asked for and any
    // sequence remained.
    std::optional<SequenceBlock> block{};
    // No sequence remains to reserve, ever.
    bool exhausted{false};
};

// The stream is no longer this writer's: released, or taken over after
// the lease expired.
struct LeaseLost {
    std::string reason;
};

using LeaseRenewal = std::variant<LeaseRenewed, LeaseLost>;

// Durable writer leases and sequence reservations of live streams (the
// PostgreSQL table live_stream_sequences). Each call is one transaction
// whose expiry times use the store's own clock. A call that throws may or
// may not have committed; callers treat it as not done, which is safe:
// an acquisition or reservation that did commit only leaves a gap.
class IngestSequenceLeaseStore {
public:
    virtual ~IngestSequenceLeaseStore() = default;

    // Takes the stream's lease unless another writer's is unexpired, and
    // reserves `block_size` sequences. A stream without a record is
    // created at 0 if no durable history references it; otherwise only with
    // `adopt_history`, after the largest sequence that history references.
    [[nodiscard]] virtual LeaseAcquisition acquire(
        const MarketDataStreamId& stream,
        const StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size,
        bool adopt_history
    ) = 0;

    // Extends the lease if it is still this writer's, and, if `block_size`
    // is not zero, reserves the next block.
    [[nodiscard]] virtual LeaseRenewal renew(
        const MarketDataStreamId& stream,
        const StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size
    ) = 0;

    // Ends the lease if it is still this writer's.
    virtual void release(const MarketDataStreamId& stream, const StreamWriter& writer) = 0;
};

struct LeasedIngestSequencesOptions {
    // How long a lease lasts in the store's clock.
    std::chrono::milliseconds lease{15'000};
    // Publication stops this long before the lease would expire, measured
    // from when the renewal began; it absorbs clock-rate differences.
    std::chrono::milliseconds safety_margin{3'000};
    std::chrono::milliseconds renew_every{5'000};
    // After an unavailable store or another writer's lease.
    std::chrono::milliseconds retry_every{1'000};
    // Sequences per reservation; a new one is reserved below half.
    std::uint64_t block_size{std::uint64_t{1} << 20U};
    // Continue a stream with durable history from before durable
    // sequencing, after the largest sequence it references (ADR 0020).
    bool adopt_history{false};
};

// A live stream's sequences under a durable, exclusive, expiring writer
// lease (ADR 0020).
//
// A keeper acquires the lease, renews it, and reserves blocks ahead of use;
// the source issues from reserved blocks in memory, so no quote waits for
// the store. Issuing and publishing stop at a local deadline, the start of
// the latest successful renewal plus the lease minus the safety margin,
// which is intended to fall before the lease expires in the store's clock.
// This is a local eligibility bound, not proof that this process has stopped
// before takeover. The database fence prevents authoritative effects from a
// deposed generation regardless of pauses or clock behavior. Losing the lease
// discards its blocks; a later acquisition reserves sequences after them.
//
// Reaching the deadline ends the publication run, even if a renewal that
// was already under way then succeeds: quotes and frames from before the
// lapse are never trusted again, and the source must begin a new run on a
// new session. The local deadline is what this process enforces; the
// database fence (ADR 0020) is what makes a deposed writer's late durable
// effect impossible.
//
// All member functions are safe to call concurrently, except that only one
// thread at a time may call maintain(), which the keeper does after start().
class LeasedIngestSequences final : public LiveIngestSequences {
public:
    using DiagnosticLog = std::function<void(std::string_view line)>;

    LeasedIngestSequences(
        MarketDataStreamId stream,
        IngestSequenceLeaseStore& store,
        StreamWriter writer,
        const MonotonicClock& clock,
        LeasedIngestSequencesOptions options = {},
        DiagnosticLog log = {}
    );
    ~LeasedIngestSequences() override;

    LeasedIngestSequences(const LeasedIngestSequences&) = delete;
    LeasedIngestSequences& operator=(const LeasedIngestSequences&) = delete;
    LeasedIngestSequences(LeasedIngestSequences&&) = delete;
    LeasedIngestSequences& operator=(LeasedIngestSequences&&) = delete;

    // Tries to acquire at once, then keeps the lease on a thread of its own.
    void start();

    // One step of the keeper: acquires, renews, or reserves, whichever is
    // due, and returns when the next step is due. Tests call it directly.
    MonotonicClock::time_point maintain();

    // Stops issuing and publishing, stops the keeper, and releases the
    // lease. Call it only after the source has stopped publishing; the
    // lease is released only once nothing can publish under it.
    // Idempotent; the destructor calls it.
    void close() noexcept;

    [[nodiscard]] std::optional<IngestSequencesUnavailable> readiness() const override;
    [[nodiscard]] BegunRun begin_run() override;
    [[nodiscard]] IssuedSequence issue(std::uint64_t run, std::uint64_t count) override;
    [[nodiscard]] std::optional<IngestSequencesUnavailable> publication_refusal(std::uint64_t run) const override;

    // The current run, if one is under way and has not ended. For tests and
    // diagnostics; a source keeps the run it began.
    [[nodiscard]] std::optional<std::uint64_t> current_run() const;

    [[nodiscard]] const MarketDataStreamId& stream() const noexcept { return stream_; }

private:
    [[nodiscard]] std::optional<IngestSequencesUnavailable> unavailable_locked(
        MonotonicClock::time_point now
    ) const;
    [[nodiscard]] std::optional<IngestSequencesUnavailable> run_refusal_locked(
        std::uint64_t run,
        MonotonicClock::time_point now
    ) const;
    [[nodiscard]] std::uint64_t reserved_remaining_locked() const noexcept;
    void add_block_locked(SequenceBlock block);
    MonotonicClock::time_point acquire_step();
    MonotonicClock::time_point renew_step();
    void keep() noexcept;
    void log(const std::string& line) const;

    const MarketDataStreamId stream_;
    IngestSequenceLeaseStore& store_;
    const StreamWriter writer_;
    const MonotonicClock& clock_;
    const LeasedIngestSequencesOptions options_;
    DiagnosticLog log_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_{false};
    // This process believes it holds the lease; publication also needs the
    // deadline not to have passed.
    bool owned_{false};
    MonotonicClock::time_point deadline_{};
    MonotonicClock::time_point renew_due_{};
    // The block being issued from: next_ to last_, while block_open_.
    // last_ stays meaningful once the block is used up, for contiguity.
    bool has_block_{false};
    bool block_open_{false};
    std::uint64_t next_{0};
    std::uint64_t last_{0};
    // A reserved block that does not continue the current one: a new run
    // begins with it once the current one is used up.
    std::optional<SequenceBlock> following_;
    // The run under way, until it ends: after an acquisition, a loss, a
    // lapse, or moving to a block that does not continue the last, a new
    // run must begin before the next issue.
    std::optional<std::uint64_t> run_;
    std::uint64_t runs_begun_{0};
    // No sequence remains to reserve.
    bool exhausted_{false};
    // Why the lease is not held, or the latest renewal failed; permanent
    // problems stop the keeper.
    std::optional<IngestSequencesUnavailable> trouble_;

    std::once_flag closed_once_;
    std::thread keeper_;
};

// This process as a writer: 128 random bits, and "pid N on HOST".
[[nodiscard]] StreamWriter this_process_stream_writer();

}  // namespace chronos::market_data
