#include "chronos/datasets/dataset_quality.hpp"

#include "chronos/datasets/json_fields.hpp"
#include "chronos/datasets/market_event_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace chronos::datasets {
namespace {

namespace json = chronos::canonical_json;

json::Value decimal(std::uint64_t value) { return json::Value(std::to_string(value)); }

json::Value nullable_decimal(const std::optional<std::int64_t>& value) {
    return value ? json::Value(std::to_string(*value)) : json::Value(nullptr);
}

bool increment(std::uint64_t& counter) {
    if (counter == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++counter;
    return true;
}

std::uint64_t& type_counter(EventTypeCounts& counts, const MarketEvent& event) {
    return *std::visit(
        [&counts](const auto& concrete) -> std::uint64_t* {
            using Event = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Event, TradeEvent>) {
                return &counts.trade;
            } else if constexpr (std::is_same_v<Event, TopOfBookEvent>) {
                return &counts.top_of_book;
            } else if constexpr (std::is_same_v<Event, MarketStatusEvent>) {
                return &counts.market_status;
            } else {
                return &counts.instrument_definition;
            }
        },
        event
    );
}

DatasetError overflow() {
    return dataset_error(
        DatasetErrorCode::CounterOverflow,
        "quality",
        "a quality count cannot be represented"
    );
}

}  // namespace

std::optional<DatasetError> DatasetQualityReport::observe_quote(
    const TopOfBookEvent& quote,
    Quotes& quotes
) {
    const bool suspect = quote.header.quality.is_suspect();
    bool counted = true;
    if (suspect) {
        counted = increment(quotes.suspect);
    }
    if (!quote.bid && !quote.ask) {
        counted = counted && increment(quotes.empty);
    } else if (!quote.ask) {
        counted = counted && increment(quotes.bid_only);
    } else if (!quote.bid) {
        counted = counted && increment(quotes.ask_only);
    } else {
        counted = counted && increment(quotes.two_sided);
        const Price bid = quote.bid->price;
        const Price ask = quote.ask->price;
        if (bid == ask) {
            counted = counted && increment(quotes.locked);
        } else if (bid > ask) {
            counted = counted && increment(quotes.crossed);
        }
        if (!suspect) {
            counted = counted && increment(quotes.valid_two_sided);
            // Positive prices, so the difference cannot overflow.
            const std::int64_t spread = ask.raw() - bid.raw();
            quotes.minimum_spread_raw =
                std::min(quotes.minimum_spread_raw.value_or(spread), spread);
            quotes.maximum_spread_raw =
                std::max(quotes.maximum_spread_raw.value_or(spread), spread);
        }
    }
    if (!counted) {
        return overflow();
    }
    return std::nullopt;
}

// An error leaves the report partly updated; the caller then abandons the
// dataset, so no update needs to be undone.
std::optional<DatasetError> DatasetQualityReport::observe(const ValidatedMarketEvent& event) {
    const MarketEventHeader& header = market_event_header(event.value());
    auto [position, inserted] = instruments_.try_emplace(header.instrument.value());
    Instrument& instrument = position->second;
    if (inserted) {
        instrument.first_receive = header.receive_timestamp;
    } else {
        if (header.event_timestamp < instrument.last_event &&
            !increment(instrument.event_time_regressions)) {
            return overflow();
        }
        // Receive time never decreases in a published dataset, and both
        // times are non-negative, so the gap is representable.
        const std::int64_t gap =
            header.receive_timestamp.nanoseconds() - instrument.last_receive.nanoseconds();
        instrument.maximum_receive_gap_ns =
            std::max(instrument.maximum_receive_gap_ns.value_or(gap), gap);
    }
    instrument.last_receive = header.receive_timestamp;
    instrument.last_event = header.event_timestamp;

    bool counted = increment(events_) &&
        increment(type_counter(by_type_, event.value())) &&
        increment(instrument.events) &&
        increment(type_counter(instrument.by_type, event.value()));
    bool flagged = false;
    for (std::size_t index = 0; index < kQualityFlags.size(); ++index) {
        if (header.quality.contains(kQualityFlags[index])) {
            flagged = true;
            counted = counted && increment(flag_counts_[index]);
        }
    }
    if (!flagged) {
        counted = counted && increment(unflagged_events_);
    }
    if (header.venue_sequence) {
        counted = counted && increment(events_with_venue_sequence_);
    }
    if (!counted) {
        return overflow();
    }
    if (const auto* quote = std::get_if<TopOfBookEvent>(&event.value())) {
        return observe_quote(*quote, instrument.quotes);
    }
    return std::nullopt;
}

json::Value DatasetQualityReport::to_json() const {
    json::Object instruments;
    json::Array without_valid_quote;
    for (const auto& [name, instrument] : instruments_) {
        const Quotes& quotes = instrument.quotes;
        if (quotes.valid_two_sided == 0) {
            without_valid_quote.emplace_back(name);
        }
        instruments.emplace_back(name, json::Object{
            {"by_event_type", event_type_counts_json(instrument.by_type)},
            {"event_time_regressions", decimal(instrument.event_time_regressions)},
            {"events", decimal(instrument.events)},
            {"maximum_receive_gap_ns", nullable_decimal(instrument.maximum_receive_gap_ns)},
            {"receive_timestamp_ns", json::Object{
                {"first", std::to_string(instrument.first_receive.nanoseconds())},
                {"last", std::to_string(instrument.last_receive.nanoseconds())},
            }},
            {"top_of_book", json::Object{
                {"ask_only", decimal(quotes.ask_only)},
                {"bid_only", decimal(quotes.bid_only)},
                {"crossed", decimal(quotes.crossed)},
                {"empty", decimal(quotes.empty)},
                {"locked", decimal(quotes.locked)},
                {"maximum_spread_raw", nullable_decimal(quotes.maximum_spread_raw)},
                {"minimum_spread_raw", nullable_decimal(quotes.minimum_spread_raw)},
                {"suspect", decimal(quotes.suspect)},
                {"two_sided", decimal(quotes.two_sided)},
                {"valid_two_sided", decimal(quotes.valid_two_sided)},
            }},
        });
    }
    json::Object flags;
    for (std::size_t index = 0; index < kQualityFlags.size(); ++index) {
        flags.emplace_back(quality_flag_name(kQualityFlags[index]), decimal(flag_counts_[index]));
    }
    flags.emplace_back("unflagged", decimal(unflagged_events_));

    return json::Value(json::Object{
        {"checks", json::Object{
            {"chronos_event_validation", "passed"},
            {"ingest_sequence", "contiguous"},
            {"receive_timestamp", "non_decreasing"},
            {"stream", "single"},
        }},
        {"events", json::Object{
            {"by_event_type", event_type_counts_json(by_type_)},
            {"total", decimal(events_)},
        }},
        {"instruments", std::move(instruments)},
        {"instruments_without_valid_two_sided_quote", std::move(without_valid_quote)},
        {"quality_flags", std::move(flags)},
        {"rules", json::Object{
            {"name", kQualityRulesName},
            {"version", kQualityRulesVersion},
        }},
        {"venue_sequence", json::Object{
            {"events_with_venue_sequence", decimal(events_with_venue_sequence_)},
            {"gap_detection", "not_performed"},
            {"reason",
             "venue sequences are provider- and channel-specific; Chronos checks "
             "only its own ingest-sequence continuity"},
        }},
    });
}

std::string DatasetQualityReport::serialize() const {
    return canonical_document(to_json());
}

}  // namespace chronos::datasets
