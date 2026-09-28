#pragma once

#include "chronos/pre_trade_risk.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

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
