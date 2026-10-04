#pragma once

#include "chronos/strategy/inference_schedule.hpp"
#include "chronos/trading_types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// The policy `long_flat_threshold` version 1, shared by the historical
// strategy runner and live deployments so they cannot drift apart.
//
// An up prediction with confidence_ppb >= threshold_ppb targets
// `target_quantity`; a down prediction at or above the threshold targets
// flat; a flat prediction, or one below the threshold, holds. A target
// becomes an order of the target minus the booked position, never while
// another order is unresolved, so consecutive "up" predictions never buy
// twice and the account is never short.
namespace chronos::strategy {

enum class PolicyReason {
    HoldFlatPrediction,
    HoldBelowThreshold,
    OrderPending,
    AlreadyAtTarget,
    OrderScheduled,
};

// The stable names journals and the API use.
[[nodiscard]] std::string_view policy_reason_name(PolicyReason reason) noexcept;

struct PolicyOrder {
    Side side{Side::Buy};
    Quantity quantity;
};

struct PolicyDecision {
    // Absent when the policy holds.
    std::optional<Quantity> target;
    PolicyReason reason{PolicyReason::HoldFlatPrediction};
    // Present exactly for OrderScheduled.
    std::optional<PolicyOrder> order;
};

[[nodiscard]] PolicyDecision decide_long_flat(
    const SchedulePrediction& prediction,
    std::uint64_t threshold_ppb,
    Quantity target_quantity,
    Quantity position,
    bool order_pending
);

}  // namespace chronos::strategy
