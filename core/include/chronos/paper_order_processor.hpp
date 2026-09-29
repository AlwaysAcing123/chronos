#pragma once

#include "chronos/top_of_book_execution.hpp"

#include <optional>
#include <string>
#include <variant>

namespace chronos {

struct PaperOrderReceipt {
    SimulatedExecutionReport execution;
    std::optional<ApplyFillReceipt> accounting;
    AccountState account_after;
};

enum class PaperOrderProcessingErrorCode {
    ProcessorFaulted,
    AccountingInvariantViolation,
};

struct PaperOrderProcessingError {
    PaperOrderProcessingErrorCode code;
    std::string message;
    std::optional<SimulatedExecutionReport> execution;
    std::optional<ApplyFillError> accounting_error;
};

using PaperOrderResult = std::variant<
    PaperOrderReceipt,
    PreTradeRiskRejection,
    SimulatedExecutionError,
    PaperOrderProcessingError
>;

struct PaperOrderProcessorRecoveryState {
    PreTradeRiskLimits risk_limits;
    PaperAccountRecoveryState account;
    TopOfBookExecutionRecoveryState execution;
    bool is_healthy{true};
};

class PaperOrderProcessor final {
public:
    PaperOrderProcessor(
        std::string account_id,
        Money starting_cash,
        PreTradeRiskLimits risk_limits
    );

    PaperOrderProcessor(const PaperOrderProcessor&) = delete;
    PaperOrderProcessor& operator=(const PaperOrderProcessor&) = delete;
    PaperOrderProcessor(PaperOrderProcessor&&) noexcept = default;
    PaperOrderProcessor& operator=(PaperOrderProcessor&&) noexcept = default;

    [[nodiscard]] const PaperAccount& account() const noexcept {
        return account_;
    }

    [[nodiscard]] const PreTradeRiskLimits& risk_limits() const noexcept {
        return risk_engine_.limits();
    }

    [[nodiscard]] bool is_healthy() const noexcept { return is_healthy_; }

    // Each executed instrument's latest execution observation and consumed
    // liquidity. Together they fix the account's market timeline.
    [[nodiscard]] const InstrumentExecutionStates& executions()
        const noexcept {
        return execution_simulator_.instruments();
    }

    [[nodiscard]] PaperOrderProcessorRecoveryState recovery_state() const;
    [[nodiscard]] static PaperOrderProcessor restore(
        PaperOrderProcessorRecoveryState recovery_state
    );

    [[nodiscard]] PaperOrderResult submit(
        const OrderIntent& intent,
        const TopOfBookEvent& market
    );

private:
    PreTradeRiskEngine risk_engine_;
    PaperAccount account_;
    TopOfBookExecutionSimulator execution_simulator_;
    bool is_healthy_{true};
};

}  // namespace chronos
