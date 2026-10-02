#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/features/top_of_book_features.hpp"
#include "chronos/fixed_decimal.hpp"
#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

// Version 1 of the future-midpoint-return label. See docs/labels-and-splits.md
// and ADR 0013.
//
// A feature row predicts at t, its available_at. Its future observation is
// the first later row of the same instrument available at or after
// t + horizon: the first, even when its midpoint is unavailable, because
// searching further would let future market state choose the horizon. The
// label is the midpoint's relative change from t to that observation, and
// it becomes known only when that observation is received.
namespace chronos::labels {

inline constexpr std::string_view kFutureMidpointReturnName = "future_midpoint_return";
inline constexpr std::string_view kFutureMidpointReturnVersion = "1";
inline constexpr std::int64_t kDefaultHorizonNanoseconds = 1'000'000'000;

// A dimensionless return, (future - current) / current, to nine decimal
// places.
struct ReturnTag;
using Return = FixedDecimal<ReturnTag, 1'000'000'000>;

// The sign of the exact return, with no deadband.
enum class Direction { Down, Flat, Up };

// Why a row has no label. Version 1's reasons, in the order they are
// checked.
enum class LabelUnavailability {
    CurrentMidpointUnavailable,
    NonpositiveCurrentMidpoint,
    // t + horizon exceeds the int64 nanosecond range.
    TimestampOverflow,
    // The artifact ends before any observation at or after t + horizon.
    NoFutureObservation,
    FutureMidpointUnavailable,
    // The return does not fit the fixed-point range.
    ReturnOverflow,
};

inline constexpr LabelUnavailability kLabelUnavailabilities[] = {
    LabelUnavailability::CurrentMidpointUnavailable,
    LabelUnavailability::NonpositiveCurrentMidpoint,
    LabelUnavailability::TimestampOverflow,
    LabelUnavailability::NoFutureObservation,
    LabelUnavailability::FutureMidpointUnavailable,
    LabelUnavailability::ReturnOverflow,
};

inline constexpr Direction kDirections[] = {Direction::Down, Direction::Flat, Direction::Up};

[[nodiscard]] std::string_view direction_name(Direction direction) noexcept;
[[nodiscard]] std::optional<Direction> direction_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view label_unavailability_name(LabelUnavailability reason) noexcept;
[[nodiscard]] std::optional<LabelUnavailability> label_unavailability_from_name(std::string_view name) noexcept;

struct FutureMidpointReturn {
    // Rounded half away from zero to the return scale.
    Return value;
    Direction direction;

    friend bool operator==(const FutureMidpointReturn&, const FutureMidpointReturn&) = default;
};

using Label = std::variant<FutureMidpointReturn, LabelUnavailability>;

// (future - current) / current with checked 128-bit arithmetic, or why it
// has no value. The direction is the sign of the exact quotient, so a move
// smaller than half the return scale's unit is still up or down even
// though its rounded value is zero.
[[nodiscard]] Label midpoint_return(Price current, Price future);

// What the labeler reads from one feature row.
struct LabelObservation {
    InstrumentId instrument;
    std::uint64_t ingest_sequence{0};
    Timestamp available_at;
    features::Feature<Price> midpoint;

    friend bool operator==(const LabelObservation&, const LabelObservation&) = default;
};

struct FutureObservation {
    std::uint64_t ingest_sequence{0};
    // When the label becomes known.
    Timestamp available_at;
    // available_at minus the prediction time: at least the horizon.
    std::int64_t elapsed_ns{0};
    features::Feature<Price> midpoint;

    friend bool operator==(const FutureObservation&, const FutureObservation&) = default;
};

struct FutureMidpointReturnRow {
    // The feature row the label belongs to.
    MarketDataStreamId stream;
    InstrumentId instrument;
    std::uint64_t ingest_sequence{0};
    // The feature row's available_at: when a prediction is made.
    Timestamp prediction_at;
    std::int64_t horizon_ns{0};
    features::Feature<Price> current_midpoint;
    // The selected future observation, whenever one exists, even if the
    // label is unavailable for another reason.
    std::optional<FutureObservation> future;
    Label label;

    friend bool operator==(const FutureMidpointReturnRow&, const FutureMidpointReturnRow&) = default;
};

// The label a row's own fields determine: its current midpoint, its
// prediction time and horizon, and its future observation. It ignores
// row.label, so a verifier can compare the two.
[[nodiscard]] Label label_of(const FutureMidpointReturnRow& row);

using LabelRowSink =std::function<std::optional<datasets::DatasetError>(const FutureMidpointReturnRow&)>;

// Labels every observation and passes the rows to sink in the observations'
// order. The observations must be one stream's rows in ingest order, with
// non-negative times that never decrease within an instrument; that is
// checked, never assumed. The horizon must be positive. The future
// observation of each row is found by one forward scan per instrument, so
// the work is linear in the number of observations.
[[nodiscard]] std::optional<datasets::DatasetError> label_future_midpoint_returns(
    const MarketDataStreamId& stream,
    const std::vector<LabelObservation>& observations,
    std::int64_t horizon_ns,
    const LabelRowSink& sink
);

}  // namespace chronos::labels
