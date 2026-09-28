#include "chronos/paper_order_codes.hpp"

#include <optional>
#include <string_view>

namespace chronos {

std::optional<std::string_view> canonical_name(OrderSourceKind kind) noexcept {
    switch (kind) {
        case OrderSourceKind::Manual:
            return "manual";
        case OrderSourceKind::Model:
            return "model";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "buy";
        case Side::Sell:
            return "sell";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(OrderType type) noexcept {
    switch (type) {
        case OrderType::Market:
            return "market";
        case OrderType::Limit:
            return "limit";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(
    TimeInForce time_in_force
) noexcept {
    switch (time_in_force) {
        case TimeInForce::ImmediateOrCancel:
            return "ioc";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(
    SimulatedExecutionStatus status
) noexcept {
    switch (status) {
        case SimulatedExecutionStatus::Filled:
            return "filled";
        case SimulatedExecutionStatus::PartiallyFilledAndCancelled:
            return "partially_filled_and_cancelled";
        case SimulatedExecutionStatus::Cancelled:
            return "cancelled";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(
    SimulatedCancellationReason reason
) noexcept {
    switch (reason) {
        case SimulatedCancellationReason::LimitNotMarketable:
            return "limit_not_marketable";
        case SimulatedCancellationReason::DisplayedLiquidityExhausted:
            return "displayed_liquidity_exhausted";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(
    PreTradeRiskErrorCode code
) noexcept {
    switch (code) {
        case PreTradeRiskErrorCode::MissingClientOrderId:
            return "missing_client_order_id";
        case PreTradeRiskErrorCode::MissingAccountId:
            return "missing_account_id";
        case PreTradeRiskErrorCode::MissingSourceId:
            return "missing_source_id";
        case PreTradeRiskErrorCode::UnknownOrderSource:
            return "unknown_order_source";
        case PreTradeRiskErrorCode::AccountMismatch:
            return "account_mismatch";
        case PreTradeRiskErrorCode::InstrumentMismatch:
            return "instrument_mismatch";
        case PreTradeRiskErrorCode::UnknownSide:
            return "unknown_side";
        case PreTradeRiskErrorCode::UnknownOrderType:
            return "unknown_order_type";
        case PreTradeRiskErrorCode::UnsupportedTimeInForce:
            return "unsupported_time_in_force";
        case PreTradeRiskErrorCode::NonpositiveQuantity:
            return "nonpositive_quantity";
        case PreTradeRiskErrorCode::UnexpectedLimitPrice:
            return "unexpected_limit_price";
        case PreTradeRiskErrorCode::MissingLimitPrice:
            return "missing_limit_price";
        case PreTradeRiskErrorCode::NonpositiveLimitPrice:
            return "nonpositive_limit_price";
        case PreTradeRiskErrorCode::NegativeSubmissionTimestamp:
            return "negative_submission_timestamp";
        case PreTradeRiskErrorCode::SubmissionTimestampMovedBackward:
            return "submission_timestamp_moved_backward";
        case PreTradeRiskErrorCode::InvalidMarketState:
            return "invalid_market_state";
        case PreTradeRiskErrorCode::MarketInstrumentMismatch:
            return "market_instrument_mismatch";
        case PreTradeRiskErrorCode::SuspectMarketState:
            return "suspect_market_state";
        case PreTradeRiskErrorCode::MarketObservationFromFuture:
            return "market_observation_from_future";
        case PreTradeRiskErrorCode::StaleMarketState:
            return "stale_market_state";
        case PreTradeRiskErrorCode::MissingOppositeBookSide:
            return "missing_opposite_book_side";
        case PreTradeRiskErrorCode::OrderQuantityLimitExceeded:
            return "order_quantity_limit_exceeded";
        case PreTradeRiskErrorCode::NotionalRoundedToZero:
            return "notional_rounded_to_zero";
        case PreTradeRiskErrorCode::OrderNotionalLimitExceeded:
            return "order_notional_limit_exceeded";
        case PreTradeRiskErrorCode::PositionLimitExceeded:
            return "position_limit_exceeded";
        case PreTradeRiskErrorCode::InsufficientCash:
            return "insufficient_cash";
        case PreTradeRiskErrorCode::InsufficientPosition:
            return "insufficient_position";
        case PreTradeRiskErrorCode::InsufficientCashForFee:
            return "insufficient_cash_for_fee";
        case PreTradeRiskErrorCode::ArithmeticOverflow:
            return "arithmetic_overflow";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(
    SimulatedExecutionErrorCode code
) noexcept {
    switch (code) {
        case SimulatedExecutionErrorCode::AccountMismatch:
            return "account_mismatch";
        case SimulatedExecutionErrorCode::InstrumentMismatch:
            return "instrument_mismatch";
        case SimulatedExecutionErrorCode::DuplicateClientOrderId:
            return "duplicate_client_order_id";
        case SimulatedExecutionErrorCode::MarketStreamMismatch:
            return "market_stream_mismatch";
        case SimulatedExecutionErrorCode::StaleMarketApproval:
            return "stale_market_approval";
        case SimulatedExecutionErrorCode::MarketSnapshotMismatch:
            return "market_snapshot_mismatch";
    }
    return std::nullopt;
}

std::optional<std::string_view> canonical_name(
    PaperOrderProcessingErrorCode code
) noexcept {
    switch (code) {
        case PaperOrderProcessingErrorCode::ProcessorFaulted:
            return "processor_faulted";
        case PaperOrderProcessingErrorCode::AccountingInvariantViolation:
            return "accounting_invariant_violation";
    }
    return std::nullopt;
}

}  // namespace chronos
