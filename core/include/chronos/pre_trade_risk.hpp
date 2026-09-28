#pragma once

#include "chronos/market_data_types.hpp"
#include "chronos/order_intent.hpp"
#include "chronos/paper_account.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace chronos {

struct PreTradeRiskLimits {
    Quantity maximum_order_quantity;
    Money maximum_order_notional;
    Quantity maximum_position_quantity;
    std::int64_t maximum_quote_age_nanoseconds{0};
    Money fee_per_fill;
};

enum class PreTradeRiskErrorCode {
    MissingClientOrderId,
    MissingAccountId,
    MissingSourceId,
    UnknownOrderSource,
    AccountMismatch,
    InstrumentMismatch,
    UnknownSide,
    UnknownOrderType,
    UnsupportedTimeInForce,
    NonpositiveQuantity,
    UnexpectedLimitPrice,
    MissingLimitPrice,
    NonpositiveLimitPrice,
    NegativeSubmissionTimestamp,
    SubmissionTimestampMovedBackward,
    InvalidMarketState,
    MarketInstrumentMismatch,
    SuspectMarketState,
    MarketObservationFromFuture,
    StaleMarketState,
    MissingOppositeBookSide,
    OrderQuantityLimitExceeded,
    NotionalRoundedToZero,
    OrderNotionalLimitExceeded,
    PositionLimitExceeded,
    InsufficientCash,
    InsufficientPosition,
    InsufficientCashForFee,
    ArithmeticOverflow,
};

struct PreTradeRiskRejection {
    PreTradeRiskErrorCode code;
    std::string field;
    std::string message;

    friend bool operator==(
        const PreTradeRiskRejection&,
        const PreTradeRiskRejection&
    ) = default;
};

class PreTradeRiskEngine;

class ApprovedOrderIntent final {
public:
    [[nodiscard]] const OrderIntent& intent() const noexcept { return intent_; }

    [[nodiscard]] const TopOfBookEvent& market() const noexcept {
        return market_;
    }

    [[nodiscard]] Price execution_price() const noexcept {
        return execution_price_;
    }

    [[nodiscard]] Money maximum_order_notional() const noexcept {
        return maximum_order_notional_;
    }

    [[nodiscard]] Money fee() const noexcept { return fee_; }

private:
    ApprovedOrderIntent(
        OrderIntent intent,
        TopOfBookEvent market,
        Price execution_price,
        Money maximum_order_notional,
        Money fee
    );

    OrderIntent intent_;
    TopOfBookEvent market_;
    Price execution_price_;
    Money maximum_order_notional_;
    Money fee_;

    friend class PreTradeRiskEngine;
};

using PreTradeRiskDecision = std::variant<
    ApprovedOrderIntent,
    PreTradeRiskRejection
>;

class PreTradeRiskEngine final {
public:
    explicit PreTradeRiskEngine(PreTradeRiskLimits limits);

    [[nodiscard]] const PreTradeRiskLimits& limits() const noexcept {
        return limits_;
    }

    [[nodiscard]] PreTradeRiskDecision evaluate(
        const OrderIntent& intent,
        const PaperAccount& account,
        const TopOfBookEvent& market
    ) const;

private:
    PreTradeRiskLimits limits_;
};

}  // namespace chronos
