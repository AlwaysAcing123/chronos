#pragma once

#include "chronos/fixed_decimal.hpp"
#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// Version 1 of the top-of-book feature set. See docs/feature-engine.md.
//
// Point in time: a row is computed when its triggering top-of-book event
// is applied, from that event and the instrument's latest trading status
// only. No later event can reach it, so no row can depend on the future.
namespace chronos::features {

inline constexpr std::string_view kTopOfBookFeatureSetName = "top_of_book";
inline constexpr std::string_view kTopOfBookFeatureSetVersion = "1";

// Dimensionless ratios such as imbalance, in [-1, 1], to nine decimal
// places.
struct RatioTag;
using Ratio = FixedDecimal<RatioTag, 1'000'000'000>;

// Why a feature has no value. Unavailable features are never zero.
enum class Unavailability {
    MissingBid,
    MissingAsk,
    MissingBothSides,
    // The quote is flagged as a potentially invalid book or as having a
    // questionable receive timestamp.
    SuspectQuote,
    // The instrument's latest trading status is halted or closed.
    TradingHalted,
    MarketClosed,
    ZeroDenominator,
    ArithmeticOverflow,
};

[[nodiscard]] std::string_view unavailability_name(Unavailability reason) noexcept;
[[nodiscard]] std::optional<Unavailability> unavailability_from_name(
    std::string_view name
) noexcept;

template <typename Value>
using Feature = std::variant<Value, Unavailability>;

struct TopOfBookFeatureRow {
    // The triggering event, which the row can be traced back to.
    MarketDataStreamId stream;
    InstrumentId instrument;
    std::uint64_t ingest_sequence{0};
    Timestamp event_timestamp;
    Timestamp receive_timestamp;
    // When the row is known: the triggering event's receive time, the
    // market clock at which Chronos applies it.
    Timestamp available_at;
    MarketDataQuality quality;
    // The instrument's latest reported trading status, or nullopt before
    // any status event.
    std::optional<TradingStatus> trading_status;

    Feature<Price> best_bid;
    Feature<Price> best_ask;
    Feature<Quantity> bid_size;
    Feature<Quantity> ask_size;
    // ask - bid, exact.
    Feature<Price> spread;
    // (bid + ask) / 2, rounded half away from zero to the price scale.
    Feature<Price> midpoint;
    // (bid_size - ask_size) / (bid_size + ask_size), rounded half away from
    // zero to the ratio scale.
    Feature<Ratio> imbalance;
    // (ask * bid_size + bid * ask_size) / (bid_size + ask_size), rounded
    // half away from zero to the price scale.
    Feature<Price> microprice;

    friend bool operator==(const TopOfBookFeatureRow&, const TopOfBookFeatureRow&) = default;
};

// The calculations on raw values, with checked 128-bit arithmetic. They
// accept any raw values, including ones validation forbids, so every
// unavailable outcome can be exercised.
[[nodiscard]] Feature<Price> spread(Price bid, Price ask);
[[nodiscard]] Feature<Price> midpoint(Price bid, Price ask);
[[nodiscard]] Feature<Ratio> imbalance(Quantity bid_size, Quantity ask_size);
[[nodiscard]] Feature<Price> microprice(Price bid, Quantity bid_size, Price ask, Quantity ask_size);

// Computes rows from the same validated event stream that replay and live
// sources produce. State is kept per instrument and holds only its latest
// trading status. Events must arrive in ingest order.
class TopOfBookFeatureEngine final {
public:
    // Applies one event. A top-of-book event produces its row; other events
    // update state and produce none.
    [[nodiscard]] std::optional<TopOfBookFeatureRow> apply(const ValidatedMarketEvent& event);

private:
    std::map<std::string, TradingStatus> trading_status_;
};

}  // namespace chronos::features
