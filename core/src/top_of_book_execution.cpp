#include "chronos/top_of_book_execution.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

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
