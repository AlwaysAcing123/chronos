#include "chronos/portfolio_valuation.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using chronos::AccountState;
using chronos::BookLevel;
using chronos::Fill;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEventHeader;
using chronos::Money;
using chronos::PaperAccount;
using chronos::PaperAccountRecoveryState;
using chronos::Percentage;
using chronos::PortfolioValuation;
using chronos::PortfolioValuationError;
using chronos::PortfolioValuationErrorCode;
using chronos::PortfolioValuationResult;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::Timestamp;
using chronos::TopOfBookEvent;

// Valuation is exact fixed-point arithmetic end to end.
static_assert(std::is_same_v<decltype(PortfolioValuation::return_percent), Percentage>);
static_assert(std::is_same_v<decltype(Percentage{}.raw()), std::int64_t>);
static_assert(std::is_same_v<decltype(Money{}.raw()), std::int64_t>);

constexpr std::int64_t kReceivedAt = 1'000;
const Timestamp kMarketTime = Timestamp::from_nanoseconds(2'000);

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

// The default ask is far above every bid used here, so a book is crossed
// only when a test crosses it deliberately.
TopOfBookEvent quote(
    std::optional<std::string_view> bid_price,
    std::uint64_t ingest_sequence = 7,
    std::string_view ask_price = "9000000000"
) {
    return TopOfBookEvent{
        .header = MarketEventHeader{
            .instrument = InstrumentId("AAPL"),
            .stream = MarketDataStreamId("valuation-stream"),
            .venue = std::nullopt,
            .event_timestamp = Timestamp::from_nanoseconds(kReceivedAt - 1),
            .receive_timestamp = Timestamp::from_nanoseconds(kReceivedAt),
            .ingest_sequence = ingest_sequence,
            .venue_sequence = std::nullopt,
            .quality = MarketDataQuality{},
        },
        .bid = bid_price
            ? std::optional<BookLevel>(BookLevel{
                  .price = price(*bid_price),
                  .quantity = quantity("10"),
              })
            : std::nullopt,
        .ask = BookLevel{.price = price(ask_price), .quantity = quantity("10")},
    };
}

PaperAccount account_with_fills(
    std::string_view starting_cash,
    std::initializer_list<Fill> fills
) {
    PaperAccount account("valued-account", InstrumentId("AAPL"), money(starting_cash));
    for (const Fill& fill : fills) {
        REQUIRE(std::holds_alternative<chronos::ApplyFillReceipt>(
            account.apply_fill(fill)
        ));
    }
    return account;
}

Fill fill(
    std::string fill_id,
    Side side,
    std::string_view fill_price,
    std::string_view fill_quantity,
    std::string_view fee,
    std::int64_t timestamp = 500
) {
    return Fill{
        .fill_id = std::move(fill_id),
        .instrument = InstrumentId("AAPL"),
        .side = side,
        .price = price(fill_price),
        .quantity = quantity(fill_quantity),
        .fee = money(fee),
        .timestamp = Timestamp::from_nanoseconds(timestamp),
    };
}

// An account in a state reachable only after many fills, restored directly.
PaperAccount restored_account(AccountState state) {
    return PaperAccount::restore(PaperAccountRecoveryState{
        .account_id = "restored-account",
        .instrument = InstrumentId("AAPL"),
        .state = state,
        .applied_fill_ids = {},
    });
}

PortfolioValuation require_valuation(PortfolioValuationResult result) {
    REQUIRE(std::holds_alternative<PortfolioValuation>(result));
    PortfolioValuation valuation = std::get<PortfolioValuation>(std::move(result));
    const auto& snapshot = valuation.snapshot;
    const AccountState& state = snapshot.state;
    // Both definitions of total PnL agree, and every derived field follows
    // from its definition.
    REQUIRE(snapshot.market_value ==
            chronos::calculate_notional(snapshot.mark_price, state.position));
    REQUIRE(snapshot.gross_unrealized_pnl ==
            snapshot.market_value - state.open_cost_basis);
    REQUIRE(snapshot.equity == state.cash + snapshot.market_value);
    REQUIRE(snapshot.net_total_pnl == snapshot.equity - state.starting_cash);
    REQUIRE(snapshot.net_total_pnl ==
            state.gross_realized_pnl + snapshot.gross_unrealized_pnl -
                state.fees_paid);
    REQUIRE(snapshot.mark_price == valuation.market.bid->price);
    REQUIRE(valuation.valued_at == kMarketTime);
    return valuation;
}

void require_error(
    const PortfolioValuationResult& result,
    PortfolioValuationErrorCode code
) {
    REQUIRE(std::holds_alternative<PortfolioValuationError>(result));
    const auto& error = std::get<PortfolioValuationError>(result);
    REQUIRE(error.code == code);
    REQUIRE(!error.message.empty());
}

PortfolioValuationResult value(
    const PaperAccount& account,
    const TopOfBookEvent& market,
    const std::optional<TopOfBookEvent>& active_market = std::nullopt,
    Timestamp market_time = kMarketTime,
    std::int64_t maximum_quote_age_nanoseconds = 1'000
) {
    return chronos::value_portfolio(
        account,
        active_market,
        market,
        market_time,
        maximum_quote_age_nanoseconds
    );
}

void test_flat_account_is_marked_at_the_best_bid() {
    const PaperAccount account = account_with_fills("10000", {});
    const TopOfBookEvent market = quote("99.5");
    const auto valuation = require_valuation(value(account, market));
    REQUIRE(valuation.market == market);
    REQUIRE(valuation.snapshot.mark_price == price("99.5"));
    REQUIRE(valuation.snapshot.market_value == Money{});
    REQUIRE(valuation.snapshot.gross_unrealized_pnl == Money{});
    REQUIRE(valuation.snapshot.net_total_pnl == Money{});
    REQUIRE(valuation.snapshot.equity == money("10000"));
    REQUIRE(valuation.return_percent == Percentage{});
}

void test_profitable_and_losing_long_positions() {
    const PaperAccount account = account_with_fills(
        "10000",
        {fill("buy", Side::Buy, "100", "10", "1")}
    );

    const auto gain = require_valuation(value(account, quote("105")));
    REQUIRE(gain.snapshot.state.cash == money("8999"));
    REQUIRE(gain.snapshot.market_value == money("1050"));
    REQUIRE(gain.snapshot.gross_unrealized_pnl == money("50"));
    REQUIRE(gain.snapshot.net_total_pnl == money("49"));
    REQUIRE(gain.snapshot.equity == money("10049"));
    REQUIRE(gain.return_percent == Percentage::parse("0.49"));

    const auto loss = require_valuation(value(account, quote("95")));
    REQUIRE(loss.snapshot.market_value == money("950"));
    REQUIRE(loss.snapshot.gross_unrealized_pnl == money("-50"));
    REQUIRE(loss.snapshot.net_total_pnl == money("-51"));
    REQUIRE(loss.snapshot.equity == money("9949"));
    REQUIRE(loss.return_percent == Percentage::parse("-0.51"));
}

// 0.333333333 × 3 = 0.999999999 books a cost basis of exactly 1 at the
// money scale. A mark of 0.333333335 gives 1.000000005, which rounds half
// away from zero to 1.00000001.
void test_fractional_marks_use_the_notional_rounding_rule() {
    const PaperAccount account = account_with_fills(
        "100",
        {fill("buy", Side::Buy, "0.333333333", "3", "0")}
    );
    REQUIRE(account.state().open_cost_basis == money("1"));

    const auto rounded_up = require_valuation(value(account, quote("0.333333335")));
    REQUIRE(rounded_up.snapshot.market_value == money("1.00000001"));
    REQUIRE(rounded_up.snapshot.gross_unrealized_pnl == money("0.00000001"));
    REQUIRE(rounded_up.snapshot.equity == money("100.00000001"));
    REQUIRE(rounded_up.return_percent == Percentage::parse("0.00000001"));

    const auto rounded_down = require_valuation(value(account, quote("0.333333334")));
    REQUIRE(rounded_down.snapshot.market_value == money("1"));
    REQUIRE(rounded_down.snapshot.net_total_pnl == Money{});
    REQUIRE(rounded_down.return_percent == Percentage{});
}

void test_fees_are_deducted_exactly_once() {
    const PaperAccount account = account_with_fills(
        "10000",
        {
            fill("buy-1", Side::Buy, "50", "4", "0.75"),
            fill("buy-2", Side::Buy, "52", "1", "0.25", 600),
        }
    );
    const auto valuation = require_valuation(value(account, quote("51")));
    REQUIRE(valuation.snapshot.state.fees_paid == money("1"));
    REQUIRE(valuation.snapshot.state.cash == money("9747"));
    REQUIRE(valuation.snapshot.state.open_cost_basis == money("252"));
    REQUIRE(valuation.snapshot.market_value == money("255"));
    REQUIRE(valuation.snapshot.gross_unrealized_pnl == money("3"));
    REQUIRE(valuation.snapshot.net_total_pnl == money("2"));
    REQUIRE(valuation.snapshot.equity == money("10002"));
    REQUIRE(valuation.return_percent == Percentage::parse("0.02"));
}

// The worked example in docs/accounting.md.
void test_partial_sale_has_realized_and_unrealized_pnl() {
    const PaperAccount account = account_with_fills(
        "10000",
        {
            fill("buy", Side::Buy, "100.05", "10", "1"),
            fill("sell", Side::Sell, "102.90", "4", "0.50", 600),
        }
    );
    const auto valuation = require_valuation(value(account, quote("103")));
    REQUIRE(valuation.snapshot.state.position == quantity("6"));
    REQUIRE(valuation.snapshot.state.average_entry_price() == price("100.05"));
    REQUIRE(valuation.snapshot.market_value == money("618"));
    REQUIRE(valuation.snapshot.gross_unrealized_pnl == money("17.70"));
    REQUIRE(valuation.snapshot.state.gross_realized_pnl == money("11.40"));
    REQUIRE(valuation.snapshot.state.fees_paid == money("1.50"));
    REQUIRE(valuation.snapshot.net_total_pnl == money("27.60"));
    REQUIRE(valuation.snapshot.equity == money("10027.60"));
    REQUIRE(valuation.return_percent == Percentage::parse("0.276"));
}

void test_fully_closed_position_carries_only_realized_pnl() {
    const PaperAccount account = account_with_fills(
        "10000",
        {
            fill("buy", Side::Buy, "20", "5", "0.1"),
            fill("sell", Side::Sell, "18", "5", "0.1", 600),
        }
    );
    const auto valuation = require_valuation(value(account, quote("50")));
    REQUIRE(valuation.snapshot.state.position == Quantity{});
    REQUIRE(valuation.snapshot.state.open_cost_basis == Money{});
    REQUIRE(valuation.snapshot.market_value == Money{});
    REQUIRE(valuation.snapshot.gross_unrealized_pnl == Money{});
    REQUIRE(valuation.snapshot.state.gross_realized_pnl == money("-10"));
    REQUIRE(valuation.snapshot.net_total_pnl == money("-10.2"));
    REQUIRE(valuation.snapshot.equity == money("9989.8"));
    REQUIRE(valuation.return_percent == Percentage::parse("-0.102"));
}

// A market value just below the Money maximum of 92233720368.54775807 is
// exact; one more price unit overflows and is reported, not wrapped.
void test_values_near_the_fixed_point_limit_are_exact_or_rejected() {
    const PaperAccount account = restored_account(AccountState{
        .starting_cash = money("92000000000"),
        .cash = Money{},
        .position = quantity("100000"),
        .open_cost_basis = money("92000000000"),
        .gross_realized_pnl = Money{},
        .fees_paid = Money{},
        .last_fill_timestamp = std::nullopt,
    });
    const auto largest = require_valuation(value(account, quote("922337.20368547")));
    REQUIRE(largest.snapshot.market_value == money("92233720368.547"));
    REQUIRE(largest.snapshot.net_total_pnl == money("233720368.547"));
    REQUIRE(largest.return_percent == Percentage::parse("0.25404388"));

    require_error(
        value(account, quote("922337.20368548")),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );
}

void test_overflowing_equity_and_return_are_rejected() {
    const PaperAccount near_maximum_cash = restored_account(AccountState{
        .starting_cash = money("90000000000"),
        .cash = money("89999999999"),
        .position = quantity("1"),
        .open_cost_basis = money("1"),
        .gross_realized_pnl = Money{},
        .fees_paid = Money{},
        .last_fill_timestamp = std::nullopt,
    });
    require_error(
        value(near_maximum_cash, quote("5000000000")),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );

    const PaperAccount tiny_stake = restored_account(AccountState{
        .starting_cash = Money::from_raw(1),
        .cash = Money::from_raw(100'000'000'000'000'001),
        .position = Quantity{},
        .open_cost_basis = Money{},
        .gross_realized_pnl = Money::from_raw(100'000'000'000'000'000),
        .fees_paid = Money{},
        .last_fill_timestamp = std::nullopt,
    });
    require_error(
        value(tiny_stake, quote("1")),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );
}

void test_unusable_markets_are_rejected_before_valuing() {
    const PaperAccount account = account_with_fills("10000", {});

    TopOfBookEvent crossed = quote("1001", 7, "1000");
    require_error(value(account, crossed), PortfolioValuationErrorCode::InvalidMarketState);
    crossed.header.quality =
        crossed.header.quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    require_error(value(account, crossed), PortfolioValuationErrorCode::SuspectMarketState);

    TopOfBookEvent questionable = quote("99");
    questionable.header.quality = questionable.header.quality.with(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    );
    require_error(
        value(account, questionable),
        PortfolioValuationErrorCode::SuspectMarketState
    );

    TopOfBookEvent other_instrument = quote("99");
    other_instrument.header.instrument = InstrumentId("SPY");
    require_error(
        value(account, other_instrument),
        PortfolioValuationErrorCode::MarketInstrumentMismatch
    );

    require_error(
        value(account, quote(std::nullopt)),
        PortfolioValuationErrorCode::MissingBestBid
    );
    require_error(
        value(account_with_fills("0", {}), quote("99")),
        PortfolioValuationErrorCode::NonpositiveStartingCash
    );
}

void test_valuation_must_continue_the_account_timeline() {
    const PaperAccount account = account_with_fills(
        "10000",
        {fill("buy", Side::Buy, "100", "1", "0", 1'500)}
    );
    const TopOfBookEvent executed = quote("99", 7);

    require_valuation(value(account, executed, executed));
    require_valuation(value(account, quote("98", 8), executed));

    TopOfBookEvent other_stream = quote("99", 8);
    other_stream.header.stream = MarketDataStreamId("other-stream");
    require_error(
        value(account, other_stream, executed),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    require_error(
        value(account, quote("99", 6), executed),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    require_error(
        value(account, quote("98", 7), executed),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    require_error(
        value(account, executed, executed, Timestamp::from_nanoseconds(1'499)),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    require_error(
        value(
            account_with_fills("10000", {}),
            executed,
            std::nullopt,
            Timestamp::from_nanoseconds(kReceivedAt - 1)
        ),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
}

void test_valuation_rejects_a_stale_market_observation() {
    const PaperAccount account = account_with_fills("10000", {});
    const TopOfBookEvent market = quote("99");

    require_valuation(value(
        account,
        market,
        std::nullopt,
        Timestamp::from_nanoseconds(kReceivedAt + 1'000),
        1'000
    ));
    require_error(
        value(
            account,
            market,
            std::nullopt,
            Timestamp::from_nanoseconds(kReceivedAt + 1'001),
            1'000
        ),
        PortfolioValuationErrorCode::StaleMarketState
    );
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "flat account is marked at the best bid",
            test_flat_account_is_marked_at_the_best_bid,
        },
        {
            "profitable and losing long positions",
            test_profitable_and_losing_long_positions,
        },
        {
            "fractional marks use the notional rounding rule",
            test_fractional_marks_use_the_notional_rounding_rule,
        },
        {"fees are deducted exactly once", test_fees_are_deducted_exactly_once},
        {
            "partial sale has realized and unrealized PnL",
            test_partial_sale_has_realized_and_unrealized_pnl,
        },
        {
            "fully closed position carries only realized PnL",
            test_fully_closed_position_carries_only_realized_pnl,
        },
        {
            "values near the fixed-point limit are exact or rejected",
            test_values_near_the_fixed_point_limit_are_exact_or_rejected,
        },
        {
            "overflowing equity and return are rejected",
            test_overflowing_equity_and_return_are_rejected,
        },
        {
            "unusable markets are rejected before valuing",
            test_unusable_markets_are_rejected_before_valuing,
        },
        {
            "valuation must continue the account timeline",
            test_valuation_must_continue_the_account_timeline,
        },
        {
            "valuation rejects a stale market observation",
            test_valuation_rejects_a_stale_market_observation,
        },
    });
}
