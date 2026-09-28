#include "chronos/paper_order_codes.hpp"
#include "test_support.hpp"

#include <initializer_list>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperOrderProcessingErrorCode;
using chronos::PreTradeRiskErrorCode;
using chronos::Side;
using chronos::SimulatedCancellationReason;
using chronos::SimulatedExecutionErrorCode;
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;

// Every name is persisted in the journal and returned by the API. These
// tables are deliberately literal: a failing assertion here means a stored
// or public contract would change.
// Every name must also decode back to its value, and the declared set must
// be exactly the listed values; the persisted-journal reader and the API
// parser depend on both.
template <typename Enum>
void require_names(
    std::initializer_list<std::pair<Enum, std::string_view>> expected
) {
    std::set<std::string_view> seen;
    std::vector<Enum> listed;
    for (const auto& [value, name] : expected) {
        REQUIRE(chronos::canonical_name(value) == name);
        REQUIRE(chronos::from_canonical_name<Enum>(name) == value);
        REQUIRE(seen.insert(name).second);
        listed.push_back(value);
    }
    REQUIRE(chronos::declared_values<Enum>() == listed);
    REQUIRE(!chronos::canonical_name(static_cast<Enum>(999)).has_value());
    REQUIRE(!chronos::from_canonical_name<Enum>("").has_value());
    REQUIRE(!chronos::from_canonical_name<Enum>("unknown:0").has_value());
}

void test_order_intent_names_are_stable() {
    require_names<OrderSourceKind>({
        {OrderSourceKind::Manual, "manual"},
        {OrderSourceKind::Model, "model"},
    });
    require_names<Side>({
        {Side::Buy, "buy"},
        {Side::Sell, "sell"},
    });
    require_names<OrderType>({
        {OrderType::Market, "market"},
        {OrderType::Limit, "limit"},
    });
    require_names<TimeInForce>({
        {TimeInForce::ImmediateOrCancel, "ioc"},
    });
}

void test_execution_outcome_names_are_stable() {
    require_names<SimulatedExecutionStatus>({
        {SimulatedExecutionStatus::Filled, "filled"},
        {
            SimulatedExecutionStatus::PartiallyFilledAndCancelled,
            "partially_filled_and_cancelled",
        },
        {SimulatedExecutionStatus::Cancelled, "cancelled"},
    });
    require_names<SimulatedCancellationReason>({
        {
            SimulatedCancellationReason::LimitNotMarketable,
            "limit_not_marketable",
        },
        {
            SimulatedCancellationReason::DisplayedLiquidityExhausted,
            "displayed_liquidity_exhausted",
        },
    });
}

void test_rejection_and_fault_names_are_stable() {
    using Risk = PreTradeRiskErrorCode;
    require_names<Risk>({
        {Risk::MissingClientOrderId, "missing_client_order_id"},
        {Risk::MissingAccountId, "missing_account_id"},
        {Risk::MissingSourceId, "missing_source_id"},
        {Risk::UnknownOrderSource, "unknown_order_source"},
        {Risk::AccountMismatch, "account_mismatch"},
        {Risk::InstrumentMismatch, "instrument_mismatch"},
        {Risk::UnknownSide, "unknown_side"},
        {Risk::UnknownOrderType, "unknown_order_type"},
        {Risk::UnsupportedTimeInForce, "unsupported_time_in_force"},
        {Risk::NonpositiveQuantity, "nonpositive_quantity"},
        {Risk::UnexpectedLimitPrice, "unexpected_limit_price"},
        {Risk::MissingLimitPrice, "missing_limit_price"},
        {Risk::NonpositiveLimitPrice, "nonpositive_limit_price"},
        {Risk::NegativeSubmissionTimestamp, "negative_submission_timestamp"},
        {
            Risk::SubmissionTimestampMovedBackward,
            "submission_timestamp_moved_backward",
        },
        {Risk::InvalidMarketState, "invalid_market_state"},
        {Risk::MarketInstrumentMismatch, "market_instrument_mismatch"},
        {Risk::SuspectMarketState, "suspect_market_state"},
        {
            Risk::MarketObservationFromFuture,
            "market_observation_from_future",
        },
        {Risk::StaleMarketState, "stale_market_state"},
        {Risk::MissingOppositeBookSide, "missing_opposite_book_side"},
        {Risk::OrderQuantityLimitExceeded, "order_quantity_limit_exceeded"},
        {Risk::NotionalRoundedToZero, "notional_rounded_to_zero"},
        {Risk::OrderNotionalLimitExceeded, "order_notional_limit_exceeded"},
        {Risk::PositionLimitExceeded, "position_limit_exceeded"},
        {Risk::InsufficientCash, "insufficient_cash"},
        {Risk::InsufficientPosition, "insufficient_position"},
        {Risk::InsufficientCashForFee, "insufficient_cash_for_fee"},
        {Risk::ArithmeticOverflow, "arithmetic_overflow"},
    });

    using Execution = SimulatedExecutionErrorCode;
    require_names<Execution>({
        {Execution::AccountMismatch, "account_mismatch"},
        {Execution::InstrumentMismatch, "instrument_mismatch"},
        {Execution::DuplicateClientOrderId, "duplicate_client_order_id"},
        {Execution::MarketStreamMismatch, "market_stream_mismatch"},
        {Execution::StaleMarketApproval, "stale_market_approval"},
        {Execution::MarketSnapshotMismatch, "market_snapshot_mismatch"},
    });

    using Processing = PaperOrderProcessingErrorCode;
    require_names<Processing>({
        {Processing::ProcessorFaulted, "processor_faulted"},
        {
            Processing::AccountingInvariantViolation,
            "accounting_invariant_violation",
        },
    });
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"order intent names", test_order_intent_names_are_stable},
        {"execution outcome names", test_execution_outcome_names_are_stable},
        {
            "rejection and fault names",
            test_rejection_and_fault_names_are_stable,
        },
    });
}
