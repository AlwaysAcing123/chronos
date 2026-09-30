#pragma once

#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_event_observer.hpp"
#include "chronos/market_state.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace chronos::market_data {

// Applies one source's events to the server-owned market state, counts
// them exactly, offers each to an optional observer, and tells subscribers.
// Every source uses it, so counting, rejection, observation, and
// notification behave the same for replay and live data.
class MarketStateFeed final {
public:
    // `observer` and `gate`, when given, must outlive the feed's use by
    // any source. With a gate, applying and observing each event holds it
    // exclusively.
    MarketStateFeed(
        MarketState& market_state,
        MarketDataNotifier& notifier,
        std::uint64_t events_already_applied = 0,
        MarketEventObserver* observer = nullptr,
        MarketEventGate* gate = nullptr
    );

    // Applies `event`, or explains why it was not applied: the market state
    // rejected it, or the event count cannot grow. A rejection interrupts
    // the market state permanently, as for any stream-contract violation.
    // An accepted event is then offered to the observer before subscribers
    // are told; if the observer fails, the event stays applied and counted,
    // the market state is suspended before the gate is released, so no
    // quote of an event that was not fully observed can be traded, and the
    // failure is reported like a rejection, so the source stops.
    [[nodiscard]] std::optional<std::string> apply(
        const ValidatedMarketEvent& event
    );

    [[nodiscard]] std::uint64_t events_applied() const noexcept {
        return events_applied_.load();
    }

    [[nodiscard]] MarketState& market_state() noexcept { return market_state_; }
    [[nodiscard]] MarketDataNotifier& notifier() noexcept { return notifier_; }

private:
    MarketState& market_state_;
    MarketDataNotifier& notifier_;
    MarketEventObserver* observer_;
    MarketEventGate* gate_;
    std::atomic<std::uint64_t> events_applied_;
};

}  // namespace chronos::market_data
