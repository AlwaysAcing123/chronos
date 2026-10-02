#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/market_data_validation.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

// Version 1 of the chronological, purged split. See docs/labels-and-splits.md
// and ADR 0014.
//
// Label rows, in ingest order with prediction times that never decrease,
// are divided into three contiguous periods, train then validation then
// test, never shuffled. A row is usable only if its label has a value and,
// for train and validation, becomes known strictly before the next period's
// first prediction: a label that crosses that boundary is purged, never
// moved. So
//
//     max usable train label availability      < validation's first prediction
//     max usable validation label availability < test's first prediction
namespace chronos::splits {

inline constexpr std::string_view kChronologicalSplitName = "chronological_purged";
inline constexpr std::string_view kChronologicalSplitVersion = "1";

enum class SplitPeriod { Train, Validation, Test };

inline constexpr SplitPeriod kSplitPeriods[] = {SplitPeriod::Train, SplitPeriod::Validation, SplitPeriod::Test};

[[nodiscard]] std::string_view period_name(SplitPeriod period) noexcept;
[[nodiscard]] std::optional<SplitPeriod> period_from_name(std::string_view name) noexcept;

// Why a row is in no split, in the order the reasons are checked.
enum class SplitExclusion {
    LabelUnavailable,
    // A training row whose label is known only at or after validation
    // begins.
    LabelCrossesValidationStart,
    // A validation row whose label is known only at or after test begins.
    LabelCrossesTestStart,
};

inline constexpr SplitExclusion kSplitExclusions[] = {
    SplitExclusion::LabelUnavailable,
    SplitExclusion::LabelCrossesValidationStart,
    SplitExclusion::LabelCrossesTestStart,
};

[[nodiscard]] std::string_view exclusion_name(SplitExclusion exclusion) noexcept;
[[nodiscard]] std::optional<SplitExclusion> exclusion_from_name(std::string_view name) noexcept;

template <typename Value>
struct ByPeriod {
    Value train{};
    Value validation{};
    Value test{};

    [[nodiscard]] Value& operator[](SplitPeriod period) noexcept {
        return period == SplitPeriod::Train ? train : period == SplitPeriod::Validation ? validation : test;
    }
    [[nodiscard]] const Value& operator[](SplitPeriod period) const noexcept {
        return period == SplitPeriod::Train ? train : period == SplitPeriod::Validation ? validation : test;
    }

    friend bool operator==(const ByPeriod&, const ByPeriod&) = default;
};

// Whole percentages of the rows for each period: each at least 1, together
// exactly 100.
using SplitPercentages = ByPeriod<std::uint64_t>;

inline constexpr SplitPercentages kDefaultSplitPercentages{.train = 60, .validation = 20, .test = 20};

[[nodiscard]] std::optional<datasets::DatasetError> check_percentages(const SplitPercentages& percentages);

// Where validation and test begin: train is rows [0, validation_start),
// validation [validation_start, test_start), and test the rest.
struct SplitBoundaries {
    std::uint64_t validation_start{0};
    std::uint64_t test_start{0};
    // The first prediction time of each.
    Timestamp validation_start_at;
    Timestamp test_start_at;

    friend bool operator==(const SplitBoundaries&, const SplitBoundaries&) = default;
};

// Each boundary is nominally floor(rows * cumulative percentage / 100),
// then moved later past every row that shares the previous row's prediction
// time, so rows with equal times are never divided. Prediction times must
// never decrease. Fails when a period would be empty.
[[nodiscard]] std::variant<SplitBoundaries, datasets::DatasetError> chronological_boundaries(
    const std::vector<Timestamp>& prediction_times,
    const SplitPercentages& percentages
);

// What the splitter reads from one label row.
struct SplitInput {
    InstrumentId instrument;
    std::uint64_t ingest_sequence{0};
    Timestamp prediction_at;
    // The label has a value.
    bool label_usable{false};
    // When the label becomes known, if a future observation exists.
    std::optional<Timestamp> label_available_at;

    friend bool operator==(const SplitInput&, const SplitInput&) = default;
};

struct SplitRow {
    MarketDataStreamId stream;
    InstrumentId instrument;
    std::uint64_t ingest_sequence{0};
    Timestamp prediction_at;
    bool label_usable{false};
    std::optional<Timestamp> label_available_at;
    // The chronological period the row falls in.
    SplitPeriod period{SplitPeriod::Train};
    // Why the row is in no split; usable in its period's split otherwise.
    std::optional<SplitExclusion> exclusion;

    friend bool operator==(const SplitRow&, const SplitRow&) = default;
};

[[nodiscard]] SplitPeriod period_of(std::uint64_t position, const SplitBoundaries& boundaries) noexcept;

// Why a row in a period is excluded, if it is. A usable label must have an
// availability time.
[[nodiscard]] std::optional<SplitExclusion> exclusion_of(
    SplitPeriod period,
    bool label_usable,
    const std::optional<Timestamp>& label_available_at,
    const SplitBoundaries& boundaries
);

using SplitRowSink = std::function<std::optional<datasets::DatasetError>(const SplitRow&)>;

// Assigns every row and passes the assignments to sink in order, once every
// period and every split is known to be nonempty.
[[nodiscard]] std::variant<SplitBoundaries, datasets::DatasetError> split_chronologically(
    const MarketDataStreamId& stream,
    const std::vector<SplitInput>& inputs,
    const SplitPercentages& percentages,
    const SplitRowSink& sink
);

}  // namespace chronos::splits
