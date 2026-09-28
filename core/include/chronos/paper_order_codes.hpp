#pragma once

#include "chronos/paper_order_processor.hpp"

#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

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

// The enumerations above declare no explicit values, so their enumerators
// are exactly the contiguous values from zero that have a canonical name.
// Deriving the declared set this way keeps canonical_name the only mapping;
// the round-trip tests pin the assumption for every enumeration.
template <typename Enum>
[[nodiscard]] std::vector<Enum> declared_values() {
    std::vector<Enum> values;
    for (std::underlying_type_t<Enum> value = 0;
         canonical_name(static_cast<Enum>(value)).has_value();
         ++value) {
        values.push_back(static_cast<Enum>(value));
    }
    return values;
}

template <typename Enum>
[[nodiscard]] std::optional<Enum> from_canonical_name(
    std::string_view name
) noexcept {
    for (std::underlying_type_t<Enum> value = 0;; ++value) {
        const auto candidate = static_cast<Enum>(value);
        const std::optional<std::string_view> candidate_name =
            canonical_name(candidate);
        if (!candidate_name) {
            return std::nullopt;
        }
        if (*candidate_name == name) {
            return candidate;
        }
    }
}

}  // namespace chronos
