#pragma once

#include "chronos/market_data_types.hpp"

#include <string>
#include <variant>
#include <vector>

namespace chronos {

enum class MarketEventValidationErrorCode {
    NegativeEventTimestamp,
    NegativeReceiveTimestamp,
    UnexplainedTimestampInversion,
    UnknownQualityFlags,
    NonpositivePrice,
    NonpositiveQuantity,
    UnknownAggressorSide,
    UnknownTradingStatus,
    MissingDisplaySymbol,
    NonpositiveMinimumPriceIncrement,
    NonpositiveMinimumQuantityIncrement,
    NegativeEffectiveTimestamp,
    InvalidEffectiveRange,
    UnflaggedCrossedBook,
};

struct MarketEventValidationError {
    MarketEventValidationErrorCode code;
    std::string field;
    std::string message;

    friend bool operator==(
        const MarketEventValidationError&,
        const MarketEventValidationError&
    ) = default;
};

struct MarketEventValidationFailure {
    std::vector<MarketEventValidationError> errors;

    friend bool operator==(
        const MarketEventValidationFailure&,
        const MarketEventValidationFailure&
    ) = default;
};

namespace detail {
class ValidatedMarketEventFactory;
}

class ValidatedMarketEvent final {
public:
    [[nodiscard]] const MarketEvent& value() const noexcept { return event_; }

    friend bool operator==(
        const ValidatedMarketEvent&,
        const ValidatedMarketEvent&
    ) = default;

private:
    explicit ValidatedMarketEvent(MarketEvent event);

    MarketEvent event_;

    friend class detail::ValidatedMarketEventFactory;
};

using MarketEventValidationResult = std::variant<
    ValidatedMarketEvent,
    MarketEventValidationFailure
>;

[[nodiscard]] MarketEventValidationResult validate_market_event(
    MarketEvent event
);

}  // namespace chronos
