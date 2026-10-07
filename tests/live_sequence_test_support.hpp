#pragma once

// Live-stream sequencing for tests without PostgreSQL (ADR 0020): an
// in-memory live_stream_sequences with a clock of its own and injected
// faults, and ready-made sequences for tests that only need a live source.

#include "chronos/market_data/leased_ingest_sequences.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace chronos::test {

// The store's semantics, as the PostgreSQL store implements them, with the
// "database" clock and failures under the test's control.
class MemoryLeaseStore final : public market_data::IngestSequenceLeaseStore {
public:
    enum class Fault {
        // Throws before changing anything.
        Unavailable,
        // Changes the row, then throws: a commit whose answer was lost.
        CommitThenUnavailable,
    };

    struct Row {
        // The first sequence never reserved; nullopt once every sequence
        // has been reserved (2^64).
        std::optional<std::uint64_t> next{0};
        std::optional<std::string> writer{};
        std::string label{};
        market_data::MonotonicClock::time_point expires{};
        std::optional<std::uint64_t> adopted_maximum{};
    };

    explicit MemoryLeaseStore(const market_data::MonotonicClock& database_clock)
        : clock_(database_clock) {}

    // A durable reference to `sequence` of `stream`, as an account, order,
    // fill, working order, or deployment would hold.
    void reference(const std::string& stream, std::uint64_t sequence) {
        const std::lock_guard lock(mutex_);
        auto& maximum = references_[stream];
        maximum = maximum ? std::max(*maximum, sequence) : sequence;
    }

    void set_row(const std::string& stream, Row row) {
        const std::lock_guard lock(mutex_);
        rows_.insert_or_assign(stream, std::move(row));
    }

    [[nodiscard]] std::optional<Row> row(const std::string& stream) const {
        const std::lock_guard lock(mutex_);
        const auto found = rows_.find(stream);
        return found == rows_.end() ? std::nullopt : std::optional<Row>(found->second);
    }

    void fail_next(Fault fault, std::size_t count = 1) {
        const std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < count; ++index) {
            faults_.push_back(fault);
        }
    }

    // The next call waits, before touching anything, until unblock().
    void block_next() {
        const std::lock_guard lock(mutex_);
        block_next_ = true;
    }

    // Runs `action` inside the next call, before it changes anything, as a
    // transaction that takes time would: a test moves the clocks in it.
    void during_next(std::function<void()> action) {
        const std::lock_guard lock(mutex_);
        during_next_ = std::move(action);
    }

    [[nodiscard]] bool await_blocked() const {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(10), [&] { return blocked_; });
    }

    void unblock() {
        {
            const std::lock_guard lock(mutex_);
            block_next_ = false;
            blocked_ = false;
        }
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t calls() const {
        const std::lock_guard lock(mutex_);
        return calls_;
    }

    market_data::LeaseAcquisition acquire(
        const MarketDataStreamId& stream,
        const market_data::StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size,
        bool adopt_history
    ) override {
        std::unique_lock lock(mutex_);
        const auto fault = enter(lock);
        auto found = rows_.find(stream.value());
        std::optional<std::uint64_t> adopted;
        const auto references = references_.find(stream.value());
        const auto referenced = references == references_.end() ? std::nullopt : references->second;
        if (found == rows_.end()) {
            if (referenced && !adopt_history) {
                return market_data::LeaseRefused{
                    "live stream " + stream.value() + " has durable history up to sequence " +
                    std::to_string(*referenced) + " from before durable sequencing",
                };
            }
            Row created;
            if (referenced) {
                created.next = *referenced == kLast ? std::nullopt : std::optional(*referenced + 1);
                created.adopted_maximum = referenced;
                adopted = referenced;
            }
            found = rows_.emplace(stream.value(), std::move(created)).first;
        }
        Row& row = found->second;
        if (row.writer && *row.writer != writer.id && row.expires > clock_.now()) {
            return market_data::LeaseHeld{.writer_label = row.label, .expires_at = "expiry"};
        }
        if (referenced && row.next && *referenced >= *row.next) {
            return market_data::LeaseRefused{"durable history reaches beyond the stream's reservations"};
        }
        if (!row.next) {
            return market_data::LeaseRefused{"live stream " + stream.value() + " is exhausted"};
        }
        const market_data::SequenceBlock block = reserve(row, block_size);
        row.writer = writer.id;
        row.label = writer.label;
        row.expires = clock_.now() + lease;
        leave(fault);
        return market_data::LeaseGranted{.block = block, .adopted_maximum = adopted};
    }

    market_data::LeaseRenewal renew(
        const MarketDataStreamId& stream,
        const market_data::StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size
    ) override {
        std::unique_lock lock(mutex_);
        const auto fault = enter(lock);
        const auto found = rows_.find(stream.value());
        if (found == rows_.end() || found->second.writer != writer.id) {
            return market_data::LeaseLost{"the lease is no longer this writer's"};
        }
        Row& row = found->second;
        row.expires = clock_.now() + lease;
        market_data::LeaseRenewed renewed;
        if (block_size != 0) {
            if (row.next) {
                renewed.block = reserve(row, block_size);
            } else {
                renewed.exhausted = true;
            }
        }
        leave(fault);
        return renewed;
    }

    void release(const MarketDataStreamId& stream, const market_data::StreamWriter& writer) override {
        std::unique_lock lock(mutex_);
        const auto fault = enter(lock);
        const auto found = rows_.find(stream.value());
        if (found != rows_.end() && found->second.writer == writer.id) {
            found->second.writer.reset();
            found->second.label.clear();
            found->second.expires = {};
        }
        leave(fault);
    }

private:
    static constexpr std::uint64_t kLast = std::numeric_limits<std::uint64_t>::max();

    static market_data::SequenceBlock reserve(Row& row, std::uint64_t block_size) {
        const std::uint64_t first = *row.next;
        const std::uint64_t last = block_size - 1 > kLast - first ? kLast : first + (block_size - 1);
        row.next = last == kLast ? std::nullopt : std::optional(last + 1);
        return market_data::SequenceBlock{.first = first, .last = last};
    }

    std::optional<Fault> enter(std::unique_lock<std::mutex>& lock) {
        ++calls_;
        if (block_next_) {
            blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [&] { return !blocked_; });
        }
        if (during_next_) {
            const std::function<void()> action = std::move(during_next_);
            during_next_ = nullptr;
            lock.unlock();
            action();
            lock.lock();
        }
        if (faults_.empty()) {
            return std::nullopt;
        }
        const Fault fault = faults_.front();
        faults_.pop_front();
        if (fault == Fault::Unavailable) {
            throw std::runtime_error("database unavailable (injected)");
        }
        return fault;
    }

    static void leave(std::optional<Fault> fault) {
        if (fault == Fault::CommitThenUnavailable) {
            throw std::runtime_error("the connection closed during COMMIT (injected)");
        }
    }

    const market_data::MonotonicClock& clock_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::map<std::string, Row, std::less<>> rows_;
    std::map<std::string, std::optional<std::uint64_t>, std::less<>> references_;
    std::deque<Fault> faults_;
    std::function<void()> during_next_;
    bool block_next_{false};
    bool blocked_{false};
    std::size_t calls_{0};
};

// Sequences for a live source under test that only needs them to work: a
// held lease in a store of its own, starting at `first`, whose clocks never
// move, so the lease never lapses.
class ReadyIngestSequences final {
public:
    explicit ReadyIngestSequences(
        std::string stream,
        std::uint64_t first = 0,
        std::uint64_t block_size = std::uint64_t{1} << 20U
    )
        : store_(database_clock_),
          sequences_(
              MarketDataStreamId(stream),
              store_,
              market_data::StreamWriter{.id = "test-writer", .label = "this test"},
              clock_,
              market_data::LeasedIngestSequencesOptions{.block_size = block_size}
          ) {
        store_.set_row(stream, MemoryLeaseStore::Row{.next = first});
        sequences_.start();
        REQUIRE(!sequences_.readiness().has_value());
    }

    [[nodiscard]] market_data::LeasedIngestSequences& sequences() { return sequences_; }
    [[nodiscard]] MemoryLeaseStore& store() { return store_; }

private:
    ManualClock clock_{ManualClock::Mode::Hold};
    ManualClock database_clock_{ManualClock::Mode::Hold};
    MemoryLeaseStore store_;
    market_data::LeasedIngestSequences sequences_;
};

}  // namespace chronos::test
