#include "chronos/features/top_of_book_features.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::features {
namespace {

using detail::WideInteger;

bool fits_int64(WideInteger value) {
    return value >= std::numeric_limits<std::int64_t>::min() &&
        value <= std::numeric_limits<std::int64_t>::max();
}

// numerator / denominator, rounded half away from zero, or why not.
std::variant<std::int64_t, Unavailability> divide(WideInteger numerator, WideInteger denominator) {
    if (denominator == 0) {
        return Unavailability::ZeroDenominator;
    }
    // A negative denominator only arises from raw values validation
    // forbids; normalizing its sign keeps rounding symmetric.
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    try {
        return detail::rounded_divide(numerator, denominator, RoundingMode::HalfAwayFromZero);
    } catch (const std::overflow_error&) {
        return Unavailability::ArithmeticOverflow;
    }
}

template <typename Value>
Feature<Value> as_feature(const std::variant<std::int64_t, Unavailability>& result) {
    if (const auto* raw = std::get_if<std::int64_t>(&result)) {
        return Value::from_raw(*raw);
    }
    return std::get<Unavailability>(result);
}

}  // namespace

std::string_view unavailability_name(Unavailability reason) noexcept {
    switch (reason) {
        case Unavailability::MissingBid:
            return "missing_bid";
        case Unavailability::MissingAsk:
            return "missing_ask";
        case Unavailability::MissingBothSides:
            return "missing_both_sides";
        case Unavailability::SuspectQuote:
            return "suspect_quote";
        case Unavailability::TradingHalted:
            return "trading_halted";
        case Unavailability::MarketClosed:
            return "market_closed";
        case Unavailability::ZeroDenominator:
            return "zero_denominator";
        case Unavailability::ArithmeticOverflow:
            return "arithmetic_overflow";
    }
    return "unknown";
}

std::optional<Unavailability> unavailability_from_name(std::string_view name) noexcept {
    for (const Unavailability reason : {
             Unavailability::MissingBid,
             Unavailability::MissingAsk,
             Unavailability::MissingBothSides,
             Unavailability::SuspectQuote,
             Unavailability::TradingHalted,
             Unavailability::MarketClosed,
             Unavailability::ZeroDenominator,
             Unavailability::ArithmeticOverflow,
         }) {
        if (name == unavailability_name(reason)) {
            return reason;
        }
    }
    return std::nullopt;
}

Feature<Price> spread(Price bid, Price ask) {
    const WideInteger difference = WideInteger{ask.raw()} - bid.raw();
    if (!fits_int64(difference)) {
        return Unavailability::ArithmeticOverflow;
    }
    return Price::from_raw(static_cast<std::int64_t>(difference));
}

Feature<Price> midpoint(Price bid, Price ask) {
    return as_feature<Price>(divide(WideInteger{bid.raw()} + ask.raw(), 2));
}

Feature<Ratio> imbalance(Quantity bid_size, Quantity ask_size) {
    const WideInteger difference = WideInteger{bid_size.raw()} - ask_size.raw();
    const WideInteger total = WideInteger{bid_size.raw()} + ask_size.raw();
    return as_feature<Ratio>(divide(difference * Ratio::scale(), total));
}

Feature<Price> microprice(Price bid, Quantity bid_size, Price ask, Quantity ask_size) {
    // Each product is below 2^126 in magnitude, but their sum can exceed
    // the 128-bit range for raw values validation forbids.
    WideInteger weighted_ask = 0;
    WideInteger weighted_bid = 0;
    WideInteger numerator = 0;
    if (__builtin_mul_overflow(WideInteger{ask.raw()}, WideInteger{bid_size.raw()}, &weighted_ask) ||
        __builtin_mul_overflow(WideInteger{bid.raw()}, WideInteger{ask_size.raw()}, &weighted_bid) ||
        __builtin_add_overflow(weighted_ask, weighted_bid, &numerator)) {
        return Unavailability::ArithmeticOverflow;
    }
    const WideInteger total = WideInteger{bid_size.raw()} + ask_size.raw();
    return as_feature<Price>(divide(numerator, total));
}

std::optional<TopOfBookFeatureRow> TopOfBookFeatureEngine::apply(const ValidatedMarketEvent& event) {
    const MarketEventHeader& header = market_event_header(event.value());
    if (const auto* status = std::get_if<MarketStatusEvent>(&event.value())) {
        trading_status_.insert_or_assign(header.instrument.value(), status->status);
        return std::nullopt;
    }
    const auto* quote = std::get_if<TopOfBookEvent>(&event.value());
    if (quote == nullptr) {
        return std::nullopt;
    }
    std::optional<TradingStatus> trading_status;
    if (const auto found = trading_status_.find(header.instrument.value()); found != trading_status_.end()) {
        trading_status = found->second;
    }

    TopOfBookFeatureRow row{
        .stream = header.stream,
        .instrument = header.instrument,
        .ingest_sequence = header.ingest_sequence,
        .event_timestamp = header.event_timestamp,
        .receive_timestamp = header.receive_timestamp,
        .available_at = header.receive_timestamp,
        .quality = header.quality,
        .trading_status = trading_status,
        .best_bid = Unavailability::MissingBid,
        .best_ask = Unavailability::MissingAsk,
        .bid_size = Unavailability::MissingBid,
        .ask_size = Unavailability::MissingAsk,
        .spread = Unavailability::MissingBothSides,
        .midpoint = Unavailability::MissingBothSides,
        .imbalance = Unavailability::MissingBothSides,
        .microprice = Unavailability::MissingBothSides,
    };
    // One rule, in order: an interrupted market, then a suspect quote, then
    // a missing side, make features unavailable; otherwise they are
    // computed.
    std::optional<Unavailability> everything;
    if (trading_status == TradingStatus::Halted) {
        everything = Unavailability::TradingHalted;
    } else if (trading_status == TradingStatus::Closed) {
        everything = Unavailability::MarketClosed;
    } else if (header.quality.is_suspect()) {
        everything = Unavailability::SuspectQuote;
    }
    if (everything) {
        row.best_bid = row.best_ask = *everything;
        row.bid_size = row.ask_size = *everything;
        row.spread = row.midpoint = row.microprice = *everything;
        row.imbalance = *everything;
        return row;
    }
    if (quote->bid) {
        row.best_bid = quote->bid->price;
        row.bid_size = quote->bid->quantity;
    }
    if (quote->ask) {
        row.best_ask = quote->ask->price;
        row.ask_size = quote->ask->quantity;
    }
    if (!quote->bid || !quote->ask) {
        const Unavailability missing = !quote->bid && !quote->ask
            ? Unavailability::MissingBothSides
            : (!quote->bid ? Unavailability::MissingBid : Unavailability::MissingAsk);
        row.spread = row.midpoint = row.microprice = missing;
        row.imbalance = missing;
        return row;
    }
    const BookLevel& bid = *quote->bid;
    const BookLevel& ask = *quote->ask;
    row.spread = spread(bid.price, ask.price);
    row.midpoint = midpoint(bid.price, ask.price);
    row.imbalance = imbalance(bid.quantity, ask.quantity);
    row.microprice = microprice(bid.price, bid.quantity, ask.price, ask.quantity);
    return row;
}

}  // namespace chronos::features
