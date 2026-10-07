#include "chronos/market_data/leased_ingest_sequences.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace chronos::market_data {
namespace {

constexpr std::uint64_t kLastSequence = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kDiagnosticLimit = 300;

// Store errors can be long; a status line stays short.
std::string bounded(std::string text) {
    if (text.size() > kDiagnosticLimit) {
        text.resize(kDiagnosticLimit - 3);
        text += "...";
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

// The number of sequences in a block, saturating at the largest count.
std::uint64_t block_count(const SequenceBlock& block) noexcept {
    const std::uint64_t span = block.last - block.first;
    return span == kLastSequence ? kLastSequence : span + 1;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept {
    return right > kLastSequence - left ? kLastSequence : left + right;
}

}  // namespace

LeasedIngestSequences::LeasedIngestSequences(
    MarketDataStreamId stream,
    IngestSequenceLeaseStore& store,
    StreamWriter writer,
    const MonotonicClock& clock,
    LeasedIngestSequencesOptions options,
    DiagnosticLog log
)
    : stream_(std::move(stream)),
      store_(store),
      writer_(std::move(writer)),
      clock_(clock),
      options_(options),
      log_(std::move(log)) {
    if (writer_.id.empty()) {
        throw std::invalid_argument("a stream writer needs an ID");
    }
    if (options_.safety_margin.count() <= 0 || options_.lease <= options_.safety_margin) {
        throw std::invalid_argument("a writer lease must outlast its safety margin");
    }
    if (options_.renew_every.count() <= 0 || options_.renew_every >= options_.lease - options_.safety_margin) {
        throw std::invalid_argument("a writer lease must be renewed before its publication deadline");
    }
    if (options_.retry_every.count() <= 0) {
        throw std::invalid_argument("retries need a positive interval");
    }
    if (options_.block_size == 0) {
        throw std::invalid_argument("a sequence block cannot be empty");
    }
}

LeasedIngestSequences::~LeasedIngestSequences() { close(); }

void LeasedIngestSequences::start() {
    if (keeper_.joinable()) {
        throw std::logic_error("the sequence keeper is already running");
    }
    static_cast<void>(maintain());
    keeper_ = std::thread([this] { keep(); });
}

void LeasedIngestSequences::close() noexcept {
    std::call_once(closed_once_, [this] {
        {
            const std::lock_guard lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
        if (keeper_.joinable()) {
            keeper_.join();
        }
        bool owned = false;
        {
            const std::lock_guard lock(mutex_);
            owned = owned_;
            owned_ = false;
            run_.reset();
        }
        if (!owned) {
            return;
        }
        try {
            store_.release(stream_, writer_);
            log("live stream " + stream_.value() + ": writer lease released");
        } catch (const std::exception& error) {
            log(
                "live stream " + stream_.value() +
                ": the writer lease could not be released and will expire: " + bounded(error.what())
            );
        } catch (...) {
            log("live stream " + stream_.value() + ": the writer lease could not be released and will expire");
        }
    });
}

std::optional<IngestSequencesUnavailable> LeasedIngestSequences::unavailable_locked(
    MonotonicClock::time_point now
) const {
    if (closed_) {
        return IngestSequencesUnavailable{"live stream " + stream_.value() + " is closed", false};
    }
    if (trouble_ && trouble_->permanent) {
        return trouble_;
    }
    if (!owned_) {
        return trouble_ ? trouble_
                        : std::optional(IngestSequencesUnavailable{
                              "waiting for the writer lease of live stream " + stream_.value(),
                              false,
                          });
    }
    if (now >= deadline_) {
        return IngestSequencesUnavailable{
            "the writer lease of live stream " + stream_.value() + " lapsed: " +
                (trouble_ ? trouble_->diagnostic : std::string("its renewal is overdue")),
            false,
        };
    }
    return std::nullopt;
}

std::optional<IngestSequencesUnavailable> LeasedIngestSequences::run_refusal_locked(
    std::uint64_t run,
    MonotonicClock::time_point now
) const {
    if (auto unavailable = unavailable_locked(now)) {
        return unavailable;
    }
    if (run_ != run) {
        return IngestSequencesUnavailable{
            "live stream " + stream_.value() + ": publication run " + std::to_string(run) +
                " has ended; nothing it numbered can be published, and a new run must begin",
            false,
        };
    }
    return std::nullopt;
}

std::uint64_t LeasedIngestSequences::reserved_remaining_locked() const noexcept {
    const std::uint64_t current =
        block_open_ ? block_count(SequenceBlock{.first = next_, .last = last_}) : 0;
    return saturating_add(current, following_ ? block_count(*following_) : 0);
}

void LeasedIngestSequences::add_block_locked(SequenceBlock block) {
    // A block that reaches the last sequence leaves nothing to reserve, so
    // the end is known at once, not at the next renewal.
    if (block.last == kLastSequence) {
        exhausted_ = true;
    }
    if (!has_block_) {
        has_block_ = true;
        block_open_ = true;
        next_ = block.first;
        last_ = block.last;
        return;
    }
    if (following_) {
        if (following_->last != kLastSequence && block.first == following_->last + 1) {
            following_->last = block.last;
        } else {
            // An unanswered reservation committed in between: the older
            // block is a gap, and the newest is the one later blocks continue.
            following_ = block;
        }
        return;
    }
    if (last_ != kLastSequence && block.first == last_ + 1) {
        if (!block_open_) {
            block_open_ = true;
            next_ = block.first;
        }
        last_ = block.last;
        return;
    }
    following_ = block;
}

std::optional<IngestSequencesUnavailable> LeasedIngestSequences::readiness() const {
    const std::lock_guard lock(mutex_);
    if (auto unavailable = unavailable_locked(clock_.now())) {
        return unavailable;
    }
    if (!block_open_ && !following_) {
        if (exhausted_) {
            return IngestSequencesUnavailable{
                "live stream " + stream_.value() + " has issued every ingest sequence; use a new stream ID",
                true,
            };
        }
        return IngestSequencesUnavailable{
            "waiting for live stream " + stream_.value() + " to reserve ingest sequences",
            false,
        };
    }
    return std::nullopt;
}

BegunRun LeasedIngestSequences::begin_run() {
    if (auto unavailable = readiness()) {
        return std::move(*unavailable);
    }
    const std::lock_guard lock(mutex_);
    // Re-checked under one lock with the change below: the keeper may have
    // lost the lease between the two.
    if (auto unavailable = unavailable_locked(clock_.now())) {
        return std::move(*unavailable);
    }
    // Whatever happens below, the run before this one is over.
    run_.reset();
    if (runs_begun_ == kLastSequence) {
        return IngestSequencesUnavailable{
            "live stream " + stream_.value() + " cannot begin another publication run in this process",
            true,
        };
    }
    if (!block_open_) {
        if (!following_) {
            return IngestSequencesUnavailable{
                "waiting for live stream " + stream_.value() + " to reserve ingest sequences",
                false,
            };
        }
        block_open_ = true;
        next_ = following_->first;
        last_ = following_->last;
        following_.reset();
    }
    ++runs_begun_;
    run_ = runs_begun_;
    return PublicationRun{.id = runs_begun_, .first = next_};
}

IssuedSequence LeasedIngestSequences::issue(std::uint64_t run, std::uint64_t count) {
    if (count == 0) {
        throw std::invalid_argument("issue at least one ingest sequence");
    }
    const std::lock_guard lock(mutex_);
    if (auto unavailable = unavailable_locked(clock_.now())) {
        return std::move(*unavailable);
    }
    if (run_ != run) {
        return IngestSequencesUnavailable{
            "live stream " + stream_.value() + " needs a new publication run",
            false,
        };
    }
    const bool fits = block_open_ && count - 1 <= last_ - next_;
    if (!fits) {
        if (following_) {
            // The rest of the current block, if any, becomes a gap.
            block_open_ = false;
            run_.reset();
            return IngestSequencesUnavailable{
                "live stream " + stream_.value() +
                    " continues in a later reserved block; a new publication run must begin",
                false,
            };
        }
        if (exhausted_) {
            return IngestSequencesUnavailable{
                "live stream " + stream_.value() + " cannot number " + std::to_string(count) +
                    " more events: its ingest sequences are exhausted; use a new stream ID",
                true,
            };
        }
        return IngestSequencesUnavailable{
            "live stream " + stream_.value() + " is waiting to reserve ingest sequences",
            false,
        };
    }
    const std::uint64_t first = next_;
    if (count - 1 == last_ - next_) {
        block_open_ = false;
    } else {
        next_ += count;
    }
    return first;
}

std::optional<IngestSequencesUnavailable> LeasedIngestSequences::publication_refusal(std::uint64_t run) const {
    const std::lock_guard lock(mutex_);
    return run_refusal_locked(run, clock_.now());
}

std::optional<std::uint64_t> LeasedIngestSequences::current_run() const {
    const std::lock_guard lock(mutex_);
    if (unavailable_locked(clock_.now())) {
        return std::nullopt;
    }
    return run_;
}

MonotonicClock::time_point LeasedIngestSequences::maintain() {
    bool acquire = false;
    {
        const std::lock_guard lock(mutex_);
        const auto now = clock_.now();
        if (closed_ || (trouble_ && trouble_->permanent)) {
            return now + std::chrono::hours(1);
        }
        if (!owned_) {
            acquire = true;
        } else {
            const bool needs_block = !exhausted_ &&
                reserved_remaining_locked() < std::max<std::uint64_t>(1, options_.block_size / 2);
            if (now < renew_due_ && !needs_block) {
                return renew_due_;
            }
        }
    }
    return acquire ? acquire_step() : renew_step();
}

MonotonicClock::time_point LeasedIngestSequences::acquire_step() {
    const auto started = clock_.now();
    LeaseAcquisition result;
    try {
        result = store_.acquire(stream_, writer_, options_.lease, options_.block_size, options_.adopt_history);
    } catch (const std::exception& error) {
        std::string diagnostic = bounded(
            "live stream " + stream_.value() + ": could not take its writer lease: " + error.what()
        );
        {
            const std::lock_guard lock(mutex_);
            trouble_ = IngestSequencesUnavailable{diagnostic, false};
        }
        log(diagnostic);
        return started + options_.retry_every;
    }
    if (const auto* granted = std::get_if<LeaseGranted>(&result)) {
        const auto due = started + options_.renew_every;
        {
            const std::lock_guard lock(mutex_);
            owned_ = true;
            deadline_ = started + options_.lease - options_.safety_margin;
            renew_due_ = due;
            has_block_ = false;
            block_open_ = false;
            following_.reset();
            exhausted_ = false;
            add_block_locked(granted->block);
            run_.reset();
            trouble_.reset();
        }
        std::string line = "live stream " + stream_.value() + ": writer lease taken; ingest sequences continue from " +
            std::to_string(granted->block.first);
        if (granted->adopted_maximum) {
            line += ", after durable history from before durable sequencing up to " +
                std::to_string(*granted->adopted_maximum);
        }
        log(line);
        return due;
    }
    if (const auto* held = std::get_if<LeaseHeld>(&result)) {
        const std::string diagnostic = bounded(
            "live stream " + stream_.value() + " is written by " + held->writer_label + " until " +
            held->expires_at + "; waiting for that lease to end"
        );
        bool changed = false;
        {
            const std::lock_guard lock(mutex_);
            changed = !trouble_ || trouble_->diagnostic != diagnostic;
            trouble_ = IngestSequencesUnavailable{diagnostic, false};
        }
        if (changed) {
            log(diagnostic);
        }
        return started + options_.retry_every;
    }
    const std::string diagnostic = bounded(std::get<LeaseRefused>(result).reason);
    {
        const std::lock_guard lock(mutex_);
        trouble_ = IngestSequencesUnavailable{diagnostic, true};
    }
    log(diagnostic);
    return started + std::chrono::hours(1);
}

MonotonicClock::time_point LeasedIngestSequences::renew_step() {
    std::uint64_t block_size = 0;
    {
        const std::lock_guard lock(mutex_);
        if (!exhausted_ &&
            reserved_remaining_locked() < std::max<std::uint64_t>(1, options_.block_size / 2)) {
            block_size = options_.block_size;
        }
    }
    const auto started = clock_.now();
    LeaseRenewal result;
    try {
        result = store_.renew(stream_, writer_, options_.lease, block_size);
    } catch (const std::exception& error) {
        // The deadline stays where the last successful renewal put it.
        std::string diagnostic = bounded(
            "live stream " + stream_.value() + ": could not renew its writer lease: " + error.what()
        );
        {
            const std::lock_guard lock(mutex_);
            trouble_ = IngestSequencesUnavailable{diagnostic, false};
        }
        log(diagnostic);
        return started + options_.retry_every;
    }
    if (const auto* renewed = std::get_if<LeaseRenewed>(&result)) {
        const std::lock_guard lock(mutex_);
        if (clock_.now() >= deadline_) {
            // Ownership lapsed before this renewal was confirmed, so for a
            // while nothing could publish: the run that was under way is
            // over, and whatever it published is not current any more.
            run_.reset();
        }
        deadline_ = started + options_.lease - options_.safety_margin;
        renew_due_ = started + options_.renew_every;
        trouble_.reset();
        if (renewed->block) {
            add_block_locked(*renewed->block);
        }
        if (renewed->exhausted) {
            exhausted_ = true;
        }
        return renew_due_;
    }
    const std::string diagnostic = bounded(
        "live stream " + stream_.value() + ": writer lease lost: " + std::get<LeaseLost>(result).reason
    );
    {
        // Its blocks can never be issued again: a later writer issues after
        // them.
        const std::lock_guard lock(mutex_);
        owned_ = false;
        has_block_ = false;
        block_open_ = false;
        following_.reset();
        run_.reset();
        trouble_ = IngestSequencesUnavailable{diagnostic, false};
    }
    log(diagnostic);
    return started + options_.retry_every;
}

void LeasedIngestSequences::keep() noexcept {
    std::unique_lock lock(mutex_);
    while (!closed_) {
        lock.unlock();
        MonotonicClock::time_point due;
        try {
            due = maintain();
        } catch (...) {
            due = clock_.now() + options_.retry_every;
        }
        lock.lock();
        if (closed_) {
            break;
        }
        clock_.wait_until(lock, changed_, due, [this] { return closed_; });
    }
}

void LeasedIngestSequences::log(const std::string& line) const {
    if (!log_) {
        return;
    }
    try {
        log_(line);
    } catch (...) {
        // A failing log must not stop the keeper.
    }
}

StreamWriter this_process_stream_writer() {
    std::random_device device;
    std::string id;
    for (int word = 0; word < 4; ++word) {
        std::array<char, 9> hex{};
        std::snprintf(hex.data(), hex.size(), "%08x", static_cast<unsigned>(device()));
        id += hex.data();
    }
    std::array<char, 256> host{};
    std::string label = "pid " + std::to_string(::getpid());
    if (::gethostname(host.data(), host.size() - 1) == 0 && host.front() != '\0') {
        label += " on " + std::string(host.data());
    }
    return StreamWriter{.id = std::move(id), .label = bounded(std::move(label))};
}

}  // namespace chronos::market_data
