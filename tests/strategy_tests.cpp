#include "chronos/canonical_json.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/features/top_of_book_features.hpp"
#include "chronos/sha256.hpp"
#include "chronos/strategy/inference_schedule.hpp"
#include "chronos/strategy/strategy_run_artifact.hpp"
#include "chronos/strategy/strategy_simulation.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace ds = chronos::datasets;
namespace ft = chronos::features;
namespace json = chronos::canonical_json;
namespace st = chronos::strategy;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::Money;
using chronos::PortfolioValuation;
using chronos::PortfolioValuationError;
using chronos::PortfolioValuationErrorCode;
using chronos::Quantity;
using chronos::Timestamp;
using chronos::TradingStatus;
using chronos::ValidatedMarketEvent;
using chronos::test::header;
using chronos::test::kDatasetStream;
using chronos::test::level;
using chronos::test::quote;
using chronos::test::read_file;
using chronos::test::status;
using chronos::test::TemporaryDirectory;
using chronos::test::trade;
using chronos::test::write_file;
using st::AccountRole;
using st::PredictedClass;

constexpr std::int64_t kMillisecond = 1'000'000;

std::int64_t ms(std::int64_t milliseconds) { return milliseconds * kMillisecond; }

ValidatedMarketEvent aapl(
    std::uint64_t sequence,
    std::int64_t at,
    std::string_view bid,
    std::string_view ask,
    std::string_view ask_size = "10"
) {
    return quote(header("AAPL", sequence, at, at), level(bid, "10"), level(ask, ask_size));
}

struct Prediction {
    PredictedClass predicted_class;
    std::uint64_t confidence_ppb;
};

// By ingest sequence. A row without one predicts flat, which holds.
using Predictions = std::map<std::uint64_t, Prediction>;

st::ScheduleWindow window(std::int64_t start, std::optional<std::int64_t> end = std::nullopt) {
    return st::ScheduleWindow{
        .name = end ? "validation" : "test",
        .start_at = Timestamp::from_nanoseconds(start),
        .end_at = end ? std::optional(Timestamp::from_nanoseconds(*end)) : std::nullopt,
    };
}

// The rows the Python package would schedule: every AAPL feature row in the
// window, with its exact model inputs, and a prediction unless they are
// unavailable.
std::vector<st::ScheduleRow> schedule_rows(
    const std::vector<ValidatedMarketEvent>& events,
    const Predictions& predictions,
    const st::ScheduleWindow& schedule_window
) {
    std::vector<st::ScheduleRow> rows;
    ft::TopOfBookFeatureEngine engine;
    for (const auto& event : events) {
        const auto row = engine.apply(event);
        if (!row || row->instrument.value() != "AAPL" || !schedule_window.contains(row->available_at)) {
            continue;
        }
        st::ScheduleRow scheduled{
            .ingest_sequence = row->ingest_sequence,
            .available_at = row->available_at,
            .inputs = st::model_inputs(*row),
            .prediction = std::nullopt,
        };
        if (std::holds_alternative<std::array<std::int64_t, st::kModelInputs>>(scheduled.inputs)) {
            const auto found = predictions.find(row->ingest_sequence);
            scheduled.prediction = found == predictions.end()
                ? st::SchedulePrediction{.predicted_class = PredictedClass::Flat, .confidence_ppb = 1'000'000'000}
                : st::SchedulePrediction{
                      .predicted_class = found->second.predicted_class,
                      .confidence_ppb = found->second.confidence_ppb,
                  };
        }
        rows.push_back(std::move(scheduled));
    }
    return rows;
}

struct Run {
    std::vector<json::Value> journal;
    std::optional<st::StrategyResults> results;
    std::optional<ds::DatasetError> error;
    std::uint64_t unused_after_window{0};
};

Run simulate(
    const std::vector<ValidatedMarketEvent>& events,
    const Predictions& predictions,
    const st::ScheduleWindow& schedule_window,
    const st::StrategyConfiguration& configuration = {},
    const std::function<void(std::vector<st::ScheduleRow>&)>& tamper = {}
) {
    std::vector<st::ScheduleRow> rows = schedule_rows(events, predictions, schedule_window);
    if (tamper) {
        tamper(rows);
    }
    Run run;
    std::size_t next = 0;
    st::StrategySimulation simulation(
        configuration,
        InstrumentId("AAPL"),
        MarketDataStreamId(std::string(kDatasetStream)),
        schedule_window,
        "schedule:test",
        [&](const json::Value& entry) -> std::optional<ds::DatasetError> {
            run.journal.push_back(entry);
            return std::nullopt;
        }
    );
    const st::ScheduleRowSource source = [&]() -> std::variant<st::ScheduleRow, ds::EndOfRows, ds::DatasetError> {
        if (next == rows.size()) {
            return ds::EndOfRows{};
        }
        return rows[next++];
    };
    for (const auto& event : events) {
        if (!simulation.accepts(event)) {
            ++run.unused_after_window;
            continue;
        }
        if (auto error = simulation.apply(event, source)) {
            run.error = std::move(error);
            return run;
        }
    }
    auto finished = simulation.finish();
    if (auto* error = std::get_if<ds::DatasetError>(&finished)) {
        run.error = *error;
        return run;
    }
    REQUIRE(next == rows.size());
    run.results = std::get<st::StrategyResults>(std::move(finished));
    return run;
}

const json::Value& member(const json::Value& value, std::string_view key) {
    const json::Object* object = value.if_object();
    REQUIRE(object != nullptr);
    for (const auto& [name, entry] : *object) {
        if (name == key) {
            return entry;
        }
    }
    throw chronos::test::TestFailure("no member " + std::string(key));
}

std::string text(const json::Value& value, std::initializer_list<std::string_view> path) {
    const json::Value* current = &value;
    for (std::string_view key : path) {
        current = &member(*current, key);
    }
    REQUIRE(current->if_string() != nullptr);
    return *current->if_string();
}

bool is_null(const json::Value& value, std::initializer_list<std::string_view> path) {
    const json::Value* current = &value;
    for (std::string_view key : path) {
        current = &member(*current, key);
    }
    return current->is_null();
}

std::vector<json::Value> entries(const Run& run, std::string_view type, std::string_view account = {}) {
    std::vector<json::Value> found;
    for (const auto& entry : run.journal) {
        if (text(entry, {"type"}) == type && (account.empty() || text(entry, {"account"}) == account)) {
            found.push_back(entry);
        }
    }
    return found;
}

const st::AccountResult& result(const Run& run, AccountRole role) {
    REQUIRE(run.results.has_value());
    return run.results->accounts[static_cast<std::size_t>(role)];
}

const PortfolioValuation& valued(const st::AccountResult& account) {
    REQUIRE(std::holds_alternative<PortfolioValuation>(account.final_valuation));
    return std::get<PortfolioValuation>(account.final_valuation);
}

Money money(std::string_view value) { return Money::parse(value); }
Quantity shares(std::string_view value) { return Quantity::parse(value); }

Quantity position(const st::AccountResult& account) {
    return account.final_state.quantity(InstrumentId("AAPL"));
}

std::string journal_bytes(const Run& run) {
    std::string bytes;
    for (const auto& entry : run.journal) {
        bytes += json::serialize(entry) + "\n";
    }
    return bytes;
}

void a_signal_executes_after_its_latency_and_never_on_its_own_quote() {
    const std::vector<ValidatedMarketEvent> events{
        aapl(0, ms(900), "100", "100.1"),
        aapl(1, ms(1000), "100", "100.1"),
        // Received at the signal's own time: before the order arrives.
        aapl(2, ms(1000), "101", "101.1"),
        // Another instrument never executes an AAPL order.
        quote(header("SPY", 3, ms(1002), ms(1002)), level("500", "10"), level("500.1", "10")),
        aapl(4, ms(1002), "100.5", "100.6"),
        aapl(5, ms(1003), "100.4", "100.45"),
    };
    const Run run = simulate(events, {{1, {PredictedClass::Up, 900'000'000}}}, window(ms(1000)));
    REQUIRE(!run.error);
    const auto executions = entries(run, "execution", "strategy");
    REQUIRE(executions.size() == 1);
    const json::Value& execution = executions.front();
    REQUIRE(text(execution, {"signal_at_ns"}) == std::to_string(ms(1000)));
    REQUIRE(text(execution, {"arrival_at_ns"}) == std::to_string(ms(1001)));
    REQUIRE(text(execution, {"executed_at_ns"}) == std::to_string(ms(1002)));
    REQUIRE(text(execution, {"observation", "ingest_sequence"}) == "4");
    REQUIRE(text(execution, {"outcome"}) == "filled");
    REQUIRE(text(execution, {"fill", "price"}) == "100.6");
    REQUIRE(text(execution, {"fill", "quantity"}) == "1");
    REQUIRE(text(execution, {"fill", "fee"}) == "0.01");
    REQUIRE(run.results->warmup_events == 1);
    REQUIRE(run.results->window_events == 5);
    // The AAPL quotes in the window; SPY's is not the schedule's instrument.
    REQUIRE(run.results->schedule_rows == 4);
    REQUIRE(run.results->decisions.evaluated == 1);
    REQUIRE(run.results->decisions.skipped_decision_interval == 3);
    // 10000 - 100.6 - 0.01, marked at the last quote's best bid, 100.4.
    const auto& strategy = result(run, AccountRole::Strategy);
    REQUIRE(strategy.final_state.cash == money("9899.39"));
    REQUIRE(valued(strategy).snapshot.equity == money("9999.79"));
    REQUIRE(valued(strategy).snapshot.gross_unrealized_pnl == money("-0.2"));
    REQUIRE(valued(strategy).snapshot.net_total_pnl == money("-0.21"));
    // Buy and hold entered at the same decision and filled on the same quote.
    const auto benchmark = entries(run, "execution", "buy_and_hold");
    REQUIRE(benchmark.size() == 1);
    REQUIRE(text(benchmark.front(), {"fill", "price"}) == "100.6");
}

void the_policy_buys_holds_and_sells_by_class_and_confidence() {
    const std::vector<ValidatedMarketEvent> events{
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1002), "100", "100.1"),
        aapl(2, ms(2000), "100", "100.1"),
        aapl(3, ms(3000), "100", "100.1"),
        aapl(4, ms(4000), "100", "100.1"),
        aapl(5, ms(5000), "101", "101.1"),
        aapl(6, ms(5002), "101.2", "101.3"),
        aapl(7, ms(6000), "101", "101.1"),
    };
    const Run run = simulate(events, {
        {0, {PredictedClass::Up, 900'000'000}},
        {2, {PredictedClass::Up, 499'999'999}},
        {3, {PredictedClass::Flat, 950'000'000}},
        {4, {PredictedClass::Up, 900'000'000}},
        // Exactly at the threshold acts.
        {5, {PredictedClass::Down, 500'000'000}},
        {7, {PredictedClass::Down, 900'000'000}},
    }, window(ms(1000)));
    REQUIRE(!run.error);
    const st::DecisionCounts& decisions = run.results->decisions;
    REQUIRE(decisions.evaluated == 6);
    REQUIRE(decisions.orders == 2);
    REQUIRE(decisions.hold_below_threshold == 1);
    REQUIRE(decisions.hold_flat_prediction == 1);
    REQUIRE(decisions.already_at_target == 2);
    REQUIRE(decisions.skipped_decision_interval == 2);
    const auto reasons = entries(run, "decision", "strategy");
    REQUIRE(reasons.size() == 6);
    REQUIRE(text(reasons[0], {"reason"}) == "order_scheduled");
    REQUIRE(text(reasons[1], {"reason"}) == "hold_below_threshold");
    REQUIRE(is_null(reasons[1], {"target"}));
    REQUIRE(text(reasons[2], {"reason"}) == "hold_flat_prediction");
    REQUIRE(text(reasons[3], {"reason"}) == "already_at_target");
    REQUIRE(text(reasons[4], {"reason"}) == "order_scheduled");
    REQUIRE(text(reasons[4], {"target"}) == "0");
    REQUIRE(text(reasons[5], {"reason"}) == "already_at_target");
    const auto orders = entries(run, "order", "strategy");
    REQUIRE(orders.size() == 2);
    REQUIRE(text(orders[1], {"side"}) == "sell");
    // Bought at the ask, 100.1; sold at the bid, 101.2; two fees of 0.01.
    const auto& strategy = result(run, AccountRole::Strategy);
    REQUIRE(position(strategy) == Quantity{});
    REQUIRE(strategy.final_state.gross_realized_pnl() == money("1.1"));
    REQUIRE(strategy.final_state.fees_paid() == money("0.02"));
    REQUIRE(valued(strategy).snapshot.equity == money("10001.08"));
    REQUIRE(valued(strategy).return_percent == chronos::Percentage::parse("0.0108"));
    REQUIRE(strategy.turnover == money("201.3"));
    // Buy and hold still holds its one share, marked at the last bid, 101.
    const auto& benchmark = result(run, AccountRole::BuyAndHold);
    REQUIRE(position(benchmark) == shares("1"));
    REQUIRE(valued(benchmark).snapshot.equity == money("10000.89"));
    REQUIRE(benchmark.orders.scheduled == 1);
    const auto& cash = result(run, AccountRole::Cash);
    REQUIRE(cash.orders.scheduled == 0);
    REQUIRE(valued(cash).snapshot.equity == money("10000"));
}

void the_decision_interval_allows_at_most_one_decision_per_interval() {
    const std::vector<ValidatedMarketEvent> events{
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1500), "100", "100.1"),
        aapl(2, ms(2000) - 1, "100", "100.1"),
        aapl(3, ms(2000), "100", "100.1"),
    };
    const Run every_second = simulate(events, {}, window(ms(1000)));
    REQUIRE(every_second.results->decisions.evaluated == 2);
    REQUIRE(every_second.results->decisions.skipped_decision_interval == 2);
    st::StrategyConfiguration half_second;
    half_second.decision_interval_ns = ms(500);
    const Run faster = simulate(events, {}, window(ms(1000)), half_second);
    REQUIRE(faster.results->decisions.evaluated == 3);
    REQUIRE(faster.results->decisions.skipped_decision_interval == 1);
}

void the_first_eligible_observation_decides_the_order_whatever_it_is() {
    const Predictions buy{{0, {PredictedClass::Up, 900'000'000}}};
    const auto suspect = MarketDataQuality{}
        .with(MarketDataQualityFlag::Replay)
        .with(MarketDataQualityFlag::PotentiallyInvalidBook);
    {
        // A suspect quote is the first observation after arrival: rejected,
        // even though a better quote follows.
        const Run run = simulate({
            aapl(0, ms(1000), "100", "100.1"),
            quote(header("AAPL", 1, ms(1002), ms(1002), suspect), level("100", "10"), level("100.1", "10")),
            aapl(2, ms(1003), "99", "99.1"),
        }, buy, window(ms(1000)));
        REQUIRE(!run.error);
        const auto& strategy = result(run, AccountRole::Strategy);
        REQUIRE(strategy.orders.rejected == 1);
        REQUIRE(strategy.orders.rejections.at("suspect_market_state") == 1);
        REQUIRE(strategy.fills == 0);
        REQUIRE(position(strategy) == Quantity{});
        REQUIRE(strategy.final_state.cash == money("10000"));
        // The suspect row has no model inputs, so no prediction.
        REQUIRE(run.results->decisions.skipped_prediction_unavailable == 1);
        REQUIRE(text(entries(run, "execution", "strategy").front(), {"rejection", "by"}) == "pre_trade_risk");
    }
    {
        // No ask to buy from.
        const Run run = simulate({
            aapl(0, ms(1000), "100", "100.1"),
            quote(header("AAPL", 1, ms(1002), ms(1002)), level("100", "10"), std::nullopt),
            aapl(2, ms(1003), "100", "100.1"),
        }, buy, window(ms(1000)));
        REQUIRE(result(run, AccountRole::Strategy).orders.rejections.at("missing_opposite_book_side") == 1);
    }
    {
        // Halted at the execution observation.
        const Run run = simulate({
            aapl(0, ms(1000), "100", "100.1"),
            status(header("AAPL", 1, ms(1001), ms(1001)), TradingStatus::Halted),
            aapl(2, ms(1002), "100", "100.1"),
        }, buy, window(ms(1000)));
        const auto& strategy = result(run, AccountRole::Strategy);
        REQUIRE(strategy.orders.rejections.at("trading_halted") == 1);
        REQUIRE(text(entries(run, "execution", "strategy").front(), {"rejection", "by"}) == "runner");
        REQUIRE(run.results->decisions.skipped_prediction_unavailable == 1);
    }
    {
        // Trades and other instruments' quotes are not execution observations.
        const Run run = simulate({
            aapl(0, ms(1000), "100", "100.1"),
            trade(header("AAPL", 1, ms(1002), ms(1002)), "100.1", "5"),
            quote(header("SPY", 2, ms(1003), ms(1003)), level("500", "10"), level("500.1", "10")),
            aapl(3, ms(1005), "100.2", "100.3"),
        }, buy, window(ms(1000)));
        const auto executions = entries(run, "execution", "strategy");
        REQUIRE(executions.size() == 1);
        REQUIRE(text(executions.front(), {"observation", "ingest_sequence"}) == "3");
        REQUIRE(text(executions.front(), {"fill", "price"}) == "100.3");
    }
}

void a_partial_fill_leaves_the_rest_to_the_next_decision() {
    const Run run = simulate({
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1002), "100", "100.1", "0.4"),
        aapl(2, ms(2000), "100", "100.1"),
        aapl(3, ms(2002), "100", "100.1"),
    }, {{0, {PredictedClass::Up, 900'000'000}}, {2, {PredictedClass::Up, 900'000'000}}}, window(ms(1000)));
    REQUIRE(!run.error);
    const auto executions = entries(run, "execution", "strategy");
    REQUIRE(executions.size() == 2);
    REQUIRE(text(executions[0], {"outcome"}) == "partially_filled_and_cancelled");
    REQUIRE(text(executions[0], {"cancellation_reason"}) == "displayed_liquidity_exhausted");
    REQUIRE(text(executions[0], {"fill", "quantity"}) == "0.4");
    REQUIRE(text(executions[0], {"position_after"}) == "0.4");
    const auto orders = entries(run, "order", "strategy");
    REQUIRE(text(orders[1], {"quantity"}) == "0.6");
    REQUIRE(text(executions[1], {"outcome"}) == "filled");
    const auto& strategy = result(run, AccountRole::Strategy);
    REQUIRE(position(strategy) == shares("1"));
    REQUIRE(strategy.fills == 2);
    // 10000 - 0.4 x 100.1 - 0.01 - 0.6 x 100.1 - 0.01.
    REQUIRE(strategy.final_state.cash == money("9899.88"));
    REQUIRE(strategy.final_state.fees_paid() == money("0.02"));
    // Buy and hold enters once and keeps its partial fill.
    REQUIRE(position(result(run, AccountRole::BuyAndHold)) == shares("0.4"));
}

void a_rejected_order_is_recorded_and_changes_nothing() {
    st::StrategyConfiguration poor;
    poor.starting_cash = money("50");
    const Run run = simulate({
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1002), "100", "100.1"),
    }, {{0, {PredictedClass::Up, 900'000'000}}}, window(ms(1000)), poor);
    REQUIRE(!run.error);
    const auto& strategy = result(run, AccountRole::Strategy);
    REQUIRE(strategy.orders.rejections.at("insufficient_cash") == 1);
    REQUIRE(strategy.final_state.cash == money("50"));
    REQUIRE(strategy.final_state.fees_paid() == Money{});
    REQUIRE(text(entries(run, "execution", "strategy").front(), {"outcome"}) == "rejected");
    REQUIRE(is_null(entries(run, "execution", "strategy").front(), {"fill"}));
}

void pending_orders_expire_at_the_window_end_and_block_new_orders() {
    {
        const Run run = simulate({
            aapl(0, ms(900), "100", "100.1"),
            aapl(1, ms(1999) + 500'000, "100", "100.1"),
            // At the window's end: never applied, so it cannot execute.
            aapl(2, ms(2000), "100", "100.1"),
        }, {{1, {PredictedClass::Up, 900'000'000}}}, window(ms(1000), ms(2000)));
        REQUIRE(!run.error);
        REQUIRE(run.unused_after_window == 1);
        REQUIRE(result(run, AccountRole::Strategy).orders.expired == 1);
        REQUIRE(result(run, AccountRole::BuyAndHold).orders.expired == 1);
        REQUIRE(result(run, AccountRole::BuyAndHold).fills == 0);
        REQUIRE(entries(run, "order_expired").size() == 2);
        REQUIRE(text(entries(run, "order_expired").front(), {"reason"}) == "window_ended_before_an_eligible_observation");
        REQUIRE(position(result(run, AccountRole::Strategy)) == Quantity{});
        REQUIRE(run.results->valued_at == Timestamp::from_nanoseconds(ms(1999) + 500'000));
    }
    {
        st::StrategyConfiguration rapid;
        rapid.decision_interval_ns = 1;
        const Run run = simulate({
            aapl(0, ms(1000), "100", "100.1"),
            aapl(1, ms(1000) + 500'000, "100", "100.1"),
            // Received exactly at the arrival time: eligible.
            aapl(2, ms(1001), "100", "100.1"),
        }, {
            {0, {PredictedClass::Up, 900'000'000}},
            {1, {PredictedClass::Up, 900'000'000}},
            {2, {PredictedClass::Up, 900'000'000}},
        }, window(ms(1000)), rapid);
        REQUIRE(!run.error);
        REQUIRE(run.results->decisions.order_pending == 1);
        REQUIRE(run.results->decisions.already_at_target == 1);
        REQUIRE(text(entries(run, "execution", "strategy").front(), {"executed_at_ns"}) == std::to_string(ms(1001)));
        REQUIRE(position(result(run, AccountRole::Strategy)) == shares("1"));
    }
}

void warmup_events_update_the_market_but_never_trade() {
    const Run run = simulate({
        status(header("AAPL", 0, ms(500), ms(500)), TradingStatus::Halted),
        aapl(1, ms(600), "90", "90.1"),
        status(header("AAPL", 2, ms(700), ms(700)), TradingStatus::Open),
        aapl(3, ms(800), "95", "95.1"),
        aapl(4, ms(1000), "100", "100.1"),
        aapl(5, ms(1002), "100", "100.1"),
    }, {{3, {PredictedClass::Up, 900'000'000}}, {4, {PredictedClass::Up, 900'000'000}}}, window(ms(1000)));
    REQUIRE(!run.error);
    REQUIRE(run.results->warmup_events == 4);
    REQUIRE(run.results->schedule_rows == 2);
    REQUIRE(run.results->decisions.evaluated == 1);
    REQUIRE(text(entries(run, "decision", "strategy").front(), {"signal", "ingest_sequence"}) == "4");
    REQUIRE(text(entries(run, "execution", "strategy").front(), {"outcome"}) == "filled");
}

std::vector<ValidatedMarketEvent> base_session() {
    return {
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1002), "100", "100.1"),
        aapl(2, ms(2000), "99.5", "99.6"),
        aapl(3, ms(3000), "100.5", "100.6"),
        aapl(4, ms(4000), "100.2", "100.3"),
        aapl(5, ms(4002), "100.25", "100.35"),
    };
}

const Predictions kBaseSessionPredictions{
    {0, {PredictedClass::Up, 900'000'000}},
    {2, {PredictedClass::Up, 900'000'000}},
    {3, {PredictedClass::Up, 900'000'000}},
    {4, {PredictedClass::Down, 900'000'000}},
};

void events_after_the_window_cannot_change_the_result() {
    std::vector<ValidatedMarketEvent> quiet = base_session();
    std::vector<ValidatedMarketEvent> eventful = base_session();
    eventful.push_back(aapl(6, ms(5000), "1", "1.01"));
    eventful.push_back(status(header("AAPL", 7, ms(5001), ms(5001)), TradingStatus::Halted));
    eventful.push_back(aapl(8, ms(6000), "500", "500.1"));
    const auto end = window(ms(1000), ms(5000));
    const Run first = simulate(quiet, kBaseSessionPredictions, end);
    const Run second = simulate(eventful, kBaseSessionPredictions, end);
    REQUIRE(!first.error);
    REQUIRE(!second.error);
    REQUIRE(second.unused_after_window == 3);
    REQUIRE(journal_bytes(first) == journal_bytes(second));
    const st::StrategyConfiguration defaults;
    REQUIRE(json::serialize(st::results_json(*first.results, defaults)) ==
            json::serialize(st::results_json(*second.results, defaults)));
    // Applying one anyway is refused.
    st::StrategySimulation simulation(
        defaults, InstrumentId("AAPL"), MarketDataStreamId(std::string(kDatasetStream)), end, "schedule:test",
        [](const json::Value&) { return std::optional<ds::DatasetError>{}; }
    );
    const auto late = aapl(0, ms(5000), "100", "100.1");
    REQUIRE(!simulation.accepts(late));
    const auto refused = simulation.apply(late, [] { return std::variant<st::ScheduleRow, ds::EndOfRows, ds::DatasetError>(ds::EndOfRows{}); });
    REQUIRE(refused.has_value());
}

void no_signal_means_no_trades_and_down_never_goes_short() {
    // Every prediction holds, falls short of the threshold, or asks for the
    // flat position the account already has.
    const Run run = simulate(base_session(), {
        {0, {PredictedClass::Down, 900'000'000}},
        {2, {PredictedClass::Up, 100'000'000}},
        {3, {PredictedClass::Down, 1'000'000'000}},
    }, window(ms(1000)));
    REQUIRE(!run.error);
    const auto& strategy = result(run, AccountRole::Strategy);
    REQUIRE(strategy.orders.scheduled == 0);
    REQUIRE(entries(run, "order", "strategy").empty());
    REQUIRE(position(strategy) == Quantity{});
    REQUIRE(valued(strategy).snapshot.equity == money("10000"));
    REQUIRE(run.results->decisions.already_at_target == 2);
    for (const auto& entry : entries(run, "valuation")) {
        REQUIRE(text(entry, {"accounts", "strategy", "position"}) == "0");
    }
    // Buy and hold still enters, at the first decision.
    REQUIRE(result(run, AccountRole::BuyAndHold).fills == 1);
}

void an_order_above_the_notional_limit_is_rejected() {
    st::StrategyConfiguration small;
    small.maximum_order_notional = money("100");
    const Run run = simulate({
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1002), "100", "100.1"),
    }, {{0, {PredictedClass::Up, 900'000'000}}}, window(ms(1000)), small);
    REQUIRE(!run.error);
    REQUIRE(result(run, AccountRole::Strategy).orders.rejections.at("order_notional_limit_exceeded") == 1);
    REQUIRE(result(run, AccountRole::BuyAndHold).orders.rejections.at("order_notional_limit_exceeded") == 1);
    REQUIRE(result(run, AccountRole::Strategy).final_state.cash == money("10000"));
}

void changing_a_later_event_never_changes_an_earlier_decision() {
    // What the journal holds before the last event is applied.
    const auto before_last = [](const Run& run) {
        std::string bytes;
        for (const auto& entry : run.journal) {
            const std::string type = text(entry, {"type"});
            if (type == "order_expired" || (type == "valuation" && text(entry, {"trigger"}) == "window_end")) {
                continue;
            }
            bytes += json::serialize(entry) + "\n";
        }
        return bytes;
    };
    std::vector<ValidatedMarketEvent> prefix = base_session();
    prefix.pop_back();
    const Run truncated = simulate(prefix, kBaseSessionPredictions, window(ms(1000)));
    std::vector<ValidatedMarketEvent> changed = base_session();
    changed.back() = aapl(5, ms(4002), "1", "1.01");
    const Run base = simulate(base_session(), kBaseSessionPredictions, window(ms(1000)));
    const Run altered = simulate(changed, kBaseSessionPredictions, window(ms(1000)));
    const std::string common = before_last(truncated);
    REQUIRE(!common.empty());
    REQUIRE(journal_bytes(base).starts_with(common));
    REQUIRE(journal_bytes(altered).starts_with(common));
    REQUIRE(journal_bytes(base) != journal_bytes(altered));
}

void a_schedule_that_disagrees_with_the_recomputed_features_is_refused() {
    const auto in_window = window(ms(1000));
    const Run changed_inputs = simulate(base_session(), kBaseSessionPredictions, in_window, {}, [](auto& rows) {
        std::get<std::array<std::int64_t, st::kModelInputs>>(rows[2].inputs)[0] += 1;
    });
    REQUIRE(changed_inputs.error.has_value());
    REQUIRE(changed_inputs.error->code == ds::DatasetErrorCode::IdentityMismatch);
    const Run changed_identity = simulate(base_session(), kBaseSessionPredictions, in_window, {}, [](auto& rows) {
        rows[1].ingest_sequence = 9;
    });
    REQUIRE(changed_identity.error->code == ds::DatasetErrorCode::IdentityMismatch);
    const Run changed_time = simulate(base_session(), kBaseSessionPredictions, in_window, {}, [](auto& rows) {
        rows[1].available_at = Timestamp::from_nanoseconds(rows[1].available_at.nanoseconds() + 1);
    });
    REQUIRE(changed_time.error->code == ds::DatasetErrorCode::IdentityMismatch);
    const Run short_schedule = simulate(base_session(), kBaseSessionPredictions, in_window, {}, [](auto& rows) {
        rows.pop_back();
    });
    REQUIRE(short_schedule.error->code == ds::DatasetErrorCode::CountMismatch);
}

void an_unmarkable_position_makes_the_valuation_explicitly_unavailable() {
    const Run run = simulate({
        aapl(0, ms(1000), "100", "100.1"),
        aapl(1, ms(1002), "100", "100.1"),
        // The market clock moves 6 s past AAPL's last quote: too old to mark.
        quote(header("SPY", 2, ms(7003), ms(7003)), level("500", "10"), level("500.1", "10")),
    }, {{0, {PredictedClass::Up, 900'000'000}}}, window(ms(1000)));
    REQUIRE(!run.error);
    const auto& strategy = result(run, AccountRole::Strategy);
    REQUIRE(std::holds_alternative<PortfolioValuationError>(strategy.final_valuation));
    REQUIRE(std::get<PortfolioValuationError>(strategy.final_valuation).code ==
            PortfolioValuationErrorCode::PositionMarkUnavailable);
    // The ledger is still exact.
    REQUIRE(strategy.final_state.cash == money("9899.89"));
    REQUIRE(strategy.drawdown.points_unvalued == 1);
    const json::Value document = st::results_json(*run.results, {});
    const json::Value& valuation = member(member(member(member(document, "accounts"), "strategy"), "final"), "valuation");
    REQUIRE(text(valuation, {"unavailable", "code"}) == "position_mark_unavailable");
    REQUIRE(text(member(member(valuation, "unavailable"), "instruments").if_array()->front(), {"reason"}) ==
            "stale_market_state");
    // A flat account needs no mark.
    REQUIRE(valued(result(run, AccountRole::Cash)).snapshot.equity == money("10000"));
}

void drawdown_and_turnover_follow_their_definitions() {
    const Run run = simulate(base_session(), kBaseSessionPredictions, window(ms(1000)));
    REQUIRE(!run.error);
    const auto& strategy = result(run, AccountRole::Strategy);
    // Equity at the valuation points: 10000 (1000 ms), 9999.89 (fill at
    // 100.1, marked 100), 9999.39 (2000 ms), 10000.39 (3000 ms), 10000.09
    // (4000 ms), 10000.13 (sold at 100.25), 10000.13 (the window's end).
    REQUIRE(strategy.drawdown.points_valued == 7);
    REQUIRE(strategy.drawdown.points_unvalued == 0);
    REQUIRE(strategy.drawdown.maximum == money("0.61"));
    REQUIRE(strategy.drawdown.maximum_percent_of_peak == chronos::Percentage::parse("0.0061"));
    REQUIRE(strategy.drawdown.peak_at == Timestamp::from_nanoseconds(ms(1000)));
    REQUIRE(strategy.drawdown.trough_at == Timestamp::from_nanoseconds(ms(2000)));
    REQUIRE(strategy.turnover == money("200.35"));
    REQUIRE(strategy.traded_quantity == shares("2"));
    REQUIRE(strategy.final_state.gross_realized_pnl() == money("0.15"));
    REQUIRE(valued(strategy).snapshot.net_total_pnl == money("0.13"));
    const json::Value document = st::results_json(*run.results, {});
    REQUIRE(text(document, {"accounts", "strategy", "turnover", "percent_of_starting_cash"}) == "2.0035");
    // Buy and hold: one share from 100.1, marked at the last bid, 100.25.
    const auto& benchmark = result(run, AccountRole::BuyAndHold);
    REQUIRE(valued(benchmark).snapshot.equity == money("10000.14"));
    REQUIRE(benchmark.drawdown.maximum == money("0.61"));
    // Cash never moves.
    const auto& cash = result(run, AccountRole::Cash);
    REQUIRE(cash.drawdown.maximum == Money{});
    REQUIRE(!cash.drawdown.maximum_percent_of_peak);
    REQUIRE(is_null(document, {"accounts", "cash", "drawdown", "maximum_percent_of_peak"}));
}

void the_same_inputs_give_the_same_journal_and_results() {
    const Run first = simulate(base_session(), kBaseSessionPredictions, window(ms(1000)));
    const Run second = simulate(base_session(), kBaseSessionPredictions, window(ms(1000)));
    REQUIRE(!first.journal.empty());
    REQUIRE(journal_bytes(first) == journal_bytes(second));
    REQUIRE(json::serialize(st::results_json(*first.results, {})) == json::serialize(st::results_json(*second.results, {})));
}

void configurations_that_cannot_run_are_refused() {
    st::StrategyConfiguration configuration;
    REQUIRE(!st::configuration_error(configuration));
    configuration.latency_ns = 0;
    REQUIRE(st::configuration_error(configuration).has_value());
    REQUIRE_THROWS_AS(
        st::StrategySimulation(configuration, InstrumentId("AAPL"), MarketDataStreamId("s"), window(0), "schedule:test",
            [](const json::Value&) { return std::optional<ds::DatasetError>{}; }),
        std::invalid_argument
    );
    configuration = {};
    configuration.threshold_ppb = 1'000'000'001;
    REQUIRE(st::configuration_error(configuration).has_value());
    configuration = {};
    configuration.target_quantity = Quantity{};
    REQUIRE(st::configuration_error(configuration).has_value());
    configuration = {};
    configuration.fee_per_fill = money("-0.01");
    REQUIRE(st::configuration_error(configuration).has_value());
    configuration = {};
    configuration.fee_per_fill = Money{};
    REQUIRE(!st::configuration_error(configuration).has_value());
}

// --- Artifacts -------------------------------------------------------------

struct ScheduleSpec {
    std::string dataset_id{};
    std::string window_name{"test"};
    std::int64_t start_at{ms(1000)};
    std::optional<std::int64_t> end_at{};
    std::int64_t validation_start_at{ms(500)};
    std::int64_t test_start_at{ms(1000)};
    std::int64_t training_through{ms(400)};
    Predictions predictions{kBaseSessionPredictions};
    // A row no event of the dataset has.
    bool extra_row{false};
    // A version 2 schedule of a registered model version with this cutoff;
    // a version 1 schedule of an experiment model without one.
    std::optional<std::int64_t> registered_cutoff_at{};
    // A version 2 schedule that names an experiment model, as no publisher may.
    bool registered_with_experiment{false};
};

json::Value inference_definition(bool registered = false) {
    return json::Value(json::Object{
        {"class", registered ? "the model version's own class rule, stated in model.runtime.class"
                             : "the model's predicted class: the largest logit, ties to the earliest class in class_order"},
        {"confidence_ppb", "floor(1e9 x the predicted class's probability), exact from its binary64 value"},
        {"rows", "every feature row of the instrument available in the window, labelled or not"},
        {"unavailable", "a row whose model inputs are unavailable has no prediction from any model"},
        {"window", "start_at_ns <= available_at_ns, and available_at_ns < end_at_ns when the window has an end"},
    });
}

json::Value model_feature_set() {
    const auto input = [](std::string_view name, std::string_view definition, std::string_view scale) {
        return json::Value(json::Object{{"definition", definition}, {"name", name}, {"scale", scale}});
    };
    return json::Value(json::Object{
        {"inputs", json::Array{
            input("spread", "feature spread", "1000000000"),
            input("imbalance", "feature imbalance", "1000000000"),
            input("microprice_minus_midpoint", "feature microprice minus feature midpoint, exact", "1000000000"),
            input("bid_size", "feature bid_size", "100000000"),
            input("ask_size", "feature ask_size", "100000000"),
        }},
        {"name", "top_of_book_baseline"},
        {"version", "1"},
    });
}

// Writes a schedule exactly as the Python package publishes one.
std::filesystem::path write_schedule(
    const std::filesystem::path& root,
    const std::vector<ValidatedMarketEvent>& events,
    const ScheduleSpec& spec
) {
    const auto schedule_window = st::ScheduleWindow{
        .name = spec.window_name,
        .start_at = Timestamp::from_nanoseconds(spec.start_at),
        .end_at = spec.end_at ? std::optional(Timestamp::from_nanoseconds(*spec.end_at)) : std::nullopt,
    };
    std::vector<st::ScheduleRow> rows = schedule_rows(events, spec.predictions, schedule_window);
    if (spec.extra_row) {
        st::ScheduleRow extra = rows.back();
        extra.ingest_sequence += 100;
        rows.push_back(extra);
    }
    std::string bytes;
    std::map<std::string, std::uint64_t> classes{{"down", 0}, {"flat", 0}, {"up", 0}};
    std::map<std::string, std::uint64_t> unavailable;
    std::uint64_t predicted = 0;
    for (const auto& row : rows) {
        json::Value inputs;
        json::Value prediction;
        if (const auto* raw = std::get_if<std::array<std::int64_t, st::kModelInputs>>(&row.inputs)) {
            json::Array values;
            for (std::int64_t value : *raw) {
                values.emplace_back(std::to_string(value));
            }
            inputs = json::Value(json::Object{{"raw", std::move(values)}});
            const std::string name(st::predicted_class_name(row.prediction->predicted_class));
            prediction = json::Value(json::Object{
                {"class", name},
                {"confidence_ppb", std::to_string(row.prediction->confidence_ppb)},
                {"probabilities", json::Array{"0.05", "0.05", "0.9"}},
            });
            ++classes[name];
            ++predicted;
        } else {
            const std::string reason(ft::unavailability_name(std::get<ft::Unavailability>(row.inputs)));
            inputs = json::Value(json::Object{{"unavailable", reason}});
            prediction = json::Value(json::Object{{"unavailable", "feature_unavailable:" + reason}});
            ++unavailable["feature_unavailable:" + reason];
        }
        bytes += json::serialize(json::Value(json::Object{
            {"available_at_ns", std::to_string(row.available_at.nanoseconds())},
            {"identity", json::Object{
                {"ingest_sequence", std::to_string(row.ingest_sequence)},
                {"instrument", "AAPL"},
                {"stream", kDatasetStream},
            }},
            {"inputs", std::move(inputs)},
            {"prediction", std::move(prediction)},
            {"record", json::Object{{"name", "chronos.inference_row"}, {"version", "1"}}},
        })) + "\n";
    }
    json::Object class_counts;
    for (const auto& [name, n] : classes) {
        class_counts.emplace_back(name, std::to_string(n));
    }
    const std::string sha(64, 'b');
    const bool registered = spec.registered_cutoff_at.has_value();
    const json::Value body(json::Object{
        {"class_order", json::Array{"down", "flat", "up"}},
        {"counts", json::Object{
            {"classes", std::move(class_counts)},
            {"predicted", std::to_string(predicted)},
            {"rows", std::to_string(rows.size())},
            {"unavailable", ds::count_map_json(unavailable)},
        }},
        {"environment", json::Object{{"python", "CPython 3.12"}}},
        {"files", json::Object{{"schedule", json::Object{
            {"name", "schedule.ndjson"},
            {"sha256", chronos::Sha256::hex(bytes)},
            {"size_bytes", std::to_string(bytes.size())},
        }}}},
        {"inference", inference_definition(registered)},
        {"model", registered && !spec.registered_with_experiment
             ? json::Object{
                   {"family", "onnx"},
                   {"instrument", "AAPL"},
                   {"model_feature_set", model_feature_set()},
                   {"model_version_id", std::string(64, '7')},
                   {"payload_sha256", std::string(64, '8')},
                   {"runtime", json::Object{
                       {"class", "the first largest probability"},
                       {"name", "chronos.onnx_sandbox"},
                       {"version", "1"},
                   }},
               }
             : json::Object{
                   {"experiment_id", std::string(64, 'e')},
                   {"instrument", "AAPL"},
                   {"model_feature_set", model_feature_set()},
                   {"name", "logistic"},
                   {"parameters_sha256", std::string(64, 'd')},
               }},
        {"row", json::Object{{"name", "chronos.inference_row"}, {"version", "1"}}},
        {"schema", json::Object{{"name", "chronos.inference_schedule"}, {"version", registered ? "2" : "1"}}},
        {"scope", "model predictions only"},
        {"source", json::Object{
            {"dataset_id", spec.dataset_id},
            {"feature_artifact_id", std::string(64, 'f')},
            {"feature_set", "top_of_book_v1"},
            {"features_sha256", sha},
            {"horizon_ns", "1000000000"},
            {"label_artifact_id", std::string(64, 'c')},
            {"label_set", "future_midpoint_return_v1"},
            {"labels_sha256", sha},
            {"split_artifact_id", std::string(64, 'a')},
            {"split_set", "chronological_purged_v1"},
            {"splits_sha256", sha},
            {"stream", kDatasetStream},
            {"test_start_at_ns", std::to_string(spec.test_start_at)},
            {"validation_start_at_ns", std::to_string(spec.validation_start_at)},
        }},
        {"training", registered
             ? json::Object{
                   {"basis", "declared"},
                   {"information_cutoff_ns", std::to_string(*spec.registered_cutoff_at)},
               }
             : json::Object{
                   {"label_available_through_ns", std::to_string(spec.training_through)},
                   {"split", "train"},
               }},
        {"window", json::Object{
            {"end_at_ns", spec.end_at ? json::Value(std::to_string(*spec.end_at)) : json::Value(nullptr)},
            {"name", spec.window_name},
            {"start_at_ns", std::to_string(spec.start_at)},
        }},
    });
    const std::string id = chronos::Sha256::hex(ds::canonical_document(body));
    json::Object manifest = *body.if_object();
    manifest.emplace_back("artifact_id", id);
    const std::filesystem::path directory = root / id;
    std::filesystem::create_directories(directory);
    write_file(directory / "manifest.json", ds::canonical_document(json::Value(std::move(manifest))));
    write_file(directory / "schedule.ndjson", bytes);
    return directory;
}

struct Fixture {
    TemporaryDirectory scratch;
    std::filesystem::path catalog;
    std::filesystem::path schedules;
    std::filesystem::path output;
    std::string dataset_id;

    Fixture() {
        catalog = scratch.path() / "normalized";
        schedules = scratch.path() / "schedules";
        output = scratch.path() / "strategies";
        for (const auto& directory : {catalog, schedules, output}) {
            std::filesystem::create_directories(directory);
        }
        dataset_id = chronos::test::published(chronos::test::write_dataset(catalog, base_session())).manifest.dataset_id;
    }
};

const st::PublishedStrategyRun& published(const std::variant<st::PublishedStrategyRun, ds::DatasetError>& result) {
    if (const auto* error = std::get_if<ds::DatasetError>(&result)) {
        throw chronos::test::TestFailure("not published: " + error->field + ": " + error->message);
    }
    return std::get<st::PublishedStrategyRun>(result);
}

void a_run_is_published_verified_reproduced_and_found_again() {
    Fixture fixture;
    const auto schedule = write_schedule(fixture.schedules, base_session(), ScheduleSpec{.dataset_id = fixture.dataset_id});
    const auto first = st::run_strategy(fixture.catalog, schedule, fixture.output, {});
    const auto& run = published(first);
    REQUIRE(run.outcome == ds::PublicationOutcome::Published);
    REQUIRE(text(run.manifest.document, {"results", "accounts", "strategy", "final", "valuation", "equity"}) == "10000.13");
    REQUIRE(text(run.manifest.document, {"source", "schedule_id"}) == schedule.filename().string());
    REQUIRE(text(run.manifest.document, {"configuration", "execution", "fee_per_fill"}) == "0.01");
    REQUIRE(text(run.manifest.document, {"events", "after_window_unused"}) == "0");
    const auto verified = st::verify_strategy_run(run.directory);
    REQUIRE(std::holds_alternative<st::StrategyRunManifest>(verified));
    REQUIRE(!st::reproduce_strategy_run(std::get<st::StrategyRunManifest>(verified), fixture.catalog, fixture.schedules));
    const auto again = st::run_strategy(fixture.catalog, schedule, fixture.output, {});
    REQUIRE(published(again).outcome == ds::PublicationOutcome::AlreadyPresent);
    REQUIRE(published(again).manifest.artifact_id == run.manifest.artifact_id);
    // Another configuration is another run.
    st::StrategyConfiguration free_trading;
    free_trading.fee_per_fill = Money{};
    const auto other = st::run_strategy(fixture.catalog, schedule, fixture.output, free_trading);
    REQUIRE(published(other).manifest.artifact_id != run.manifest.artifact_id);
    REQUIRE(text(published(other).manifest.document, {"configuration", "execution", "fee_per_fill"}) == "0");
    const auto listed = st::list_strategy_runs(fixture.output);
    REQUIRE(std::get<std::vector<st::StrategyRunEntry>>(listed).size() == 2);
    // The journal is the simulation's, line for line.
    const std::string journal = read_file(run.directory / "journal.ndjson");
    const Run in_memory = simulate(base_session(), kBaseSessionPredictions, window(ms(1000)));
    REQUIRE(journal == journal_bytes(in_memory));
}

void tampered_runs_and_schedules_are_refused() {
    Fixture fixture;
    const auto schedule = write_schedule(fixture.schedules, base_session(), ScheduleSpec{.dataset_id = fixture.dataset_id});
    const auto published_run = st::run_strategy(fixture.catalog, schedule, fixture.output, {});
    const auto& run = published(published_run);
    // A copy whose journal changed.
    const auto copy_root = fixture.scratch.path() / "copies";
    std::filesystem::create_directories(copy_root / run.manifest.artifact_id);
    for (const char* name : {"manifest.json", "journal.ndjson"}) {
        write_file(copy_root / run.manifest.artifact_id / name, read_file(run.directory / name));
    }
    std::string journal = read_file(run.directory / "journal.ndjson");
    journal[journal.find("\"buy\"") + 1] = 'x';
    write_file(copy_root / run.manifest.artifact_id / "journal.ndjson", journal);
    REQUIRE(std::holds_alternative<ds::DatasetError>(st::verify_strategy_run(copy_root / run.manifest.artifact_id)));

    const auto refused = [&](ScheduleSpec spec, ds::DatasetErrorCode code) {
        const auto root = fixture.scratch.path() / ("schedules-" + std::to_string(static_cast<int>(code)) + "-" +
                                                    std::to_string(spec.training_through) + spec.window_name);
        std::filesystem::create_directories(root);
        const auto directory = write_schedule(root, base_session(), spec);
        const auto result = st::run_strategy(fixture.catalog, directory, fixture.output, {});
        REQUIRE(std::holds_alternative<ds::DatasetError>(result));
        REQUIRE(std::get<ds::DatasetError>(result).code == code);
    };
    // A training label known when the window begins.
    refused(ScheduleSpec{.dataset_id = fixture.dataset_id, .training_through = ms(1000)}, ds::DatasetErrorCode::LabelLeakage);
    // A window that is not the split's.
    refused(
        ScheduleSpec{.dataset_id = fixture.dataset_id, .window_name = "validation", .end_at = ms(3000)},
        ds::DatasetErrorCode::InvalidValue
    );
    // A row the dataset does not have.
    refused(ScheduleSpec{.dataset_id = fixture.dataset_id, .extra_row = true}, ds::DatasetErrorCode::CountMismatch);
    // A dataset the catalog does not hold.
    {
        const auto root = fixture.scratch.path() / "schedules-missing";
        std::filesystem::create_directories(root);
        const auto directory = write_schedule(root, base_session(), ScheduleSpec{.dataset_id = std::string(64, '0')});
        REQUIRE(std::holds_alternative<ds::DatasetError>(st::run_strategy(fixture.catalog, directory, fixture.output, {})));
    }
    // A manifest edited without a new ID.
    {
        const auto root = fixture.scratch.path() / "schedules-edited";
        std::filesystem::create_directories(root);
        const auto directory = write_schedule(root, base_session(), ScheduleSpec{.dataset_id = fixture.dataset_id});
        std::string manifest = read_file(directory / "manifest.json");
        const auto at = manifest.find("\"logistic\"");
        manifest.replace(at, std::string_view("\"logistic\"").size(), "\"majority\"");
        write_file(directory / "manifest.json", manifest);
        const auto result = st::run_strategy(fixture.catalog, directory, fixture.output, {});
        REQUIRE(std::get<ds::DatasetError>(result).code == ds::DatasetErrorCode::IdentityMismatch);
    }
    // Rows edited after publication.
    {
        const auto root = fixture.scratch.path() / "schedules-rows";
        std::filesystem::create_directories(root);
        const auto directory = write_schedule(root, base_session(), ScheduleSpec{.dataset_id = fixture.dataset_id});
        std::string rows = read_file(directory / "schedule.ndjson");
        const auto at = rows.find("\"900000000\"");
        rows.replace(at, std::string_view("\"900000000\"").size(), "\"100000000\"");
        write_file(directory / "schedule.ndjson", rows);
        REQUIRE(std::holds_alternative<ds::DatasetError>(st::run_strategy(fixture.catalog, directory, fixture.output, {})));
    }
}

void a_registered_model_schedule_gives_a_run_that_names_the_version_and_no_experiment() {
    Fixture fixture;
    const auto schedule = write_schedule(
        fixture.schedules,
        base_session(),
        ScheduleSpec{.dataset_id = fixture.dataset_id, .registered_cutoff_at = ms(1000)}
    );
    const auto result = st::run_strategy(fixture.catalog, schedule, fixture.output, {});
    const auto& run = published(result);
    const json::Value& document = run.manifest.document;
    REQUIRE(text(document, {"schema", "version"}) == "2");
    REQUIRE(text(document, {"source", "model_version_id"}) == std::string(64, '7'));
    REQUIRE(text(document, {"source", "family"}) == "onnx");
    REQUIRE(text(document, {"source", "payload_sha256"}) == std::string(64, '8'));
    REQUIRE(text(document, {"source", "information_cutoff", "basis"}) == "declared");
    REQUIRE(text(document, {"source", "information_cutoff", "at_ns"}) == std::to_string(ms(1000)));
    for (const auto& member : *member(document, "source").if_object()) {
        REQUIRE(member.first != "experiment_id");
        REQUIRE(member.first != "parameters_sha256");
    }
    // The same predictions give the same account results as a version 1 run.
    const auto v1 = write_schedule(fixture.schedules, base_session(), ScheduleSpec{.dataset_id = fixture.dataset_id});
    const auto experiment_result = st::run_strategy(fixture.catalog, v1, fixture.output, {});
    const auto& experiment_run = published(experiment_result);
    REQUIRE(member(document, "results") == member(experiment_run.manifest.document, "results"));
    REQUIRE(text(experiment_run.manifest.document, {"schema", "version"}) == "1");
    const auto verified = st::verify_strategy_run(run.directory);
    REQUIRE(std::holds_alternative<st::StrategyRunManifest>(verified));
    REQUIRE(!st::reproduce_strategy_run(std::get<st::StrategyRunManifest>(verified), fixture.catalog, fixture.schedules));
}

void a_registered_model_must_not_see_the_window_or_claim_an_experiment() {
    Fixture fixture;
    const auto attempt = [&](ScheduleSpec spec, std::string_view name) {
        const auto root = fixture.scratch.path() / ("schedules-" + std::string(name));
        std::filesystem::create_directories(root);
        return st::run_strategy(fixture.catalog, write_schedule(root, base_session(), spec), fixture.output, {});
    };
    // A cutoff after the window's start could have seen the window.
    const auto late = attempt(
        ScheduleSpec{.dataset_id = fixture.dataset_id, .registered_cutoff_at = ms(1000) + 1}, "late"
    );
    REQUIRE(std::get<ds::DatasetError>(late).code == ds::DatasetErrorCode::LabelLeakage);
    const auto at_start = attempt(ScheduleSpec{.dataset_id = fixture.dataset_id, .registered_cutoff_at = ms(1000)}, "at-start");
    REQUIRE(std::holds_alternative<st::PublishedStrategyRun>(at_start));
    // A version 2 schedule names a model version, never an experiment.
    const auto mixed = attempt(
        ScheduleSpec{.dataset_id = fixture.dataset_id, .registered_cutoff_at = ms(900), .registered_with_experiment = true},
        "mixed"
    );
    REQUIRE(std::holds_alternative<ds::DatasetError>(mixed));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"a signal executes after its latency and never on its own quote",
         a_signal_executes_after_its_latency_and_never_on_its_own_quote},
        {"the policy buys, holds, and sells by class and confidence",
         the_policy_buys_holds_and_sells_by_class_and_confidence},
        {"the decision interval allows at most one decision per interval",
         the_decision_interval_allows_at_most_one_decision_per_interval},
        {"the first eligible observation decides the order, whatever it is",
         the_first_eligible_observation_decides_the_order_whatever_it_is},
        {"a partial fill leaves the rest to the next decision", a_partial_fill_leaves_the_rest_to_the_next_decision},
        {"a rejected order is recorded and changes nothing", a_rejected_order_is_recorded_and_changes_nothing},
        {"pending orders expire at the window end and block new orders",
         pending_orders_expire_at_the_window_end_and_block_new_orders},
        {"warmup events update the market but never trade", warmup_events_update_the_market_but_never_trade},
        {"events after the window cannot change the result", events_after_the_window_cannot_change_the_result},
        {"no signal means no trades, and down never goes short",
         no_signal_means_no_trades_and_down_never_goes_short},
        {"an order above the notional limit is rejected", an_order_above_the_notional_limit_is_rejected},
        {"changing a later event never changes an earlier decision",
         changing_a_later_event_never_changes_an_earlier_decision},
        {"a schedule that disagrees with the recomputed features is refused",
         a_schedule_that_disagrees_with_the_recomputed_features_is_refused},
        {"an unmarkable position makes the valuation explicitly unavailable",
         an_unmarkable_position_makes_the_valuation_explicitly_unavailable},
        {"drawdown and turnover follow their definitions", drawdown_and_turnover_follow_their_definitions},
        {"the same inputs give the same journal and results", the_same_inputs_give_the_same_journal_and_results},
        {"configurations that cannot run are refused", configurations_that_cannot_run_are_refused},
        {"a run is published, verified, reproduced, and found again",
         a_run_is_published_verified_reproduced_and_found_again},
        {"tampered runs and schedules are refused", tampered_runs_and_schedules_are_refused},
        {"a registered model's schedule gives a run that names the version and no experiment",
         a_registered_model_schedule_gives_a_run_that_names_the_version_and_no_experiment},
        {"a registered model must not see the window or claim an experiment",
         a_registered_model_must_not_see_the_window_or_claim_an_experiment},
    });
}
