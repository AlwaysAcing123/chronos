#include "chronos/splits/chronological_split.hpp"

#include "chronos/fixed_decimal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::splits {
namespace {

using datasets::DatasetError;
using datasets::DatasetErrorCode;

// floor(rows * percent / 100), without overflow.
std::uint64_t nominal_boundary(std::uint64_t rows, std::uint64_t percent) {
    return static_cast<std::uint64_t>(detail::WideInteger{rows} * percent / 100);
}

std::uint64_t past_equal_times(const std::vector<Timestamp>& times, std::uint64_t boundary) {
    while (boundary > 0 && boundary < times.size() && times[boundary] == times[boundary - 1]) {
        ++boundary;
    }
    return boundary;
}

DatasetError empty_split(std::string_view what) {
    return datasets::dataset_error(DatasetErrorCode::EmptySplit, "splits", std::string(what));
}

}  // namespace

std::string_view period_name(SplitPeriod period) noexcept {
    switch (period) {
        case SplitPeriod::Train:
            return "train";
        case SplitPeriod::Validation:
            return "validation";
        case SplitPeriod::Test:
            return "test";
    }
    return "unknown";
}

std::optional<SplitPeriod> period_from_name(std::string_view name) noexcept {
    for (const SplitPeriod period : kSplitPeriods) {
        if (name == period_name(period)) {
            return period;
        }
    }
    return std::nullopt;
}

std::string_view exclusion_name(SplitExclusion exclusion) noexcept {
    switch (exclusion) {
        case SplitExclusion::LabelUnavailable:
            return "label_unavailable";
        case SplitExclusion::LabelCrossesValidationStart:
            return "label_crosses_validation_start";
        case SplitExclusion::LabelCrossesTestStart:
            return "label_crosses_test_start";
    }
    return "unknown";
}

std::optional<SplitExclusion> exclusion_from_name(std::string_view name) noexcept {
    for (const SplitExclusion exclusion : kSplitExclusions) {
        if (name == exclusion_name(exclusion)) {
            return exclusion;
        }
    }
    return std::nullopt;
}

std::optional<DatasetError> check_percentages(const SplitPercentages& percentages) {
    for (const SplitPeriod period : kSplitPeriods) {
        // With all three at least 1 and adding up to 100, none exceeds 98.
        if (percentages[period] == 0 || percentages[period] > 98) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidArgument,
                "percentages",
                std::string(period_name(period)) + " must be a whole percentage from 1 to 98"
            );
        }
    }
    if (percentages.train + percentages.validation + percentages.test != 100) {
        return datasets::dataset_error(DatasetErrorCode::InvalidArgument, "percentages", "must add up to exactly 100");
    }
    return std::nullopt;
}

std::variant<SplitBoundaries, DatasetError> chronological_boundaries(
    const std::vector<Timestamp>& prediction_times,
    const SplitPercentages& percentages
) {
    if (auto error = check_percentages(percentages)) {
        return std::move(*error);
    }
    for (std::size_t index = 1; index < prediction_times.size(); ++index) {
        if (prediction_times[index] < prediction_times[index - 1]) {
            return datasets::dataset_error(
                DatasetErrorCode::ReceiveTimeMovedBackward,
                "prediction_at_ns",
                "prediction times must never decrease in row order",
                index + 1
            );
        }
    }
    const std::uint64_t rows = prediction_times.size();
    const std::uint64_t validation_start = past_equal_times(prediction_times, nominal_boundary(rows, percentages.train));
    const std::uint64_t test_start =
        past_equal_times(prediction_times, nominal_boundary(rows, percentages.train + percentages.validation));
    if (validation_start == 0) {
        return empty_split("the training period is empty: there are too few rows for its percentage");
    }
    if (test_start <= validation_start) {
        return empty_split("the validation period is empty: too few rows, or one prediction time spans its whole share");
    }
    if (test_start >= rows) {
        return empty_split("the test period is empty: too few rows, or the last prediction time spans its whole share");
    }
    return SplitBoundaries{
        .validation_start = validation_start,
        .test_start = test_start,
        .validation_start_at = prediction_times[validation_start],
        .test_start_at = prediction_times[test_start],
    };
}

SplitPeriod period_of(std::uint64_t position, const SplitBoundaries& boundaries) noexcept {
    return position < boundaries.validation_start ? SplitPeriod::Train
        : position < boundaries.test_start ? SplitPeriod::Validation
        : SplitPeriod::Test;
}

std::optional<SplitExclusion> exclusion_of(
    SplitPeriod period,
    bool label_usable,
    const std::optional<Timestamp>& label_available_at,
    const SplitBoundaries& boundaries
) {
    if (!label_usable || !label_available_at) {
        return SplitExclusion::LabelUnavailable;
    }
    if (period == SplitPeriod::Train && *label_available_at >= boundaries.validation_start_at) {
        return SplitExclusion::LabelCrossesValidationStart;
    }
    if (period == SplitPeriod::Validation && *label_available_at >= boundaries.test_start_at) {
        return SplitExclusion::LabelCrossesTestStart;
    }
    return std::nullopt;
}

std::variant<SplitBoundaries, DatasetError> split_chronologically(
    const MarketDataStreamId& stream,
    const std::vector<SplitInput>& inputs,
    const SplitPercentages& percentages,
    const SplitRowSink& sink
) {
    std::vector<Timestamp> times;
    times.reserve(inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (inputs[index].label_usable && !inputs[index].label_available_at) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidValue,
                "label_available_at_ns",
                "a usable label must have an availability time",
                index + 1
            );
        }
        times.push_back(inputs[index].prediction_at);
    }
    auto found = chronological_boundaries(times, percentages);
    if (auto* error = std::get_if<DatasetError>(&found)) {
        return std::move(*error);
    }
    const SplitBoundaries boundaries = std::get<SplitBoundaries>(found);
    const auto assignment = [&](std::size_t index) {
        const SplitInput& input = inputs[index];
        const SplitPeriod period = period_of(index, boundaries);
        return SplitRow{
            .stream = stream,
            .instrument = input.instrument,
            .ingest_sequence = input.ingest_sequence,
            .prediction_at = input.prediction_at,
            .label_usable = input.label_usable,
            .label_available_at = input.label_available_at,
            .period = period,
            .exclusion = exclusion_of(period, input.label_usable, input.label_available_at, boundaries),
        };
    };
    ByPeriod<std::uint64_t> usable;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const SplitRow row = assignment(index);
        if (!row.exclusion) {
            ++usable[row.period];
        }
    }
    for (const SplitPeriod period : kSplitPeriods) {
        if (usable[period] == 0) {
            return empty_split(
                "the " + std::string(period_name(period)) +
                " split has no usable rows once unavailable and boundary-crossing labels are excluded"
            );
        }
    }
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        if (auto error = sink(assignment(index))) {
            return std::move(*error);
        }
    }
    return boundaries;
}

}  // namespace chronos::splits
