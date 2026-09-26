#include "chronos/market_data_types.hpp"
#include "chronos/paper_account.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
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

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, std::string_view expression, int line) {
    if (!condition) {
        throw TestFailure(
            "line " + std::to_string(line) + ": requirement failed: " +
            std::string(expression)
        );
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

template <typename Exception, typename Function>
void require_throws(Function&& function, int line) {
    try {
        std::invoke(std::forward<Function>(function));
    } catch (const Exception&) {
        return;
    } catch (const std::exception& error) {
        throw TestFailure(
            "line " + std::to_string(line) + ": expected a different exception; got: " +
            error.what()
        );
    }
    throw TestFailure(
        "line " + std::to_string(line) + ": expected an exception"
    );
}

#define REQUIRE_THROWS_AS(expression, exception_type) \
    require_throws<exception_type>([&] { static_cast<void>(expression); }, __LINE__)

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

bool same_state(const AccountState& left, const AccountState& right) {
    return left.starting_cash == right.starting_cash &&
        left.cash == right.cash &&
        left.position == right.position &&
        left.open_cost_basis == right.open_cost_basis &&
        left.gross_realized_pnl == right.gross_realized_pnl &&
        left.fees_paid == right.fees_paid &&
        left.last_fill_timestamp == right.last_fill_timestamp;
}

void require_unchanged_after_rejection(
    PaperAccount& account,
    const Fill& rejected_fill,
    ApplyFillErrorCode expected
) {
    const AccountState before = account.state();
    require_error(account.apply_fill(rejected_fill), expected);
    REQUIRE(same_state(account.state(), before));
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
    REQUIRE_THROWS_AS(
        PaperAccount("", InstrumentId("AAPL"), money("1")),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(InstrumentId(""), std::invalid_argument);
    REQUIRE_THROWS_AS(
        PaperAccount("acct", InstrumentId("AAPL"), money("-0.00000001")),
        std::invalid_argument
    );

    const PaperAccount account("acct", InstrumentId("AAPL"), money("0"));
    REQUIRE(account.account_id() == "acct");
    REQUIRE(account.instrument() == InstrumentId("AAPL"));
    REQUIRE(account.state().cash == money("0"));
}

void test_primary_buy_sell_and_snapshot_example() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("10000"));

    const auto buy_result = account.apply_fill(
        fill("buy-1", Side::Buy, "100.05", "10", "1", 100)
    );
    const auto buy = receipt(buy_result);
    REQUIRE(buy.notional == money("1000.5"));
    REQUIRE(buy.gross_realized_pnl_change == money("0"));
    REQUIRE(buy.before.cash == money("10000"));
    REQUIRE(buy.after.cash == money("8998.5"));
    REQUIRE(account.state().position == quantity("10"));
    REQUIRE(account.state().open_cost_basis == money("1000.5"));
    REQUIRE(account.state().average_entry_price() == price("100.05"));
    REQUIRE(account.state().fees_paid == money("1"));

    const auto sell_result = account.apply_fill(
        fill("sell-1", Side::Sell, "102.90", "4", "0.5", 200)
    );
    const auto sell = receipt(sell_result);
    REQUIRE(sell.notional == money("411.6"));
    REQUIRE(sell.gross_realized_pnl_change == money("11.4"));
    REQUIRE(account.state().cash == money("9409.6"));
    REQUIRE(account.state().position == quantity("6"));
    REQUIRE(account.state().open_cost_basis == money("600.3"));
    REQUIRE(account.state().gross_realized_pnl == money("11.4"));
    REQUIRE(account.state().fees_paid == money("1.5"));

    const auto snapshot = account.snapshot(price("103"));
    REQUIRE(snapshot.market_value == money("618"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("17.7"));
    REQUIRE(snapshot.gross_total_pnl == money("29.1"));
    REQUIRE(snapshot.net_total_pnl == money("27.6"));
    REQUIRE(snapshot.equity == money("10027.6"));
    REQUIRE(snapshot.equity - snapshot.state.starting_cash == snapshot.net_total_pnl);
}

void test_multiple_buys_partial_sale_and_complete_close() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("10000"));
    receipt(account.apply_fill(fill("b1", Side::Buy, "10", "2", "0", 1)));
    receipt(account.apply_fill(fill("b2", Side::Buy, "20", "1", "0", 2)));

    REQUIRE(account.state().position == quantity("3"));
    REQUIRE(account.state().open_cost_basis == money("40"));
    REQUIRE(account.state().average_entry_price() == price("13.333333333"));

    const auto partial = receipt(
        account.apply_fill(fill("s1", Side::Sell, "16", "1", "0", 3))
    );
    REQUIRE(partial.gross_realized_pnl_change == money("2.66666667"));
    REQUIRE(account.state().position == quantity("2"));
    REQUIRE(account.state().open_cost_basis == money("26.66666667"));
    REQUIRE(account.state().average_entry_price() == price("13.333333335"));

    const auto closing = receipt(
        account.apply_fill(fill("s2", Side::Sell, "15", "2", "0", 4))
    );
    REQUIRE(closing.gross_realized_pnl_change == money("3.33333333"));
    REQUIRE(account.state().position == quantity("0"));
    REQUIRE(account.state().open_cost_basis == money("0"));
    REQUIRE(account.state().average_entry_price() == Price{});
    REQUIRE(account.state().gross_realized_pnl == money("6"));
    REQUIRE(account.state().cash == money("10006"));
}

void test_realized_loss_and_fees() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("100"));
    receipt(account.apply_fill(fill("b", Side::Buy, "10", "5", "1", 1)));
    const auto sale = receipt(
        account.apply_fill(fill("s", Side::Sell, "8", "5", "2", 2))
    );

    REQUIRE(sale.gross_realized_pnl_change == money("-10"));
    REQUIRE(account.state().gross_realized_pnl == money("-10"));
    REQUIRE(account.state().fees_paid == money("3"));
    REQUIRE(account.state().cash == money("87"));

    const auto snapshot = account.snapshot(price("8"));
    REQUIRE(snapshot.gross_total_pnl == money("-10"));
    REQUIRE(snapshot.net_total_pnl == money("-13"));
    REQUIRE(snapshot.equity == money("87"));
}

void test_empty_account_snapshot() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("100"));
    const auto snapshot = account.snapshot(price("25"));
    REQUIRE(snapshot.market_value == money("0"));
    REQUIRE(snapshot.gross_unrealized_pnl == money("0"));
    REQUIRE(snapshot.net_total_pnl == money("0"));
    REQUIRE(snapshot.equity == money("100"));
    REQUIRE_THROWS_AS(account.snapshot(price("0")), std::invalid_argument);
    REQUIRE_THROWS_AS(account.snapshot(price("-1")), std::invalid_argument);
}

void test_fill_field_validation_is_atomic() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("100"));

    require_unchanged_after_rejection(
        account,
        fill("", Side::Buy, "1", "1", "0", 1),
        ApplyFillErrorCode::MissingFillId
    );
    require_unchanged_after_rejection(
        account,
        fill("wrong-instrument", Side::Buy, "1", "1", "0", 1, "MSFT"),
        ApplyFillErrorCode::InstrumentMismatch
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
    PaperAccount account("acct", InstrumentId("AAPL"), money("10"));
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

void test_duplicate_fill_and_timestamp_rules() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("100"));
    const Fill first = fill("same-id", Side::Buy, "10", "1", "0", 100);
    receipt(account.apply_fill(first));

    require_unchanged_after_rejection(
        account,
        first,
        ApplyFillErrorCode::DuplicateFill
    );
    receipt(account.apply_fill(fill("equal-time", Side::Buy, "10", "1", "0", 100)));
    require_unchanged_after_rejection(
        account,
        fill("old-time", Side::Buy, "10", "1", "0", 99),
        ApplyFillErrorCode::TimestampMovedBackward
    );
}

void test_rejected_fill_id_is_not_consumed() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("10"));
    require_error(
        account.apply_fill(fill("retry", Side::Buy, "11", "1", "0", 1)),
        ApplyFillErrorCode::InsufficientCash
    );
    receipt(account.apply_fill(fill("retry", Side::Buy, "10", "1", "0", 1)));
    REQUIRE(account.state().position == quantity("1"));
}

void test_notional_that_rounds_to_zero_is_rejected() {
    PaperAccount account("acct", InstrumentId("AAPL"), money("1"));
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
    PaperAccount account("acct", InstrumentId("AAPL"), money("1000"));
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

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"decimal parsing and formatting", test_decimal_parsing_and_formatting},
        {"decimal parser rejects invalid input", test_decimal_parser_rejects_invalid_input},
        {"decimal parser handles int64 boundaries", test_decimal_parser_handles_int64_boundaries},
        {"decimal arithmetic detects overflow", test_decimal_addition_and_subtraction_detect_overflow},
        {"fixed-point multiplication rounding", test_fixed_point_multiplication_rounding},
        {"proportions and average price", test_proportions_and_average_price},
        {"price/notional/average round trip", test_price_notional_average_round_trip},
        {"account constructor validation", test_account_constructor_validation},
        {"primary buy/sell/snapshot example", test_primary_buy_sell_and_snapshot_example},
        {"multiple buys and complete close", test_multiple_buys_partial_sale_and_complete_close},
        {"realized loss and fees", test_realized_loss_and_fees},
        {"empty account snapshot", test_empty_account_snapshot},
        {"fill field validation is atomic", test_fill_field_validation_is_atomic},
        {"cash and position rejection is atomic", test_insufficient_cash_and_position_are_atomic},
        {"duplicate fill and timestamp rules", test_duplicate_fill_and_timestamp_rules},
        {"rejected fill ID is reusable", test_rejected_fill_id_is_not_consumed},
        {"zero rounded notional is rejected", test_notional_that_rounds_to_zero_is_rejected},
        {"fill arithmetic overflow is atomic", test_fill_arithmetic_overflow_is_atomic},
        {"market identifiers are strong and nonempty", test_market_identifiers_are_strong_and_nonempty},
        {"market quality flags are composable", test_market_data_quality_flags_are_composable},
        {"market events preserve semantics", test_provider_independent_market_events_preserve_semantics},
    };

    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& failure) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << failure.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << tests.size() - static_cast<std::size_t>(failed) << "/"
              << tests.size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
