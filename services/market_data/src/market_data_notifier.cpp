#include "chronos/market_data/market_data_notifier.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>

namespace chronos::market_data {

MarketDataNotifier::MarketDataNotifier(
    const MonotonicClock& clock,
    Options options
)
    : clock_(clock),
      options_(options) {
    if (options_.maximum_subscribers == 0 ||
        options_.minimum_interval.count() < 0 ||
        options_.heartbeat_interval.count() <= 0) {
        throw std::invalid_argument("invalid market-data notifier options");
    }
}

void MarketDataNotifier::publish() {
    {
        const std::lock_guard lock(mutex_);
        // At one change per nanosecond this would take centuries; staying
        // at the maximum still reports "changed" to every subscriber.
        if (revision_ != std::numeric_limits<std::uint64_t>::max()) {
            ++revision_;
        }
    }
    changed_.notify_all();
}

void MarketDataNotifier::publish_activity() {
    {
        const std::lock_guard lock(mutex_);
        if (revision_ != std::numeric_limits<std::uint64_t>::max()) {
            ++revision_;
        }
        if (activity_revision_ != std::numeric_limits<std::uint64_t>::max()) {
            ++activity_revision_;
        }
    }
    changed_.notify_all();
}

std::uint64_t MarketDataNotifier::revision() const {
    const std::lock_guard lock(mutex_);
    return revision_;
}

std::uint64_t MarketDataNotifier::activity_revision() const {
    const std::lock_guard lock(mutex_);
    return activity_revision_;
}

std::variant<MarketDataSubscription, MarketDataNotifier::SubscribeError>
MarketDataNotifier::subscribe() {
    const std::lock_guard lock(mutex_);
    if (closed_) {
        return SubscribeError::Closed;
    }
    if (subscribers_ == options_.maximum_subscribers) {
        return SubscribeError::TooManySubscribers;
    }
    ++subscribers_;
    return MarketDataSubscription(*this);
}

void MarketDataNotifier::close() {
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
    }
    changed_.notify_all();
}

void MarketDataNotifier::unsubscribe() noexcept {
    const std::lock_guard lock(mutex_);
    --subscribers_;
}

MarketDataNotice MarketDataNotifier::next(MarketDataSubscription& subscriber) {
    std::unique_lock lock(mutex_);
    while (true) {
        if (closed_) {
            return MarketDataNotifierClosed{};
        }
        const MonotonicClock::time_point now = clock_.now();
        const bool changed = subscriber.delivered_revision_ != revision_;
        const bool throttled = now < subscriber.next_change_allowed_;
        if (changed && !throttled) {
            subscriber.delivered_revision_ = revision_;
            subscriber.next_change_allowed_ = now + options_.minimum_interval;
            subscriber.next_heartbeat_ = now + options_.heartbeat_interval;
            return MarketDataChanged{
                .revision = revision_,
                .activity_revision = activity_revision_,
            };
        }
        if (now >= subscriber.next_heartbeat_) {
            subscriber.next_heartbeat_ = now + options_.heartbeat_interval;
            return MarketDataHeartbeat{};
        }
        // While throttled, later changes merge into the pending one, so
        // only closing wakes the wait early.
        const MonotonicClock::time_point deadline = changed
            ? std::min(subscriber.next_change_allowed_, subscriber.next_heartbeat_)
            : subscriber.next_heartbeat_;
        const std::uint64_t seen = revision_;
        clock_.wait_until(lock, changed_, deadline, [&] {
            return closed_ || (!changed && revision_ != seen);
        });
    }
}

MarketDataSubscription::MarketDataSubscription(MarketDataNotifier& notifier)
    : notifier_(&notifier),
      next_change_allowed_(),
      next_heartbeat_(notifier.clock_.now() + notifier.options_.heartbeat_interval) {}

MarketDataSubscription::MarketDataSubscription(
    MarketDataSubscription&& other
) noexcept
    : notifier_(std::exchange(other.notifier_, nullptr)),
      delivered_revision_(other.delivered_revision_),
      next_change_allowed_(other.next_change_allowed_),
      next_heartbeat_(other.next_heartbeat_) {}

MarketDataSubscription::~MarketDataSubscription() {
    if (notifier_ != nullptr) {
        notifier_->unsubscribe();
    }
}

MarketDataNotice MarketDataSubscription::next() {
    return notifier_->next(*this);
}

}  // namespace chronos::market_data
