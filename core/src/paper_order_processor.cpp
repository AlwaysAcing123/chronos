#include "chronos/paper_order_processor.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos {

PaperOrderProcessor::PaperOrderProcessor(
    std::string account_id,
    InstrumentId instrument,
    Money starting_cash,
    PreTradeRiskLimits risk_limits
)
    : risk_engine_(risk_limits),
      account_(account_id, instrument, starting_cash),
      execution_simulator_(
          std::move(account_id),
          std::move(instrument)
      ) {}

PaperOrderResult PaperOrderProcessor::submit(
    const OrderIntent& intent,
    const TopOfBookEvent& market
) {
    if (!is_healthy_) {
        return PaperOrderProcessingError{
            .code = PaperOrderProcessingErrorCode::ProcessorFaulted,
            .message =
                "paper order processor is halted after an accounting "
                "invariant violation",
            .execution = std::nullopt,
            .accounting_error = std::nullopt,
        };
    }

    PreTradeRiskDecision risk_decision =
        risk_engine_.evaluate(intent, account_, market);
    if (std::holds_alternative<PreTradeRiskRejection>(risk_decision)) {
        return std::get<PreTradeRiskRejection>(std::move(risk_decision));
    }

    SimulatedExecutionResult execution_result = execution_simulator_.execute(
        std::get<ApprovedOrderIntent>(std::move(risk_decision))
    );
    if (std::holds_alternative<SimulatedExecutionError>(execution_result)) {
        return std::get<SimulatedExecutionError>(
            std::move(execution_result)
        );
    }

    SimulatedExecutionReport execution =
        std::get<SimulatedExecutionReport>(std::move(execution_result));
    std::optional<ApplyFillReceipt> accounting;
    if (execution.fill) {
        ApplyFillResult apply_result = account_.apply_fill(*execution.fill);
        if (std::holds_alternative<ApplyFillError>(apply_result)) {
            is_healthy_ = false;
            return PaperOrderProcessingError{
                .code = PaperOrderProcessingErrorCode::
                    AccountingInvariantViolation,
                .message =
                    "risk-approved simulated fill was rejected by accounting",
                .execution = std::move(execution),
                .accounting_error = std::get<ApplyFillError>(
                    std::move(apply_result)
                ),
            };
        }
        accounting = std::get<ApplyFillReceipt>(std::move(apply_result));
    }

    return PaperOrderReceipt{
        .execution = std::move(execution),
        .accounting = std::move(accounting),
        .account_after = account_.state(),
    };
}

}  // namespace chronos
