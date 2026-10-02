#include "chronos/labels/future_midpoint_return.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::labels {
namespace {

using datasets::DatasetError;
using datasets::DatasetErrorCode;
using detail::WideInteger;

// The end of the horizon, or nullopt when it exceeds the int64 range.
std::optional<std::int64_t> horizon_end(Timestamp prediction_at, std::int64_t horizon_ns) {
    std::int64_t end = 0;
    if (__builtin_add_overflow(prediction_at.nanoseconds(), horizon_ns, &end)) {
        return std::nullopt;
    }
    return end;
}

}  // namespace

std::string_view direction_name(Direction direction) noexcept {
    switch (direction) {
        case Direction::Down:
            return "down";
        case Direction::Flat:
            return "flat";
        case Direction::Up:
            return "up";
    }
    return "unknown";
}

std::optional<Direction> direction_from_name(std::string_view name) noexcept {
    for (const Direction direction : kDirections) {
        if (name == direction_name(direction)) {
            return direction;
        }
    }
    return std::nullopt;
}

std::string_view label_unavailability_name(LabelUnavailability reason) noexcept {
    switch (reason) {
        case LabelUnavailability::CurrentMidpointUnavailable:
            return "current_midpoint_unavailable";
        case LabelUnavailability::NonpositiveCurrentMidpoint:
            return "nonpositive_current_midpoint";
        case LabelUnavailability::TimestampOverflow:
            return "timestamp_overflow";
        case LabelUnavailability::NoFutureObservation:
            return "no_future_observation";
        case LabelUnavailability::FutureMidpointUnavailable:
            return "future_midpoint_unavailable";
        case LabelUnavailability::ReturnOverflow:
            return "return_overflow";
    }
    return "unknown";
}

std::optional<LabelUnavailability> label_unavailability_from_name(std::string_view name) noexcept {
    for (const LabelUnavailability reason : kLabelUnavailabilities) {
        if (name == label_unavailability_name(reason)) {
            return reason;
        }
    }
    return std::nullopt;
}

Label midpoint_return(Price current, Price future) {
    if (current.raw() <= 0) {
        return LabelUnavailability::NonpositiveCurrentMidpoint;
    }
    const WideInteger change = WideInteger{future.raw()} - current.raw();
    WideInteger numerator = 0;
    if (__builtin_mul_overflow(change, WideInteger{Return::scale()}, &numerator)) {
        return LabelUnavailability::ReturnOverflow;
    }
    std::int64_t raw = 0;
    try {
        raw = detail::rounded_divide(numerator, current.raw(), RoundingMode::HalfAwayFromZero);
    } catch (const std::overflow_error&) {
        return LabelUnavailability::ReturnOverflow;
    }
    // current is positive, so the exact quotient has the sign of the change.
    const Direction direction = change < 0 ? Direction::Down : change == 0 ? Direction::Flat : Direction::Up;
    return FutureMidpointReturn{.value = Return::from_raw(raw), .direction = direction};
}

Label label_of(const FutureMidpointReturnRow& row) {
    const auto* current = std::get_if<Price>(&row.current_midpoint);
    if (current == nullptr) {
        return LabelUnavailability::CurrentMidpointUnavailable;
    }
    if (current->raw() <= 0) {
        return LabelUnavailability::NonpositiveCurrentMidpoint;
    }
    if (!horizon_end(row.prediction_at, row.horizon_ns)) {
        return LabelUnavailability::TimestampOverflow;
    }
    if (!row.future) {
        return LabelUnavailability::NoFutureObservation;
    }
    const auto* future = std::get_if<Price>(&row.future->midpoint);
    if (future == nullptr) {
        return LabelUnavailability::FutureMidpointUnavailable;
    }
    return midpoint_return(*current, *future);
}

std::optional<DatasetError> label_future_midpoint_returns(
    const MarketDataStreamId& stream,
    const std::vector<LabelObservation>& observations,
    std::int64_t horizon_ns,
    const LabelRowSink& sink
) {
    if (horizon_ns <= 0) {
        return datasets::dataset_error(
            DatasetErrorCode::InvalidArgument,
            "horizon_ns",
            "must be a positive number of nanoseconds"
        );
    }
    // Each instrument's observations, as positions in `observations`.
    std::map<std::string, std::vector<std::size_t>> by_instrument;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        const LabelObservation& observation = observations[index];
        if (observation.available_at.nanoseconds() < 0) {
            return datasets::dataset_error(DatasetErrorCode::InvalidValue, "available_at_ns", "must not be negative", index + 1);
        }
        auto& positions = by_instrument[observation.instrument.value()];
        if (!positions.empty() && observation.available_at < observations[positions.back()].available_at) {
            return datasets::dataset_error(
                DatasetErrorCode::ReceiveTimeMovedBackward,
                "available_at_ns",
                "moved backward within instrument " + observation.instrument.value(),
                index + 1
            );
        }
        positions.push_back(index);
    }

    // Times never decrease within an instrument, so neither do the horizon
    // ends, and each scan resumes where the previous row's stopped.
    std::vector<std::optional<std::size_t>> future(observations.size());
    for (const auto& [instrument, positions] : by_instrument) {
        std::size_t candidate = 0;
        for (std::size_t current = 0; current < positions.size(); ++current) {
            const auto end = horizon_end(observations[positions[current]].available_at, horizon_ns);
            if (!end) {
                // Every later row of the instrument overflows too.
                break;
            }
            candidate = std::max(candidate, current + 1);
            while (candidate < positions.size() && observations[positions[candidate]].available_at.nanoseconds() < *end) {
                ++candidate;
            }
            if (candidate < positions.size()) {
                future[positions[current]] = positions[candidate];
            }
        }
    }

    for (std::size_t index = 0; index < observations.size(); ++index) {
        const LabelObservation& current = observations[index];
        FutureMidpointReturnRow row{
            .stream = stream,
            .instrument = current.instrument,
            .ingest_sequence = current.ingest_sequence,
            .prediction_at = current.available_at,
            .horizon_ns = horizon_ns,
            .current_midpoint = current.midpoint,
            .future = std::nullopt,
            .label = LabelUnavailability::NoFutureObservation,
        };
        if (future[index]) {
            const LabelObservation& observed = observations[*future[index]];
            // Both times are non-negative and observed is the later, so the
            // difference cannot overflow.
            row.future = FutureObservation{
                .ingest_sequence = observed.ingest_sequence,
                .available_at = observed.available_at,
                .elapsed_ns = observed.available_at.nanoseconds() - current.available_at.nanoseconds(),
                .midpoint = observed.midpoint,
            };
        }
        row.label = label_of(row);
        if (auto error = sink(row)) {
            return error;
        }
    }
    return std::nullopt;
}

}  // namespace chronos::labels
