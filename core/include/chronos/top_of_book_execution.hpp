#pragma once

#include "chronos/pre_trade_risk.hpp"

#include <cstdint>
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

struct TopOfBookExecutionRecoveryState {
    std::string account_id;
    InstrumentId instrument;
    std::optional<TopOfBookEvent> active_market;
    Quantity consumed_bid_quantity;
    Quantity consumed_ask_quantity;
    std::vector<std::string> processed_client_order_ids;
};

// An account's executions follow one market-data stream forward: the same
// stream, a non-decreasing ingest sequence, and one state per sequence.
// Returns the rule `market` breaks relative to the account's last execution
// market, if any. Anything that uses a market observation on the account's
// behalf, not only execution, must continue the same timeline.
[[nodiscard]] std::optional<SimulatedExecutionErrorCode>
market_lineage_violation(
    const std::optional<TopOfBookEvent>& active_market,
    const TopOfBookEvent& market
);

class TopOfBookExecutionSimulator final {
public:
    TopOfBookExecutionSimulator(
        std::string account_id,
        InstrumentId instrument
    );

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

    [[nodiscard]] const InstrumentId& instrument() const noexcept {
        return instrument_;
    }

    // The observation of the latest execution; absent before the first.
    [[nodiscard]] const std::optional<TopOfBookEvent>& active_market()
        const noexcept {
        return active_market_;
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
    InstrumentId instrument_;
    std::optional<TopOfBookEvent> active_market_;
    Quantity consumed_bid_quantity_;
    Quantity consumed_ask_quantity_;
    std::unordered_set<std::string> processed_client_order_ids_;
};

}  // namespace chronos
