#pragma once

#include "chronos/paper_order_processor.hpp"

#include <optional>
#include <string_view>

namespace chronos {

// Stable machine-readable names for the paper-order vocabulary. PostgreSQL
// journal records and the paper-trading API share these names, so renaming a
// value is a persisted-data and public-contract change.
//
// A value outside the declared enumeration has no canonical name. Callers
// decide explicitly whether to preserve, reject, or report such a value
// instead of receiving a sentinel string that could be mistaken for data.

[[nodiscard]] std::optional<std::string_view> canonical_name(
    OrderSourceKind kind
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    Side side
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    OrderType type
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    TimeInForce time_in_force
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    SimulatedExecutionStatus status
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    SimulatedCancellationReason reason
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    PreTradeRiskErrorCode code
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    SimulatedExecutionErrorCode code
) noexcept;

[[nodiscard]] std::optional<std::string_view> canonical_name(
    PaperOrderProcessingErrorCode code
) noexcept;

}  // namespace chronos
