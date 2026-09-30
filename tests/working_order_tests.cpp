// Good-til-cancelled working orders: acceptance, reservations, matching
// against later observations, the one-fee rule, cancellation, and recovery.
#include "chronos/paper_order_processor.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::AccountReservations;
using chronos::BookLevel;
using chronos::InstrumentId;
using chronos::MarketDataQualityFlag;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperOrderCancelError;
using chronos::PaperOrderCancelErrorCode;
using chronos::PaperOrderProcessor;
using chronos::PaperOrderProcessorRecoveryState;
using chronos::PaperOrderReceipt;
using chronos::PaperOrderResult;
using chronos::PaperOrderStatus;
using chronos::PreTradeRiskErrorCode;
using chronos::PreTradeRiskLimits;
using chronos::PreTradeRiskRejection;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedExecutionError;
using chronos::SimulatedExecutionErrorCode;
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::WorkingOrder;
using chronos::WorkingOrderCancellation;
using chronos::WorkingOrderMatch;
using chronos::WorkingOrderSkipReason;

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

PreTradeRiskLimits limits(
    std::string_view fee = "0.01",
    std::string_view maximum_position = "100",
    std::string_view maximum_notional = "100000"
) {
    return PreTradeRiskLimits{
        .maximum_order_quantity = quantity("100"),
        .maximum_order_notional = money(maximum_notional),
        .maximum_position_quantity = quantity(maximum_position),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = money(fee),
    };
}

std::optional<BookLevel> level(std::string_view at, std::string_view size) {
    return BookLevel{.price = price(at), .quantity = quantity(size)};
}

TopOfBookEvent market(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::optional<BookLevel> bid,
    std::optional<BookLevel> ask,
    std::string_view instrument = "AAPL"
) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = InstrumentId(std::string(instrument)),
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
        .bid = std::move(bid),
        .ask = std::move(ask),
    };
}

// The default book: 99 bid, 101 ask, five shares each.
TopOfBookEvent market(std::uint64_t sequence, std::int64_t receive_timestamp) {
    return market(
        sequence,
        receive_timestamp,
        level("99", "5"),
        level("101", "5")
    );
}

TopOfBookEvent asks(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view at,
    std::string_view size
) {
    return market(sequence, receive_timestamp, level("90", "5"), level(at, size));
}

TopOfBookEvent bids(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view at,
    std::string_view size
) {
    return market(sequence, receive_timestamp, level(at, size), level("200", "5"));
}

OrderIntent order(
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at,
    TimeInForce time_in_force,
    std::optional<std::string_view> limit = std::nullopt,
    std::string_view instrument = "AAPL"
) {
    return OrderIntent{
        .client_order_id = std::move(client_order_id),
        .account_id = "account-1",
        .source = OrderSource{.kind = OrderSourceKind::Manual, .source_id = "user-1"},
        .instrument = InstrumentId(std::string(instrument)),
        .side = side,
        .type = limit ? OrderType::Limit : OrderType::Market,
        .quantity = quantity(order_quantity),
        .limit_price = limit ? std::optional<Price>(price(*limit)) : std::nullopt,
        .time_in_force = time_in_force,
        .submitted_at = Timestamp::from_nanoseconds(submitted_at),
    };
}

OrderIntent gtc(
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::string_view limit,
    std::int64_t submitted_at
) {
    return order(
        std::move(client_order_id),
        side,
        order_quantity,
        submitted_at,
        TimeInForce::GoodTilCancelled,
        limit
    );
}

OrderIntent ioc(
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at
) {
    return order(
        std::move(client_order_id),
        side,
        order_quantity,
        submitted_at,
        TimeInForce::ImmediateOrCancel
    );
}

PaperOrderProcessor processor(
    std::string_view starting_cash = "10000",
    PreTradeRiskLimits risk = limits()
) {
    return PaperOrderProcessor("account-1", money(starting_cash), risk);
}

// The accounting identity and the reservation invariants hold, and the
// cached reservations are exactly those of the working orders.
void require_consistent(const PaperOrderProcessor& paper) {
    const auto& state = paper.account().state();
    REQUIRE(
        state.cash + state.open_cost_basis() ==
        state.starting_cash + state.gross_realized_pnl() - state.fees_paid()
    );
    const AccountReservations expected =
        chronos::account_reservations(paper.working_orders());
    REQUIRE(paper.reservations() == expected);
    REQUIRE(expected.reserved_cash <= state.cash);
    for (const auto& [instrument, reserved] : expected.instruments) {
        REQUIRE(reserved.reserved_sell_quantity <= state.quantity(instrument));
    }
    // A restored copy is the same account.
    const PaperOrderProcessor copy =
        PaperOrderProcessor::restore(paper.recovery_state());
    REQUIRE(copy.account().state() == state);
    REQUIRE(copy.working_orders() == paper.working_orders());
    REQUIRE(copy.reservations() == paper.reservations());
}

// A copy, so callers may bind it to a reference past the submission's
// temporary result.
PaperOrderReceipt receipt(const PaperOrderResult& result) {
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(result));
    return std::get<PaperOrderReceipt>(result);
}

void require_rejection(const PaperOrderResult& result, PreTradeRiskErrorCode code) {
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(result));
    REQUIRE(std::get<PreTradeRiskRejection>(result).code == code);
}

WorkingOrderMatch match(PaperOrderProcessor& paper, const TopOfBookEvent& observation) {
    WorkingOrderMatch report =
        paper.match(observation, observation.header.receive_timestamp);
    require_consistent(paper);
    return report;
}

const WorkingOrder& working(const PaperOrderProcessor& paper, std::string_view id) {
    for (const WorkingOrder& order : paper.working_orders()) {
        if (order.intent.client_order_id == id) {
            return order;
        }
    }
    throw chronos::test::TestFailure("no working order " + std::string(id));
}

void test_market_orders_cannot_be_good_til_cancelled() {
    PaperOrderProcessor paper = processor();
    OrderIntent intent = ioc("m1", Side::Buy, "1", 100);
    intent.time_in_force = TimeInForce::GoodTilCancelled;
    const PaperOrderResult result = paper.submit(intent, market(1, 100));
    require_rejection(result, PreTradeRiskErrorCode::MarketOrderRequiresImmediateOrCancel);
    REQUIRE(std::get<PreTradeRiskRejection>(result).field == "time_in_force");
    REQUIRE(paper.working_orders().empty());
    REQUIRE(paper.executions().empty());

    // An undeclared time in force is still unsupported, for either type.
    OrderIntent unknown = gtc("u1", Side::Buy, "1", "100", 100);
    unknown.time_in_force = static_cast<TimeInForce>(9);
    require_rejection(
        paper.submit(unknown, market(1, 100)),
        PreTradeRiskErrorCode::UnsupportedTimeInForce
    );
}

void test_non_marketable_order_rests_without_taking_liquidity() {
    PaperOrderProcessor paper = processor();
    const PaperOrderReceipt& accepted =
        receipt(paper.submit(gtc("b1", Side::Buy, "10", "100", 100), market(1, 100)));
    REQUIRE(accepted.execution.status == SimulatedExecutionStatus::Working);
    REQUIRE(!accepted.execution.fill.has_value());
    REQUIRE(!accepted.accounting.has_value());
    REQUIRE(!accepted.execution.cancellation_reason.has_value());
    REQUIRE(accepted.execution.unfilled_quantity == quantity("10"));
    REQUIRE(paper.account().state().cash == money("10000"));

    const WorkingOrder& order = working(paper, "b1");
    REQUIRE(order.accepted_ingest_sequence == 1);
    REQUIRE(order.fee == money("0.01"));
    REQUIRE(order.status() == PaperOrderStatus::Working);
    REQUIRE(order.remaining_quantity() == quantity("10"));
    REQUIRE(!order.fee_charged());
    // Limit times remainder, plus the fee until the first fill.
    REQUIRE(paper.reservations().reserved_cash == money("1000.01"));
    REQUIRE(paper.reservations().working_buy_quantity(InstrumentId("AAPL")) == quantity("10"));
    // The observation is recorded but nothing was taken from it.
    const auto& execution = paper.executions().at(InstrumentId("AAPL"));
    REQUIRE(execution.active_market.header.ingest_sequence == 1);
    REQUIRE(execution.consumed_ask_quantity == Quantity{});
    require_consistent(paper);

    // The working buy left all five displayed shares for the next order.
    const PaperOrderReceipt& taker =
        receipt(paper.submit(ioc("t1", Side::Buy, "5", 100), market(1, 100)));
    REQUIRE(taker.execution.fill->quantity == quantity("5"));
    require_consistent(paper);
}

void test_marketable_order_fills_now_and_rests_the_remainder() {
    PaperOrderProcessor paper = processor();
    const PaperOrderReceipt& accepted =
        receipt(paper.submit(gtc("b1", Side::Buy, "10", "102", 100), market(1, 100)));
    REQUIRE(accepted.execution.status == SimulatedExecutionStatus::PartiallyFilled);
    REQUIRE(!accepted.execution.cancellation_reason.has_value());
    REQUIRE(accepted.execution.fill->fill_id == "b1:fill:0");
    REQUIRE(accepted.execution.fill->price == price("101"));
    REQUIRE(accepted.execution.fill->quantity == quantity("5"));
    REQUIRE(accepted.execution.fill->fee == money("0.01"));
    REQUIRE(accepted.execution.unfilled_quantity == quantity("5"));
    REQUIRE(paper.account().state().cash == money("9494.99"));

    const WorkingOrder& order = working(paper, "b1");
    REQUIRE(order.status() == PaperOrderStatus::PartiallyFilled);
    REQUIRE(order.fill_count == 1);
    REQUIRE(order.fee_charged());
    // The fee is paid, so only the remainder at the limit stays reserved.
    REQUIRE(paper.reservations().reserved_cash == money("510"));
    require_consistent(paper);

    // Its acceptance observation cannot fill it again.
    REQUIRE(match(paper, market(1, 100)).fills.empty());

    const WorkingOrderMatch second = match(paper, asks(2, 200, "101.5", "3"));
    REQUIRE(second.fills.size() == 1);
    REQUIRE(second.fills[0].accounting.fill.fill_id == "b1:fill:1");
    REQUIRE(second.fills[0].accounting.fill.price == price("101.5"));
    REQUIRE(second.fills[0].accounting.fill.quantity == quantity("3"));
    REQUIRE(second.fills[0].accounting.fill.fee == Money{});
    REQUIRE(second.fills[0].accounting.fill.timestamp == Timestamp::from_nanoseconds(200));
    REQUIRE(second.fills[0].status == PaperOrderStatus::PartiallyFilled);
    REQUIRE(paper.account().state().cash == money("9190.49"));
    REQUIRE(paper.reservations().reserved_cash == money("204"));
    REQUIRE(working(paper, "b1").updated_at == Timestamp::from_nanoseconds(200));

    REQUIRE(match(paper, asks(3, 300, "103", "10")).fills.empty());

    const WorkingOrderMatch last = match(paper, asks(4, 400, "102", "10"));
    REQUIRE(last.fills.size() == 1);
    REQUIRE(last.fills[0].accounting.fill.fill_id == "b1:fill:2");
    REQUIRE(last.fills[0].accounting.fill.quantity == quantity("2"));
    REQUIRE(last.fills[0].status == PaperOrderStatus::Filled);
    REQUIRE(last.fills[0].order.remaining_quantity() == Quantity{});
    REQUIRE(paper.working_orders().empty());
    REQUIRE(paper.reservations() == AccountReservations{});

    // Three fills, one fee.
    const auto& position = *paper.account().state().position(InstrumentId("AAPL"));
    REQUIRE(position.quantity == quantity("10"));
    REQUIRE(position.open_cost_basis == money("1013.5"));
    REQUIRE(position.fees_paid == money("0.01"));
    REQUIRE(paper.account().state().cash == money("8986.49"));
    require_consistent(paper);
}

void test_order_that_fills_completely_does_not_rest() {
    PaperOrderProcessor paper = processor();
    const PaperOrderReceipt& filled = receipt(paper.submit(
        gtc("b1", Side::Buy, "4", "102", 100),
        market(1, 100)
    ));
    REQUIRE(filled.execution.status == SimulatedExecutionStatus::Filled);
    REQUIRE(filled.execution.fill->quantity == quantity("4"));
    REQUIRE(filled.execution.fill->fee == money("0.01"));
    REQUIRE(paper.working_orders().empty());
    REQUIRE(paper.reservations() == AccountReservations{});
    REQUIRE(paper.account().state().cash == money("9595.99"));
    require_consistent(paper);

    // Its client order ID is consumed like any other.
    const PaperOrderResult again =
        paper.submit(gtc("b1", Side::Buy, "4", "102", 100), market(1, 100));
    REQUIRE(std::holds_alternative<SimulatedExecutionError>(again));
    REQUIRE(
        std::get<SimulatedExecutionError>(again).code ==
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );
}

void test_priority_is_acceptance_order_and_new_snapshots_reset() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "4", "100", 100), market(1, 100))));
    // A higher limit accepted later does not jump the queue.
    static_cast<void>(receipt(paper.submit(gtc("b2", Side::Buy, "4", "100.5", 100), market(1, 100))));

    const WorkingOrderMatch first = match(paper, asks(2, 200, "100", "3"));
    REQUIRE(first.fills.size() == 1);
    REQUIRE(first.fills[0].order.intent.client_order_id == "b1");
    REQUIRE(first.fills[0].accounting.fill.quantity == quantity("3"));
    REQUIRE(working(paper, "b2").filled_quantity == Quantity{});

    // The same observation again takes nothing: it was already offered.
    REQUIRE(match(paper, asks(2, 200, "100", "3")).fills.empty());

    // A strictly newer observation offers its whole displayed size again.
    const WorkingOrderMatch second = match(paper, asks(3, 300, "100", "3"));
    REQUIRE(second.fills.size() == 2);
    REQUIRE(second.fills[0].order.intent.client_order_id == "b1");
    REQUIRE(second.fills[0].accounting.fill.quantity == quantity("1"));
    REQUIRE(second.fills[0].status == PaperOrderStatus::Filled);
    REQUIRE(second.fills[1].order.intent.client_order_id == "b2");
    REQUIRE(second.fills[1].accounting.fill.quantity == quantity("2"));
    REQUIRE(second.fills[1].accounting.fill.fee == money("0.01"));
    REQUIRE(paper.working_orders().size() == 1);
    const auto& execution = paper.executions().at(InstrumentId("AAPL"));
    REQUIRE(execution.active_market.header.ingest_sequence == 3);
    REQUIRE(execution.consumed_ask_quantity == quantity("3"));

    // A new order on that observation gets only what the working orders
    // left: nothing.
    const PaperOrderReceipt& late =
        receipt(paper.submit(ioc("t1", Side::Buy, "1", 300), asks(3, 300, "100", "3")));
    REQUIRE(late.execution.status == SimulatedExecutionStatus::Cancelled);
    require_consistent(paper);
}

void test_ineligible_observations_are_skipped_without_cancelling() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "5", "100", 100), market(1, 100))));
    const PaperOrderProcessorRecoveryState before = paper.recovery_state();
    const auto unchanged = [&] {
        REQUIRE(paper.account().state() == before.account.state);
        REQUIRE(paper.working_orders() == before.working_orders);
        REQUIRE(paper.executions() == before.execution.instruments);
    };

    TopOfBookEvent suspect = asks(2, 200, "99", "5");
    suspect.header.quality = suspect.header.quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    REQUIRE(!match(paper, suspect).changed_account());

    TopOfBookEvent questionable = asks(2, 200, "99", "5");
    questionable.header.quality =
        questionable.header.quality.with(MarketDataQualityFlag::QuestionableReceiveTimestamp);
    REQUIRE(!match(paper, questionable).changed_account());

    // Stale at the market clock it is offered with.
    TopOfBookEvent stale = asks(2, 200, "99", "5");
    REQUIRE(!paper.match(stale, Timestamp::from_nanoseconds(1'201)).changed_account());
    // An observation received after the clock is not current either.
    REQUIRE(!paper.match(stale, Timestamp::from_nanoseconds(199)).changed_account());

    TopOfBookEvent other_stream = asks(2, 200, "99", "5");
    other_stream.header.stream = chronos::MarketDataStreamId("other-stream");
    REQUIRE(!match(paper, other_stream).changed_account());

    TopOfBookEvent other_instrument = asks(2, 200, "99", "5");
    other_instrument.header.instrument = InstrumentId("SPY");
    REQUIRE(!match(paper, other_instrument).changed_account());

    REQUIRE(!match(paper, market(2, 200, level("98", "5"), std::nullopt)).changed_account());
    REQUIRE(!match(paper, asks(2, 200, "100.01", "5")).changed_account());

    // An unflagged crossed book violates the market-data contract.
    REQUIRE(!match(paper, market(2, 200, level("100", "5"), level("99", "5"))).changed_account());

    // An observation older than the acceptance observation, and the
    // acceptance observation itself.
    REQUIRE(!match(paper, asks(0, 50, "99", "5")).changed_account());
    REQUIRE(!match(paper, market(1, 100)).changed_account());
    unchanged();

    // A halted account never fills.
    PaperOrderProcessorRecoveryState halted_state = paper.recovery_state();
    halted_state.is_healthy = false;
    PaperOrderProcessor halted = PaperOrderProcessor::restore(std::move(halted_state));
    REQUIRE(!halted.match(asks(2, 200, "99", "5"), Timestamp::from_nanoseconds(200)).changed_account());
    REQUIRE(halted.working_orders().size() == 1);

    // None of this cancelled the order: the first eligible observation
    // fills it.
    const WorkingOrderMatch filled = match(paper, asks(2, 200, "99", "5"));
    REQUIRE(filled.fills.size() == 1);
    REQUIRE(filled.fills[0].accounting.fill.price == price("99"));
}

void test_working_sell_reserves_shares_and_fills_at_the_bid() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(ioc("b1", Side::Buy, "5", 100), market(1, 100))));
    static_cast<void>(receipt(paper.submit(ioc("b2", Side::Buy, "5", 200), market(2, 200))));
    REQUIRE(paper.account().state().quantity(InstrumentId("AAPL")) == quantity("10"));

    const PaperOrderReceipt& rested =
        receipt(paper.submit(gtc("s1", Side::Sell, "6", "102", 300), market(3, 300)));
    REQUIRE(rested.execution.status == SimulatedExecutionStatus::Working);
    REQUIRE(paper.reservations().reserved_sell_quantity(InstrumentId("AAPL")) == quantity("6"));
    // The fee is reserved from cash until the first fill.
    REQUIRE(paper.reservations().reserved_cash == money("0.01"));
    require_consistent(paper);

    // Reserved shares cannot be sold twice.
    require_rejection(
        paper.submit(ioc("s2", Side::Sell, "5", 300), market(3, 300)),
        PreTradeRiskErrorCode::InsufficientPosition
    );
    require_rejection(
        paper.submit(gtc("s3", Side::Sell, "5", "102", 300), market(3, 300)),
        PreTradeRiskErrorCode::InsufficientPosition
    );
    static_cast<void>(receipt(paper.submit(ioc("s4", Side::Sell, "4", 300), market(3, 300))));
    require_consistent(paper);

    const Money cash_before = paper.account().state().cash;
    const WorkingOrderMatch sold = match(paper, bids(4, 400, "102.5", "10"));
    REQUIRE(sold.fills.size() == 1);
    // A sale fills at the bid, above its limit.
    REQUIRE(sold.fills[0].accounting.fill.price == price("102.5"));
    REQUIRE(sold.fills[0].accounting.fill.quantity == quantity("6"));
    REQUIRE(sold.fills[0].accounting.fill.fee == money("0.01"));
    REQUIRE(sold.fills[0].status == PaperOrderStatus::Filled);
    REQUIRE(paper.account().state().cash == cash_before + money("615") - money("0.01"));
    REQUIRE(paper.account().state().quantity(InstrumentId("AAPL")) == Quantity{});
    REQUIRE(paper.reservations() == AccountReservations{});
    require_consistent(paper);
}

void test_sell_with_immediate_partial_fill_releases_its_fee_reservation() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(ioc("b1", Side::Buy, "5", 100), market(1, 100))));
    static_cast<void>(receipt(paper.submit(ioc("b2", Side::Buy, "5", 200), market(2, 200))));
    const PaperOrderReceipt& partial =
        receipt(paper.submit(gtc("s1", Side::Sell, "8", "98", 300), market(3, 300)));
    REQUIRE(partial.execution.status == SimulatedExecutionStatus::PartiallyFilled);
    REQUIRE(partial.execution.fill->price == price("99"));
    REQUIRE(partial.execution.fill->quantity == quantity("5"));
    REQUIRE(paper.reservations().reserved_sell_quantity(InstrumentId("AAPL")) == quantity("3"));
    REQUIRE(paper.reservations().reserved_cash == Money{});
    require_consistent(paper);
}

void test_price_improvement_releases_buying_power() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "10", "100", 100), market(1, 100))));
    const auto available = [&] {
        return paper.account().state().cash - paper.reservations().reserved_cash;
    };
    REQUIRE(available() == money("8999.99"));

    const WorkingOrderMatch improved = match(paper, asks(2, 200, "98", "4"));
    REQUIRE(improved.fills.size() == 1);
    REQUIRE(paper.account().state().cash == money("9607.99"));
    REQUIRE(paper.reservations().reserved_cash == money("600"));
    // Four shares cost 8 less than their reservation at the limit.
    REQUIRE(available() == money("9007.99"));
}

void test_cancellation_releases_the_remainder() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "10", "100", 100), market(1, 100))));
    static_cast<void>(receipt(paper.submit(gtc("b2", Side::Buy, "10", "102", 100), market(1, 100))));
    REQUIRE(paper.reservations().reserved_cash == money("1000.01") + money("510"));

    const auto cancelled = paper.cancel("b1", Timestamp::from_nanoseconds(150));
    REQUIRE(std::holds_alternative<WorkingOrderCancellation>(cancelled));
    const auto& first = std::get<WorkingOrderCancellation>(cancelled);
    REQUIRE(first.status == PaperOrderStatus::Cancelled);
    REQUIRE(first.cancelled_at == Timestamp::from_nanoseconds(150));
    REQUIRE(first.order.intent.client_order_id == "b1");
    // No fill, so no fee.
    REQUIRE(paper.account().state().fees_paid() == money("0.01"));
    REQUIRE(paper.reservations().reserved_cash == money("510"));
    require_consistent(paper);

    // A partial fill keeps its fill and its one fee.
    static_cast<void>(match(paper, asks(2, 200, "101", "2")));
    const Money cash = paper.account().state().cash;
    const auto partial = paper.cancel("b2", Timestamp::from_nanoseconds(150));
    const auto& second = std::get<WorkingOrderCancellation>(partial);
    REQUIRE(second.status == PaperOrderStatus::PartiallyFilledAndCancelled);
    // Cancelled no earlier than the order's latest fill.
    REQUIRE(second.cancelled_at == Timestamp::from_nanoseconds(200));
    REQUIRE(second.order.filled_quantity == quantity("7"));
    REQUIRE(paper.account().state().cash == cash);
    REQUIRE(paper.account().state().quantity(InstrumentId("AAPL")) == quantity("7"));
    REQUIRE(paper.account().state().fees_paid() == money("0.01"));
    REQUIRE(paper.reservations() == AccountReservations{});
    require_consistent(paper);

    for (const std::string_view id : {"b1", "b2", "missing"}) {
        const auto again = paper.cancel(id, Timestamp::from_nanoseconds(300));
        REQUIRE(std::holds_alternative<PaperOrderCancelError>(again));
        REQUIRE(
            std::get<PaperOrderCancelError>(again).code ==
            PaperOrderCancelErrorCode::OrderNotWorking
        );
    }
}

void test_halted_account_can_cancel() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "10", "100", 100), market(1, 100))));
    PaperOrderProcessorRecoveryState state = paper.recovery_state();
    state.is_healthy = false;
    PaperOrderProcessor halted = PaperOrderProcessor::restore(std::move(state));
    REQUIRE(std::holds_alternative<WorkingOrderCancellation>(
        halted.cancel("b1", Timestamp::from_nanoseconds(100))
    ));
    REQUIRE(halted.working_orders().empty());
    REQUIRE(!halted.is_healthy());
}

void test_reservations_cannot_be_spent_twice() {
    PaperOrderProcessor paper = processor("1000");
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "5", "100", 100), market(1, 100))));
    REQUIRE(paper.reservations().reserved_cash == money("500.01"));
    // 499.99 remains, one cent short of a second identical order.
    require_rejection(
        paper.submit(gtc("b2", Side::Buy, "5", "100", 100), market(1, 100)),
        PreTradeRiskErrorCode::InsufficientCash
    );
    static_cast<void>(receipt(paper.submit(gtc("b3", Side::Buy, "4", "100", 100), market(1, 100))));
    REQUIRE(paper.reservations().reserved_cash == money("900.02"));
    // An immediate order cannot use reserved cash either.
    require_rejection(
        paper.submit(ioc("t1", Side::Buy, "1", 100), market(1, 100)),
        PreTradeRiskErrorCode::InsufficientCash
    );
    require_consistent(paper);
}

void test_limits_use_the_limit_price_and_working_buys() {
    // The notional limit applies to quantity times the limit price, even
    // when the order would execute below it.
    PaperOrderProcessor notional = processor("10000", limits("0.01", "100", "1000"));
    require_rejection(
        notional.submit(gtc("g1", Side::Buy, "10", "100.5", 100), asks(1, 100, "99", "20")),
        PreTradeRiskErrorCode::OrderNotionalLimitExceeded
    );
    OrderIntent immediate = gtc("i1", Side::Buy, "10", "100.5", 100);
    immediate.time_in_force = TimeInForce::ImmediateOrCancel;
    REQUIRE(receipt(notional.submit(immediate, asks(1, 100, "99", "20"))).execution.fill.has_value());

    // The position limit counts working buys' unfilled shares.
    PaperOrderProcessor position = processor("10000", limits("0.01", "10"));
    static_cast<void>(receipt(position.submit(gtc("g1", Side::Buy, "6", "100", 100), market(1, 100))));
    require_rejection(
        position.submit(ioc("i1", Side::Buy, "5", 100), market(1, 100)),
        PreTradeRiskErrorCode::PositionLimitExceeded
    );
    require_rejection(
        position.submit(gtc("g2", Side::Buy, "5", "100", 100), market(1, 100)),
        PreTradeRiskErrorCode::PositionLimitExceeded
    );
    static_cast<void>(receipt(position.submit(ioc("i2", Side::Buy, "4", 100), market(1, 100))));
    require_consistent(position);
}

void test_overflowing_reservation_fails_closed() {
    PaperOrderProcessor paper = processor();
    const PaperOrderProcessorRecoveryState before = paper.recovery_state();
    require_rejection(
        paper.submit(gtc("g1", Side::Buy, "100", "9000000000", 100), market(1, 100)),
        PreTradeRiskErrorCode::ArithmeticOverflow
    );
    REQUIRE(paper.account().state() == before.account.state);
    REQUIRE(paper.working_orders().empty());
    REQUIRE(paper.executions().empty());
    REQUIRE_THROWS_AS(
        chronos::reservation_of(WorkingOrder{
            .intent = gtc("g2", Side::Buy, "100", "9000000000", 100),
            .accepted_ingest_sequence = 1,
            .fee = Money{},
            .filled_quantity = Quantity{},
            .fill_count = 0,
            .updated_at = Timestamp::from_nanoseconds(100),
        }),
        std::overflow_error
    );
}

// Rounded notionals do not add up exactly: at 1.20, three one-unit shares
// cost 4 money units, but six cost 7, not 8. With the whole account
// reserved, a three-unit partial fill would leave 3 units of cash for a
// remainder that reserves 4, so it is not taken; the whole order fills.
void test_rounding_shortfall_is_skipped_not_booked() {
    PaperOrderProcessor paper = processor("0.00000007", limits("0"));
    const auto book = [](std::uint64_t sequence, std::string_view size) {
        return market(
            sequence,
            static_cast<std::int64_t>(sequence) * 100,
            level("1.1", "1"),
            level("1.2", size)
        );
    };
    const PaperOrderReceipt& accepted = receipt(paper.submit(
        gtc("g1", Side::Buy, "0.00000006", "1.2", 100),
        book(1, "0.00000003")
    ));
    REQUIRE(accepted.execution.status == SimulatedExecutionStatus::Working);
    REQUIRE(!accepted.execution.fill.has_value());
    REQUIRE(
        paper.executions().at(InstrumentId("AAPL")).consumed_ask_quantity ==
        quantity("0.00000003")
    );
    REQUIRE(paper.reservations().reserved_cash == money("0.00000007"));
    require_consistent(paper);

    const WorkingOrderMatch partial = match(paper, book(2, "0.00000003"));
    REQUIRE(partial.fills.empty());
    REQUIRE(partial.skipped.size() == 1);
    REQUIRE(partial.skipped[0].reason == WorkingOrderSkipReason::ReservationShortfall);
    REQUIRE(partial.changed_account());
    const auto& skipped_execution = paper.executions().at(InstrumentId("AAPL"));
    REQUIRE(skipped_execution.active_market.header.ingest_sequence == 2);
    REQUIRE(skipped_execution.consumed_ask_quantity == quantity("0.00000003"));

    const WorkingOrderMatch whole = match(paper, book(3, "0.00000006"));
    REQUIRE(whole.fills.size() == 1);
    REQUIRE(whole.fills[0].status == PaperOrderStatus::Filled);
    REQUIRE(paper.account().state().cash == Money{});
    REQUIRE(paper.working_orders().empty());
}

// A skipped order is still first in line. A newer order on the same side
// must not consume the observation's liquidity merely because the older
// order could not take a partial fill without uncovering reservations.
void test_skipped_order_keeps_priority_for_the_observation() {
    PaperOrderProcessor paper = processor("0.00000008", limits("0"));
    const TopOfBookEvent acceptance = market(
        1,
        100,
        level("1.1", "1"),
        level("1.3", "1")
    );
    const PaperOrderResult first = paper.submit(
        gtc("first", Side::Buy, "0.00000006", "1.2", 100),
        acceptance
    );
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(first));
    const PaperOrderResult second = paper.submit(
        gtc("second", Side::Buy, "0.00000001", "1.2", 100),
        acceptance
    );
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(second));

    const WorkingOrderMatch result = match(
        paper,
        market(
            2,
            200,
            level("1.1", "1"),
            level("1.2", "0.00000003")
        )
    );
    REQUIRE(result.fills.empty());
    REQUIRE(result.skipped.size() == 1);
    REQUIRE(result.skipped[0].client_order_id == "first");
    REQUIRE(
        result.skipped[0].reason ==
        WorkingOrderSkipReason::ReservationShortfall
    );
    REQUIRE(working(paper, "first").filled_quantity == Quantity{});
    REQUIRE(working(paper, "second").filled_quantity == Quantity{});
}

void test_accounts_do_not_share_liquidity_and_replay_deterministically() {
    const auto run = [] {
        PaperOrderProcessor paper = processor();
        static_cast<void>(paper.submit(gtc("b1", Side::Buy, "4", "100", 100), market(1, 100)));
        static_cast<void>(paper.submit(gtc("b2", Side::Buy, "4", "99", 100), market(1, 100)));
        static_cast<void>(paper.match(asks(2, 200, "100", "3"), Timestamp::from_nanoseconds(200)));
        static_cast<void>(paper.match(asks(3, 300, "99", "3"), Timestamp::from_nanoseconds(300)));
        static_cast<void>(paper.cancel("b2", Timestamp::from_nanoseconds(350)));
        return paper.recovery_state();
    };
    const PaperOrderProcessorRecoveryState first = run();
    const PaperOrderProcessorRecoveryState second = run();
    REQUIRE(first.account.state == second.account.state);
    REQUIRE(first.account.applied_fill_ids == second.account.applied_fill_ids);
    REQUIRE(first.execution.instruments == second.execution.instruments);
    REQUIRE(first.working_orders == second.working_orders);
    // b1 took 3 then 1; b2 took 2; one fee each.
    REQUIRE(first.account.state.quantity(InstrumentId("AAPL")) == quantity("6"));
    REQUIRE(first.account.state.fees_paid() == money("0.02"));

    // Two accounts offered the same observation each see its full size.
    PaperOrderProcessor one = processor();
    PaperOrderProcessor two = PaperOrderProcessor("account-2", money("10000"), limits());
    static_cast<void>(receipt(one.submit(gtc("b1", Side::Buy, "5", "100", 100), market(1, 100))));
    OrderIntent other = gtc("b1", Side::Buy, "5", "100", 100);
    other.account_id = "account-2";
    static_cast<void>(receipt(two.submit(other, market(1, 100))));
    REQUIRE(match(one, asks(2, 200, "100", "5")).fills[0].accounting.fill.quantity == quantity("5"));
    REQUIRE(match(two, asks(2, 200, "100", "5")).fills[0].accounting.fill.quantity == quantity("5"));
}

void test_corrupt_working_orders_are_rejected_on_recovery() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "10", "102", 100), market(1, 100))));
    const PaperOrderProcessorRecoveryState valid = paper.recovery_state();
    static_cast<void>(PaperOrderProcessor::restore(valid));

    const auto rejects = [&](auto corrupt) {
        PaperOrderProcessorRecoveryState state = valid;
        corrupt(state);
        REQUIRE_THROWS_AS(PaperOrderProcessor::restore(std::move(state)), std::invalid_argument);
    };
    rejects([](auto& state) { state.working_orders[0].fill_count = 0; });
    rejects([](auto& state) { state.working_orders[0].fill_count = 2; });
    rejects([](auto& state) { state.working_orders[0].filled_quantity = quantity("10"); });
    rejects([](auto& state) { state.working_orders[0].intent.time_in_force = TimeInForce::ImmediateOrCancel; });
    rejects([](auto& state) { state.working_orders[0].intent.type = OrderType::Market; });
    rejects([](auto& state) { state.working_orders[0].intent.account_id = "account-2"; });
    rejects([](auto& state) { state.working_orders[0].accepted_ingest_sequence = 2; });
    rejects([](auto& state) { state.working_orders[0].fee = money("-0.01"); });
    rejects([](auto& state) { state.working_orders[0].updated_at = Timestamp::from_nanoseconds(99); });
    rejects([](auto& state) { state.working_orders.push_back(state.working_orders[0]); });
    rejects([](auto& state) { state.execution.processed_client_order_ids.clear(); });
    // Consistent accounting, but a reservation larger than the cash.
    rejects([](auto& state) { state.working_orders[0].intent.limit_price = price("2000"); });
    rejects([](auto& state) {
        state.working_orders[0].intent.limit_price = price("9000000000");
        state.working_orders[0].intent.quantity = quantity("100");
    });
}

// Reservations and the market timeline make an accounting rejection of a
// matched fill impossible for a consistent account, so one halts the
// account: the order faults, its fill is not booked, and nothing else
// fills until the account is repaired. Cancellation still works.
void test_accounting_rejection_while_matching_halts_the_account() {
    PaperOrderProcessor paper = processor();
    static_cast<void>(receipt(paper.submit(gtc("b1", Side::Buy, "10", "102", 100), market(1, 100))));
    static_cast<void>(receipt(paper.submit(gtc("b2", Side::Buy, "1", "100", 100), market(1, 100))));
    PaperOrderProcessorRecoveryState state = paper.recovery_state();
    // A position whose latest fill is later than any new observation.
    state.account.state.positions.at(InstrumentId("AAPL")).last_fill_timestamp =
        Timestamp::from_nanoseconds(5'000);
    PaperOrderProcessor damaged = PaperOrderProcessor::restore(std::move(state));
    const Money cash = damaged.account().state().cash;

    const WorkingOrderMatch faulted =
        damaged.match(asks(2, 200, "100", "10"), Timestamp::from_nanoseconds(200));
    REQUIRE(faulted.fills.empty());
    REQUIRE(faulted.fault.has_value());
    REQUIRE(faulted.changed_account());
    REQUIRE(faulted.fault->order.intent.client_order_id == "b1");
    REQUIRE(faulted.fault->unbooked_fill.fill_id == "b1:fill:1");
    REQUIRE(
        faulted.fault->accounting_error.code ==
        chronos::ApplyFillErrorCode::TimestampMovedBackward
    );
    REQUIRE(!damaged.is_healthy());
    REQUIRE(damaged.account().state().cash == cash);
    REQUIRE(damaged.working_orders().size() == 1);
    REQUIRE(damaged.reservations().reserved_cash == money("100.01"));

    REQUIRE(!damaged.match(asks(3, 300, "100", "10"), Timestamp::from_nanoseconds(300)).changed_account());
    REQUIRE(std::holds_alternative<WorkingOrderCancellation>(
        damaged.cancel("b2", Timestamp::from_nanoseconds(300))
    ));
    REQUIRE(damaged.reservations() == AccountReservations{});
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"market orders cannot be GTC", test_market_orders_cannot_be_good_til_cancelled},
        {"non-marketable order rests", test_non_marketable_order_rests_without_taking_liquidity},
        {"marketable order fills and rests", test_marketable_order_fills_now_and_rests_the_remainder},
        {"complete fill does not rest", test_order_that_fills_completely_does_not_rest},
        {"FIFO priority and snapshot reset", test_priority_is_acceptance_order_and_new_snapshots_reset},
        {"ineligible observations are skipped", test_ineligible_observations_are_skipped_without_cancelling},
        {"working sell reserves shares", test_working_sell_reserves_shares_and_fills_at_the_bid},
        {"sell partial fill releases fee", test_sell_with_immediate_partial_fill_releases_its_fee_reservation},
        {"price improvement releases cash", test_price_improvement_releases_buying_power},
        {"cancellation releases remainder", test_cancellation_releases_the_remainder},
        {"halted account can cancel", test_halted_account_can_cancel},
        {"no double reservation", test_reservations_cannot_be_spent_twice},
        {"limits use limit price and working buys", test_limits_use_the_limit_price_and_working_buys},
        {"overflow fails closed", test_overflowing_reservation_fails_closed},
        {"rounding shortfall is skipped", test_rounding_shortfall_is_skipped_not_booked},
        {"skipped order keeps priority", test_skipped_order_keeps_priority_for_the_observation},
        {"independent and deterministic", test_accounts_do_not_share_liquidity_and_replay_deterministically},
        {"corrupt working orders rejected", test_corrupt_working_orders_are_rejected_on_recovery},
        {"accounting rejection halts", test_accounting_rejection_while_matching_halts_the_account},
    });
}
