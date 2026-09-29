#include "chronos/top_of_book_execution.hpp"

#include "chronos/market_data_validation.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace chronos {
namespace {

SimulatedExecutionError execution_error(
    SimulatedExecutionErrorCode code,
    std::string field,
    std::string message
) {
    return SimulatedExecutionError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
    };
}

bool is_marketable(const ApprovedOrderIntent& approved) {
    const auto& intent = approved.intent();
    if (intent.type == OrderType::Market) {
        return true;
    }
    return intent.side == Side::Buy
        ? *intent.limit_price >= approved.execution_price()
        : *intent.limit_price <= approved.execution_price();
}

SimulatedExecutionError lineage_error(SimulatedExecutionErrorCode code) {
    switch (code) {
        case SimulatedExecutionErrorCode::MarketStreamMismatch:
            return execution_error(
                code,
                "market.header.stream",
                "approved order belongs to a different market-data stream"
            );
        case SimulatedExecutionErrorCode::StaleMarketApproval:
            return execution_error(
                code,
                "market.header.ingest_sequence",
                "approved order uses an older market observation"
            );
        case SimulatedExecutionErrorCode::MarketSnapshotMismatch:
            return execution_error(
                code,
                "market",
                "one ingest sequence cannot identify different market states"
            );
        default:
            throw std::logic_error("not a market-lineage violation");
    }
}

void validate_recovered_execution(
    const InstrumentId& instrument,
    const InstrumentExecutionState& state,
    const std::optional<MarketDataStreamId>& stream
) {
    if (state.consumed_bid_quantity.raw() < 0 ||
        state.consumed_ask_quantity.raw() < 0) {
        throw std::invalid_argument(
            "recovered consumed liquidity must not be negative"
        );
    }
    auto validation = validate_market_event(MarketEvent(state.active_market));
    if (std::holds_alternative<MarketEventValidationFailure>(validation)) {
        throw std::invalid_argument(
            "recovered active market failed validation"
        );
    }
    if (state.active_market.header.instrument != instrument) {
        throw std::invalid_argument(
            "recovered active market instrument does not match"
        );
    }
    if (state.active_market.header.stream != *stream) {
        throw std::invalid_argument(
            "recovered instruments must share one market stream"
        );
    }
    const auto& bid = state.active_market.bid;
    if ((!bid && state.consumed_bid_quantity.raw() != 0) ||
        (bid && state.consumed_bid_quantity > bid->quantity)) {
        throw std::invalid_argument(
            "recovered consumed bid quantity exceeds the market"
        );
    }
    const auto& ask = state.active_market.ask;
    if ((!ask && state.consumed_ask_quantity.raw() != 0) ||
        (ask && state.consumed_ask_quantity > ask->quantity)) {
        throw std::invalid_argument(
            "recovered consumed ask quantity exceeds the market"
        );
    }
}

}  // namespace

std::optional<MarketDataStreamId> bound_market_stream(
    const InstrumentExecutionStates& executions
) {
    if (executions.empty()) {
        return std::nullopt;
    }
    return executions.begin()->second.active_market.header.stream;
}

std::optional<SimulatedExecutionErrorCode> market_lineage_violation(
    const InstrumentExecutionStates& executions,
    const TopOfBookEvent& market
) {
    const auto stream = bound_market_stream(executions);
    if (stream && market.header.stream != *stream) {
        return SimulatedExecutionErrorCode::MarketStreamMismatch;
    }
    const auto found = executions.find(market.header.instrument);
    if (found == executions.end()) {
        return std::nullopt;
    }
    const TopOfBookEvent& active_market = found->second.active_market;
    const MarketEventHeader& active = active_market.header;
    if (market.header.ingest_sequence < active.ingest_sequence) {
        return SimulatedExecutionErrorCode::StaleMarketApproval;
    }
    if (market.header.ingest_sequence == active.ingest_sequence &&
        market != active_market) {
        return SimulatedExecutionErrorCode::MarketSnapshotMismatch;
    }
    return std::nullopt;
}

TopOfBookExecutionSimulator::TopOfBookExecutionSimulator(
    std::string account_id
)
    : account_id_(std::move(account_id)) {
    if (account_id_.empty()) {
        throw std::invalid_argument(
            "execution simulator account ID must not be empty"
        );
    }
}

TopOfBookExecutionRecoveryState
TopOfBookExecutionSimulator::recovery_state() const {
    std::vector<std::string> processed_client_order_ids(
        processed_client_order_ids_.begin(),
        processed_client_order_ids_.end()
    );
    std::sort(
        processed_client_order_ids.begin(),
        processed_client_order_ids.end()
    );
    return TopOfBookExecutionRecoveryState{
        .account_id = account_id_,
        .instruments = instruments_,
        .processed_client_order_ids =
            std::move(processed_client_order_ids),
    };
}

TopOfBookExecutionSimulator TopOfBookExecutionSimulator::restore(
    TopOfBookExecutionRecoveryState recovery_state
) {
    const auto stream = bound_market_stream(recovery_state.instruments);
    for (const auto& [instrument, state] : recovery_state.instruments) {
        validate_recovered_execution(instrument, state, stream);
    }

    TopOfBookExecutionSimulator simulator(
        std::move(recovery_state.account_id)
    );
    simulator.instruments_ = std::move(recovery_state.instruments);
    for (std::string& client_order_id :
         recovery_state.processed_client_order_ids) {
        if (client_order_id.empty()) {
            throw std::invalid_argument(
                "recovered client order ID must not be empty"
            );
        }
        const bool inserted = simulator.processed_client_order_ids_
            .insert(std::move(client_order_id))
            .second;
        if (!inserted) {
            throw std::invalid_argument(
                "recovered client order IDs must be unique"
            );
        }
    }
    return simulator;
}

SimulatedExecutionResult TopOfBookExecutionSimulator::execute(
    const ApprovedOrderIntent& approved
) {
    const auto& intent = approved.intent();
    const auto& market = approved.market();
    if (intent.account_id != account_id_) {
        return execution_error(
            SimulatedExecutionErrorCode::AccountMismatch,
            "account_id",
            "approved order account does not match the simulator"
        );
    }
    if (processed_client_order_ids_.contains(intent.client_order_id)) {
        return execution_error(
            SimulatedExecutionErrorCode::DuplicateClientOrderId,
            "client_order_id",
            "client order ID has already been processed"
        );
    }

    if (const auto violation = market_lineage_violation(
            instruments_,
            market
        )) {
        return lineage_error(*violation);
    }

    // Only this instrument's liquidity is consulted or consumed. A strictly
    // newer observation of the instrument starts a fresh snapshot.
    const auto existing = instruments_.find(intent.instrument);
    InstrumentExecutionState state = existing != instruments_.end() &&
            market.header.ingest_sequence ==
                existing->second.active_market.header.ingest_sequence
        ? existing->second
        : InstrumentExecutionState{
              .active_market = market,
              .consumed_bid_quantity = Quantity{},
              .consumed_ask_quantity = Quantity{},
          };

    const auto finish = [&](SimulatedExecutionReport report) {
        processed_client_order_ids_.insert(intent.client_order_id);
        instruments_.insert_or_assign(intent.instrument, std::move(state));
        return report;
    };

    if (!is_marketable(approved)) {
        return finish(SimulatedExecutionReport{
            .intent = intent,
            .status = SimulatedExecutionStatus::Cancelled,
            .fill = std::nullopt,
            .unfilled_quantity = intent.quantity,
            .cancellation_reason =
                SimulatedCancellationReason::LimitNotMarketable,
            .market_ingest_sequence = market.header.ingest_sequence,
        });
    }

    const BookLevel& displayed_level = intent.side == Side::Buy
        ? *market.ask
        : *market.bid;
    Quantity& consumed_quantity = intent.side == Side::Buy
        ? state.consumed_ask_quantity
        : state.consumed_bid_quantity;
    const Quantity available_quantity =
        displayed_level.quantity - consumed_quantity;
    if (available_quantity.raw() == 0) {
        return finish(SimulatedExecutionReport{
            .intent = intent,
            .status = SimulatedExecutionStatus::Cancelled,
            .fill = std::nullopt,
            .unfilled_quantity = intent.quantity,
            .cancellation_reason =
                SimulatedCancellationReason::DisplayedLiquidityExhausted,
            .market_ingest_sequence = market.header.ingest_sequence,
        });
    }

    const Quantity fill_quantity = std::min(
        intent.quantity,
        available_quantity
    );
    const Quantity unfilled_quantity = intent.quantity - fill_quantity;
    consumed_quantity = consumed_quantity + fill_quantity;
    const Fill fill{
        .fill_id = intent.client_order_id + ":fill:0",
        .instrument = intent.instrument,
        .side = intent.side,
        .price = displayed_level.price,
        .quantity = fill_quantity,
        .fee = approved.fee(),
        .timestamp = intent.submitted_at,
    };
    const bool is_complete = unfilled_quantity.raw() == 0;
    return finish(SimulatedExecutionReport{
        .intent = intent,
        .status = is_complete
            ? SimulatedExecutionStatus::Filled
            : SimulatedExecutionStatus::PartiallyFilledAndCancelled,
        .fill = fill,
        .unfilled_quantity = unfilled_quantity,
        .cancellation_reason = is_complete
            ? std::nullopt
            : std::optional<SimulatedCancellationReason>(
                  SimulatedCancellationReason::DisplayedLiquidityExhausted
              ),
        .market_ingest_sequence = market.header.ingest_sequence,
    });
}

}  // namespace chronos
