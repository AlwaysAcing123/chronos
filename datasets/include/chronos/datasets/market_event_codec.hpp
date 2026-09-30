#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/market_data_validation.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::datasets {

// Version 1 of the normalized market-event record. Each record is one
// canonical JSON object on one line of events.ndjson. See
// docs/normalized-market-datasets.md for the field list.
inline constexpr std::string_view kMarketEventSchemaVersion = "1";

// Quality flags in bit order, which is also the order they are listed in a
// record.
inline constexpr std::array<MarketDataQualityFlag, 5> kQualityFlags = {
    MarketDataQualityFlag::Snapshot,
    MarketDataQualityFlag::Replay,
    MarketDataQualityFlag::QuestionableReceiveTimestamp,
    MarketDataQualityFlag::PotentiallyInvalidBook,
    MarketDataQualityFlag::SourceSpecific,
};

[[nodiscard]] std::string_view quality_flag_name(MarketDataQualityFlag flag) noexcept;
[[nodiscard]] std::string_view event_type_name(const MarketEvent& event) noexcept;
[[nodiscard]] std::string_view trading_status_name(TradingStatus status) noexcept;

// The canonical record for an event, without the line's newline. Fails with
// UnrepresentableText when a text field is not valid UTF-8, which JSON
// cannot carry exactly.
[[nodiscard]] std::variant<std::string, DatasetError> encode_market_event(
    const ValidatedMarketEvent& event
);

// Decodes one record. It must be canonical, use only known fields, and pass
// Chronos market-event validation; nothing else is ever returned.
[[nodiscard]] std::variant<ValidatedMarketEvent, DatasetError> decode_market_event(
    std::string_view record,
    std::optional<std::uint64_t> line = std::nullopt
);

}  // namespace chronos::datasets
