#pragma once

#include "chronos/pre_trade_risk.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace chronos {

enum class SimulatedExecutionStatus {
    Filled,
    PartiallyFilledAndCancelled,
    Cancelled,
};

enum class SimulatedCancellationReason {
    LimitNotMarketable,
    DisplayedLiquidityExhausted,
};

struct SimulatedExecutionReport {
    OrderIntent intent;
    SimulatedExecutionStatus status;
    std::optional<Fill> fill;
    Quantity unfilled_quantity;
    std::optional<SimulatedCancellationReason> cancellation_reason;
    std::uint64_t market_ingest_sequence;

    friend bool operator==(
        const SimulatedExecutionReport&,
        const SimulatedExecutionReport&
    ) = default;
};

enum class SimulatedExecutionErrorCode {
    AccountMismatch,
    // Retained so journaled history stays readable; accounts are no longer
    // bound to one instrument, so it is not produced.
    InstrumentMismatch,
    DuplicateClientOrderId,
    MarketStreamMismatch,
    StaleMarketApproval,
    MarketSnapshotMismatch,
};

struct SimulatedExecutionError {
    SimulatedExecutionErrorCode code;
    std::string field;
    std::string message;

    friend bool operator==(
        const SimulatedExecutionError&,
        const SimulatedExecutionError&
    ) = default;
};

using SimulatedExecutionResult = std::variant<
    SimulatedExecutionReport,
    SimulatedExecutionError
>;

// One account's simulated use of one instrument's displayed liquidity: the
// observation of the instrument's latest execution and the bid and ask
// quantity the account's orders have taken from it. Another account, or
// another instrument, never shares these counters.
struct InstrumentExecutionState {
    TopOfBookEvent active_market;
    Quantity consumed_bid_quantity;
    Quantity consumed_ask_quantity;

    friend bool operator==(
        const InstrumentExecutionState&,
        const InstrumentExecutionState&
    ) = default;
};

// Every instrument an account has executed, ordered by instrument.
using InstrumentExecutionStates =
    std::map<InstrumentId, InstrumentExecutionState>;

struct TopOfBookExecutionRecoveryState {
    std::string account_id;
    InstrumentExecutionStates instruments;
    // Client order IDs are unique across the whole account.
    std::vector<std::string> processed_client_order_ids;
};

// An account's executions follow one market-data stream forward. The first
// execution binds the account to its stream; every instrument must then
// execute on that stream, and within one instrument the ingest sequence must
// not decrease and one sequence identifies one state. Instruments may be
// observed at different sequences: an instrument's current quote can be
// older than another's. Returns the rule `market` breaks, if any. Anything
// that uses a market observation on the account's behalf, not only
// execution, must continue the same timeline.
[[nodiscard]] std::optional<SimulatedExecutionErrorCode>
market_lineage_violation(
    const InstrumentExecutionStates& executions,
    const TopOfBookEvent& market
);

// The stream an account is bound to; absent before its first execution.
[[nodiscard]] std::optional<MarketDataStreamId> bound_market_stream(
    const InstrumentExecutionStates& executions
);

class TopOfBookExecutionSimulator final {
public:
    explicit TopOfBookExecutionSimulator(std::string account_id);

    TopOfBookExecutionSimulator(const TopOfBookExecutionSimulator&) = delete;
    TopOfBookExecutionSimulator& operator=(
        const TopOfBookExecutionSimulator&
    ) = delete;
    TopOfBookExecutionSimulator(
        TopOfBookExecutionSimulator&&
    ) noexcept = default;
    TopOfBookExecutionSimulator& operator=(
        TopOfBookExecutionSimulator&&
    ) noexcept = default;

    [[nodiscard]] const std::string& account_id() const noexcept {
        return account_id_;
    }

    // Per-instrument execution state; empty before the first execution.
    [[nodiscard]] const InstrumentExecutionStates& instruments()
        const noexcept {
        return instruments_;
    }

    [[nodiscard]] TopOfBookExecutionRecoveryState recovery_state() const;
    [[nodiscard]] static TopOfBookExecutionSimulator restore(
        TopOfBookExecutionRecoveryState recovery_state
    );

    [[nodiscard]] SimulatedExecutionResult execute(
        const ApprovedOrderIntent& approved
    );

private:
    std::string account_id_;
    InstrumentExecutionStates instruments_;
    std::unordered_set<std::string> processed_client_order_ids_;
};

}  // namespace chronos
