#include "chronos/portfolio_valuation.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::AccountState;
using chronos::BookLevel;
using chronos::Fill;
using chronos::InstrumentExecutionState;
using chronos::InstrumentExecutionStates;
using chronos::InstrumentId;
using chronos::InstrumentQuote;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEventHeader;
using chronos::MarketQuotes;
using chronos::MarketStateUnavailable;
using chronos::MarketStateUnavailableCode;
using chronos::Money;
using chronos::PaperAccount;
using chronos::PaperAccountRecoveryState;
using chronos::Percentage;
using chronos::PortfolioValuation;
using chronos::PortfolioValuationError;
using chronos::PortfolioValuationErrorCode;
using chronos::PortfolioValuationResult;
using chronos::PositionState;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::Timestamp;
using chronos::TopOfBookEvent;

// Valuation is exact fixed-point arithmetic end to end.
static_assert(std::is_same_v<decltype(PortfolioValuation::return_percent), Percentage>);
static_assert(std::is_same_v<decltype(Percentage{}.raw()), std::int64_t>);
static_assert(std::is_same_v<decltype(Money{}.raw()), std::int64_t>);

constexpr std::string_view kStream = "valuation-stream";
constexpr std::int64_t kReceivedAt = 1'000;
const Timestamp kMarketTime = Timestamp::from_nanoseconds(2'000);

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

// The default ask is far above every bid used here, so a book is crossed
// only when a test crosses it deliberately.
TopOfBookEvent quote(
    std::string_view instrument,
    std::optional<std::string_view> bid_price,
    std::uint64_t ingest_sequence = 7,
    std::string_view ask_price = "9000000000"
) {
    return TopOfBookEvent{
        .header = MarketEventHeader{
            .instrument = InstrumentId(std::string(instrument)),
            .stream = MarketDataStreamId(std::string(kStream)),
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

MarketStateUnavailable unavailable(MarketStateUnavailableCode code) {
    return MarketStateUnavailable{.code = code, .message = "no current quote"};
}

// One consistent market read: the listed instruments' quotes at `market_time`.
MarketQuotes market(
    std::initializer_list<std::pair<std::string_view, InstrumentQuote>> quotes,
    std::optional<Timestamp> market_time = kMarketTime
) {
    MarketQuotes read{
        .stream = MarketDataStreamId(std::string(kStream)),
        .market_time = market_time,
        .quotes = {},
    };
    for (const auto& [instrument, value] : quotes) {
        read.quotes.insert_or_assign(InstrumentId(std::string(instrument)), value);
    }
    return read;
}

// Execution state recording that each quote was last executed against.
InstrumentExecutionStates executed(std::initializer_list<TopOfBookEvent> markets) {
    InstrumentExecutionStates states;
    for (const TopOfBookEvent& active : markets) {
        states.insert_or_assign(active.header.instrument, InstrumentExecutionState{
            .active_market = active,
            .consumed_bid_quantity = Quantity{},
            .consumed_ask_quantity = Quantity{},
        });
    }
    return states;
}

Fill fill(
    std::string fill_id,
    Side side,
    std::string_view fill_price,
    std::string_view fill_quantity,
    std::string_view fee,
    std::int64_t timestamp = 500,
    std::string_view instrument = "AAPL"
) {
    return Fill{
        .fill_id = std::move(fill_id),
        .instrument = InstrumentId(std::string(instrument)),
        .side = side,
        .price = price(fill_price),
        .quantity = quantity(fill_quantity),
        .fee = money(fee),
        .timestamp = Timestamp::from_nanoseconds(timestamp),
    };
}

PaperAccount account_with_fills(
    std::string_view starting_cash,
    std::initializer_list<Fill> fills
) {
    PaperAccount account("valued-account", money(starting_cash));
    for (const Fill& fill : fills) {
        REQUIRE(std::holds_alternative<chronos::ApplyFillReceipt>(
            account.apply_fill(fill)
        ));
    }
    return account;
}

PositionState open_position(std::string_view held, std::string_view cost) {
    return PositionState{
        .quantity = quantity(held),
        .open_cost_basis = money(cost),
        .gross_realized_pnl = Money{},
        .fees_paid = Money{},
        .last_fill_timestamp = Timestamp::from_nanoseconds(500),
    };
}

// An account in a state reachable only after many fills, restored directly.
PaperAccount restored_account(AccountState state) {
    std::vector<std::string> fill_ids;
    for (const auto& [instrument, position] : state.positions) {
        fill_ids.push_back(instrument.value() + ":fill");
    }
    return PaperAccount::restore(PaperAccountRecoveryState{
        .account_id = "restored-account",
        .state = std::move(state),
        .applied_fill_ids = std::move(fill_ids),
    });
}

PortfolioValuation require_valuation(PortfolioValuationResult result) {
    REQUIRE(std::holds_alternative<PortfolioValuation>(result));
    PortfolioValuation valuation = std::get<PortfolioValuation>(std::move(result));
    const auto& snapshot = valuation.snapshot;
    const AccountState& state = snapshot.state;
    // Every position and total follows from its definition, and both
    // definitions of total PnL agree.
    Money market_value;
    Money unrealized;
    for (const auto& position : snapshot.open_positions) {
        REQUIRE(position.position.is_open());
        REQUIRE(position.market_value ==
            chronos::calculate_notional(position.mark_price, position.position.quantity));
        REQUIRE(position.gross_unrealized_pnl ==
            position.market_value - position.position.open_cost_basis);
        REQUIRE(valuation.marks.at(position.instrument).bid->price == position.mark_price);
        market_value = market_value + position.market_value;
        unrealized = unrealized + position.gross_unrealized_pnl;
    }
    REQUIRE(valuation.marks.size() == snapshot.open_positions.size());
    REQUIRE(snapshot.market_value == market_value);
    REQUIRE(snapshot.gross_unrealized_pnl == unrealized);
    REQUIRE(snapshot.gross_unrealized_pnl ==
        snapshot.market_value - snapshot.open_cost_basis);
    REQUIRE(snapshot.open_cost_basis == state.open_cost_basis());
    REQUIRE(snapshot.gross_realized_pnl == state.gross_realized_pnl());
    REQUIRE(snapshot.fees_paid == state.fees_paid());
    REQUIRE(snapshot.equity == state.cash + snapshot.market_value);
    REQUIRE(snapshot.net_total_pnl == snapshot.equity - state.starting_cash);
    REQUIRE(snapshot.net_total_pnl ==
        snapshot.gross_realized_pnl + snapshot.gross_unrealized_pnl - snapshot.fees_paid);
    return valuation;
}

const PortfolioValuationError& require_error(
    const PortfolioValuationResult& result,
    PortfolioValuationErrorCode code
) {
    REQUIRE(std::holds_alternative<PortfolioValuationError>(result));
    const auto& error = std::get<PortfolioValuationError>(result);
    REQUIRE(error.code == code);
    REQUIRE(!error.message.empty());
    return error;
}

using MarkReason = std::variant<MarketStateUnavailableCode, PortfolioValuationErrorCode>;

// The error names exactly these instruments, in order, with these reasons.
void require_mark_failures(
    const PortfolioValuationResult& result,
    std::initializer_list<std::pair<std::string_view, MarkReason>> expected
) {
    const auto& error = require_error(result, PortfolioValuationErrorCode::PositionMarkUnavailable);
    REQUIRE(error.instruments.size() == expected.size());
    std::size_t index = 0;
    for (const auto& [instrument, reason] : expected) {
        REQUIRE(error.instruments[index].instrument == InstrumentId(std::string(instrument)));
        REQUIRE(error.instruments[index].reason == reason);
        REQUIRE(!error.instruments[index].message.empty());
        ++index;
    }
}

PortfolioValuationResult value(
    const PaperAccount& account,
    const MarketQuotes& read,
    const InstrumentExecutionStates& executions = {},
    std::int64_t maximum_quote_age_nanoseconds = 1'000
) {
    return chronos::value_portfolio(
        account,
        executions,
        read,
        maximum_quote_age_nanoseconds
    );
}

void test_an_account_without_positions_needs_no_quote() {
    const PaperAccount account = account_with_fills("10000", {});
    const auto valuation = require_valuation(value(account, market({})));
    REQUIRE(valuation.snapshot.open_positions.empty());
    REQUIRE(valuation.marks.empty());
    REQUIRE(valuation.snapshot.market_value == Money{});
    REQUIRE(valuation.snapshot.net_total_pnl == Money{});
    REQUIRE(valuation.snapshot.equity == money("10000"));
    REQUIRE(valuation.return_percent == Percentage{});
    REQUIRE(valuation.valued_at == kMarketTime);

    // Before the stream's first event there is no market time to report.
    const auto early = require_valuation(value(account, market({}, std::nullopt)));
    REQUIRE(!early.valued_at.has_value());
    // A configured symbol without a quote does not matter when it is not held.
    require_valuation(value(account, market({
        {"SPY", unavailable(MarketStateUnavailableCode::NoTopOfBook)},
    })));
}

void test_profitable_and_losing_long_positions() {
    const PaperAccount account = account_with_fills(
        "10000",
        {fill("buy", Side::Buy, "100", "10", "1")}
    );

    const auto gain = require_valuation(value(account, market({{"AAPL", quote("AAPL", "105")}})));
    REQUIRE(gain.snapshot.state.cash == money("8999"));
    REQUIRE(gain.snapshot.market_value == money("1050"));
    REQUIRE(gain.snapshot.gross_unrealized_pnl == money("50"));
    REQUIRE(gain.snapshot.net_total_pnl == money("49"));
    REQUIRE(gain.snapshot.equity == money("10049"));
    REQUIRE(gain.return_percent == Percentage::parse("0.49"));

    const auto loss = require_valuation(value(account, market({{"AAPL", quote("AAPL", "95")}})));
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
    REQUIRE(account.state().open_cost_basis() == money("1"));

    const auto rounded_up = require_valuation(
        value(account, market({{"AAPL", quote("AAPL", "0.333333335")}}))
    );
    REQUIRE(rounded_up.snapshot.market_value == money("1.00000001"));
    REQUIRE(rounded_up.snapshot.gross_unrealized_pnl == money("0.00000001"));
    REQUIRE(rounded_up.snapshot.equity == money("100.00000001"));
    REQUIRE(rounded_up.return_percent == Percentage::parse("0.00000001"));

    const auto rounded_down = require_valuation(
        value(account, market({{"AAPL", quote("AAPL", "0.333333334")}}))
    );
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
    const auto valuation = require_valuation(value(account, market({{"AAPL", quote("AAPL", "51")}})));
    REQUIRE(valuation.snapshot.fees_paid == money("1"));
    REQUIRE(valuation.snapshot.state.cash == money("9747"));
    REQUIRE(valuation.snapshot.open_cost_basis == money("252"));
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
    const auto valuation = require_valuation(value(account, market({{"AAPL", quote("AAPL", "103")}})));
    const auto& position = valuation.snapshot.open_positions.at(0);
    REQUIRE(position.position.quantity == quantity("6"));
    REQUIRE(position.position.average_entry_price() == price("100.05"));
    REQUIRE(position.position.gross_realized_pnl == money("11.40"));
    REQUIRE(position.position.fees_paid == money("1.50"));
    REQUIRE(valuation.snapshot.market_value == money("618"));
    REQUIRE(valuation.snapshot.gross_unrealized_pnl == money("17.70"));
    REQUIRE(valuation.snapshot.gross_realized_pnl == money("11.40"));
    REQUIRE(valuation.snapshot.net_total_pnl == money("27.60"));
    REQUIRE(valuation.snapshot.equity == money("10027.60"));
    REQUIRE(valuation.return_percent == Percentage::parse("0.276"));
}

void test_a_closed_position_needs_no_mark() {
    const PaperAccount account = account_with_fills(
        "10000",
        {
            fill("buy", Side::Buy, "20", "5", "0.1"),
            fill("sell", Side::Sell, "18", "5", "0.1", 600),
        }
    );
    // AAPL is closed, so its missing quote does not prevent valuation.
    const auto valuation = require_valuation(value(account, market({
        {"AAPL", unavailable(MarketStateUnavailableCode::NoTopOfBook)},
    })));
    REQUIRE(valuation.snapshot.open_positions.empty());
    REQUIRE(valuation.snapshot.market_value == Money{});
    REQUIRE(valuation.snapshot.gross_realized_pnl == money("-10"));
    REQUIRE(valuation.snapshot.net_total_pnl == money("-10.2"));
    REQUIRE(valuation.snapshot.equity == money("9989.8"));
    REQUIRE(valuation.return_percent == Percentage::parse("-0.102"));
}

void test_several_positions_reconcile_exactly() {
    const PaperAccount account = account_with_fills(
        "100000",
        {
            fill("aapl-buy", Side::Buy, "200.5", "10", "0.35", 500),
            fill("spy-buy", Side::Buy, "500.25", "4", "0.35", 510, "SPY"),
            fill("qqq-buy", Side::Buy, "400", "2", "0.35", 520, "QQQ"),
            fill("qqq-sell", Side::Sell, "410", "2", "0.35", 530, "QQQ"),
            fill("aapl-sell", Side::Sell, "210", "3", "0.35", 540),
        }
    );
    // QQQ is closed and unquoted; MSFT is configured but never traded.
    const auto valuation = require_valuation(value(account, market({
        {"AAPL", quote("AAPL", "205", 9)},
        {"SPY", quote("SPY", "499.75", 4)},
        {"QQQ", unavailable(MarketStateUnavailableCode::NoTopOfBook)},
        {"MSFT", unavailable(MarketStateUnavailableCode::SourceUnavailable)},
    })));
    const auto& positions = valuation.snapshot.open_positions;
    REQUIRE(positions.size() == 2);
    REQUIRE(positions[0].instrument == InstrumentId("AAPL"));
    REQUIRE(positions[0].position.quantity == quantity("7"));
    REQUIRE(positions[0].position.open_cost_basis == money("1403.5"));
    REQUIRE(positions[0].position.gross_realized_pnl == money("28.5"));
    REQUIRE(positions[0].position.fees_paid == money("0.7"));
    REQUIRE(positions[0].mark_price == price("205"));
    REQUIRE(positions[0].market_value == money("1435"));
    REQUIRE(positions[0].gross_unrealized_pnl == money("31.5"));
    REQUIRE(positions[1].instrument == InstrumentId("SPY"));
    REQUIRE(positions[1].position.open_cost_basis == money("2001"));
    REQUIRE(positions[1].market_value == money("1999"));
    REQUIRE(positions[1].gross_unrealized_pnl == money("-2"));
    REQUIRE(valuation.marks.at(InstrumentId("SPY")).header.ingest_sequence == 4);

    REQUIRE(valuation.snapshot.open_cost_basis == money("3404.5"));
    REQUIRE(valuation.snapshot.market_value == money("3434"));
    REQUIRE(valuation.snapshot.gross_unrealized_pnl == money("29.5"));
    // QQQ's realized gain and fees still count toward the account totals.
    REQUIRE(valuation.snapshot.gross_realized_pnl == money("48.5"));
    REQUIRE(valuation.snapshot.fees_paid == money("1.75"));
    REQUIRE(valuation.snapshot.net_total_pnl == money("76.25"));
    REQUIRE(valuation.snapshot.equity == money("100076.25"));
    REQUIRE(valuation.return_percent == Percentage::parse("0.07625"));
}

void test_every_unmarkable_position_is_reported() {
    const PaperAccount account = account_with_fills(
        "100000",
        {
            fill("aapl", Side::Buy, "100", "1", "0"),
            fill("spy", Side::Buy, "500", "1", "0", 500, "SPY"),
            fill("qqq", Side::Buy, "400", "1", "0", 500, "QQQ"),
        }
    );
    TopOfBookEvent stale_spy = quote("SPY", "501");
    stale_spy.header.receive_timestamp = Timestamp::from_nanoseconds(kReceivedAt - 1);
    stale_spy.header.event_timestamp = Timestamp::from_nanoseconds(kReceivedAt - 2);
    // Two positions cannot be marked; no partial total is produced.
    const auto result = value(
        account,
        market({
            {"AAPL", unavailable(MarketStateUnavailableCode::AwaitingFreshQuote)},
            {"SPY", stale_spy},
            {"QQQ", quote("QQQ", "401")},
        })
    );
    require_mark_failures(result, {
        {"AAPL", MarketStateUnavailableCode::AwaitingFreshQuote},
        {"SPY", PortfolioValuationErrorCode::StaleMarketState},
    });

    // A held instrument the caller did not ask the market for is reported
    // as having no quote.
    require_mark_failures(
        value(account, market({{"AAPL", quote("AAPL", "101")}, {"SPY", quote("SPY", "501")}})),
        {{"QQQ", MarketStateUnavailableCode::NoTopOfBook}}
    );
}

void test_an_interrupted_or_disconnected_market_fails_closed_for_held_positions() {
    const PaperAccount holding = account_with_fills(
        "10000",
        {fill("aapl", Side::Buy, "100", "1", "0")}
    );
    require_mark_failures(
        value(holding, market({{"AAPL", unavailable(MarketStateUnavailableCode::StreamInterrupted)}})),
        {{"AAPL", MarketStateUnavailableCode::StreamInterrupted}}
    );
    require_mark_failures(
        value(holding, market({{"AAPL", unavailable(MarketStateUnavailableCode::SourceUnavailable)}})),
        {{"AAPL", MarketStateUnavailableCode::SourceUnavailable}}
    );
    // An account holding nothing is exactly its cash whatever the feed does.
    const PaperAccount flat = account_with_fills("10000", {});
    require_valuation(value(flat, market({
        {"AAPL", unavailable(MarketStateUnavailableCode::SourceUnavailable)},
    })));
}

// A market value just below the Money maximum of 92233720368.54775807 is
// exact; one more price unit overflows and is reported, not wrapped.
void test_values_near_the_fixed_point_limit_are_exact_or_rejected() {
    const PaperAccount account = restored_account(AccountState{
        .starting_cash = money("92000000000"),
        .cash = Money{},
        .positions = {{InstrumentId("AAPL"), open_position("100000", "92000000000")}},
    });
    const auto largest = require_valuation(
        value(account, market({{"AAPL", quote("AAPL", "922337.20368547")}}))
    );
    REQUIRE(largest.snapshot.market_value == money("92233720368.547"));
    REQUIRE(largest.snapshot.net_total_pnl == money("233720368.547"));
    REQUIRE(largest.return_percent == Percentage::parse("0.25404388"));

    require_error(
        value(account, market({{"AAPL", quote("AAPL", "922337.20368548")}})),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );
}

void test_overflow_at_every_aggregate_is_rejected() {
    // Each position's value fits; their sum does not.
    const PaperAccount pair = restored_account(AccountState{
        .starting_cash = money("1000"),
        .cash = money("998"),
        .positions = {
            {InstrumentId("AAPL"), open_position("10", "1")},
            {InstrumentId("SPY"), open_position("10", "1")},
        },
    });
    require_valuation(value(pair, market({
        {"AAPL", quote("AAPL", "5500000000")},
        {"SPY", quote("SPY", "1")},
    })));
    require_error(
        value(pair, market({
            {"AAPL", quote("AAPL", "5500000000")},
            {"SPY", quote("SPY", "5500000000")},
        })),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );

    // Cash plus market value overflows equity.
    const PaperAccount near_maximum_cash = restored_account(AccountState{
        .starting_cash = money("90000000000"),
        .cash = money("89999999999"),
        .positions = {{InstrumentId("AAPL"), open_position("1", "1")}},
    });
    require_error(
        value(near_maximum_cash, market({{"AAPL", quote("AAPL", "5000000000")}})),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );

    // The return percentage overflows on a tiny stake.
    const PaperAccount tiny_stake = restored_account(AccountState{
        .starting_cash = Money::from_raw(1),
        .cash = Money::from_raw(100'000'000'000'000'001),
        .positions = {{
            InstrumentId("AAPL"),
            PositionState{
                .quantity = Quantity{},
                .open_cost_basis = Money{},
                .gross_realized_pnl = Money::from_raw(100'000'000'000'000'000),
                .fees_paid = Money{},
                .last_fill_timestamp = Timestamp::from_nanoseconds(500),
            },
        }},
    });
    require_error(
        value(tiny_stake, market({})),
        PortfolioValuationErrorCode::ArithmeticOverflow
    );
}

void test_unusable_quotes_cannot_mark_a_position() {
    const PaperAccount account = account_with_fills(
        "10000",
        {fill("aapl", Side::Buy, "100", "1", "0")}
    );
    const auto reason_for = [&account](const TopOfBookEvent& aapl_quote) {
        return value(account, market({{"AAPL", aapl_quote}}));
    };

    TopOfBookEvent crossed = quote("AAPL", "1001", 7, "1000");
    require_mark_failures(reason_for(crossed), {{"AAPL", PortfolioValuationErrorCode::InvalidMarketState}});
    crossed.header.quality =
        crossed.header.quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    require_mark_failures(reason_for(crossed), {{"AAPL", PortfolioValuationErrorCode::SuspectMarketState}});

    TopOfBookEvent questionable = quote("AAPL", "99");
    questionable.header.quality = questionable.header.quality.with(
        MarketDataQualityFlag::QuestionableReceiveTimestamp
    );
    require_mark_failures(reason_for(questionable), {{"AAPL", PortfolioValuationErrorCode::SuspectMarketState}});

    // An SPY quote cannot mark the AAPL position.
    require_mark_failures(
        reason_for(quote("SPY", "99")),
        {{"AAPL", PortfolioValuationErrorCode::MarketInstrumentMismatch}}
    );
    require_mark_failures(
        reason_for(quote("AAPL", std::nullopt)),
        {{"AAPL", PortfolioValuationErrorCode::MissingBestBid}}
    );
    require_error(
        value(account_with_fills("0", {}), market({})),
        PortfolioValuationErrorCode::NonpositiveStartingCash
    );
}

void test_valuation_must_continue_the_account_timeline() {
    const PaperAccount account = account_with_fills(
        "10000",
        {
            fill("aapl", Side::Buy, "100", "1", "0", 1'500),
            fill("spy", Side::Buy, "500", "1", "0", 1'400, "SPY"),
        }
    );
    const TopOfBookEvent aapl_executed = quote("AAPL", "99", 7);
    const TopOfBookEvent spy_executed = quote("SPY", "499", 12);
    const auto executions = executed({aapl_executed, spy_executed});

    require_valuation(value(account, market({{"AAPL", aapl_executed}, {"SPY", spy_executed}}), executions));
    // Instruments continue independently: AAPL at 8 is valid although SPY
    // last executed at 12.
    require_valuation(value(account, market({{"AAPL", quote("AAPL", "98", 8)}, {"SPY", spy_executed}}), executions));

    // Per instrument: an older or contradictory observation fails.
    require_mark_failures(
        value(account, market({{"AAPL", quote("AAPL", "99", 6)}, {"SPY", quote("SPY", "499", 11)}}), executions),
        {
            {"AAPL", PortfolioValuationErrorCode::MarketTimelineMismatch},
            {"SPY", PortfolioValuationErrorCode::MarketTimelineMismatch},
        }
    );
    require_mark_failures(
        value(account, market({{"AAPL", quote("AAPL", "98", 7)}, {"SPY", spy_executed}}), executions),
        {{"AAPL", PortfolioValuationErrorCode::MarketTimelineMismatch}}
    );

    // Account-wide: another stream, or market time before the latest fill.
    MarketQuotes other_stream = market({{"AAPL", aapl_executed}, {"SPY", spy_executed}});
    other_stream.stream = MarketDataStreamId("other-stream");
    const auto stream_result = value(account, other_stream, executions);
    const auto& stream_error = require_error(
        stream_result,
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    REQUIRE(stream_error.instruments.empty());
    require_error(
        value(
            account,
            market({{"AAPL", aapl_executed}, {"SPY", spy_executed}}, Timestamp::from_nanoseconds(1'499)),
            executions
        ),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    // Quotes without a market clock cannot be current.
    require_mark_failures(
        value(account, market({{"AAPL", aapl_executed}, {"SPY", spy_executed}}, std::nullopt), executions),
        {
            {"AAPL", PortfolioValuationErrorCode::MarketTimelineMismatch},
            {"SPY", PortfolioValuationErrorCode::MarketTimelineMismatch},
        }
    );
    // Before any market event, each open position simply has no quote.
    require_mark_failures(
        value(account, market({
            {"AAPL", unavailable(MarketStateUnavailableCode::NoTopOfBook)},
            {"SPY", unavailable(MarketStateUnavailableCode::NoTopOfBook)},
        }, std::nullopt), executions),
        {
            {"AAPL", MarketStateUnavailableCode::NoTopOfBook},
            {"SPY", MarketStateUnavailableCode::NoTopOfBook},
        }
    );

    // A quote received after market time is a timeline error for its position.
    require_mark_failures(
        value(
            account_with_fills("10000", {fill("aapl", Side::Buy, "100", "1", "0", 900)}),
            market({{"AAPL", quote("AAPL", "99")}}, Timestamp::from_nanoseconds(kReceivedAt - 1))
        ),
        {{"AAPL", PortfolioValuationErrorCode::MarketTimelineMismatch}}
    );
}

void test_a_closed_account_is_still_bound_to_its_timeline() {
    const PaperAccount account = account_with_fills(
        "10000",
        {
            fill("buy", Side::Buy, "20", "5", "0", 1'500),
            fill("sell", Side::Sell, "21", "5", "0", 1'600),
        }
    );
    const auto executions = executed({quote("AAPL", "20", 7)});
    // Nothing is held, but a different replay or live session still must not
    // value the account's history.
    MarketQuotes other_stream = market({});
    other_stream.stream = MarketDataStreamId("other-stream");
    require_error(
        value(account, other_stream, executions),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    require_error(
        value(account, market({}, Timestamp::from_nanoseconds(1'599)), executions),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    const auto unstarted_result = value(account, market({}, std::nullopt), executions);
    const auto& unstarted = require_error(
        unstarted_result,
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
    REQUIRE(unstarted.instruments.empty());
    const auto valuation = require_valuation(value(account, market({}), executions));
    REQUIRE(valuation.snapshot.equity == money("10005"));
}

void test_valuation_rejects_a_stale_market_observation() {
    const PaperAccount account = account_with_fills(
        "10000",
        {fill("aapl", Side::Buy, "100", "1", "0")}
    );
    const TopOfBookEvent aapl = quote("AAPL", "99");

    require_valuation(value(
        account,
        market({{"AAPL", aapl}}, Timestamp::from_nanoseconds(kReceivedAt + 1'000)),
        {},
        1'000
    ));
    require_mark_failures(
        value(
            account,
            market({{"AAPL", aapl}}, Timestamp::from_nanoseconds(kReceivedAt + 1'001)),
            {},
            1'000
        ),
        {{"AAPL", PortfolioValuationErrorCode::StaleMarketState}}
    );
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"an account without positions needs no quote", test_an_account_without_positions_needs_no_quote},
        {"profitable and losing long positions", test_profitable_and_losing_long_positions},
        {
            "fractional marks use the notional rounding rule",
            test_fractional_marks_use_the_notional_rounding_rule,
        },
        {"fees are deducted exactly once", test_fees_are_deducted_exactly_once},
        {
            "partial sale has realized and unrealized PnL",
            test_partial_sale_has_realized_and_unrealized_pnl,
        },
        {"a closed position needs no mark", test_a_closed_position_needs_no_mark},
        {"several positions reconcile exactly", test_several_positions_reconcile_exactly},
        {"every unmarkable position is reported", test_every_unmarkable_position_is_reported},
        {
            "interrupted or disconnected market fails closed",
            test_an_interrupted_or_disconnected_market_fails_closed_for_held_positions,
        },
        {
            "values near the fixed-point limit are exact or rejected",
            test_values_near_the_fixed_point_limit_are_exact_or_rejected,
        },
        {"overflow at every aggregate is rejected", test_overflow_at_every_aggregate_is_rejected},
        {"unusable quotes cannot mark a position", test_unusable_quotes_cannot_mark_a_position},
        {
            "valuation must continue the account timeline",
            test_valuation_must_continue_the_account_timeline,
        },
        {
            "a closed account is still bound to its timeline",
            test_a_closed_account_is_still_bound_to_its_timeline,
        },
        {
            "valuation rejects a stale market observation",
            test_valuation_rejects_a_stale_market_observation,
        },
    });
}
