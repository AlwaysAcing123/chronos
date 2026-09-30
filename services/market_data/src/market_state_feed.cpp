#include "chronos/market_data/market_state_feed.hpp"

#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace chronos::market_data {

MarketStateFeed::MarketStateFeed(
    MarketState& market_state,
    MarketDataNotifier& notifier,
    std::uint64_t events_already_applied,
    MarketEventObserver* observer,
    MarketEventGate* gate
)
    : market_state_(market_state),
      notifier_(notifier),
      observer_(observer),
      gate_(gate),
      events_applied_(events_already_applied) {}

std::optional<std::string> MarketStateFeed::apply(
    const ValidatedMarketEvent& event
) {
    // Only the source's own thread applies events, so the check and the
    // increment below cannot interleave with another apply.
    if (events_applied_.load() == std::numeric_limits<std::uint64_t>::max()) {
        return "the applied-event count cannot be represented";
    }
    std::optional<std::unique_lock<MarketEventGate::Mutex>> exclusive;
    if (gate_ != nullptr) {
        exclusive.emplace(gate_->apply_event());
    }
    const MarketStateUpdateResult update = market_state_.apply(event);
    if (const auto* rejected = std::get_if<MarketStateUpdateError>(&update)) {
        return "market state rejected an event: " + rejected->message;
    }
    events_applied_.fetch_add(1);
    std::optional<std::string> observer_failure;
    if (observer_ != nullptr) {
        try {
            observer_->observe(event);
        } catch (const std::exception& error) {
            observer_failure =
                std::string("market event observer failed: ") + error.what();
        } catch (...) {
            observer_failure = "market event observer failed";
        }
        if (observer_failure) {
            // The event is applied but was not fully observed, so its quotes
            // must not be traded against: a submission waiting for the gate
            // would otherwise take it before the source stops. Suspended
            // first, it finds no usable quote.
            market_state_.suspend();
        }
    }
    exclusive.reset();
    notifier_.publish();
    return observer_failure;
}

}  // namespace chronos::market_data
