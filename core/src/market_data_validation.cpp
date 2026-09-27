#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos {
namespace {

constexpr std::uint32_t quality_flag_bits(MarketDataQualityFlag flag) {
    return static_cast<std::uint32_t>(flag);
}

constexpr std::uint32_t kKnownQualityFlags =
    quality_flag_bits(MarketDataQualityFlag::Snapshot) |
    quality_flag_bits(MarketDataQualityFlag::Replay) |
    quality_flag_bits(MarketDataQualityFlag::QuestionableReceiveTimestamp) |
    quality_flag_bits(MarketDataQualityFlag::PotentiallyInvalidBook) |
    quality_flag_bits(MarketDataQualityFlag::SourceSpecific);

using ValidationErrors = std::vector<MarketEventValidationError>;

void add_error(
    ValidationErrors& errors,
    MarketEventValidationErrorCode code,
    std::string_view field,
    std::string_view message
) {
    errors.push_back(MarketEventValidationError{
        .code = code,
        .field = std::string(field),
        .message = std::string(message),
    });
}

void validate_header(
    const MarketEventHeader& header,
    ValidationErrors& errors
) {
    const std::int64_t event_timestamp = header.event_timestamp.nanoseconds();
    const std::int64_t receive_timestamp =
        header.receive_timestamp.nanoseconds();

    if (event_timestamp < 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NegativeEventTimestamp,
            "header.event_timestamp",
            "event timestamp must not be negative"
        );
    }
    if (receive_timestamp < 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NegativeReceiveTimestamp,
            "header.receive_timestamp",
            "receive timestamp must not be negative"
        );
    }
    if (event_timestamp >= 0 && receive_timestamp >= 0 &&
        receive_timestamp < event_timestamp &&
        !header.quality.contains(
            MarketDataQualityFlag::QuestionableReceiveTimestamp
        )) {
        add_error(
            errors,
            MarketEventValidationErrorCode::UnexplainedTimestampInversion,
            "header.receive_timestamp",
            "receive timestamp precedes event timestamp without a quality flag"
        );
    }
    if ((header.quality.raw() & ~kKnownQualityFlags) != 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::UnknownQualityFlags,
            "header.quality",
            "market-data quality contains unknown flag bits"
        );
    }
}

void validate_positive_price(
    Price price,
    std::string_view field,
    ValidationErrors& errors
) {
    if (price.raw() <= 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NonpositivePrice,
            field,
            "price must be positive"
        );
    }
}

void validate_positive_quantity(
    Quantity quantity,
    std::string_view field,
    ValidationErrors& errors
) {
    if (quantity.raw() <= 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NonpositiveQuantity,
            field,
            "quantity must be positive"
        );
    }
}

void validate_trade(const TradeEvent& trade, ValidationErrors& errors) {
    validate_positive_price(trade.price, "trade.price", errors);
    validate_positive_quantity(trade.quantity, "trade.quantity", errors);

    switch (trade.aggressor_side) {
        case AggressorSide::Buy:
        case AggressorSide::Sell:
        case AggressorSide::Unknown:
            break;
        default:
            add_error(
                errors,
                MarketEventValidationErrorCode::UnknownAggressorSide,
                "trade.aggressor_side",
                "trade contains an unknown aggressor side"
            );
    }
}

void validate_book_level(
    const std::optional<BookLevel>& level,
    std::string_view price_field,
    std::string_view quantity_field,
    ValidationErrors& errors
) {
    if (!level.has_value()) {
        return;
    }

    validate_positive_price(level->price, price_field, errors);
    validate_positive_quantity(level->quantity, quantity_field, errors);
}

void validate_top_of_book(
    const TopOfBookEvent& top_of_book,
    ValidationErrors& errors
) {
    validate_book_level(
        top_of_book.bid,
        "top_of_book.bid.price",
        "top_of_book.bid.quantity",
        errors
    );
    validate_book_level(
        top_of_book.ask,
        "top_of_book.ask.price",
        "top_of_book.ask.quantity",
        errors
    );

    const bool has_positive_two_sided_book =
        top_of_book.bid.has_value() && top_of_book.ask.has_value() &&
        top_of_book.bid->price.raw() > 0 &&
        top_of_book.ask->price.raw() > 0;
    if (has_positive_two_sided_book &&
        top_of_book.bid->price > top_of_book.ask->price &&
        !top_of_book.header.quality.contains(
            MarketDataQualityFlag::PotentiallyInvalidBook
        )) {
        add_error(
            errors,
            MarketEventValidationErrorCode::UnflaggedCrossedBook,
            "top_of_book",
            "crossed top of book requires the potentially-invalid-book flag"
        );
    }
}

void validate_market_status(
    const MarketStatusEvent& market_status,
    ValidationErrors& errors
) {
    switch (market_status.status) {
        case TradingStatus::Unknown:
        case TradingStatus::PreOpen:
        case TradingStatus::Open:
        case TradingStatus::Halted:
        case TradingStatus::Closed:
            break;
        default:
            add_error(
                errors,
                MarketEventValidationErrorCode::UnknownTradingStatus,
                "market_status.status",
                "market status contains an unknown value"
            );
    }
}

void validate_instrument_definition(
    const InstrumentDefinitionEvent& definition,
    ValidationErrors& errors
) {
    if (definition.display_symbol.empty()) {
        add_error(
            errors,
            MarketEventValidationErrorCode::MissingDisplaySymbol,
            "instrument_definition.display_symbol",
            "instrument display symbol must not be empty"
        );
    }
    if (definition.minimum_price_increment.raw() <= 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NonpositiveMinimumPriceIncrement,
            "instrument_definition.minimum_price_increment",
            "minimum price increment must be positive"
        );
    }
    if (definition.minimum_quantity_increment.raw() <= 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NonpositiveMinimumQuantityIncrement,
            "instrument_definition.minimum_quantity_increment",
            "minimum quantity increment must be positive"
        );
    }

    const std::int64_t effective_from =
        definition.effective_from.nanoseconds();
    if (effective_from < 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NegativeEffectiveTimestamp,
            "instrument_definition.effective_from",
            "effective-from timestamp must not be negative"
        );
    }

    if (!definition.effective_until.has_value()) {
        return;
    }

    const std::int64_t effective_until =
        definition.effective_until->nanoseconds();
    if (effective_until < 0) {
        add_error(
            errors,
            MarketEventValidationErrorCode::NegativeEffectiveTimestamp,
            "instrument_definition.effective_until",
            "effective-until timestamp must not be negative"
        );
    } else if (effective_from >= 0 && effective_until <= effective_from) {
        add_error(
            errors,
            MarketEventValidationErrorCode::InvalidEffectiveRange,
            "instrument_definition.effective_until",
            "effective-until timestamp must be later than effective-from"
        );
    }
}

const MarketEventHeader& event_header(const MarketEvent& event) {
    return std::visit(
        [](const auto& concrete_event) -> const MarketEventHeader& {
            return concrete_event.header;
        },
        event
    );
}

void validate_event_fields(
    const MarketEvent& event,
    ValidationErrors& errors
) {
    std::visit(
        [&errors](const auto& concrete_event) {
            using Event = std::decay_t<decltype(concrete_event)>;
            if constexpr (std::is_same_v<Event, TradeEvent>) {
                validate_trade(concrete_event, errors);
            } else if constexpr (std::is_same_v<Event, TopOfBookEvent>) {
                validate_top_of_book(concrete_event, errors);
            } else if constexpr (std::is_same_v<Event, MarketStatusEvent>) {
                validate_market_status(concrete_event, errors);
            } else if constexpr (
                std::is_same_v<Event, InstrumentDefinitionEvent>
            ) {
                validate_instrument_definition(concrete_event, errors);
            }
        },
        event
    );
}

}  // namespace

namespace detail {

class ValidatedMarketEventFactory final {
public:
    static ValidatedMarketEvent create(MarketEvent event) {
        return ValidatedMarketEvent(std::move(event));
    }
};

}  // namespace detail

ValidatedMarketEvent::ValidatedMarketEvent(MarketEvent event)
    : event_(std::move(event)) {}

MarketEventValidationResult validate_market_event(MarketEvent event) {
    ValidationErrors errors;
    validate_header(event_header(event), errors);
    validate_event_fields(event, errors);

    if (!errors.empty()) {
        return MarketEventValidationFailure{.errors = std::move(errors)};
    }
    return detail::ValidatedMarketEventFactory::create(std::move(event));
}

}  // namespace chronos
