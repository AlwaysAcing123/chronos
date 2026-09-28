#include "chronos/pre_trade_risk.hpp"
#include "chronos/top_of_book_execution.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using chronos::ApprovedOrderIntent;
using chronos::BookLevel;
using chronos::Fill;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperAccount;
using chronos::PreTradeRiskDecision;
using chronos::PreTradeRiskEngine;
using chronos::PreTradeRiskErrorCode;
using chronos::PreTradeRiskLimits;
using chronos::PreTradeRiskRejection;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedCancellationReason;
using chronos::SimulatedExecutionError;
using chronos::SimulatedExecutionErrorCode;
using chronos::SimulatedExecutionReport;
using chronos::SimulatedExecutionResult;
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TopOfBookExecutionSimulator;

static_assert(!std::is_default_constructible_v<ApprovedOrderIntent>);
static_assert(!std::is_copy_constructible_v<TopOfBookExecutionSimulator>);
static_assert(std::is_move_constructible_v<TopOfBookExecutionSimulator>);

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

PreTradeRiskLimits standard_limits() {
    return PreTradeRiskLimits{
        .maximum_order_quantity = quantity("1000"),
        .maximum_order_notional = money("1000000"),
        .maximum_position_quantity = quantity("2000"),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = money("0.01"),
    };
}

TopOfBookEvent market(
    std::uint64_t sequence = 10,
    std::int64_t receive_time = 100,
    std::string stream = "market-stream",
    std::string instrument = "AAPL",
    std::optional<BookLevel> bid = BookLevel{
        .price = price("99"),
        .quantity = quantity("5"),
    },
    std::optional<BookLevel> ask = BookLevel{
        .price = price("101"),
        .quantity = quantity("5"),
    },
    MarketDataQuality quality = MarketDataQuality{}
) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = InstrumentId(std::move(instrument)),
            .stream = MarketDataStreamId(std::move(stream)),
            .venue = std::nullopt,
            .event_timestamp = Timestamp::from_nanoseconds(receive_time - 1),
            .receive_timestamp = Timestamp::from_nanoseconds(receive_time),
            .ingest_sequence = sequence,
            .venue_sequence = std::nullopt,
            .quality = quality,
        },
        .bid = bid,
        .ask = ask,
    };
}

OrderIntent order(
    std::string id,
    Side side = Side::Buy,
    std::string_view order_quantity = "1",
    OrderType type = OrderType::Market,
    std::optional<Price> limit_price = std::nullopt,
    OrderSourceKind source_kind = OrderSourceKind::Manual,
    std::string source_id = "user-1",
    std::int64_t submitted_at = 110,
    std::string account_id = "account-1",
    std::string instrument = "AAPL"
) {
    return OrderIntent{
        .client_order_id = std::move(id),
        .account_id = std::move(account_id),
        .source = OrderSource{
            .kind = source_kind,
            .source_id = std::move(source_id),
        },
        .instrument = InstrumentId(std::move(instrument)),
        .side = side,
        .type = type,
        .quantity = quantity(order_quantity),
        .limit_price = limit_price,
        .time_in_force = TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(submitted_at),
    };
}

PaperAccount account_with_position(
    std::string_view position,
    std::string_view starting_cash = "10000"
) {
    PaperAccount account(
        "account-1",
        InstrumentId("AAPL"),
        money(starting_cash)
    );
    if (quantity(position).raw() == 0) {
        return account;
    }
    const auto result = account.apply_fill(Fill{
        .fill_id = "seed-position",
        .instrument = InstrumentId("AAPL"),
        .side = Side::Buy,
        .price = price("10"),
        .quantity = quantity(position),
        .fee = Money{},
        .timestamp = Timestamp::from_nanoseconds(1),
    });
    REQUIRE(std::holds_alternative<chronos::ApplyFillReceipt>(result));
    return account;
}

ApprovedOrderIntent require_approval(PreTradeRiskDecision decision) {
    REQUIRE(std::holds_alternative<ApprovedOrderIntent>(decision));
    return std::get<ApprovedOrderIntent>(std::move(decision));
}

const PreTradeRiskRejection& require_rejection(
    const PreTradeRiskDecision& decision,
    PreTradeRiskErrorCode code
) {
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(decision));
    const auto& error = std::get<PreTradeRiskRejection>(decision);
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

SimulatedExecutionReport require_report(SimulatedExecutionResult result) {
    REQUIRE(std::holds_alternative<SimulatedExecutionReport>(result));
    return std::get<SimulatedExecutionReport>(std::move(result));
}

const SimulatedExecutionError& require_execution_error(
    const SimulatedExecutionResult& result,
    SimulatedExecutionErrorCode code
) {
    REQUIRE(std::holds_alternative<SimulatedExecutionError>(result));
    const auto& error = std::get<SimulatedExecutionError>(result);
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

void test_configuration_is_validated() {
    auto limits = standard_limits();
    limits.maximum_order_quantity = Quantity{};
    REQUIRE_THROWS_AS(PreTradeRiskEngine(limits), std::invalid_argument);

    limits = standard_limits();
    limits.maximum_order_notional = Money{};
    REQUIRE_THROWS_AS(PreTradeRiskEngine(limits), std::invalid_argument);

    limits = standard_limits();
    limits.maximum_position_quantity = Quantity{};
    REQUIRE_THROWS_AS(PreTradeRiskEngine(limits), std::invalid_argument);

    limits = standard_limits();
    limits.maximum_quote_age_nanoseconds = -1;
    REQUIRE_THROWS_AS(PreTradeRiskEngine(limits), std::invalid_argument);

    limits = standard_limits();
    limits.fee_per_fill = money("-0.01");
    REQUIRE_THROWS_AS(PreTradeRiskEngine(limits), std::invalid_argument);

    REQUIRE_THROWS_AS(
        TopOfBookExecutionSimulator("", InstrumentId("AAPL")),
        std::invalid_argument
    );
}

void test_manual_and_model_orders_share_the_same_path() {
    PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent quote = market();
    const auto manual = require_approval(risk.evaluate(
        order("manual", Side::Buy, "2"),
        account,
        quote
    ));
    const auto model = require_approval(risk.evaluate(
        order(
            "model",
            Side::Buy,
            "2",
            OrderType::Market,
            std::nullopt,
            OrderSourceKind::Model,
            "model-v1"
        ),
        account,
        quote
    ));

    TopOfBookExecutionSimulator manual_simulator(
        "account-1",
        InstrumentId("AAPL")
    );
    TopOfBookExecutionSimulator model_simulator(
        "account-1",
        InstrumentId("AAPL")
    );
    const auto manual_report = require_report(
        manual_simulator.execute(manual)
    );
    const auto model_report = require_report(model_simulator.execute(model));

    REQUIRE(manual_report.status == SimulatedExecutionStatus::Filled);
    REQUIRE(model_report.status == SimulatedExecutionStatus::Filled);
    REQUIRE(manual_report.fill->price == model_report.fill->price);
    REQUIRE(manual_report.fill->quantity == model_report.fill->quantity);
    REQUIRE(manual_report.fill->fee == model_report.fill->fee);
    REQUIRE(manual_report.fill->price == price("101"));
    REQUIRE(manual_report.fill->quantity == quantity("2"));
    REQUIRE(manual_report.fill->fee == money("0.01"));
    REQUIRE(manual.execution_price() == price("101"));
    REQUIRE(manual.maximum_order_notional() == money("202"));
}

void test_limit_orders_obey_price_constraints() {
    PaperAccount buyer = account_with_position("0");
    PaperAccount seller = account_with_position("5");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent quote = market();

    TopOfBookExecutionSimulator buy_simulator(
        "account-1",
        InstrumentId("AAPL")
    );
    const auto marketable_buy = require_report(buy_simulator.execute(
        require_approval(risk.evaluate(
            order(
                "buy-fill",
                Side::Buy,
                "1",
                OrderType::Limit,
                price("101")
            ),
            buyer,
            quote
        ))
    ));
    REQUIRE(marketable_buy.status == SimulatedExecutionStatus::Filled);

    const auto passive_buy = require_report(buy_simulator.execute(
        require_approval(risk.evaluate(
            order(
                "buy-cancel",
                Side::Buy,
                "1",
                OrderType::Limit,
                price("100")
            ),
            buyer,
            quote
        ))
    ));
    REQUIRE(passive_buy.status == SimulatedExecutionStatus::Cancelled);
    REQUIRE(!passive_buy.fill.has_value());
    REQUIRE(
        passive_buy.cancellation_reason ==
        SimulatedCancellationReason::LimitNotMarketable
    );

    TopOfBookExecutionSimulator sell_simulator(
        "account-1",
        InstrumentId("AAPL")
    );
    const auto marketable_sell = require_report(sell_simulator.execute(
        require_approval(risk.evaluate(
            order(
                "sell-fill",
                Side::Sell,
                "1",
                OrderType::Limit,
                price("99")
            ),
            seller,
            quote
        ))
    ));
    REQUIRE(marketable_sell.fill->price == price("99"));

    const auto passive_sell = require_report(sell_simulator.execute(
        require_approval(risk.evaluate(
            order(
                "sell-cancel",
                Side::Sell,
                "1",
                OrderType::Limit,
                price("100")
            ),
            seller,
            quote
        ))
    ));
    REQUIRE(passive_sell.status == SimulatedExecutionStatus::Cancelled);
    REQUIRE(
        passive_sell.cancellation_reason ==
        SimulatedCancellationReason::LimitNotMarketable
    );
}

void test_displayed_liquidity_is_not_reused_before_a_new_quote() {
    PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent first_quote = market();
    TopOfBookExecutionSimulator simulator(
        "account-1",
        InstrumentId("AAPL")
    );

    const auto first = require_report(simulator.execute(require_approval(
        risk.evaluate(order("first", Side::Buy, "3"), account, first_quote)
    )));
    REQUIRE(first.status == SimulatedExecutionStatus::Filled);
    REQUIRE(first.fill->quantity == quantity("3"));

    const auto second = require_report(simulator.execute(require_approval(
        risk.evaluate(order("second", Side::Buy, "4"), account, first_quote)
    )));
    REQUIRE(
        second.status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(second.fill->quantity == quantity("2"));
    REQUIRE(second.unfilled_quantity == quantity("2"));
    REQUIRE(
        second.cancellation_reason ==
        SimulatedCancellationReason::DisplayedLiquidityExhausted
    );

    const auto exhausted = require_report(simulator.execute(require_approval(
        risk.evaluate(order("exhausted", Side::Buy), account, first_quote)
    )));
    REQUIRE(exhausted.status == SimulatedExecutionStatus::Cancelled);
    REQUIRE(!exhausted.fill.has_value());
    REQUIRE(exhausted.unfilled_quantity == quantity("1"));

    const TopOfBookEvent next_quote = market(11);
    const auto replenished = require_report(simulator.execute(require_approval(
        risk.evaluate(order("replenished", Side::Buy), account, next_quote)
    )));
    REQUIRE(replenished.status == SimulatedExecutionStatus::Filled);
    REQUIRE(replenished.fill->quantity == quantity("1"));
}

void test_bid_and_ask_liquidity_are_consumed_independently() {
    const PaperAccount buyer = account_with_position("0");
    const PaperAccount seller = account_with_position("5");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent quote = market();
    TopOfBookExecutionSimulator simulator(
        "account-1",
        InstrumentId("AAPL")
    );

    const auto buy = require_report(simulator.execute(require_approval(
        risk.evaluate(order("buy", Side::Buy, "5"), buyer, quote)
    )));
    REQUIRE(buy.status == SimulatedExecutionStatus::Filled);

    const auto sell = require_report(simulator.execute(require_approval(
        risk.evaluate(order("sell", Side::Sell, "5"), seller, quote)
    )));
    REQUIRE(sell.status == SimulatedExecutionStatus::Filled);
    REQUIRE(sell.fill->price == price("99"));
    REQUIRE(sell.fill->quantity == quantity("5"));
}

void test_duplicate_orders_are_rejected_without_consuming_more_liquidity() {
    PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent quote = market();
    const auto approved = require_approval(risk.evaluate(
        order("duplicate", Side::Buy, "2"),
        account,
        quote
    ));
    TopOfBookExecutionSimulator simulator(
        "account-1",
        InstrumentId("AAPL")
    );

    require_report(simulator.execute(approved));
    require_execution_error(
        simulator.execute(approved),
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );

    const auto cancelled_order = require_approval(risk.evaluate(
        order(
            "duplicate-cancel",
            Side::Buy,
            "1",
            OrderType::Limit,
            price("100")
        ),
        account,
        quote
    ));
    const auto cancelled = require_report(simulator.execute(cancelled_order));
    REQUIRE(cancelled.status == SimulatedExecutionStatus::Cancelled);
    require_execution_error(
        simulator.execute(cancelled_order),
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );

    const auto remaining = require_report(simulator.execute(require_approval(
        risk.evaluate(order("remaining", Side::Buy, "4"), account, quote)
    )));
    REQUIRE(remaining.fill->quantity == quantity("3"));
    REQUIRE(remaining.unfilled_quantity == quantity("1"));
}

void test_order_shape_validation_is_structured() {
    const PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent quote = market();

    auto candidate = order("valid");
    candidate.client_order_id.clear();
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::MissingClientOrderId
    );

    candidate = order("valid");
    candidate.account_id.clear();
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::MissingAccountId
    );

    candidate = order("valid");
    candidate.source.source_id.clear();
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::MissingSourceId
    );

    candidate = order("valid");
    candidate.source.kind = static_cast<OrderSourceKind>(99);
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::UnknownOrderSource
    );

    candidate = order("valid");
    candidate.side = static_cast<Side>(99);
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::UnknownSide
    );

    candidate = order("valid");
    candidate.type = static_cast<OrderType>(99);
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::UnknownOrderType
    );

    candidate = order("valid");
    candidate.time_in_force = static_cast<TimeInForce>(99);
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::UnsupportedTimeInForce
    );

    candidate = order("valid");
    candidate.quantity = Quantity{};
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::NonpositiveQuantity
    );

    candidate = order("valid");
    candidate.limit_price = price("100");
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::UnexpectedLimitPrice
    );

    candidate = order("valid", Side::Buy, "1", OrderType::Limit);
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::MissingLimitPrice
    );

    candidate = order(
        "valid",
        Side::Buy,
        "1",
        OrderType::Limit,
        Price{}
    );
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::NonpositiveLimitPrice
    );

    candidate = order("valid");
    candidate.submitted_at = Timestamp::from_nanoseconds(-1);
    require_rejection(
        risk.evaluate(candidate, account, quote),
        PreTradeRiskErrorCode::NegativeSubmissionTimestamp
    );
}

void test_account_and_market_boundaries_are_enforced() {
    const PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const TopOfBookEvent quote = market();

    require_rejection(
        risk.evaluate(
            order(
                "wrong-account",
                Side::Buy,
                "1",
                OrderType::Market,
                std::nullopt,
                OrderSourceKind::Manual,
                "user-1",
                110,
                "other-account"
            ),
            account,
            quote
        ),
        PreTradeRiskErrorCode::AccountMismatch
    );
    require_rejection(
        risk.evaluate(
            order(
                "wrong-instrument",
                Side::Buy,
                "1",
                OrderType::Market,
                std::nullopt,
                OrderSourceKind::Manual,
                "user-1",
                110,
                "account-1",
                "SPY"
            ),
            account,
            quote
        ),
        PreTradeRiskErrorCode::InstrumentMismatch
    );
    require_rejection(
        risk.evaluate(order("market-mismatch"), account, market(
            10,
            100,
            "market-stream",
            "SPY"
        )),
        PreTradeRiskErrorCode::MarketInstrumentMismatch
    );

    TopOfBookEvent invalid = quote;
    invalid.ask->price = Price{};
    require_rejection(
        risk.evaluate(order("invalid-market"), account, invalid),
        PreTradeRiskErrorCode::InvalidMarketState
    );

    const auto suspect_quality = MarketDataQuality{}.with(
        MarketDataQualityFlag::PotentiallyInvalidBook
    );
    require_rejection(
        risk.evaluate(
            order("suspect"),
            account,
            market(
                10,
                100,
                "market-stream",
                "AAPL",
                BookLevel{price("99"), quantity("5")},
                BookLevel{price("101"), quantity("5")},
                suspect_quality
            )
        ),
        PreTradeRiskErrorCode::SuspectMarketState
    );

    const auto questionable_timestamp = MarketDataQuality{}.with(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    );
    require_rejection(
        risk.evaluate(
            order("questionable-timestamp"),
            account,
            market(
                10,
                100,
                "market-stream",
                "AAPL",
                BookLevel{price("99"), quantity("5")},
                BookLevel{price("101"), quantity("5")},
                questionable_timestamp
            )
        ),
        PreTradeRiskErrorCode::SuspectMarketState
    );

    require_rejection(
        risk.evaluate(order("future", Side::Buy, "1", OrderType::Market,
            std::nullopt, OrderSourceKind::Manual, "user-1", 99), account,
            quote),
        PreTradeRiskErrorCode::MarketObservationFromFuture
    );
    require_rejection(
        risk.evaluate(order("stale", Side::Buy, "1", OrderType::Market,
            std::nullopt, OrderSourceKind::Manual, "user-1", 1'101), account,
            quote),
        PreTradeRiskErrorCode::StaleMarketState
    );

    require_rejection(
        risk.evaluate(
            order("missing-ask"),
            account,
            market(10, 100, "market-stream", "AAPL",
                BookLevel{price("99"), quantity("5")}, std::nullopt)
        ),
        PreTradeRiskErrorCode::MissingOppositeBookSide
    );
}

void test_quantity_notional_cash_and_position_limits() {
    PaperAccount empty = account_with_position("0", "1000");
    PaperAccount low_cash = account_with_position("0", "999.90");
    PaperAccount owned = account_with_position("5", "1000");
    auto limits = standard_limits();
    limits.maximum_order_quantity = quantity("10");
    limits.maximum_order_notional = money("1000");
    limits.maximum_position_quantity = quantity("10");
    const PreTradeRiskEngine risk(limits);
    const TopOfBookEvent quote = market();

    require_rejection(
        risk.evaluate(order("quantity", Side::Buy, "11"), empty, quote),
        PreTradeRiskErrorCode::OrderQuantityLimitExceeded
    );
    require_rejection(
        risk.evaluate(order("notional", Side::Buy, "10"), empty, quote),
        PreTradeRiskErrorCode::OrderNotionalLimitExceeded
    );
    require_rejection(
        risk.evaluate(order("cash", Side::Buy, "9.9"), low_cash, quote),
        PreTradeRiskErrorCode::InsufficientCash
    );
    require_rejection(
        risk.evaluate(order("position", Side::Buy, "6"), owned, quote),
        PreTradeRiskErrorCode::PositionLimitExceeded
    );
    require_rejection(
        risk.evaluate(order("sell", Side::Sell, "6"), owned, quote),
        PreTradeRiskErrorCode::InsufficientPosition
    );

    const OrderIntent old_order = order(
        "old-submission",
        Side::Sell,
        "1",
        OrderType::Market,
        std::nullopt,
        OrderSourceKind::Manual,
        "user-1",
        0
    );
    require_rejection(
        risk.evaluate(old_order, owned, quote),
        PreTradeRiskErrorCode::SubmissionTimestampMovedBackward
    );
}

void test_fee_zero_notional_and_overflow_rules() {
    auto fee_limits = standard_limits();
    fee_limits.fee_per_fill = money("0.01");
    const PreTradeRiskEngine fee_risk(fee_limits);
    PaperAccount no_cash = account_with_position("1", "10");
    REQUIRE(no_cash.state().cash == Money{});
    require_rejection(
        fee_risk.evaluate(order("fee", Side::Sell), no_cash, market()),
        PreTradeRiskErrorCode::InsufficientCashForFee
    );

    const PreTradeRiskEngine risk(standard_limits());
    const PaperAccount normal = account_with_position("0");
    const TopOfBookEvent tiny_market = market(
        10,
        100,
        "market-stream",
        "AAPL",
        BookLevel{Price::from_raw(1), Quantity::from_raw(1)},
        BookLevel{Price::from_raw(1), Quantity::from_raw(1)}
    );
    OrderIntent tiny_order = order("tiny");
    tiny_order.quantity = Quantity::from_raw(1);
    require_rejection(
        risk.evaluate(tiny_order, normal, tiny_market),
        PreTradeRiskErrorCode::NotionalRoundedToZero
    );

    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const PreTradeRiskEngine maximum_risk(PreTradeRiskLimits{
        .maximum_order_quantity = Quantity::from_raw(maximum),
        .maximum_order_notional = Money::from_raw(maximum),
        .maximum_position_quantity = Quantity::from_raw(maximum),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = Money{},
    });
    const PaperAccount maximum_account(
        "account-1",
        InstrumentId("AAPL"),
        Money::from_raw(maximum)
    );
    const TopOfBookEvent huge_market = market(
        10,
        100,
        "market-stream",
        "AAPL",
        BookLevel{Price::from_raw(maximum), Quantity::from_raw(maximum)},
        BookLevel{Price::from_raw(maximum), Quantity::from_raw(maximum)}
    );
    OrderIntent huge_order = order("overflow");
    huge_order.quantity = Quantity::from_raw(maximum);
    require_rejection(
        maximum_risk.evaluate(huge_order, maximum_account, huge_market),
        PreTradeRiskErrorCode::ArithmeticOverflow
    );
}

void test_execution_rejects_wrong_account_instrument_and_market_lineage() {
    const PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const auto first = require_approval(risk.evaluate(
        order("first"),
        account,
        market(10)
    ));

    TopOfBookExecutionSimulator wrong_account(
        "other-account",
        InstrumentId("AAPL")
    );
    require_execution_error(
        wrong_account.execute(first),
        SimulatedExecutionErrorCode::AccountMismatch
    );
    TopOfBookExecutionSimulator wrong_instrument(
        "account-1",
        InstrumentId("SPY")
    );
    require_execution_error(
        wrong_instrument.execute(first),
        SimulatedExecutionErrorCode::InstrumentMismatch
    );

    TopOfBookExecutionSimulator simulator(
        "account-1",
        InstrumentId("AAPL")
    );
    require_report(simulator.execute(first));

    const auto stale = require_approval(risk.evaluate(
        order("stale"),
        account,
        market(9)
    ));
    require_execution_error(
        simulator.execute(stale),
        SimulatedExecutionErrorCode::StaleMarketApproval
    );

    const auto other_stream = require_approval(risk.evaluate(
        order("other-stream"),
        account,
        market(11, 100, "other-stream")
    ));
    require_execution_error(
        simulator.execute(other_stream),
        SimulatedExecutionErrorCode::MarketStreamMismatch
    );

    const auto changed_snapshot = require_approval(risk.evaluate(
        order("changed-snapshot"),
        account,
        market(
            10,
            100,
            "market-stream",
            "AAPL",
            BookLevel{price("99"), quantity("5")},
            BookLevel{price("101"), quantity("6")}
        )
    ));
    require_execution_error(
        simulator.execute(changed_snapshot),
        SimulatedExecutionErrorCode::MarketSnapshotMismatch
    );
}

void test_fill_identity_and_metadata_are_deterministic() {
    const PaperAccount account = account_with_position("0");
    const PreTradeRiskEngine risk(standard_limits());
    const auto approved = require_approval(risk.evaluate(
        order("deterministic", Side::Buy, "2"),
        account,
        market(42)
    ));
    TopOfBookExecutionSimulator simulator(
        "account-1",
        InstrumentId("AAPL")
    );
    const auto report = require_report(simulator.execute(approved));

    REQUIRE(report.intent.source.kind == OrderSourceKind::Manual);
    REQUIRE(report.market_ingest_sequence == 42);
    REQUIRE(report.fill->fill_id == "deterministic:fill:0");
    REQUIRE(report.fill->timestamp == Timestamp::from_nanoseconds(110));
    REQUIRE(report.fill->side == Side::Buy);
    REQUIRE(report.unfilled_quantity == Quantity{});
    REQUIRE(!report.cancellation_reason.has_value());
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"configuration validation", test_configuration_is_validated},
        {
            "manual and model shared path",
            test_manual_and_model_orders_share_the_same_path,
        },
        {"limit price constraints", test_limit_orders_obey_price_constraints},
        {
            "shadow displayed liquidity",
            test_displayed_liquidity_is_not_reused_before_a_new_quote,
        },
        {
            "independent bid and ask liquidity",
            test_bid_and_ask_liquidity_are_consumed_independently,
        },
        {
            "duplicate order idempotency",
            test_duplicate_orders_are_rejected_without_consuming_more_liquidity,
        },
        {"order shape validation", test_order_shape_validation_is_structured},
        {
            "account and market boundaries",
            test_account_and_market_boundaries_are_enforced,
        },
        {
            "quantity notional cash and position limits",
            test_quantity_notional_cash_and_position_limits,
        },
        {
            "fee zero notional and overflow",
            test_fee_zero_notional_and_overflow_rules,
        },
        {
            "execution lineage errors",
            test_execution_rejects_wrong_account_instrument_and_market_lineage,
        },
        {
            "deterministic fill metadata",
            test_fill_identity_and_metadata_are_deterministic,
        },
    });
}
