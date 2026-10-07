// The precedence of a deployment's condition (ADR 0019): lifecycle, then
// the source, then the quote, then a pending order, then the latest
// candidate. Pure, so every case is exact and needs no database.
#include "chronos/paper_trading/deployment_runtime.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

using chronos::DeploymentState;
using chronos::ModelDeploymentRecord;
using chronos::Timestamp;
using chronos::TradingStatus;
using chronos::market_data::MarketDataSourceState;
using chronos::paper_trading::deployment_condition;
using chronos::paper_trading::DeploymentActivity;
using chronos::paper_trading::DeploymentCondition;
using chronos::paper_trading::DeploymentMarketCondition;
using chronos::paper_trading::DeploymentPendingOrder;
using chronos::paper_trading::DeploymentQuoteCondition;
using chronos::paper_trading::DeploymentRuntimeOutcome;

Timestamp at(std::int64_t nanoseconds) { return Timestamp::from_nanoseconds(nanoseconds); }

const Timestamp kWall = at(1'790'000'000'000'000'000);

ModelDeploymentRecord running() {
    ModelDeploymentRecord record;
    record.state = DeploymentState::Running;
    record.state_reason = "started";
    record.run_generation = 2;
    record.config.training_cutoff = at(1'000);
    record.config.decision_interval_ns = 250;
    return record;
}

// A delivering source and a fresh, clean quote.
DeploymentMarketCondition healthy() {
    return DeploymentMarketCondition{
        .market_time = at(5'000),
        .source_state = MarketDataSourceState::Live,
        .trading_status = std::nullopt,
        .quote = DeploymentQuoteCondition{
            .available = true,
            .unavailable_reason = std::nullopt,
            .ingest_sequence = 9,
            .received_at = at(4'900),
            .age_ns = 100,
            .maximum_age_ns = 1'000,
            .stale = false,
            .suspect = false,
        },
    };
}

DeploymentRuntimeOutcome outcome(std::string code, std::int64_t received = 2'000) {
    return DeploymentRuntimeOutcome{
        .code = std::move(code),
        .ingest_sequence = 7,
        .observation_receive = at(received),
        .decision_sequence = std::nullopt,
        .at_market = at(2'100),
        .at_wall = at(1'790'000'000'000'000'100),
    };
}

DeploymentActivity with_candidate(std::string code) {
    return DeploymentActivity{.last_candidate = outcome(std::move(code)), .last_submission = std::nullopt, .pending = std::nullopt};
}

DeploymentCondition condition(
    const ModelDeploymentRecord& record,
    const std::optional<DeploymentActivity>& activity,
    const std::optional<DeploymentMarketCondition>& market = healthy(),
    bool active = true
) {
    return deployment_condition(record, active, activity, market, kWall);
}

void test_the_lifecycle_beats_everything_else() {
    for (const auto& [state, reason] : {
             std::pair{DeploymentState::Stopped, "server_restarted"},
             std::pair{DeploymentState::Faulted, "inference_timeout"},
             std::pair{DeploymentState::Stopping, "kill_switch"},
         }) {
        ModelDeploymentRecord record = running();
        record.state = state;
        record.state_reason = reason;
        DeploymentMarketCondition reconnecting = healthy();
        reconnecting.source_state = MarketDataSourceState::Reconnecting;
        // An old outcome and a broken source change nothing.
        const DeploymentCondition found = condition(record, with_candidate("decided_order"), reconnecting);
        REQUIRE(found.code == std::string(chronos::deployment_state_name(state)));
        REQUIRE(found.reason == std::optional<std::string>(reason));
        REQUIRE(!found.observed_at_wall.has_value());
    }
    REQUIRE(condition(running(), with_candidate("decided_hold"), healthy(), false).code == "not_running_here");
}

void test_a_source_that_is_not_delivering_comes_next() {
    for (const auto& [state, code] : {
             std::pair{MarketDataSourceState::None, "no_market_data_source"},
             std::pair{MarketDataSourceState::Ready, "source_not_started"},
             std::pair{MarketDataSourceState::Paused, "source_paused"},
             std::pair{MarketDataSourceState::Completed, "source_completed"},
             std::pair{MarketDataSourceState::Connecting, "source_connecting"},
             std::pair{MarketDataSourceState::Authenticating, "source_connecting"},
             std::pair{MarketDataSourceState::Subscribing, "source_connecting"},
             std::pair{MarketDataSourceState::Reconnecting, "source_reconnecting"},
             std::pair{MarketDataSourceState::Failed, "source_failed"},
             std::pair{MarketDataSourceState::Stopped, "source_stopped"},
         }) {
        DeploymentMarketCondition market = healthy();
        market.source_state = state;
        DeploymentActivity pending = with_candidate("decided_order");
        pending.pending = DeploymentPendingOrder{
            .decision_sequence = 4, .observation_receive = at(2'000), .not_before_receive = at(2'010), .expires_at_market = at(9'000),
        };
        const DeploymentCondition found = condition(running(), pending, market);
        REQUIRE(found.code == code);
        REQUIRE(found.reason == std::optional<std::string>(std::string(chronos::market_data::state_name(state))));
        // A current fact: observed now, in both clocks.
        REQUIRE(found.observed_at_market == std::optional(at(5'000)));
        REQUIRE(found.observed_at_wall == std::optional(kWall));
    }
    // Running replays and live feeds deliver; without a source, nothing is
    // said about one.
    for (const auto state : {MarketDataSourceState::Running, MarketDataSourceState::Live}) {
        DeploymentMarketCondition market = healthy();
        market.source_state = state;
        REQUIRE(condition(running(), with_candidate("decided_hold"), market).code == "watching");
    }
    DeploymentMarketCondition unknown = healthy();
    unknown.source_state.reset();
    REQUIRE(condition(running(), with_candidate("decided_hold"), unknown).code == "watching");
}

void test_the_quote_comes_before_pending_and_candidate_outcomes() {
    DeploymentMarketCondition halted = healthy();
    halted.trading_status = TradingStatus::Halted;
    const DeploymentCondition not_trading = condition(running(), with_candidate("decided_hold"), halted);
    REQUIRE(not_trading.code == "instrument_not_trading");
    REQUIRE(not_trading.reason == std::optional<std::string>("halted"));
    DeploymentMarketCondition open = healthy();
    open.trading_status = TradingStatus::Open;
    REQUIRE(condition(running(), with_candidate("decided_hold"), open).code == "watching");

    DeploymentMarketCondition missing = healthy();
    missing.quote.available = false;
    missing.quote.unavailable_reason = "awaiting_fresh_quote";
    const DeploymentCondition unavailable = condition(running(), with_candidate("decided_hold"), missing);
    REQUIRE(unavailable.code == "quote_unavailable");
    REQUIRE(unavailable.reason == std::optional<std::string>("awaiting_fresh_quote"));

    DeploymentMarketCondition suspect = healthy();
    suspect.quote.suspect = true;
    REQUIRE(condition(running(), with_candidate("decided_hold"), suspect).code == "quote_suspect");
    DeploymentMarketCondition stale = healthy();
    stale.quote.stale = true;
    REQUIRE(condition(running(), with_candidate("decided_hold"), stale).code == "quote_stale");
    // A deployment of another stream has no market facts at all.
    REQUIRE(condition(running(), with_candidate("decided_hold"), std::nullopt).code == "watching");
}

void test_a_pending_order_reports_its_latest_check_only_if_it_is_that_orders() {
    DeploymentActivity activity = with_candidate("decided_order");
    activity.pending = DeploymentPendingOrder{
        .decision_sequence = 4, .observation_receive = at(2'000), .not_before_receive = at(2'010), .expires_at_market = at(9'000),
    };
    DeploymentRuntimeOutcome check = outcome("awaiting_latency");
    check.decision_sequence = 4;
    activity.last_submission = check;
    const DeploymentCondition pending = condition(running(), activity);
    REQUIRE(pending.code == "order_pending");
    REQUIRE(pending.reason == std::optional<std::string>("awaiting_latency"));
    REQUIRE(pending.decision_sequence == std::optional<std::int64_t>(4));
    REQUIRE(pending.not_before_receive == std::optional(at(2'010)));
    REQUIRE(pending.expires_at == std::optional(at(9'000)));
    REQUIRE(pending.observed_at_market == std::optional(at(2'100)));
    // An earlier order's check says nothing about this one.
    activity.last_submission->decision_sequence = 3;
    REQUIRE(!condition(running(), activity).reason.has_value());
    // A durable pending decision without a runner snapshot.
    ModelDeploymentRecord record = running();
    record.pending_decision = 6;
    const DeploymentCondition durable = condition(record, std::nullopt);
    REQUIRE(durable.code == "order_pending");
    REQUIRE(durable.decision_sequence == std::optional<std::int64_t>(6));
}

void test_the_latest_candidate_explains_the_rest() {
    const DeploymentCondition cutoff = condition(running(), with_candidate("before_training_cutoff"));
    REQUIRE(cutoff.code == "before_training_cutoff");
    REQUIRE(cutoff.training_cutoff == std::optional(at(1'000)));
    REQUIRE(cutoff.observation_receive == std::optional(at(2'000)));
    REQUIRE(cutoff.observed_at_wall == std::optional(at(1'790'000'000'000'000'100)));

    ModelDeploymentRecord decided = running();
    decided.watermark_receive = at(1'900);
    decided.watermark_ingest_sequence = 6;
    const DeploymentCondition interval = condition(decided, with_candidate("decision_interval"));
    REQUIRE(interval.code == "waiting_for_decision_interval");
    REQUIRE(interval.next_eligible_receive == std::optional(at(2'150)));
    // An interval that would overflow has no next time, not a wrong one.
    decided.watermark_receive = at(std::numeric_limits<std::int64_t>::max() - 10);
    REQUIRE(!condition(decided, with_candidate("decision_interval")).next_eligible_receive.has_value());

    const DeploymentCondition inputs = condition(running(), with_candidate("inputs_unavailable:missing_ask"));
    REQUIRE(inputs.code == "inputs_unavailable");
    REQUIRE(inputs.reason == std::optional<std::string>("missing_ask"));
    for (const char* code : {"decided_hold", "decided_order", "order_pending"}) {
        const DeploymentCondition watching = condition(running(), with_candidate(code));
        REQUIRE(watching.code == "watching");
        REQUIRE(watching.reason == std::optional<std::string>(code));
    }
    for (const char* code : {"not_after_watermark", "stale_signal", "account_unavailable", "persistence_unavailable",
                             "decision_refused", "inference_failed:inference_timeout"}) {
        REQUIRE(condition(running(), with_candidate(code)).code == code);
    }
    REQUIRE(condition(running(), DeploymentActivity{}).code == "waiting_for_candidate");
    REQUIRE(condition(running(), std::nullopt).code == "waiting_for_candidate");
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"the lifecycle beats everything else", test_the_lifecycle_beats_everything_else},
        {"a source that is not delivering comes next", test_a_source_that_is_not_delivering_comes_next},
        {"the quote comes before pending and candidate outcomes", test_the_quote_comes_before_pending_and_candidate_outcomes},
        {
            "a pending order reports its latest check only if it is that order's",
            test_a_pending_order_reports_its_latest_check_only_if_it_is_that_orders,
        },
        {"the latest candidate explains the rest", test_the_latest_candidate_explains_the_rest},
    });
}
