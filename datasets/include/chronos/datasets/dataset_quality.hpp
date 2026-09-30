#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/market_data_validation.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace chronos::datasets {

// Version 1 of the quality report's rules. The report is a function of the
// events alone, so the same events always produce the same bytes.
inline constexpr std::string_view kQualityRulesName = "chronos.dataset_quality";
inline constexpr std::string_view kQualityRulesVersion = "1";

// Observations about a stream that already passed every fatal check:
// Chronos validation, one stream, contiguous ingest sequences, and
// non-decreasing receive time. It reports what the data shows and fixes
// nothing.
class DatasetQualityReport final {
public:
    [[nodiscard]] std::optional<DatasetError> observe(const ValidatedMarketEvent& event);

    [[nodiscard]] canonical_json::Value to_json() const;
    // The exact bytes of quality.json.
    [[nodiscard]] std::string serialize() const;

private:
    struct Quotes {
        std::uint64_t two_sided{0};
        std::uint64_t bid_only{0};
        std::uint64_t ask_only{0};
        std::uint64_t empty{0};
        std::uint64_t suspect{0};
        std::uint64_t locked{0};
        std::uint64_t crossed{0};
        // Two-sided, not suspect, and so not crossed.
        std::uint64_t valid_two_sided{0};
        std::optional<std::int64_t> minimum_spread_raw;
        std::optional<std::int64_t> maximum_spread_raw;
    };

    struct Instrument {
        std::uint64_t events{0};
        EventTypeCounts by_type;
        Timestamp first_receive;
        Timestamp last_receive;
        Timestamp last_event;
        std::uint64_t event_time_regressions{0};
        std::optional<std::int64_t> maximum_receive_gap_ns;
        Quotes quotes;
    };

    [[nodiscard]] std::optional<DatasetError> observe_quote(
        const TopOfBookEvent& quote,
        Quotes& quotes
    );

    std::uint64_t events_{0};
    EventTypeCounts by_type_;
    std::array<std::uint64_t, 5> flag_counts_{};
    std::uint64_t unflagged_events_{0};
    std::uint64_t events_with_venue_sequence_{0};
    std::map<std::string, Instrument> instruments_;
};

}  // namespace chronos::datasets
