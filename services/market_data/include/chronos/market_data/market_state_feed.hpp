#pragma once

#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_state.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace chronos::market_data {

// Applies one source's events to the server-owned market state, counts
// them exactly, and tells subscribers. Every source uses it, so counting,
// rejection, and notification behave the same for replay and live data.
class MarketStateFeed final {
public:
    MarketStateFeed(
        MarketState& market_state,
        MarketDataNotifier& notifier,
        std::uint64_t events_already_applied = 0
    );

    // Applies `event`, or explains why it was not applied: the market state
    // rejected it, or the event count cannot grow. A rejection interrupts
    // the market state permanently, as for any stream-contract violation.
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
    std::atomic<std::uint64_t> events_applied_;
};

}  // namespace chronos::market_data
