#include "chronos/market_data_types.hpp"
#include "chronos/paper_account.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::AccountState;
using chronos::ApplyFillError;
using chronos::ApplyFillErrorCode;
using chronos::ApplyFillReceipt;
using chronos::Fill;
using chronos::InstrumentId;
using chronos::InstrumentDefinitionEvent;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEvent;
using chronos::MarketEventHeader;
using chronos::MarketStatusEvent;
using chronos::Money;
using chronos::PaperAccount;
using chronos::PaperAccountRecoveryState;
using chronos::PositionState;
using chronos::Price;
using chronos::Quantity;
using chronos::RoundingMode;
using chronos::Side;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::VenueId;

static_assert(!std::is_same_v<InstrumentId, VenueId>);
static_assert(!std::is_same_v<InstrumentId, MarketDataStreamId>);

Money money(std::string_view value) {
    return Money::parse(value);
}

Price price(std::string_view value) {
    return Price::parse(value);
}

Quantity quantity(std::string_view value) {
    return Quantity::parse(value);
}

Fill fill(
    std::string id,
    Side side,
    std::string_view fill_price,
    std::string_view fill_quantity,
    std::string_view fee,
    std::int64_t timestamp,
    std::string instrument = "AAPL"
) {
    return Fill{
        .fill_id = std::move(id),
        .instrument = InstrumentId(std::move(instrument)),
        .side = side,
        .price = price(fill_price),
        .quantity = quantity(fill_quantity),
        .fee = money(fee),
        .timestamp = Timestamp::from_nanoseconds(timestamp),
    };
}

ApplyFillReceipt receipt(const chronos::ApplyFillResult& result) {
    REQUIRE(std::holds_alternative<ApplyFillReceipt>(result));
    return std::get<ApplyFillReceipt>(result);
}

const ApplyFillError& error(const chronos::ApplyFillResult& result) {
    REQUIRE(std::holds_alternative<ApplyFillError>(result));
    return std::get<ApplyFillError>(result);
}

void require_error(
    const chronos::ApplyFillResult& result,
    ApplyFillErrorCode expected
) {
    REQUIRE(error(result).code == expected);
}

void require_unchanged_after_rejection(
    PaperAccount& account,
    const Fill& rejected_fill,
    ApplyFillErrorCode expected
) {
    const AccountState before = account.state();
    require_error(account.apply_fill(rejected_fill), expected);
    REQUIRE(account.state() == before);
}

const PositionState& ledger(const PaperAccount& account, std::string_view instrument) {
    const PositionState* position =
        account.state().position(InstrumentId(std::string(instrument)));
    REQUIRE(position != nullptr);
    return *position;
}

std::map<InstrumentId, Price> marks(
    std::initializer_list<std::pair<std::string_view, std::string_view>> values
) {
    std::map<InstrumentId, Price> result;
    for (const auto& [instrument, mark] : values) {
        result.emplace(InstrumentId(std::string(instrument)), price(mark));
    }
    return result;
}

PositionState position_state(
    std::int64_t quantity_raw,
    std::int64_t cost_raw,
    std::int64_t realized_raw,
    std::int64_t fees_raw,
    std::int64_t last_fill_ns
) {
    return PositionState{
        .quantity = Quantity::from_raw(quantity_raw),
        .open_cost_basis = Money::from_raw(cost_raw),
        .gross_realized_pnl = Money::from_raw(realized_raw),
        .fees_paid = Money::from_raw(fees_raw),
        .last_fill_timestamp = Timestamp::from_nanoseconds(last_fill_ns),
    };
}

PaperAccountRecoveryState recovery(
    std::int64_t starting_cash_raw,
    std::int64_t cash_raw,
    std::map<InstrumentId, PositionState> positions
) {
    std::vector<std::string> fill_ids;
    for (const auto& [instrument, position] : positions) {
        fill_ids.push_back(instrument.value() + "-fill");
    }
    return PaperAccountRecoveryState{
        .account_id = "acct",
        .state = AccountState{
            .starting_cash = Money::from_raw(starting_cash_raw),
            .cash = Money::from_raw(cash_raw),
            .positions = std::move(positions),
        },
        .applied_fill_ids = std::move(fill_ids),
    };
}

void test_decimal_parsing_and_formatting() {
    REQUIRE(Money::scale() == 100'000'000);
    REQUIRE(Price::scale() == 1'000'000'000);
    REQUIRE(Quantity::scale() == 100'000'000);
    REQUIRE(money("0").raw() == 0);
    REQUIRE(money("1").raw() == 100'000'000);
    REQUIRE(money("+12.34").raw() == 1'234'000'000);
    REQUIRE(money("-0.00000001").raw() == -1);
    REQUIRE(money(".5").raw() == 50'000'000);
    REQUIRE(money("1.").raw() == 100'000'000);

    REQUIRE(money("12.34000000").to_string() == "12.34");
    REQUIRE(money("-0.00000001").to_string() == "-0.00000001");
    REQUIRE(price("0.000000001").raw() == 1);
    REQUIRE(price("12.340000000").to_string() == "12.34");
    REQUIRE(Money::from_raw(std::numeric_limits<std::int64_t>::min()).to_string() ==
            "-92233720368.54775808");
}

void test_decimal_parser_rejects_invalid_input() {
    REQUIRE_THROWS_AS(money(""), std::invalid_argument);
    REQUIRE_THROWS_AS(money("+"), std::invalid_argument);
    REQUIRE_THROWS_AS(money("."), std::invalid_argument);
    REQUIRE_THROWS_AS(money("1.000000001"), std::invalid_argument);
    REQUIRE_THROWS_AS(price("1.0000000001"), std::invalid_argument);
    REQUIRE_THROWS_AS(money("1.2.3"), std::invalid_argument);
    REQUIRE_THROWS_AS(money(" 1"), std::invalid_argument);
    REQUIRE_THROWS_AS(money("1e2"), std::invalid_argument);
}

void test_decimal_parser_handles_int64_boundaries() {
    REQUIRE(money("92233720368.54775807").raw() ==
            std::numeric_limits<std::int64_t>::max());
    REQUIRE(money("-92233720368.54775808").raw() ==
            std::numeric_limits<std::int64_t>::min());
    REQUIRE_THROWS_AS(money("92233720368.54775808"), std::overflow_error);
    REQUIRE_THROWS_AS(money("-92233720368.54775809"), std::overflow_error);
    REQUIRE_THROWS_AS(money("92233720369"), std::overflow_error);

    REQUIRE(price("9223372036.854775807").raw() ==
            std::numeric_limits<std::int64_t>::max());
    REQUIRE(price("-9223372036.854775808").raw() ==
            std::numeric_limits<std::int64_t>::min());
    REQUIRE_THROWS_AS(price("9223372036.854775808"), std::overflow_error);
    REQUIRE_THROWS_AS(price("-9223372036.854775809"), std::overflow_error);
}

void test_decimal_addition_and_subtraction_detect_overflow() {
    const Money maximum = Money::from_raw(std::numeric_limits<std::int64_t>::max());
    const Money minimum = Money::from_raw(std::numeric_limits<std::int64_t>::min());
    REQUIRE((money("1.25") + money("2.75")) == money("4"));
    REQUIRE((money("1.25") - money("2.75")) == money("-1.5"));
    REQUIRE_THROWS_AS(maximum + Money::from_raw(1), std::overflow_error);
    REQUIRE_THROWS_AS(minimum - Money::from_raw(1), std::overflow_error);
}

void test_fixed_point_multiplication_rounding() {
    const Price one_money_half_tick = Price::from_raw(10);
    const Quantity half = Quantity::from_raw(50'000'000);

    REQUIRE(chronos::calculate_notional(one_money_half_tick, half).raw() == 1);
    REQUIRE(chronos::calculate_notional(
                one_money_half_tick,
                half,
                RoundingMode::TowardZero
            ).raw() == 0);
    REQUIRE(chronos::calculate_notional(Price::from_raw(-10), half).raw() == -1);
    REQUIRE(chronos::calculate_notional(
                Price::from_raw(-10),
                half,
                RoundingMode::TowardZero
            ).raw() == 0);
    REQUIRE(chronos::calculate_notional(price("100.05"), quantity("10")) ==
            money("1000.5"));
}

void test_proportions_and_average_price() {
    REQUIRE(chronos::proportional_amount(
                money("40"), quantity("1"), quantity("3")
            ) == money("13.33333333"));
    REQUIRE(chronos::average_price(money("40"), quantity("3")) ==
            price("13.333333333"));
    REQUIRE_THROWS_AS(
        chronos::proportional_amount(money("1"), quantity("2"), quantity("1")),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        chronos::average_price(money("1"), Quantity{}),
        std::invalid_argument
    );
}

void test_percentage_of_is_exact_and_rounds_half_away_from_zero() {
    using chronos::Percentage;
    using chronos::percentage_of;
    REQUIRE(percentage_of(money("27.6"), money("10000")) ==
            Percentage::parse("0.276"));
    REQUIRE(percentage_of(money("-1.5"), money("1000")) ==
            Percentage::parse("-0.15"));
    REQUIRE(percentage_of(Money{}, money("1000")) == Percentage{});
    REQUIRE(percentage_of(money("1000"), money("1000")) ==
            Percentage::parse("100"));
    REQUIRE(percentage_of(money("1"), money("3")) ==
            Percentage::parse("33.33333333"));
    REQUIRE(percentage_of(money("2"), money("3")) ==
            Percentage::parse("66.66666667"));

    // 1 raw unit of change over 2e10 raw units of base is exactly half a
    // percentage unit, which rounds away from zero in both directions.
    const Money half_unit_base = Money::from_raw(20'000'000'000);
    REQUIRE(percentage_of(Money::from_raw(1), half_unit_base).raw() == 1);
    REQUIRE(percentage_of(Money::from_raw(-1), half_unit_base).raw() == -1);
    REQUIRE(percentage_of(
                Money::from_raw(1),
                half_unit_base,
                RoundingMode::TowardZero
            ).raw() == 0);

    REQUIRE_THROWS_AS(
        percentage_of(money("1"), Money{}),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        percentage_of(money("1"), money("-1")),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        percentage_of(
            Money::from_raw(std::numeric_limits<std::int64_t>::max()),
            Money::from_raw(1)
        ),
        std::overflow_error
    );
}

void test_price_notional_average_round_trip() {
    const std::vector<std::pair<Price, Quantity>> exactly_representable_cases{
        {price("0.000000001"), quantity("10")},
        {price("0.00000001"), quantity("1")},
        {price("0.01"), quantity("100")},
        {price("100.123456789"), quantity("10")},
        {price("1000000.12345678"), quantity("3")},
    };

    for (const auto& [original_price, traded_quantity] : exactly_representable_cases) {
        const Money notional = chronos::calculate_notional(
            original_price,
            traded_quantity
        );
        REQUIRE(chronos::average_price(notional, traded_quantity) == original_price);
    }

    REQUIRE(chronos::calculate_notional(
                price("100.123456789"), quantity("10")
            ) == money("1001.23456789"));
}

void test_account_constructor_validation() {
    REQUIRE_THROWS_AS(PaperAccount("", money("1")), std::invalid_argument);
    REQUIRE_THROWS_AS(InstrumentId(""), std::invalid_argument);
    REQUIRE_THROWS_AS(
        PaperAccount("acct", money("-0.00000001")),
        std::invalid_argument
    );

    const PaperAccount account("acct", money("0"));
    REQUIRE(account.account_id() == "acct");
    REQUIRE(account.state().cash == money("0"));
    REQUIRE(account.state().positions.empty());
    REQUIRE(account.state().open_cost_basis() == money("0"));
    REQUIRE(account.state().gross_realized_pnl() == money("0"));
    REQUIRE(account.state().fees_paid() == money("0"));
    REQUIRE(!account.state().latest_fill_timestamp().has_value());
    REQUIRE(account.state().quantity(InstrumentId("AAPL")) == Quantity{});
}

void test_primary_buy_sell_and_snapshot_example() {
    PaperAccount account("acct", money("10000"));

    const auto buy_result = account.apply_fill(
        fill("buy-1", Side::Buy, "100.05", "10", "1", 100)
    );
    const auto buy = receipt(buy_result);
    REQUIRE(buy.notional == money("1000.5"));
    REQUIRE(buy.gross_realized_pnl_change == money("0"));
    REQUIRE(buy.before.cash == money("10000"));
    REQUIRE(buy.before.positions.empty());
    REQUIRE(buy.after.cash == money("8998.5"));
    REQUIRE(ledger(account, "AAPL").quantity == quantity("10"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("1000.5"));
    REQUIRE(ledger(account, "AAPL").average_entry_price() == price("100.05"));
    REQUIRE(ledger(account, "AAPL").fees_paid == money("1"));

    const auto sell_result = account.apply_fill(
        fill("sell-1", Side::Sell, "102.90", "4", "0.5", 200)
    );
    const auto sell = receipt(sell_result);
    REQUIRE(sell.notional == money("411.6"));
    REQUIRE(sell.gross_realized_pnl_change == money("11.4"));
    REQUIRE(account.state().cash == money("9409.6"));
    REQUIRE(ledger(account, "AAPL").quantity == quantity("6"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("600.3"));
    REQUIRE(ledger(account, "AAPL").gross_realized_pnl == money("11.4"));
    REQUIRE(ledger(account, "AAPL").fees_paid == money("1.5"));
    REQUIRE(account.state().fees_paid() == money("1.5"));

    const auto snapshot = account.snapshot(marks({{"AAPL", "103"}}));
    REQUIRE(snapshot.open_positions.size() == 1);
    REQUIRE(snapshot.open_positions[0].mark_price == price("103"));
    REQUIRE(snapshot.open_positions[0].market_value == money("618"));
    REQUIRE(snapshot.open_positions[0].gross_unrealized_pnl == money("17.7"));
    REQUIRE(snapshot.market_value == money("618"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("17.7"));
    REQUIRE(snapshot.gross_total_pnl == money("29.1"));
    REQUIRE(snapshot.net_total_pnl == money("27.6"));
    REQUIRE(snapshot.equity == money("10027.6"));
    REQUIRE(snapshot.equity - snapshot.state.starting_cash == snapshot.net_total_pnl);
}

void test_multiple_buys_partial_sale_and_complete_close() {
    PaperAccount account("acct", money("10000"));
    receipt(account.apply_fill(fill("b1", Side::Buy, "10", "2", "0", 1)));
    receipt(account.apply_fill(fill("b2", Side::Buy, "20", "1", "0", 2)));

    REQUIRE(ledger(account, "AAPL").quantity == quantity("3"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("40"));
    REQUIRE(ledger(account, "AAPL").average_entry_price() == price("13.333333333"));

    const auto partial = receipt(
        account.apply_fill(fill("s1", Side::Sell, "16", "1", "0", 3))
    );
    REQUIRE(partial.gross_realized_pnl_change == money("2.66666667"));
    REQUIRE(ledger(account, "AAPL").quantity == quantity("2"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("26.66666667"));
    REQUIRE(ledger(account, "AAPL").average_entry_price() == price("13.333333335"));

    const auto closing = receipt(
        account.apply_fill(fill("s2", Side::Sell, "15", "2", "0", 4))
    );
    REQUIRE(closing.gross_realized_pnl_change == money("3.33333333"));
    REQUIRE(account.state().cash == money("10006"));

    // A closed position keeps its ledger: quantity and cost basis are zero,
    // and the realized result stays attributed to the instrument.
    const PositionState& closed = ledger(account, "AAPL");
    REQUIRE(!closed.is_open());
    REQUIRE(closed.quantity == quantity("0"));
    REQUIRE(closed.open_cost_basis == money("0"));
    REQUIRE(closed.average_entry_price() == Price{});
    REQUIRE(closed.gross_realized_pnl == money("6"));
    REQUIRE(closed.last_fill_timestamp == Timestamp::from_nanoseconds(4));
    REQUIRE(account.state().positions.size() == 1);

    // Reopening the instrument continues the same ledger.
    receipt(account.apply_fill(fill("b3", Side::Buy, "11", "1", "0", 5)));
    REQUIRE(ledger(account, "AAPL").quantity == quantity("1"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("11"));
    REQUIRE(ledger(account, "AAPL").gross_realized_pnl == money("6"));
    REQUIRE(account.state().positions.size() == 1);
}

void test_realized_loss_and_fees() {
    PaperAccount account("acct", money("100"));
    receipt(account.apply_fill(fill("b", Side::Buy, "10", "5", "1", 1)));
    const auto sale = receipt(
        account.apply_fill(fill("s", Side::Sell, "8", "5", "2", 2))
    );

    REQUIRE(sale.gross_realized_pnl_change == money("-10"));
    REQUIRE(ledger(account, "AAPL").gross_realized_pnl == money("-10"));
    REQUIRE(ledger(account, "AAPL").fees_paid == money("3"));
    REQUIRE(account.state().cash == money("87"));

    // Only open positions need a mark; the closed AAPL ledger needs none.
    const auto snapshot = account.snapshot({});
    REQUIRE(snapshot.open_positions.empty());
    REQUIRE(snapshot.gross_total_pnl == money("-10"));
    REQUIRE(snapshot.net_total_pnl == money("-13"));
    REQUIRE(snapshot.equity == money("87"));
}

void test_empty_account_snapshot() {
    const PaperAccount account("acct", money("100"));
    const auto snapshot = account.snapshot({});
    REQUIRE(snapshot.open_positions.empty());
    REQUIRE(snapshot.market_value == money("0"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("0"));
    REQUIRE(snapshot.net_total_pnl == money("0"));
    REQUIRE(snapshot.equity == money("100"));
    // Marks for instruments the account does not hold are ignored.
    REQUIRE(account.snapshot(marks({{"AAPL", "25"}})).equity == money("100"));
}

void test_shared_cash_funds_positions_in_several_instruments() {
    PaperAccount account("acct", money("10000"));
    receipt(account.apply_fill(fill("aapl-1", Side::Buy, "200", "10", "1", 10)));
    const auto spy = receipt(
        account.apply_fill(fill("spy-1", Side::Buy, "500.5", "4", "1", 20, "SPY"))
    );

    // One cash balance pays for both instruments and both fees.
    REQUIRE(spy.before.cash == money("7999"));
    REQUIRE(account.state().cash == money("5996"));
    REQUIRE(account.state().positions.size() == 2);
    REQUIRE(ledger(account, "AAPL").quantity == quantity("10"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("2000"));
    REQUIRE(ledger(account, "AAPL").fees_paid == money("1"));
    REQUIRE(ledger(account, "SPY").quantity == quantity("4"));
    REQUIRE(ledger(account, "SPY").open_cost_basis == money("2002"));
    REQUIRE(ledger(account, "SPY").average_entry_price() == price("500.5"));
    REQUIRE(ledger(account, "SPY").fees_paid == money("1"));
    REQUIRE(account.state().open_cost_basis() == money("4002"));
    REQUIRE(account.state().fees_paid() == money("2"));
    REQUIRE(account.state().latest_fill_timestamp() == Timestamp::from_nanoseconds(20));

    // Ledgers iterate in instrument order regardless of fill order.
    std::vector<std::string> order;
    for (const auto& [instrument, position] : account.state().positions) {
        order.push_back(instrument.value());
    }
    REQUIRE((order == std::vector<std::string>{"AAPL", "SPY"}));
}

void test_a_fill_changes_only_its_own_instrument() {
    PaperAccount account("acct", money("10000"));
    receipt(account.apply_fill(fill("aapl-1", Side::Buy, "200", "10", "0", 10)));
    receipt(account.apply_fill(fill("spy-1", Side::Buy, "500", "4", "0", 20, "SPY")));
    const PositionState spy_before = ledger(account, "SPY");

    const auto sale = receipt(
        account.apply_fill(fill("aapl-2", Side::Sell, "210", "4", "0.5", 30))
    );
    REQUIRE(sale.gross_realized_pnl_change == money("40"));
    REQUIRE(ledger(account, "AAPL").quantity == quantity("6"));
    REQUIRE(ledger(account, "AAPL").open_cost_basis == money("1200"));
    REQUIRE(ledger(account, "AAPL").gross_realized_pnl == money("40"));
    REQUIRE(ledger(account, "AAPL").fees_paid == money("0.5"));
    REQUIRE(ledger(account, "SPY") == spy_before);
    REQUIRE(sale.before.positions.at(InstrumentId("SPY")) ==
        sale.after.positions.at(InstrumentId("SPY")));

    // Closing AAPL keeps SPY open and untouched.
    receipt(account.apply_fill(fill("aapl-3", Side::Sell, "190", "6", "0", 40)));
    REQUIRE(!ledger(account, "AAPL").is_open());
    REQUIRE(ledger(account, "AAPL").gross_realized_pnl == money("-20"));
    REQUIRE(ledger(account, "SPY") == spy_before);
    REQUIRE(account.state().cash == money("10000") - money("2000") -
        money("2000") + money("840") - money("0.5") + money("1140"));
}

void test_position_and_account_pnl_reconcile_exactly() {
    PaperAccount account("acct", money("100000"));
    receipt(account.apply_fill(fill("a1", Side::Buy, "100.01", "3", "0.35", 1)));
    receipt(account.apply_fill(fill("a2", Side::Buy, "99.97", "2", "0.35", 2)));
    receipt(account.apply_fill(fill("s1", Side::Buy, "500.123456789", "7", "0.35", 3, "SPY")));
    receipt(account.apply_fill(fill("a3", Side::Sell, "101.5", "4", "0.35", 4)));
    receipt(account.apply_fill(fill("s2", Side::Sell, "499", "2", "0.35", 5, "SPY")));

    const PositionState& aapl = ledger(account, "AAPL");
    const PositionState& spy = ledger(account, "SPY");
    // Different entry prices keep exact per-instrument cost bases.
    REQUIRE(aapl.quantity == quantity("1"));
    REQUIRE(aapl.open_cost_basis == money("99.994"));
    REQUIRE(aapl.gross_realized_pnl == money("6.024"));
    REQUIRE(spy.quantity == quantity("5"));
    REQUIRE(spy.open_cost_basis == money("2500.61728394"));
    REQUIRE(spy.gross_realized_pnl == money("-2.24691358"));
    REQUIRE(aapl.fees_paid == money("1.05"));
    REQUIRE(spy.fees_paid == money("0.7"));

    const auto snapshot = account.snapshot(marks({{"AAPL", "102"}, {"SPY", "501.25"}}));
    REQUIRE(snapshot.open_positions.size() == 2);
    const auto& aapl_mark = snapshot.open_positions[0];
    const auto& spy_mark = snapshot.open_positions[1];
    REQUIRE(aapl_mark.instrument == InstrumentId("AAPL"));
    REQUIRE(aapl_mark.market_value == money("102"));
    REQUIRE(aapl_mark.gross_unrealized_pnl == money("2.006"));
    REQUIRE(spy_mark.instrument == InstrumentId("SPY"));
    REQUIRE(spy_mark.market_value == money("2506.25"));
    REQUIRE(spy_mark.gross_unrealized_pnl == money("5.63271606"));

    REQUIRE(snapshot.market_value == money("2608.25"));
    REQUIRE(snapshot.open_cost_basis == money("2600.61128394"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("7.63871606"));
    REQUIRE(snapshot.gross_realized_pnl == money("3.77708642"));
    REQUIRE(snapshot.fees_paid == money("1.75"));
    REQUIRE(snapshot.gross_total_pnl == money("11.41580248"));
    REQUIRE(snapshot.net_total_pnl == money("9.66580248"));
    REQUIRE(account.state().cash == money("97401.41580248"));
    REQUIRE(snapshot.equity == account.state().cash + money("2608.25"));
    REQUIRE(snapshot.equity - snapshot.state.starting_cash == snapshot.net_total_pnl);
}

void test_snapshot_requires_a_mark_for_every_open_position() {
    PaperAccount account("acct", money("10000"));
    receipt(account.apply_fill(fill("a", Side::Buy, "10", "1", "0", 1)));
    receipt(account.apply_fill(fill("s", Side::Buy, "10", "1", "0", 1, "SPY")));
    receipt(account.apply_fill(fill("q", Side::Buy, "10", "1", "0", 1, "QQQ")));
    receipt(account.apply_fill(fill("q2", Side::Sell, "10", "1", "0", 2, "QQQ")));

    REQUIRE_THROWS_AS(account.snapshot(marks({{"AAPL", "10"}})), std::invalid_argument);
    REQUIRE_THROWS_AS(
        account.snapshot(marks({{"AAPL", "10"}, {"SPY", "0"}})),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        account.snapshot(marks({{"AAPL", "10"}, {"SPY", "-1"}})),
        std::invalid_argument
    );
    // The closed QQQ ledger needs no mark.
    const auto snapshot = account.snapshot(marks({{"AAPL", "11"}, {"SPY", "9"}}));
    REQUIRE(snapshot.open_positions.size() == 2);
    REQUIRE(snapshot.equity == money("10000"));
}

void test_fill_field_validation_is_atomic() {
    PaperAccount account("acct", money("100"));

    require_unchanged_after_rejection(
        account,
        fill("", Side::Buy, "1", "1", "0", 1),
        ApplyFillErrorCode::MissingFillId
    );
    require_unchanged_after_rejection(
        account,
        fill("zero-price", Side::Buy, "0", "1", "0", 1),
        ApplyFillErrorCode::NonpositivePrice
    );
    require_unchanged_after_rejection(
        account,
        fill("negative-price", Side::Buy, "-1", "1", "0", 1),
        ApplyFillErrorCode::NonpositivePrice
    );
    require_unchanged_after_rejection(
        account,
        fill("zero-quantity", Side::Buy, "1", "0", "0", 1),
        ApplyFillErrorCode::NonpositiveQuantity
    );
    require_unchanged_after_rejection(
        account,
        fill("negative-fee", Side::Buy, "1", "1", "-0.01", 1),
        ApplyFillErrorCode::NegativeFee
    );
    require_unchanged_after_rejection(
        account,
        fill("negative-time", Side::Buy, "1", "1", "0", -1),
        ApplyFillErrorCode::NegativeTimestamp
    );

    Fill unknown = fill("unknown-side", Side::Buy, "1", "1", "0", 1);
    unknown.side = static_cast<Side>(99);
    require_unchanged_after_rejection(
        account,
        unknown,
        ApplyFillErrorCode::UnknownSide
    );
}

void test_insufficient_cash_and_position_are_atomic() {
    PaperAccount account("acct", money("10"));
    require_unchanged_after_rejection(
        account,
        fill("too-expensive", Side::Buy, "10", "1", "0.01", 1),
        ApplyFillErrorCode::InsufficientCash
    );
    require_unchanged_after_rejection(
        account,
        fill("no-position", Side::Sell, "10", "1", "0", 1),
        ApplyFillErrorCode::InsufficientPosition
    );

    receipt(account.apply_fill(fill("buy", Side::Buy, "10", "1", "0", 1)));
    require_unchanged_after_rejection(
        account,
        fill("oversell", Side::Sell, "10", "1.00000001", "0", 2),
        ApplyFillErrorCode::InsufficientPosition
    );
    require_unchanged_after_rejection(
        account,
        fill("fee-too-large", Side::Sell, "10", "1", "10.00000001", 2),
        ApplyFillErrorCode::InsufficientCash
    );
}

void test_shared_cash_and_positions_limit_each_instrument() {
    PaperAccount account("acct", money("1000"));
    receipt(account.apply_fill(fill("aapl", Side::Buy, "100", "6", "0", 1)));

    // The cash AAPL spent is not available to SPY.
    require_unchanged_after_rejection(
        account,
        fill("spy-too-large", Side::Buy, "100", "4.00000001", "0", 2, "SPY"),
        ApplyFillErrorCode::InsufficientCash
    );
    receipt(account.apply_fill(fill("spy", Side::Buy, "100", "4", "0", 2, "SPY")));
    REQUIRE(account.state().cash == money("0"));

    // Owning AAPL does not allow selling more SPY than is held.
    require_unchanged_after_rejection(
        account,
        fill("spy-oversell", Side::Sell, "100", "5", "0", 3, "SPY"),
        ApplyFillErrorCode::InsufficientPosition
    );
    require_unchanged_after_rejection(
        account,
        fill("unheld-sell", Side::Sell, "100", "1", "0", 3, "QQQ"),
        ApplyFillErrorCode::InsufficientPosition
    );
    REQUIRE(account.state().position(InstrumentId("QQQ")) == nullptr);
}

void test_duplicate_fill_and_timestamp_rules() {
    PaperAccount account("acct", money("100"));
    const Fill first = fill("same-id", Side::Buy, "10", "1", "0", 100);
    receipt(account.apply_fill(first));

    require_unchanged_after_rejection(
        account,
        first,
        ApplyFillErrorCode::DuplicateFill
    );
    // A fill ID identifies one fill for the whole account, whatever its
    // instrument.
    Fill same_id_other_instrument = first;
    same_id_other_instrument.instrument = InstrumentId("SPY");
    require_unchanged_after_rejection(
        account,
        same_id_other_instrument,
        ApplyFillErrorCode::DuplicateFill
    );
    receipt(account.apply_fill(fill("equal-time", Side::Buy, "10", "1", "0", 100)));
    require_unchanged_after_rejection(
        account,
        fill("old-time", Side::Buy, "10", "1", "0", 99),
        ApplyFillErrorCode::TimestampMovedBackward
    );
}

void test_fill_times_are_ordered_per_instrument() {
    PaperAccount account("acct", money("1000"));
    receipt(account.apply_fill(fill("aapl-1", Side::Buy, "10", "1", "0", 100)));

    // An earlier SPY fill is valid: time is ordered within an instrument.
    receipt(account.apply_fill(fill("spy-1", Side::Buy, "10", "1", "0", 50, "SPY")));
    receipt(account.apply_fill(fill("spy-2", Side::Buy, "10", "1", "0", 75, "SPY")));
    REQUIRE(ledger(account, "SPY").last_fill_timestamp == Timestamp::from_nanoseconds(75));
    REQUIRE(account.state().latest_fill_timestamp() == Timestamp::from_nanoseconds(100));

    require_unchanged_after_rejection(
        account,
        fill("spy-late", Side::Buy, "10", "1", "0", 74, "SPY"),
        ApplyFillErrorCode::TimestampMovedBackward
    );
    require_unchanged_after_rejection(
        account,
        fill("aapl-late", Side::Sell, "10", "1", "0", 99),
        ApplyFillErrorCode::TimestampMovedBackward
    );
}

void test_rejected_fill_id_is_not_consumed() {
    PaperAccount account("acct", money("10"));
    require_error(
        account.apply_fill(fill("retry", Side::Buy, "11", "1", "0", 1)),
        ApplyFillErrorCode::InsufficientCash
    );
    receipt(account.apply_fill(fill("retry", Side::Buy, "10", "1", "0", 1)));
    REQUIRE(ledger(account, "AAPL").quantity == quantity("1"));
}

void test_preview_does_not_change_the_account() {
    PaperAccount account("acct", money("100"));
    const Fill buy = fill("preview", Side::Buy, "10", "2", "1", 1);
    const auto preview = receipt(account.preview_fill(buy));
    REQUIRE(preview.after.cash == money("79"));
    REQUIRE(account.state().cash == money("100"));
    REQUIRE(account.state().positions.empty());
    // The previewed fill ID is still unused.
    const auto applied = receipt(account.apply_fill(buy));
    REQUIRE(applied.after == preview.after);
}

void test_notional_that_rounds_to_zero_is_rejected() {
    PaperAccount account("acct", money("1"));
    const Fill tiny{
        .fill_id = "tiny",
        .instrument = InstrumentId("AAPL"),
        .side = Side::Buy,
        .price = Price::from_raw(1),
        .quantity = Quantity::from_raw(1),
        .fee = Money{},
        .timestamp = Timestamp::from_nanoseconds(1),
    };
    require_unchanged_after_rejection(
        account,
        tiny,
        ApplyFillErrorCode::NotionalRoundedToZero
    );
}

void test_fill_arithmetic_overflow_is_atomic() {
    PaperAccount account("acct", money("1000"));
    const auto maximum = std::numeric_limits<std::int64_t>::max();

    const Fill nearly_maximum_position{
        .fill_id = "large",
        .instrument = InstrumentId("AAPL"),
        .side = Side::Buy,
        .price = Price::from_raw(10),
        .quantity = Quantity::from_raw(maximum - 25'000'000),
        .fee = Money{},
        .timestamp = Timestamp::from_nanoseconds(1),
    };
    receipt(account.apply_fill(nearly_maximum_position));

    const Fill overflowing_position{
        .fill_id = "overflow",
        .instrument = InstrumentId("AAPL"),
        .side = Side::Buy,
        .price = Price::from_raw(10),
        .quantity = Quantity::from_raw(50'000'000),
        .fee = Money{},
        .timestamp = Timestamp::from_nanoseconds(2),
    };
    require_unchanged_after_rejection(
        account,
        overflowing_position,
        ApplyFillErrorCode::ArithmeticOverflow
    );
}

void test_account_totals_must_stay_representable() {
    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kOneDollar = 100'000'000;
    // AAPL's cost basis alone is almost the whole fixed-point range, earned
    // back through a realized gain, and $2 of cash remains.
    PaperAccount account = PaperAccount::restore(recovery(
        kOneDollar,
        2 * kOneDollar,
        {{InstrumentId("AAPL"), position_state(kOneDollar, kMaximum - kOneDollar, kMaximum, 0, 1)}}
    ));

    // A $1 SPY buy brings total open cost to exactly the maximum.
    const auto fits = receipt(account.preview_fill(
        fill("spy-fits", Side::Buy, "1", "1", "0", 2, "SPY")
    ));
    REQUIRE(fits.after.open_cost_basis().raw() == kMaximum);
    // A $1.50 buy fits in SPY's own ledger but not in the account total.
    require_unchanged_after_rejection(
        account,
        fill("spy-overflow", Side::Buy, "1.5", "1", "0", 2, "SPY"),
        ApplyFillErrorCode::ArithmeticOverflow
    );
    require_unchanged_after_rejection(
        account,
        fill("fee-too-large", Side::Buy, "1", "1", "1.00000001", 2, "SPY"),
        ApplyFillErrorCode::InsufficientCash
    );
}

void test_snapshot_overflow_at_every_total() {
    constexpr std::int64_t kUnit = 100'000'000;
    // Two 10-share positions, each worth 60% of the fixed-point range at a
    // $5.5 billion mark: each position value fits, their sum does not.
    const PaperAccount pair = PaperAccount::restore(recovery(
        1'000 * kUnit,
        998 * kUnit,
        {
            {InstrumentId("AAPL"), position_state(10 * kUnit, kUnit, 0, 0, 1)},
            {InstrumentId("SPY"), position_state(10 * kUnit, kUnit, 0, 0, 1)},
        }
    ));
    REQUIRE(pair.snapshot(marks({{"AAPL", "5500000000"}, {"SPY", "1"}})).market_value ==
        money("55000000010"));
    REQUIRE_THROWS_AS(
        pair.snapshot(marks({{"AAPL", "5500000000"}, {"SPY", "5500000000"}})),
        std::overflow_error
    );

    // One large position overflows its own market value.
    const PaperAccount large = PaperAccount::restore(recovery(
        1'000 * kUnit,
        999 * kUnit,
        {{InstrumentId("AAPL"), position_state(100 * kUnit, kUnit, 0, 0, 1)}}
    ));
    REQUIRE_THROWS_AS(
        large.snapshot(marks({{"AAPL", "1000000000"}})),
        std::overflow_error
    );

    // Cash plus market value can overflow equity.
    constexpr std::int64_t kStartingCash = 9'000'000'000'000'000'000;
    const PaperAccount rich = PaperAccount::restore(recovery(
        kStartingCash,
        kStartingCash - kUnit,
        {{InstrumentId("AAPL"), position_state(kUnit, kUnit, 0, 0, 1)}}
    ));
    REQUIRE(rich.snapshot(marks({{"AAPL", "1"}})).equity.raw() == kStartingCash);
    REQUIRE_THROWS_AS(
        rich.snapshot(marks({{"AAPL", "3000000000"}})),
        std::overflow_error
    );
}

void test_recovery_round_trips_and_validates_positions() {
    PaperAccount account("acct", money("1000"));
    receipt(account.apply_fill(fill("a", Side::Buy, "10", "3", "0.5", 5)));
    receipt(account.apply_fill(fill("s", Side::Buy, "20", "2", "0.5", 6, "SPY")));
    receipt(account.apply_fill(fill("a2", Side::Sell, "12", "3", "0.5", 7)));

    const PaperAccountRecoveryState saved = account.recovery_state();
    REQUIRE((saved.applied_fill_ids == std::vector<std::string>{"a", "a2", "s"}));
    PaperAccount restored = PaperAccount::restore(saved);
    REQUIRE(restored.state() == account.state());
    require_error(
        restored.apply_fill(fill("s", Side::Buy, "20", "1", "0", 8, "SPY")),
        ApplyFillErrorCode::DuplicateFill
    );

    const auto rejects = [&saved](auto mutate) {
        PaperAccountRecoveryState broken = saved;
        mutate(broken);
        REQUIRE_THROWS_AS(PaperAccount::restore(broken), std::invalid_argument);
    };
    rejects([](PaperAccountRecoveryState& state) {
        state.state.cash = state.state.cash + Money::from_raw(1);
    });
    rejects([](PaperAccountRecoveryState& state) {
        state.state.positions.at(InstrumentId("SPY")).quantity = Quantity::from_raw(-1);
    });
    rejects([](PaperAccountRecoveryState& state) {
        state.state.positions.at(InstrumentId("AAPL")).open_cost_basis = Money::from_raw(1);
        state.state.cash = state.state.cash - Money::from_raw(1);
    });
    rejects([](PaperAccountRecoveryState& state) {
        state.state.positions.at(InstrumentId("SPY")).fees_paid = Money::from_raw(-1);
    });
    rejects([](PaperAccountRecoveryState& state) {
        state.state.positions.at(InstrumentId("SPY")).last_fill_timestamp =
            Timestamp::from_nanoseconds(-1);
    });
    rejects([](PaperAccountRecoveryState& state) { state.applied_fill_ids.clear(); });
    rejects([](PaperAccountRecoveryState& state) {
        state.applied_fill_ids.push_back("a");
    });
    rejects([](PaperAccountRecoveryState& state) {
        state.applied_fill_ids.push_back("");
    });
    rejects([](PaperAccountRecoveryState& state) {
        state.state.positions.clear();
        state.state.cash = state.state.starting_cash;
    });

    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    // Totals beyond the fixed-point range cannot be recovered even when each
    // ledger fits.
    REQUIRE_THROWS_AS(
        PaperAccount::restore(recovery(
            0,
            0,
            {
                {InstrumentId("AAPL"), position_state(0, 0, kMaximum, kMaximum, 1)},
                {InstrumentId("SPY"), position_state(0, 0, kMaximum, kMaximum, 1)},
            }
        )),
        std::invalid_argument
    );
}

void test_market_identifiers_are_strong_and_nonempty() {
    const InstrumentId aapl("AAPL");
    const InstrumentId another_aapl("AAPL");
    const InstrumentId spy("SPY");

    REQUIRE(aapl.value() == "AAPL");
    REQUIRE(aapl == another_aapl);
    REQUIRE(aapl != spy);
    REQUIRE_THROWS_AS(MarketDataStreamId(""), std::invalid_argument);
    REQUIRE_THROWS_AS(VenueId(""), std::invalid_argument);
    REQUIRE_THROWS_AS(chronos::CurrencyCode(""), std::invalid_argument);
}

void test_market_data_quality_flags_are_composable() {
    const MarketDataQuality quality = MarketDataQuality{}
        .with(MarketDataQualityFlag::Snapshot)
        .with(MarketDataQualityFlag::PotentiallyInvalidBook);

    REQUIRE(quality.contains(MarketDataQualityFlag::Snapshot));
    REQUIRE(quality.contains(MarketDataQualityFlag::PotentiallyInvalidBook));
    REQUIRE(!quality.contains(MarketDataQualityFlag::Replay));
    REQUIRE(quality.raw() == (
            static_cast<std::uint32_t>(MarketDataQualityFlag::Snapshot) |
                static_cast<std::uint32_t>(
                    MarketDataQualityFlag::PotentiallyInvalidBook
                )));
}

MarketEventHeader market_event_header() {
    return MarketEventHeader{
        .instrument = InstrumentId("AAPL"),
        .stream = MarketDataStreamId("eq-us-mini-mbp1-2026-09-25"),
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(1'000),
        .receive_timestamp = Timestamp::from_nanoseconds(1'100),
        .ingest_sequence = 42,
        .venue_sequence = std::nullopt,
        .quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay),
    };
}

void test_provider_independent_market_events_preserve_semantics() {
    const TradeEvent trade{
        .header = market_event_header(),
        .price = price("201.123456789"),
        .quantity = quantity("10"),
        .aggressor_side = chronos::AggressorSide::Buy,
    };
    const MarketEvent event = trade;

    REQUIRE(std::holds_alternative<TradeEvent>(event));
    REQUIRE(std::get<TradeEvent>(event) == trade);
    REQUIRE(trade.header.instrument == InstrumentId("AAPL"));
    REQUIRE(trade.header.ingest_sequence == 42);
    REQUIRE(!trade.header.venue.has_value());
    REQUIRE(!trade.header.venue_sequence.has_value());
    REQUIRE(trade.header.quality.contains(MarketDataQualityFlag::Replay));

    const TopOfBookEvent one_sided_book{
        .header = market_event_header(),
        .bid = chronos::BookLevel{
            .price = price("201.12"),
            .quantity = quantity("500"),
        },
        .ask = std::nullopt,
    };
    REQUIRE(one_sided_book.bid.has_value());
    REQUIRE(!one_sided_book.ask.has_value());

    const MarketStatusEvent status{
        .header = market_event_header(),
        .status = chronos::TradingStatus::Halted,
        .reason = "regulatory halt",
    };
    REQUIRE(status.status == chronos::TradingStatus::Halted);

    const InstrumentDefinitionEvent definition{
        .header = market_event_header(),
        .display_symbol = "AAPL",
        .currency = chronos::CurrencyCode("USD"),
        .minimum_price_increment = price("0.01"),
        .minimum_quantity_increment = quantity("1"),
        .effective_from = Timestamp::from_nanoseconds(900),
        .effective_until = std::nullopt,
    };
    REQUIRE(definition.currency.value() == "USD");
    REQUIRE(definition.minimum_price_increment == price("0.01"));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"decimal parsing and formatting", test_decimal_parsing_and_formatting},
        {"decimal parser rejects invalid input", test_decimal_parser_rejects_invalid_input},
        {"decimal parser handles int64 boundaries", test_decimal_parser_handles_int64_boundaries},
        {"decimal arithmetic detects overflow", test_decimal_addition_and_subtraction_detect_overflow},
        {"fixed-point multiplication rounding", test_fixed_point_multiplication_rounding},
        {"proportions and average price", test_proportions_and_average_price},
        {"percentage of is exact and rounds half away", test_percentage_of_is_exact_and_rounds_half_away_from_zero},
        {"price/notional/average round trip", test_price_notional_average_round_trip},
        {"account constructor validation", test_account_constructor_validation},
        {"primary buy/sell/snapshot example", test_primary_buy_sell_and_snapshot_example},
        {"multiple buys and complete close", test_multiple_buys_partial_sale_and_complete_close},
        {"realized loss and fees", test_realized_loss_and_fees},
        {"empty account snapshot", test_empty_account_snapshot},
        {"shared cash funds several instruments", test_shared_cash_funds_positions_in_several_instruments},
        {"a fill changes only its own instrument", test_a_fill_changes_only_its_own_instrument},
        {"position and account PnL reconcile exactly", test_position_and_account_pnl_reconcile_exactly},
        {"snapshot requires a mark per open position", test_snapshot_requires_a_mark_for_every_open_position},
        {"fill field validation is atomic", test_fill_field_validation_is_atomic},
        {"cash and position rejection is atomic", test_insufficient_cash_and_position_are_atomic},
        {"shared cash and positions limit each instrument", test_shared_cash_and_positions_limit_each_instrument},
        {"duplicate fill and timestamp rules", test_duplicate_fill_and_timestamp_rules},
        {"fill times are ordered per instrument", test_fill_times_are_ordered_per_instrument},
        {"rejected fill ID is reusable", test_rejected_fill_id_is_not_consumed},
        {"preview does not change the account", test_preview_does_not_change_the_account},
        {"zero rounded notional is rejected", test_notional_that_rounds_to_zero_is_rejected},
        {"fill arithmetic overflow is atomic", test_fill_arithmetic_overflow_is_atomic},
        {"account totals must stay representable", test_account_totals_must_stay_representable},
        {"snapshot overflow at every total", test_snapshot_overflow_at_every_total},
        {"recovery round trips and validates positions", test_recovery_round_trips_and_validates_positions},
        {"market identifiers are strong and nonempty", test_market_identifiers_are_strong_and_nonempty},
        {"market quality flags are composable", test_market_data_quality_flags_are_composable},
        {"market events preserve semantics", test_provider_independent_market_events_preserve_semantics},
    });
}
