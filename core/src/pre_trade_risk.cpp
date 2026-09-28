#include "chronos/pre_trade_risk.hpp"

#include "chronos/market_data_validation.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace chronos {
namespace {

PreTradeRiskRejection rejection(
    PreTradeRiskErrorCode code,
    std::string field,
    std::string message
) {
    return PreTradeRiskRejection{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
    };
}

std::optional<PreTradeRiskRejection> validate_intent(
    const OrderIntent& intent
) {
    if (intent.client_order_id.empty()) {
        return rejection(
            PreTradeRiskErrorCode::MissingClientOrderId,
            "client_order_id",
            "client order ID must not be empty"
        );
    }
    if (intent.account_id.empty()) {
        return rejection(
            PreTradeRiskErrorCode::MissingAccountId,
            "account_id",
            "account ID must not be empty"
        );
    }
    if (intent.source.source_id.empty()) {
        return rejection(
            PreTradeRiskErrorCode::MissingSourceId,
            "source.source_id",
            "order source ID must not be empty"
        );
    }
    switch (intent.source.kind) {
        case OrderSourceKind::Manual:
        case OrderSourceKind::Model:
            break;
        default:
            return rejection(
                PreTradeRiskErrorCode::UnknownOrderSource,
                "source.kind",
                "order source kind is not recognized"
            );
    }
    switch (intent.side) {
        case Side::Buy:
        case Side::Sell:
            break;
        default:
            return rejection(
                PreTradeRiskErrorCode::UnknownSide,
                "side",
                "order side is not recognized"
            );
    }
    switch (intent.type) {
        case OrderType::Market:
            if (intent.limit_price) {
                return rejection(
                    PreTradeRiskErrorCode::UnexpectedLimitPrice,
                    "limit_price",
                    "market orders must not specify a limit price"
                );
            }
            break;
        case OrderType::Limit:
            if (!intent.limit_price) {
                return rejection(
                    PreTradeRiskErrorCode::MissingLimitPrice,
                    "limit_price",
                    "limit orders require a limit price"
                );
            }
            if (intent.limit_price->raw() <= 0) {
                return rejection(
                    PreTradeRiskErrorCode::NonpositiveLimitPrice,
                    "limit_price",
                    "limit price must be positive"
                );
            }
            break;
        default:
            return rejection(
                PreTradeRiskErrorCode::UnknownOrderType,
                "type",
                "order type is not recognized"
            );
    }
    if (intent.time_in_force != TimeInForce::ImmediateOrCancel) {
        return rejection(
            PreTradeRiskErrorCode::UnsupportedTimeInForce,
            "time_in_force",
            "the first execution slice supports immediate-or-cancel only"
        );
    }
    if (intent.quantity.raw() <= 0) {
        return rejection(
            PreTradeRiskErrorCode::NonpositiveQuantity,
            "quantity",
            "order quantity must be positive"
        );
    }
    if (intent.submitted_at.nanoseconds() < 0) {
        return rejection(
            PreTradeRiskErrorCode::NegativeSubmissionTimestamp,
            "submitted_at",
            "order submission timestamp must not be negative"
        );
    }
    return std::nullopt;
}

}  // namespace

MarketObservationRecency classify_market_observation_recency(
    Timestamp observation_received_at,
    Timestamp market_time,
    std::int64_t maximum_quote_age_nanoseconds
) {
    if (maximum_quote_age_nanoseconds < 0) {
        throw std::invalid_argument(
            "maximum quote age must not be negative"
        );
    }
    if (observation_received_at > market_time) {
        return MarketObservationRecency::FromFuture;
    }
    if (market_time.nanoseconds() - observation_received_at.nanoseconds() >
        maximum_quote_age_nanoseconds) {
        return MarketObservationRecency::Stale;
    }
    return MarketObservationRecency::Current;
}

ApprovedOrderIntent::ApprovedOrderIntent(
    OrderIntent intent,
    TopOfBookEvent market,
    Price execution_price,
    Money maximum_order_notional,
    Money fee
)
    : intent_(std::move(intent)),
      market_(std::move(market)),
      execution_price_(execution_price),
      maximum_order_notional_(maximum_order_notional),
      fee_(fee) {}

PreTradeRiskEngine::PreTradeRiskEngine(PreTradeRiskLimits limits)
    : limits_(limits) {
    if (limits_.maximum_order_quantity.raw() <= 0) {
        throw std::invalid_argument(
            "maximum order quantity must be positive"
        );
    }
    if (limits_.maximum_order_notional.raw() <= 0) {
        throw std::invalid_argument(
            "maximum order notional must be positive"
        );
    }
    if (limits_.maximum_position_quantity.raw() <= 0) {
        throw std::invalid_argument(
            "maximum position quantity must be positive"
        );
    }
    if (limits_.maximum_quote_age_nanoseconds < 0) {
        throw std::invalid_argument(
            "maximum quote age must not be negative"
        );
    }
    if (limits_.fee_per_fill.raw() < 0) {
        throw std::invalid_argument("fee per fill must not be negative");
    }
}

PreTradeRiskDecision PreTradeRiskEngine::evaluate(
    const OrderIntent& intent,
    const PaperAccount& account,
    const TopOfBookEvent& market
) const {
    if (const auto error = validate_intent(intent)) {
        return *error;
    }
    if (intent.account_id != account.account_id()) {
        return rejection(
            PreTradeRiskErrorCode::AccountMismatch,
            "account_id",
            "order account does not match the paper account"
        );
    }
    if (intent.instrument != account.instrument()) {
        return rejection(
            PreTradeRiskErrorCode::InstrumentMismatch,
            "instrument",
            "order instrument does not match the paper account"
        );
    }
    if (account.state().last_fill_timestamp &&
        intent.submitted_at < *account.state().last_fill_timestamp) {
        return rejection(
            PreTradeRiskErrorCode::SubmissionTimestampMovedBackward,
            "submitted_at",
            "order submission timestamp precedes the latest account fill"
        );
    }

    auto market_validation = validate_market_event(MarketEvent(market));
    if (std::holds_alternative<MarketEventValidationFailure>(
            market_validation
        )) {
        return rejection(
            PreTradeRiskErrorCode::InvalidMarketState,
            "market",
            "top-of-book state failed the market-data contract"
        );
    }
    if (market.header.instrument != intent.instrument) {
        return rejection(
            PreTradeRiskErrorCode::MarketInstrumentMismatch,
            "market.header.instrument",
            "top-of-book instrument does not match the order"
        );
    }
    if (market.header.quality.is_suspect()) {
        return rejection(
            PreTradeRiskErrorCode::SuspectMarketState,
            "market.header.quality",
            "suspect top-of-book state is not eligible for execution"
        );
    }

    switch (classify_market_observation_recency(
        market.header.receive_timestamp,
        intent.submitted_at,
        limits_.maximum_quote_age_nanoseconds
    )) {
        case MarketObservationRecency::Current:
            break;
        case MarketObservationRecency::FromFuture:
            return rejection(
                PreTradeRiskErrorCode::MarketObservationFromFuture,
                "market.header.receive_timestamp",
                "order cannot use a market observation received in the future"
            );
        case MarketObservationRecency::Stale:
            return rejection(
                PreTradeRiskErrorCode::StaleMarketState,
                "market.header.receive_timestamp",
                "top-of-book state is older than the configured risk limit"
            );
    }

    const std::optional<BookLevel>& opposite_side =
        intent.side == Side::Buy ? market.ask : market.bid;
    if (!opposite_side) {
        return rejection(
            PreTradeRiskErrorCode::MissingOppositeBookSide,
            intent.side == Side::Buy ? "market.ask" : "market.bid",
            "top of book has no displayed liquidity for this order side"
        );
    }
    if (intent.quantity > limits_.maximum_order_quantity) {
        return rejection(
            PreTradeRiskErrorCode::OrderQuantityLimitExceeded,
            "quantity",
            "order quantity exceeds the configured risk limit"
        );
    }

    try {
        const Money maximum_notional = calculate_notional(
            opposite_side->price,
            intent.quantity
        );
        if (maximum_notional.raw() == 0) {
            return rejection(
                PreTradeRiskErrorCode::NotionalRoundedToZero,
                "quantity",
                "order notional is smaller than supported money precision"
            );
        }
        if (maximum_notional > limits_.maximum_order_notional) {
            return rejection(
                PreTradeRiskErrorCode::OrderNotionalLimitExceeded,
                "quantity",
                "order notional exceeds the configured risk limit"
            );
        }

        if (intent.side == Side::Buy) {
            const Money cash_required =
                maximum_notional + limits_.fee_per_fill;
            if (cash_required > account.state().cash) {
                return rejection(
                    PreTradeRiskErrorCode::InsufficientCash,
                    "quantity",
                    "paper account lacks cash for the order and fee"
                );
            }
            const Quantity resulting_position =
                account.state().position + intent.quantity;
            if (resulting_position > limits_.maximum_position_quantity) {
                return rejection(
                    PreTradeRiskErrorCode::PositionLimitExceeded,
                    "quantity",
                    "order would exceed the configured position limit"
                );
            }
        } else {
            if (intent.quantity > account.state().position) {
                return rejection(
                    PreTradeRiskErrorCode::InsufficientPosition,
                    "quantity",
                    "sell order exceeds the paper account position"
                );
            }
            if (limits_.fee_per_fill > account.state().cash) {
                return rejection(
                    PreTradeRiskErrorCode::InsufficientCashForFee,
                    "fee_per_fill",
                    "paper account lacks cash for a possible partial-fill fee"
                );
            }
        }

        return ApprovedOrderIntent(
            intent,
            market,
            opposite_side->price,
            maximum_notional,
            limits_.fee_per_fill
        );
    } catch (const std::overflow_error&) {
        return rejection(
            PreTradeRiskErrorCode::ArithmeticOverflow,
            "quantity",
            "pre-trade risk arithmetic exceeds the fixed-point range"
        );
    }
}

}  // namespace chronos
