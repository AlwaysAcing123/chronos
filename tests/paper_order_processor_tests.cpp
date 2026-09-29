#include "chronos/paper_order_processor.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using chronos::AccountState;
using chronos::ApplyFillErrorCode;
using chronos::BookLevel;
using chronos::InstrumentId;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperOrderProcessor;
using chronos::PaperOrderProcessingError;
using chronos::PaperOrderProcessingErrorCode;
using chronos::PaperOrderReceipt;
using chronos::PaperOrderResult;
using chronos::PreTradeRiskErrorCode;
using chronos::PreTradeRiskLimits;
using chronos::PreTradeRiskRejection;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedCancellationReason;
using chronos::SimulatedExecutionError;
using chronos::SimulatedExecutionErrorCode;
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;

static_assert(!std::is_copy_constructible_v<PaperOrderProcessor>);
static_assert(std::is_move_constructible_v<PaperOrderProcessor>);

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

PreTradeRiskLimits risk_limits(
    std::int64_t maximum_quote_age_nanoseconds = 1'000
) {
    return PreTradeRiskLimits{
        .maximum_order_quantity = quantity("100"),
        .maximum_order_notional = money("100000"),
        .maximum_position_quantity = quantity("100"),
        .maximum_quote_age_nanoseconds = maximum_quote_age_nanoseconds,
        .fee_per_fill = money("0.01"),
    };
}

TopOfBookEvent market(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view bid_price = "99",
    std::string_view bid_quantity = "5",
    std::string_view ask_price = "101",
    std::string_view ask_quantity = "5"
) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = InstrumentId("AAPL"),
            .stream = chronos::MarketDataStreamId("market-stream"),
            .venue = std::nullopt,
            .event_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp - 1),
            .receive_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp),
            .ingest_sequence = sequence,
            .venue_sequence = std::nullopt,
            .quality = chronos::MarketDataQuality{},
        },
        .bid = BookLevel{
            .price = price(bid_price),
            .quantity = quantity(bid_quantity),
        },
        .ask = BookLevel{
            .price = price(ask_price),
            .quantity = quantity(ask_quantity),
        },
    };
}

OrderIntent order(
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at,
    OrderType type = OrderType::Market,
    std::optional<Price> limit_price = std::nullopt,
    OrderSourceKind source_kind = OrderSourceKind::Manual,
    std::string source_id = "user-1"
) {
    return OrderIntent{
        .client_order_id = std::move(client_order_id),
        .account_id = "account-1",
        .source = OrderSource{
            .kind = source_kind,
            .source_id = std::move(source_id),
        },
        .instrument = InstrumentId("AAPL"),
        .side = side,
        .type = type,
        .quantity = quantity(order_quantity),
        .limit_price = limit_price,
        .time_in_force = TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(submitted_at),
    };
}

TopOfBookEvent spy_market(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view ask_quantity = "5"
) {
    TopOfBookEvent quote = market(sequence, receive_timestamp, "499", "5", "501", ask_quantity);
    quote.header.instrument = InstrumentId("SPY");
    return quote;
}

OrderIntent spy_order(
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at
) {
    OrderIntent intent = order(std::move(client_order_id), side, order_quantity, submitted_at);
    intent.instrument = InstrumentId("SPY");
    return intent;
}

PaperOrderProcessor processor(
    Money starting_cash = money("1000"),
    PreTradeRiskLimits limits = risk_limits()
) {
    return PaperOrderProcessor("account-1", starting_cash, limits);
}

PaperOrderReceipt require_receipt(PaperOrderResult result) {
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(result));
    return std::get<PaperOrderReceipt>(std::move(result));
}

const PreTradeRiskRejection& require_risk_rejection(
    const PaperOrderResult& result,
    PreTradeRiskErrorCode code
) {
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(result));
    const auto& rejection = std::get<PreTradeRiskRejection>(result);
    REQUIRE(rejection.code == code);
    REQUIRE(!rejection.field.empty());
    REQUIRE(!rejection.message.empty());
    return rejection;
}

const SimulatedExecutionError& require_execution_error(
    const PaperOrderResult& result,
    SimulatedExecutionErrorCode code
) {
    REQUIRE(std::holds_alternative<SimulatedExecutionError>(result));
    const auto& error = std::get<SimulatedExecutionError>(result);
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

const PaperOrderProcessingError& require_processing_error(
    const PaperOrderResult& result,
    PaperOrderProcessingErrorCode code
) {
    REQUIRE(std::holds_alternative<PaperOrderProcessingError>(result));
    const auto& error = std::get<PaperOrderProcessingError>(result);
    REQUIRE(error.code == code);
    REQUIRE(!error.message.empty());
    return error;
}

void require_empty_account(const AccountState& state) {
    REQUIRE(state.starting_cash == money("1000"));
    REQUIRE(state.cash == money("1000"));
    REQUIRE(state.positions.empty());
    REQUIRE(!state.latest_fill_timestamp().has_value());
}

void test_filled_buy_is_booked_and_reconciles() {
    PaperOrderProcessor engine = processor();
    const auto receipt = require_receipt(engine.submit(
        order(
            "model-buy",
            Side::Buy,
            "3",
            110,
            OrderType::Market,
            std::nullopt,
            OrderSourceKind::Model,
            "model-v1"
        ),
        market(10, 100)
    ));

    REQUIRE(receipt.execution.status == SimulatedExecutionStatus::Filled);
    REQUIRE(receipt.execution.fill.has_value());
    REQUIRE(receipt.execution.intent.source.kind == OrderSourceKind::Model);
    REQUIRE(receipt.execution.intent.source.source_id == "model-v1");
    REQUIRE(receipt.accounting.has_value());
    REQUIRE(receipt.accounting->fill == *receipt.execution.fill);
    REQUIRE(receipt.accounting->notional == money("303"));
    REQUIRE(receipt.account_after.cash == money("696.99"));
    REQUIRE(receipt.account_after.quantity(InstrumentId("AAPL")) == quantity("3"));
    REQUIRE(receipt.account_after.open_cost_basis() == money("303"));
    REQUIRE(receipt.account_after.fees_paid() == money("0.01"));
    REQUIRE(
        receipt.account_after.latest_fill_timestamp() ==
        Timestamp::from_nanoseconds(110)
    );

    const auto snapshot = engine.account().snapshot({{InstrumentId("AAPL"), price("100")}});
    REQUIRE(snapshot.market_value == money("300"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("-3"));
    REQUIRE(snapshot.gross_total_pnl == money("-3"));
    REQUIRE(snapshot.net_total_pnl == money("-3.01"));
    REQUIRE(snapshot.equity == money("996.99"));
    REQUIRE(
        snapshot.equity - snapshot.state.starting_cash ==
        snapshot.net_total_pnl
    );
}

void test_buy_then_sell_updates_realized_and_unrealized_pnl() {
    PaperOrderProcessor engine = processor();
    require_receipt(engine.submit(
        order("buy", Side::Buy, "2", 110),
        market(10, 100)
    ));
    const auto sale = require_receipt(engine.submit(
        order("sell", Side::Sell, "1", 130),
        market(11, 120, "103", "5", "105", "5")
    ));

    REQUIRE(sale.execution.fill->price == price("103"));
    REQUIRE(sale.accounting->gross_realized_pnl_change == money("2"));
    REQUIRE(sale.account_after.cash == money("900.98"));
    REQUIRE(sale.account_after.quantity(InstrumentId("AAPL")) == quantity("1"));
    REQUIRE(sale.account_after.open_cost_basis() == money("101"));
    REQUIRE(sale.account_after.gross_realized_pnl() == money("2"));
    REQUIRE(sale.account_after.fees_paid() == money("0.02"));

    const auto snapshot = engine.account().snapshot({{InstrumentId("AAPL"), price("102")}});
    REQUIRE(snapshot.market_value == money("102"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("1"));
    REQUIRE(snapshot.gross_total_pnl == money("3"));
    REQUIRE(snapshot.net_total_pnl == money("2.98"));
    REQUIRE(snapshot.equity == money("1002.98"));
    REQUIRE(
        snapshot.equity - snapshot.state.starting_cash ==
        snapshot.net_total_pnl
    );
}

void test_partial_fill_books_only_the_executed_quantity() {
    PaperOrderProcessor engine = processor();
    const auto receipt = require_receipt(engine.submit(
        order("partial", Side::Buy, "7", 110),
        market(10, 100)
    ));

    REQUIRE(
        receipt.execution.status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(receipt.execution.fill->quantity == quantity("5"));
    REQUIRE(receipt.execution.unfilled_quantity == quantity("2"));
    REQUIRE(
        receipt.execution.cancellation_reason ==
        SimulatedCancellationReason::DisplayedLiquidityExhausted
    );
    REQUIRE(receipt.accounting->notional == money("505"));
    REQUIRE(receipt.account_after.cash == money("494.99"));
    REQUIRE(receipt.account_after.quantity(InstrumentId("AAPL")) == quantity("5"));
    REQUIRE(receipt.account_after.open_cost_basis() == money("505"));
    REQUIRE(receipt.account_after.fees_paid() == money("0.01"));
}

void test_cancelled_order_does_not_change_the_account() {
    PaperOrderProcessor engine = processor();
    const OrderIntent passive_order = order(
        "passive",
        Side::Buy,
        "1",
        110,
        OrderType::Limit,
        price("100")
    );
    const auto receipt = require_receipt(engine.submit(
        passive_order,
        market(10, 100)
    ));

    REQUIRE(receipt.execution.status == SimulatedExecutionStatus::Cancelled);
    REQUIRE(!receipt.execution.fill.has_value());
    REQUIRE(!receipt.accounting.has_value());
    require_empty_account(receipt.account_after);
    require_empty_account(engine.account().state());

    require_execution_error(
        engine.submit(passive_order, market(10, 100)),
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );
    require_empty_account(engine.account().state());
}

void test_risk_rejection_reserves_nothing_and_can_be_retried() {
    PaperOrderProcessor engine = processor(money("1000"), risk_limits(5));
    const OrderIntent retryable = order("retry", Side::Buy, "1", 110);

    require_risk_rejection(
        engine.submit(retryable, market(10, 100)),
        PreTradeRiskErrorCode::StaleMarketState
    );
    require_empty_account(engine.account().state());

    const auto accepted = require_receipt(engine.submit(
        retryable,
        market(11, 109)
    ));
    REQUIRE(accepted.execution.status == SimulatedExecutionStatus::Filled);
    REQUIRE(accepted.account_after.quantity(InstrumentId("AAPL")) == quantity("1"));
}

void test_submission_time_cannot_move_behind_account_time() {
    PaperOrderProcessor engine = processor();
    require_receipt(engine.submit(
        order("first", Side::Buy, "1", 110),
        market(10, 100)
    ));

    OrderIntent retryable = order("time-retry", Side::Buy, "1", 109);
    require_risk_rejection(
        engine.submit(retryable, market(11, 108)),
        PreTradeRiskErrorCode::SubmissionTimestampMovedBackward
    );
    REQUIRE(engine.account().state().quantity(InstrumentId("AAPL")) == quantity("1"));

    retryable.submitted_at = Timestamp::from_nanoseconds(110);
    const auto accepted = require_receipt(engine.submit(
        retryable,
        market(11, 108)
    ));
    REQUIRE(accepted.account_after.quantity(InstrumentId("AAPL")) == quantity("2"));
    REQUIRE(
        accepted.account_after.latest_fill_timestamp() ==
        Timestamp::from_nanoseconds(110)
    );
}

void test_duplicate_filled_order_cannot_change_account_twice() {
    PaperOrderProcessor engine = processor();
    const OrderIntent intent = order("once", Side::Buy, "2", 110);
    const auto first = require_receipt(engine.submit(intent, market(10, 100)));
    REQUIRE(first.account_after.quantity(InstrumentId("AAPL")) == quantity("2"));
    const Money cash_after_first = first.account_after.cash;

    require_execution_error(
        engine.submit(intent, market(10, 100)),
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );
    REQUIRE(engine.account().state().quantity(InstrumentId("AAPL")) == quantity("2"));
    REQUIRE(engine.account().state().cash == cash_after_first);
    REQUIRE(engine.is_healthy());
}

void test_overflowing_sale_is_a_risk_rejection() {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const PreTradeRiskLimits maximum_limits{
        .maximum_order_quantity = quantity("10"),
        .maximum_order_notional = Money::from_raw(maximum),
        .maximum_position_quantity = quantity("10"),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = Money{},
    };
    PaperOrderProcessor engine = processor(
        Money::from_raw(maximum),
        maximum_limits
    );
    require_receipt(engine.submit(
        order("buy-before-overflow", Side::Buy, "1", 110),
        market(10, 100, "0.9", "5", "1", "5")
    ));
    const AccountState state_before = engine.account().state();

    // The sale's proceeds would overflow cash. Risk previews the fill
    // through accounting, so the order is rejected and the processor stays
    // healthy instead of faulting.
    require_risk_rejection(
        engine.submit(
            order("overflowing-sale", Side::Sell, "1", 130),
            market(11, 120, "2", "5", "2.1", "5")
        ),
        PreTradeRiskErrorCode::ArithmeticOverflow
    );
    REQUIRE(engine.is_healthy());
    REQUIRE(engine.account().state() == state_before);
}

void test_accounting_invariant_failure_halts_the_processor() {
    PaperOrderProcessor seeded = processor();
    require_receipt(seeded.submit(
        order("seed", Side::Buy, "1", 110),
        market(10, 100)
    ));
    // A recovered state in which accounting already applied the fill the
    // next execution will produce: execution and accounting disagree, the
    // defensive case the halt exists for.
    auto state = seeded.recovery_state();
    state.account.applied_fill_ids.push_back("conflict:fill:0");
    PaperOrderProcessor engine = PaperOrderProcessor::restore(std::move(state));
    const AccountState state_before_failure = engine.account().state();

    const PaperOrderResult failed = engine.submit(
        order("conflict", Side::Buy, "1", 120),
        market(11, 110)
    );
    const auto& invariant_error = require_processing_error(
        failed,
        PaperOrderProcessingErrorCode::AccountingInvariantViolation
    );
    REQUIRE(invariant_error.execution.has_value());
    REQUIRE(invariant_error.accounting_error.has_value());
    REQUIRE(
        invariant_error.accounting_error->code ==
        ApplyFillErrorCode::DuplicateFill
    );
    REQUIRE(!engine.is_healthy());
    REQUIRE(engine.account().state() == state_before_failure);

    const PaperOrderResult halted_result = engine.submit(
        order("after-fault", Side::Buy, "1", 140),
        market(12, 130)
    );
    const auto& halted = require_processing_error(
        halted_result,
        PaperOrderProcessingErrorCode::ProcessorFaulted
    );
    REQUIRE(!halted.execution.has_value());
    REQUIRE(!halted.accounting_error.has_value());
}

void test_recovery_preserves_account_and_execution_state() {
    PaperOrderProcessor engine = processor();
    const OrderIntent original = order("original", Side::Buy, "3", 110);
    require_receipt(engine.submit(original, market(10, 100)));

    const auto saved_state = engine.recovery_state();
    REQUIRE(saved_state.account.applied_fill_ids.size() == 1);
    REQUIRE(saved_state.execution.processed_client_order_ids.size() == 1);
    REQUIRE(saved_state.execution.instruments.at(InstrumentId("AAPL")).consumed_ask_quantity == quantity("3"));
    PaperOrderProcessor recovered = PaperOrderProcessor::restore(saved_state);

    require_execution_error(
        recovered.submit(original, market(10, 100)),
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );
    const auto remainder = require_receipt(recovered.submit(
        order("after-restart", Side::Buy, "3", 110),
        market(10, 100)
    ));
    REQUIRE(
        remainder.execution.status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(remainder.execution.fill->quantity == quantity("2"));
    REQUIRE(remainder.execution.unfilled_quantity == quantity("1"));
    REQUIRE(remainder.account_after.quantity(InstrumentId("AAPL")) == quantity("5"));
    REQUIRE(remainder.account_after.cash == money("494.98"));
    REQUIRE(remainder.account_after.open_cost_basis() == money("505"));
    REQUIRE(remainder.account_after.fees_paid() == money("0.02"));
}

void test_corrupt_recovery_state_is_rejected() {
    PaperOrderProcessor engine = processor();
    require_receipt(engine.submit(
        order("saved", Side::Buy, "1", 110),
        market(10, 100)
    ));

    auto state = engine.recovery_state();
    state.account.state.cash = money("-0.01");
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );

    state = engine.recovery_state();
    state.account.state.positions.at(InstrumentId("AAPL")).open_cost_basis =
        money("100");
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );

    state = engine.recovery_state();
    state.account.applied_fill_ids.clear();
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );

    state = engine.recovery_state();
    state.execution.instruments.at(InstrumentId("AAPL")).consumed_ask_quantity =
        quantity("6");
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );

    state = engine.recovery_state();
    state.execution.account_id = "other-account";
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );

    state = engine.recovery_state();
    state.account.applied_fill_ids.push_back(
        state.account.applied_fill_ids.front()
    );
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );

    state = engine.recovery_state();
    state.execution.processed_client_order_ids.push_back(
        state.execution.processed_client_order_ids.front()
    );
    REQUIRE_THROWS_AS(
        PaperOrderProcessor::restore(std::move(state)),
        std::invalid_argument
    );
}


void test_orders_for_several_instruments_share_one_account() {
    PaperOrderProcessor engine = processor(money("10000"));
    const auto aapl = require_receipt(engine.submit(
        order("aapl-buy", Side::Buy, "5", 110),
        market(10, 100)
    ));
    REQUIRE(aapl.account_after.cash == money("9494.99"));
    const auto spy = require_receipt(engine.submit(
        spy_order("spy-buy", Side::Buy, "4", 120),
        spy_market(8, 115)
    ));
    // Shared cash pays for SPY after AAPL.
    REQUIRE(spy.accounting->before.cash == money("9494.99"));
    REQUIRE(spy.account_after.cash == money("7490.98"));
    REQUIRE(spy.account_after.quantity(InstrumentId("SPY")) == quantity("4"));
    REQUIRE(spy.account_after.quantity(InstrumentId("AAPL")) == quantity("5"));

    // Selling part of AAPL leaves SPY unchanged.
    const auto sale = require_receipt(engine.submit(
        order("aapl-sell", Side::Sell, "2", 130),
        market(11, 125)
    ));
    REQUIRE(sale.account_after.quantity(InstrumentId("AAPL")) == quantity("3"));
    REQUIRE(sale.account_after.positions.at(InstrumentId("SPY")) ==
        spy.account_after.positions.at(InstrumentId("SPY")));

    // An AAPL quote cannot fill an SPY order.
    const AccountState before_mismatch = engine.account().state();
    require_risk_rejection(
        engine.submit(spy_order("spy-on-aapl", Side::Buy, "1", 140), market(12, 135)),
        PreTradeRiskErrorCode::MarketInstrumentMismatch
    );
    REQUIRE(engine.account().state() == before_mismatch);
    REQUIRE(engine.executions().at(InstrumentId("SPY")).consumed_ask_quantity ==
        quantity("4"));

    // Per-instrument consumption survives recovery; SPY's remaining
    // displayed shares are still limited to one.
    PaperOrderProcessor recovered = PaperOrderProcessor::restore(engine.recovery_state());
    REQUIRE(recovered.account().state() == engine.account().state());
    REQUIRE(recovered.executions() == engine.executions());
    const auto spy_rest = require_receipt(recovered.submit(
        spy_order("spy-rest", Side::Buy, "3", 150),
        spy_market(8, 115)
    ));
    REQUIRE(spy_rest.execution.fill->quantity == quantity("1"));

    // A recovered position needs its instrument's execution state.
    auto orphaned = engine.recovery_state();
    orphaned.execution.instruments.erase(InstrumentId("SPY"));
    REQUIRE_THROWS_AS(PaperOrderProcessor::restore(std::move(orphaned)), std::invalid_argument);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "filled buy accounting reconciliation",
            test_filled_buy_is_booked_and_reconciles,
        },
        {
            "buy sell realized and unrealized pnl",
            test_buy_then_sell_updates_realized_and_unrealized_pnl,
        },
        {
            "partial fill books executed quantity",
            test_partial_fill_books_only_the_executed_quantity,
        },
        {
            "cancelled order preserves account",
            test_cancelled_order_does_not_change_the_account,
        },
        {
            "risk rejection can retry",
            test_risk_rejection_reserves_nothing_and_can_be_retried,
        },
        {
            "account time cannot regress",
            test_submission_time_cannot_move_behind_account_time,
        },
        {
            "duplicate fill cannot double book",
            test_duplicate_filled_order_cannot_change_account_twice,
        },
        {
            "overflowing sale is a risk rejection",
            test_overflowing_sale_is_a_risk_rejection,
        },
        {
            "accounting invariant failure halts processor",
            test_accounting_invariant_failure_halts_the_processor,
        },
        {
            "recovery preserves account and execution state",
            test_recovery_preserves_account_and_execution_state,
        },
        {
            "corrupt recovery state is rejected",
            test_corrupt_recovery_state_is_rejected,
        },
        {
            "several instruments share one account",
            test_orders_for_several_instruments_share_one_account,
        },
    });
}
