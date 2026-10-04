#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/features/top_of_book_features.hpp"
#include "chronos/market_data_validation.hpp"
#include "chronos/paper_order_processor.hpp"
#include "chronos/portfolio_valuation.hpp"
#include "chronos/strategy/inference_schedule.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// The deterministic historical evaluation of one inference schedule: a
// versioned long/flat policy trading one instrument in fresh paper accounts
// through the same PaperOrderProcessor, risk, fill, fee, liquidity,
// accounting, and valuation rules as paper trading, beside two benchmarks
// under identical conditions. See docs/strategy-evaluation.md.
//
// Events are applied in ingest order. For each event:
//   1. it updates the market: the instrument's latest quote and status, and
//      the feature engine;
//   2. each account's pending order whose arrival time has come executes
//      against this event, when it is a top-of-book event of the instrument
//      received at or after the arrival time (the first such event is the
//      order's execution observation, whatever its quality);
//   3. a top-of-book event of the instrument inside the window takes the
//      schedule's next row, which must match the features recomputed from
//      the event, and the policy may decide;
//   4. a decision schedules an order that arrives `latency_ns` later, so it
//      can never execute against the quote that produced its signal.
// Events before the window only warm up the market state; events at or
// after its end are never applied.
namespace chronos::strategy {

inline constexpr std::string_view kPolicyName = "long_flat_threshold";
inline constexpr std::string_view kPolicyVersion = "1";
inline constexpr std::string_view kRunnerName = "chronos.strategy.event_loop";
inline constexpr std::string_view kRunnerVersion = "1";
inline constexpr std::string_view kJournalRecordName = "chronos.strategy_journal_entry";
inline constexpr std::string_view kJournalRecordVersion = "1";

// Every default was fixed before any strategy was run on real data.
struct StrategyConfiguration {
    // The long target; the account is long this many shares or flat.
    Quantity target_quantity{Quantity::from_raw(Quantity::scale())};
    // A prediction acts only with confidence_ppb >= threshold_ppb.
    std::uint64_t threshold_ppb{500'000'000};
    // Signal to order arrival; positive.
    std::int64_t latency_ns{1'000'000};
    // A decision only once this long after the previous one; at most one
    // per second of receive time by default.
    std::int64_t decision_interval_ns{1'000'000'000};
    Money starting_cash{Money::from_raw(10'000 * Money::scale())};
    // Charged on every fill; an assumption, not a broker's fee schedule.
    Money fee_per_fill{Money::from_raw(Money::scale() / 100)};
    std::int64_t maximum_quote_age_ns{5'000'000'000};
    Money maximum_order_notional{Money::from_raw(1'000'000 * Money::scale())};

    friend bool operator==(const StrategyConfiguration&, const StrategyConfiguration&) = default;
};

// Why a configuration cannot run, if it cannot.
[[nodiscard]] std::optional<std::string> configuration_error(const StrategyConfiguration& configuration);

[[nodiscard]] canonical_json::Value configuration_json(const StrategyConfiguration& configuration);

enum class AccountRole {
    Strategy,
    BuyAndHold,
    Cash,
};

inline constexpr std::array<AccountRole, 3> kAccountRoles = {
    AccountRole::Strategy, AccountRole::BuyAndHold, AccountRole::Cash,
};

[[nodiscard]] std::string_view account_name(AccountRole role) noexcept;

struct OrderCounts {
    std::uint64_t scheduled{0};
    std::uint64_t filled{0};
    std::uint64_t partially_filled_and_cancelled{0};
    std::uint64_t cancelled{0};
    std::uint64_t rejected{0};
    // Still pending when the window ended.
    std::uint64_t expired{0};
    std::map<std::string, std::uint64_t> rejections;
    friend bool operator==(const OrderCounts&, const OrderCounts&) = default;
};

struct Drawdown {
    std::uint64_t points_valued{0};
    std::uint64_t points_unvalued{0};
    // The largest fall from a running peak of equity, over valued points.
    Money maximum;
    std::optional<Percentage> maximum_percent_of_peak;
    std::optional<Timestamp> peak_at;
    std::optional<Timestamp> trough_at;
};

struct AccountResult {
    AccountRole role{AccountRole::Strategy};
    AccountState final_state;
    PortfolioValuationResult final_valuation;
    OrderCounts orders;
    std::uint64_t fills{0};
    Quantity traded_quantity;
    // The sum of every fill's notional.
    Money turnover;
    Drawdown drawdown;
};

struct DecisionCounts {
    std::uint64_t evaluated{0};
    std::uint64_t orders{0};
    std::uint64_t already_at_target{0};
    std::uint64_t hold_flat_prediction{0};
    std::uint64_t hold_below_threshold{0};
    std::uint64_t order_pending{0};
    std::uint64_t skipped_prediction_unavailable{0};
    std::uint64_t skipped_decision_interval{0};
};

struct StrategyResults {
    std::string instrument;
    std::array<AccountResult, 3> accounts;
    DecisionCounts decisions;
    std::uint64_t warmup_events{0};
    std::uint64_t window_events{0};
    std::uint64_t schedule_rows{0};
    std::optional<Timestamp> first_window_event_at;
    // The market clock of the final valuation: the last event applied.
    std::optional<Timestamp> valued_at;
};

[[nodiscard]] canonical_json::Value results_json(const StrategyResults& results, const StrategyConfiguration& configuration);

// The canonical record of one journal entry, without its newline.
using JournalSink = std::function<std::optional<datasets::DatasetError>(const canonical_json::Value&)>;
using ScheduleRowSource = std::function<std::variant<ScheduleRow, datasets::EndOfRows, datasets::DatasetError>()>;

class StrategySimulation final {
public:
    StrategySimulation(
        StrategyConfiguration configuration,
        InstrumentId instrument,
        MarketDataStreamId stream,
        ScheduleWindow window,
        // The model order source's ID, such as "schedule:<id>".
        std::string source_id,
        JournalSink journal
    );
    ~StrategySimulation();
    StrategySimulation(const StrategySimulation&) = delete;
    StrategySimulation& operator=(const StrategySimulation&) = delete;

    // Whether an event is before the window's end, so it may be applied.
    [[nodiscard]] bool accepts(const ValidatedMarketEvent& event) const noexcept;

    // Applies the next event in ingest order, taking a schedule row from
    // `rows` for each top-of-book event of the instrument in the window.
    [[nodiscard]] std::optional<datasets::DatasetError> apply(
        const ValidatedMarketEvent& event,
        const ScheduleRowSource& rows
    );

    // Ends the window: every pending order expires, and every account is
    // valued at the last applied event. Call once, after the last event.
    [[nodiscard]] std::variant<StrategyResults, datasets::DatasetError> finish();

private:
    class State;
    std::unique_ptr<State> state_;
};

}  // namespace chronos::strategy
