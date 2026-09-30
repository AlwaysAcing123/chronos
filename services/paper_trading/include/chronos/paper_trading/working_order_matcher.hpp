#pragma once

#include "chronos/market_data/market_event_observer.hpp"
#include "chronos/postgresql_paper_order_repository.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace chronos::paper_trading {

struct WorkingOrderMatcherOptions {
    // Waits between attempts while PostgreSQL is unavailable, doubling from
    // the first to the maximum.
    std::chrono::milliseconds first_retry_delay{50};
    std::chrono::milliseconds maximum_retry_delay{2'000};
};

enum class WorkingOrderMatcherState {
    // Offering each observation to the accounts it can fill.
    Running,
    // PostgreSQL is unavailable; the same observation is retried and the
    // market-data source waits for it.
    Retrying,
    // Stopped for shutdown; later observations are not matched.
    Stopped,
};

[[nodiscard]] std::string_view state_name(WorkingOrderMatcherState state);

struct WorkingOrderMatcherStatus {
    WorkingOrderMatcherState state;
    // Top-of-book observations offered, accounts matched against one, and
    // fills booked.
    std::uint64_t observations;
    std::uint64_t account_matches;
    std::uint64_t fills;
    // Attempts repeated because PostgreSQL was unavailable.
    std::uint64_t retries;
    // Account matches abandoned because the account's stored state is
    // inconsistent; each is logged.
    std::uint64_t failed_account_matches;
    // Time spent matching one observation, including retries.
    std::chrono::nanoseconds total_matching_time;
    std::chrono::nanoseconds longest_matching_time;
};

// Fills durable working orders from the market-data stream.
//
// The matcher observes every event the market state accepts, on the
// source's thread, before the next event is applied (see
// MarketEventObserver). For a top of book it asks PostgreSQL which healthy
// accounts on the stream have a working order the observation could fill,
// then matches each in its own transaction under that account's row lock:
// the account is recovered, PaperOrderProcessor::match offers it the
// observation, and what changed commits. The account's execution state
// records the observation, so a retried or repeated match of it books
// nothing twice, and submissions and cancellations of the account, which
// take the same lock, are serialized with it.
//
// It owns its repository and so its own PostgreSQL connection; the service
// keeps a separate one, so API reads and writes never wait for matching
// except on the one account being matched.
//
// When PostgreSQL is unavailable the matcher retries the same observation
// until it succeeds or stop() is called. The source is held meanwhile, so
// no later observation overtakes it. An account whose stored state cannot
// be recovered is logged and skipped for that observation; the others are
// still matched.
class WorkingOrderMatcher final : public market_data::MarketEventObserver {
public:
    using DiagnosticLog = std::function<void(std::string_view line)>;
    using ActivityCallback = std::function<void()>;

    // `on_activity` runs on the source thread after an observation changed
    // at least one account: booked a fill, or faulted an order and halted
    // its account.
    WorkingOrderMatcher(
        std::unique_ptr<PostgresPaperOrderRepository> repository,
        WorkingOrderMatcherOptions options = {},
        ActivityCallback on_activity = {},
        DiagnosticLog log = {}
    );

    WorkingOrderMatcher(const WorkingOrderMatcher&) = delete;
    WorkingOrderMatcher& operator=(const WorkingOrderMatcher&) = delete;
    WorkingOrderMatcher(WorkingOrderMatcher&&) = delete;
    WorkingOrderMatcher& operator=(WorkingOrderMatcher&&) = delete;
    ~WorkingOrderMatcher() override;

    // The source's thread only.
    void observe(const ValidatedMarketEvent& event) override;

    // Ends any retry promptly and ignores later observations. Safe from any
    // thread, and more than once.
    void stop() noexcept;

    [[nodiscard]] WorkingOrderMatcherStatus status() const;

private:
    // Matches one account; returns the fills it booked, or nothing when the
    // account did not change.
    [[nodiscard]] std::optional<std::uint64_t> match_account(
        const std::string& account_id,
        const TopOfBookEvent& observation
    );
    [[nodiscard]] bool wait_before_retry(std::chrono::milliseconds delay);
    void log(std::string_view line) const;

    std::unique_ptr<PostgresPaperOrderRepository> repository_;
    const WorkingOrderMatcherOptions options_;
    ActivityCallback on_activity_;
    DiagnosticLog log_;

    mutable std::mutex mutex_;
    std::condition_variable stop_requested_;
    bool stopping_{false};
    WorkingOrderMatcherStatus status_;
    // Accounts already reported as unrecoverable, so each is logged once.
    std::set<std::string> reported_accounts_;
};

}  // namespace chronos::paper_trading
