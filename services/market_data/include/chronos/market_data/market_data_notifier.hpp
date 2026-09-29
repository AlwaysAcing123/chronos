#pragma once

#include "chronos/market_data/monotonic_clock.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <variant>

namespace chronos::market_data {

struct MarketDataChanged {
    // The latest revision; every earlier change is included in it.
    std::uint64_t revision;
};

struct MarketDataHeartbeat {};

struct MarketDataNotifierClosed {};

using MarketDataNotice = std::variant<
    MarketDataChanged,
    MarketDataHeartbeat,
    MarketDataNotifierClosed
>;

class MarketDataSubscription;

// Tells browser views that market or source state changed, without
// carrying market data and without ever slowing the source.
//
// A source calls publish() after every applied event and state change; it
// only increments a revision. Each subscriber remembers the last revision it
// delivered, so there is no queue: changes that arrive while a subscriber
// is throttled merge into one notice. Every event still reaches the market
// state; only these notices are coalesced.
class MarketDataNotifier final {
public:
    struct Options {
        std::size_t maximum_subscribers{8};
        // At most one change notice per interval per subscriber.
        MonotonicClock::duration minimum_interval{std::chrono::milliseconds(250)};
        // A heartbeat keeps idle connections open and detects gone clients.
        MonotonicClock::duration heartbeat_interval{std::chrono::seconds(10)};
    };

    MarketDataNotifier(const MonotonicClock& clock, Options options);

    MarketDataNotifier(const MarketDataNotifier&) = delete;
    MarketDataNotifier& operator=(const MarketDataNotifier&) = delete;

    void publish();

    [[nodiscard]] std::uint64_t revision() const;

    [[nodiscard]] std::size_t maximum_subscribers() const noexcept {
        return options_.maximum_subscribers;
    }

    enum class SubscribeError {
        TooManySubscribers,
        Closed,
    };

    [[nodiscard]] std::variant<MarketDataSubscription, SubscribeError>
    subscribe();

    // Ends every subscription and refuses new ones, so connections waiting
    // for a notice return promptly at shutdown.
    void close();

private:
    friend class MarketDataSubscription;

    [[nodiscard]] MarketDataNotice next(MarketDataSubscription& subscriber);
    void unsubscribe() noexcept;

    const MonotonicClock& clock_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t revision_{0};
    std::size_t subscribers_{0};
    bool closed_{false};
};

// One subscriber's view of the notifier. Moving it transfers the slot;
// destroying it releases the slot. The notifier must outlive it.
class MarketDataSubscription final {
public:
    MarketDataSubscription(MarketDataSubscription&& other) noexcept;
    MarketDataSubscription& operator=(MarketDataSubscription&&) = delete;
    MarketDataSubscription(const MarketDataSubscription&) = delete;
    MarketDataSubscription& operator=(const MarketDataSubscription&) = delete;
    ~MarketDataSubscription();

    // Blocks until a change may be delivered, a heartbeat is due, or the
    // notifier closes. The first call reports the current revision at once,
    // so a reconnecting view always refreshes.
    [[nodiscard]] MarketDataNotice next();

private:
    friend class MarketDataNotifier;

    explicit MarketDataSubscription(MarketDataNotifier& notifier);

    MarketDataNotifier* notifier_;
    std::optional<std::uint64_t> delivered_revision_;
    MonotonicClock::time_point next_change_allowed_;
    MonotonicClock::time_point next_heartbeat_;
};

}  // namespace chronos::market_data
