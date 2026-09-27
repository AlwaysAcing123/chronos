#include "chronos/current_market_view.hpp"

#include <type_traits>
#include <variant>

namespace chronos {

MarketViewUpdate CurrentMarketView::apply(
    const ValidatedMarketEvent& event
) {
    const auto& header = market_event_header(event.value());
    auto& view = instruments_[header.instrument.value()];
    return std::visit(
        [&view](const auto& concrete_event) {
            using Event = std::decay_t<decltype(concrete_event)>;
            if constexpr (std::is_same_v<Event, TradeEvent>) {
                view.last_trade = concrete_event;
                return MarketViewUpdate::LastTrade;
            } else if constexpr (std::is_same_v<Event, TopOfBookEvent>) {
                view.top_of_book = concrete_event;
                return MarketViewUpdate::TopOfBook;
            } else if constexpr (std::is_same_v<Event, MarketStatusEvent>) {
                view.trading_status = concrete_event;
                return MarketViewUpdate::TradingStatus;
            } else {
                static_assert(
                    std::is_same_v<Event, InstrumentDefinitionEvent>
                );
                view.definition = concrete_event;
                return MarketViewUpdate::InstrumentDefinition;
            }
        },
        event.value()
    );
}

const InstrumentMarketView* CurrentMarketView::find(
    const InstrumentId& instrument
) const noexcept {
    const auto position = instruments_.find(instrument.value());
    return position == instruments_.end() ? nullptr : &position->second;
}

void CurrentMarketView::reset() noexcept { instruments_.clear(); }

}  // namespace chronos
