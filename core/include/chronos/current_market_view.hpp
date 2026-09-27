#pragma once

#include "chronos/market_data_validation.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace chronos {

enum class MarketViewUpdate {
    LastTrade,
    TopOfBook,
    TradingStatus,
    InstrumentDefinition,
};

struct InstrumentMarketView {
    std::optional<TradeEvent> last_trade;
    std::optional<TopOfBookEvent> top_of_book;
    std::optional<MarketStatusEvent> trading_status;
    std::optional<InstrumentDefinitionEvent> definition;

    friend bool operator==(
        const InstrumentMarketView&,
        const InstrumentMarketView&
    ) = default;
};

class CurrentMarketView final {
public:
    [[nodiscard]] MarketViewUpdate apply(
        const ValidatedMarketEvent& event
    );

    [[nodiscard]] const InstrumentMarketView* find(
        const InstrumentId& instrument
    ) const noexcept;

    [[nodiscard]] const std::map<std::string, InstrumentMarketView>&
    instruments() const noexcept {
        return instruments_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return instruments_.size();
    }

    [[nodiscard]] bool empty() const noexcept { return instruments_.empty(); }

    void reset() noexcept;

    friend bool operator==(
        const CurrentMarketView&,
        const CurrentMarketView&
    ) = default;

private:
    std::map<std::string, InstrumentMarketView> instruments_;
};

}  // namespace chronos
