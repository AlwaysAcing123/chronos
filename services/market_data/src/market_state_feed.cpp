#include "chronos/market_data/market_state_feed.hpp"

#include <limits>
#include <optional>
#include <string>
#include <variant>

namespace chronos::market_data {

MarketStateFeed::MarketStateFeed(
    MarketState& market_state,
    MarketDataNotifier& notifier,
    std::uint64_t events_already_applied
)
    : market_state_(market_state),
      notifier_(notifier),
      events_applied_(events_already_applied) {}

std::optional<std::string> MarketStateFeed::apply(
    const ValidatedMarketEvent& event
) {
    // Only the source's own thread applies events, so the check and the
    // increment below cannot interleave with another apply.
    if (events_applied_.load() == std::numeric_limits<std::uint64_t>::max()) {
        return "the applied-event count cannot be represented";
    }
    const MarketStateUpdateResult update = market_state_.apply(event);
    if (const auto* rejected = std::get_if<MarketStateUpdateError>(&update)) {
        return "market state rejected an event: " + rejected->message;
    }
    events_applied_.fetch_add(1);
    notifier_.publish();
    return std::nullopt;
}

}  // namespace chronos::market_data
