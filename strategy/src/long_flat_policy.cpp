#include "chronos/strategy/long_flat_policy.hpp"

namespace chronos::strategy {

std::string_view policy_reason_name(PolicyReason reason) noexcept {
    switch (reason) {
        case PolicyReason::HoldFlatPrediction: return "hold_flat_prediction";
        case PolicyReason::HoldBelowThreshold: return "hold_below_threshold";
        case PolicyReason::OrderPending: return "order_pending";
        case PolicyReason::AlreadyAtTarget: return "already_at_target";
        case PolicyReason::OrderScheduled: return "order_scheduled";
    }
    return "hold_flat_prediction";
}

PolicyDecision decide_long_flat(
    const SchedulePrediction& prediction,
    std::uint64_t threshold_ppb,
    Quantity target_quantity,
    Quantity position,
    bool order_pending
) {
    if (prediction.predicted_class == PredictedClass::Flat) {
        return {.target = std::nullopt, .reason = PolicyReason::HoldFlatPrediction, .order = std::nullopt};
    }
    if (prediction.confidence_ppb < threshold_ppb) {
        return {.target = std::nullopt, .reason = PolicyReason::HoldBelowThreshold, .order = std::nullopt};
    }
    const Quantity target = prediction.predicted_class == PredictedClass::Up ? target_quantity : Quantity{};
    if (order_pending) {
        return {.target = target, .reason = PolicyReason::OrderPending, .order = std::nullopt};
    }
    if (target == position) {
        return {.target = target, .reason = PolicyReason::AlreadyAtTarget, .order = std::nullopt};
    }
    const bool buy = target > position;
    return {
        .target = target,
        .reason = PolicyReason::OrderScheduled,
        .order = PolicyOrder{
            .side = buy ? Side::Buy : Side::Sell,
            .quantity = buy ? target - position : position - target,
        },
    };
}

}  // namespace chronos::strategy
