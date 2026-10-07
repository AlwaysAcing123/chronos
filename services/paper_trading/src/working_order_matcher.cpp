#include "chronos/paper_trading/working_order_matcher.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::paper_trading {
namespace {

// The live stream was taken over: this process no longer writes it
// (ADR 0020). No account is at fault and no retry can help, so observation
// ends, which suspends the market state and stops the source.
class LiveStreamWriterDeposed final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace

std::string_view state_name(WorkingOrderMatcherState state) {
    switch (state) {
        case WorkingOrderMatcherState::Running:
            return "running";
        case WorkingOrderMatcherState::Retrying:
            return "retrying";
        case WorkingOrderMatcherState::Stopped:
            return "stopped";
    }
    throw std::logic_error("unknown working-order matcher state");
}

WorkingOrderMatcher::WorkingOrderMatcher(
    std::unique_ptr<PostgresPaperOrderRepository> repository,
    WorkingOrderMatcherOptions options,
    ActivityCallback on_activity,
    DiagnosticLog log
)
    : repository_(std::move(repository)),
      options_(options),
      on_activity_(std::move(on_activity)),
      log_(std::move(log)),
      status_{
          .state = WorkingOrderMatcherState::Running,
          .observations = 0,
          .account_matches = 0,
          .fills = 0,
          .retries = 0,
          .failed_account_matches = 0,
          .total_matching_time = {},
          .longest_matching_time = {},
      } {
    if (repository_ == nullptr) {
        throw std::invalid_argument("working-order matcher requires a repository");
    }
    if (options_.first_retry_delay.count() <= 0 ||
        options_.maximum_retry_delay < options_.first_retry_delay) {
        throw std::invalid_argument("invalid working-order matcher retry delays");
    }
}

WorkingOrderMatcher::~WorkingOrderMatcher() { stop(); }

void WorkingOrderMatcher::stop() noexcept {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        status_.state = WorkingOrderMatcherState::Stopped;
    }
    stop_requested_.notify_all();
}

WorkingOrderMatcherStatus WorkingOrderMatcher::status() const {
    const std::lock_guard lock(mutex_);
    return status_;
}

void WorkingOrderMatcher::log(std::string_view line) const {
    if (log_) {
        log_(line);
    }
}

bool WorkingOrderMatcher::wait_before_retry(std::chrono::milliseconds delay) {
    std::unique_lock lock(mutex_);
    stop_requested_.wait_for(lock, delay, [this] { return stopping_; });
    return !stopping_;
}

std::optional<std::uint64_t> WorkingOrderMatcher::match_account(
    const std::string& account_id,
    const TopOfBookEvent& observation
) {
    std::optional<PaperAccountLock> locked =
        repository_->lock_account(account_id);
    if (!locked) {
        return std::nullopt;
    }
    // The market clock is the observation's own receive time: it was just
    // applied, and no later event is applied until matching returns.
    const WorkingOrderMatch match = locked->processor().match(
        observation,
        observation.header.receive_timestamp
    );
    if (!match.changed_account()) {
        locked->release();
        return std::nullopt;
    }
    const CommitPaperOrderResult committed = locked->commit_match(match);
    if (const auto* conflict = std::get_if<PaperPersistenceConflict>(&committed)) {
        if (conflict->code == PaperPersistenceConflictCode::StaleLiveStreamWriter) {
            throw LiveStreamWriterDeposed("working orders cannot be matched: " + conflict->message);
        }
        // Only a writer bypassing the account lock could cause this.
        throw std::runtime_error(
            "working-order match conflicted: " + conflict->message
        );
    }
    if (match.fault) {
        log(
            "paper account " + account_id + " halted: working order " +
            match.fault->order.intent.client_order_id +
            " faulted while matching: " + match.fault->accounting_error.message
        );
    }
    return match.fills.size();
}

void WorkingOrderMatcher::observe(const ValidatedMarketEvent& event) {
    const auto* observation = std::get_if<TopOfBookEvent>(&event.value());
    if (observation == nullptr) {
        // Trades and other events never fill working orders.
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) {
            return;
        }
        ++status_.observations;
    }
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t fills = 0;
    bool changed = false;
    // Accounts already matched against this observation. A retry after a
    // lost connection skips them; matching one again would book nothing,
    // but costs a transaction.
    std::set<std::string> matched;
    std::chrono::milliseconds delay = options_.first_retry_delay;
    while (true) {
        try {
            for (const std::string& account_id :
                 repository_->accounts_to_match(*observation)) {
                if (matched.contains(account_id)) {
                    continue;
                }
                try {
                    if (const auto booked =
                            match_account(account_id, *observation)) {
                        fills += *booked;
                        changed = true;
                    }
                    const std::lock_guard lock(mutex_);
                    ++status_.account_matches;
                } catch (const PaperPersistenceUnavailable&) {
                    throw;
                } catch (const LiveStreamWriterDeposed&) {
                    throw;
                } catch (const std::exception& error) {
                    // A stored state that cannot be recovered or written is a
                    // defect for that account alone. It stays as it is; the
                    // other accounts are still matched.
                    bool first_report = false;
                    {
                        const std::lock_guard lock(mutex_);
                        ++status_.failed_account_matches;
                        first_report = reported_accounts_.insert(account_id).second;
                    }
                    if (first_report) {
                        log(
                            "working orders of paper account " + account_id +
                            " could not be matched: " + error.what()
                        );
                    }
                }
                matched.insert(account_id);
            }
            break;
        } catch (const PaperPersistenceUnavailable& error) {
            {
                const std::lock_guard lock(mutex_);
                if (stopping_) {
                    break;
                }
                status_.state = WorkingOrderMatcherState::Retrying;
                ++status_.retries;
            }
            log(
                std::string("working-order matching is waiting for PostgreSQL: ") +
                error.what()
            );
            if (!wait_before_retry(delay)) {
                log(
                    "working-order matching stopped before observation " +
                    std::to_string(observation->header.ingest_sequence) +
                    " was matched"
                );
                break;
            }
            delay = std::min(delay * 2, options_.maximum_retry_delay);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    {
        const std::lock_guard lock(mutex_);
        if (!stopping_) {
            status_.state = WorkingOrderMatcherState::Running;
        }
        status_.fills += fills;
        status_.total_matching_time +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
        status_.longest_matching_time = std::max(
            status_.longest_matching_time,
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
        );
    }
    if (changed && on_activity_) {
        on_activity_();
    }
}

}  // namespace chronos::paper_trading
