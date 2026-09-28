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

}  // namespace

TopOfBookExecutionSimulator::TopOfBookExecutionSimulator(
    std::string account_id,
    InstrumentId instrument
)
    : account_id_(std::move(account_id)),
      instrument_(std::move(instrument)) {
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
        .instrument = instrument_,
        .active_market = active_market_,
        .consumed_bid_quantity = consumed_bid_quantity_,
        .consumed_ask_quantity = consumed_ask_quantity_,
        .processed_client_order_ids =
            std::move(processed_client_order_ids),
    };
}

TopOfBookExecutionSimulator TopOfBookExecutionSimulator::restore(
    TopOfBookExecutionRecoveryState recovery_state
) {
    if (recovery_state.consumed_bid_quantity.raw() < 0 ||
        recovery_state.consumed_ask_quantity.raw() < 0) {
        throw std::invalid_argument(
            "recovered consumed liquidity must not be negative"
        );
    }
    if (!recovery_state.active_market) {
        if (recovery_state.consumed_bid_quantity.raw() != 0 ||
            recovery_state.consumed_ask_quantity.raw() != 0) {
            throw std::invalid_argument(
                "recovered liquidity requires an active market"
            );
        }
    } else {
        auto validation = validate_market_event(
            MarketEvent(*recovery_state.active_market)
        );
        if (std::holds_alternative<MarketEventValidationFailure>(validation)) {
            throw std::invalid_argument(
                "recovered active market failed validation"
            );
        }
        if (recovery_state.active_market->header.instrument !=
            recovery_state.instrument) {
            throw std::invalid_argument(
                "recovered active market instrument does not match"
            );
        }
        const auto& bid = recovery_state.active_market->bid;
        if ((!bid && recovery_state.consumed_bid_quantity.raw() != 0) ||
            (bid && recovery_state.consumed_bid_quantity > bid->quantity)) {
            throw std::invalid_argument(
                "recovered consumed bid quantity exceeds the market"
            );
        }
        const auto& ask = recovery_state.active_market->ask;
        if ((!ask && recovery_state.consumed_ask_quantity.raw() != 0) ||
            (ask && recovery_state.consumed_ask_quantity > ask->quantity)) {
            throw std::invalid_argument(
                "recovered consumed ask quantity exceeds the market"
            );
        }
    }

    TopOfBookExecutionSimulator simulator(
        std::move(recovery_state.account_id),
        std::move(recovery_state.instrument)
    );
    simulator.active_market_ = std::move(recovery_state.active_market);
    simulator.consumed_bid_quantity_ =
        recovery_state.consumed_bid_quantity;
    simulator.consumed_ask_quantity_ =
        recovery_state.consumed_ask_quantity;
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
    if (intent.instrument != instrument_) {
        return execution_error(
            SimulatedExecutionErrorCode::InstrumentMismatch,
            "instrument",
            "approved order instrument does not match the simulator"
        );
    }
    if (processed_client_order_ids_.contains(intent.client_order_id)) {
        return execution_error(
            SimulatedExecutionErrorCode::DuplicateClientOrderId,
            "client_order_id",
            "client order ID has already been processed"
        );
    }

    if (active_market_) {
        if (market.header.stream != active_market_->header.stream) {
            return execution_error(
                SimulatedExecutionErrorCode::MarketStreamMismatch,
                "market.header.stream",
                "approved order belongs to a different market-data stream"
            );
        }
        if (market.header.ingest_sequence <
            active_market_->header.ingest_sequence) {
            return execution_error(
                SimulatedExecutionErrorCode::StaleMarketApproval,
                "market.header.ingest_sequence",
                "approved order uses an older market observation"
            );
        }
        if (market.header.ingest_sequence ==
                active_market_->header.ingest_sequence &&
            market != *active_market_) {
            return execution_error(
                SimulatedExecutionErrorCode::MarketSnapshotMismatch,
                "market",
                "one ingest sequence cannot identify different market states"
            );
        }
    }

    const bool is_new_market = !active_market_ ||
        market.header.ingest_sequence > active_market_->header.ingest_sequence;
    if (is_new_market) {
        active_market_ = market;
        consumed_bid_quantity_ = Quantity{};
        consumed_ask_quantity_ = Quantity{};
    }

    if (!is_marketable(approved)) {
        processed_client_order_ids_.insert(intent.client_order_id);
        return SimulatedExecutionReport{
            .intent = intent,
            .status = SimulatedExecutionStatus::Cancelled,
            .fill = std::nullopt,
            .unfilled_quantity = intent.quantity,
            .cancellation_reason =
                SimulatedCancellationReason::LimitNotMarketable,
            .market_ingest_sequence = market.header.ingest_sequence,
        };
    }

    const BookLevel& displayed_level = intent.side == Side::Buy
        ? *market.ask
        : *market.bid;
    const Quantity consumed_quantity = intent.side == Side::Buy
        ? consumed_ask_quantity_
        : consumed_bid_quantity_;
    const Quantity available_quantity =
        displayed_level.quantity - consumed_quantity;
    if (available_quantity.raw() == 0) {
        processed_client_order_ids_.insert(intent.client_order_id);
        return SimulatedExecutionReport{
            .intent = intent,
            .status = SimulatedExecutionStatus::Cancelled,
            .fill = std::nullopt,
            .unfilled_quantity = intent.quantity,
            .cancellation_reason =
                SimulatedCancellationReason::DisplayedLiquidityExhausted,
            .market_ingest_sequence = market.header.ingest_sequence,
        };
    }

    const Quantity fill_quantity = std::min(
        intent.quantity,
        available_quantity
    );
    const Quantity unfilled_quantity = intent.quantity - fill_quantity;
    const Quantity updated_consumed_quantity =
        consumed_quantity + fill_quantity;
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
    SimulatedExecutionReport report{
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
    };

    processed_client_order_ids_.insert(intent.client_order_id);
    if (intent.side == Side::Buy) {
        consumed_ask_quantity_ = updated_consumed_quantity;
    } else {
        consumed_bid_quantity_ = updated_consumed_quantity;
    }
    return report;
}

}  // namespace chronos
