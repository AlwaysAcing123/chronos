#include "chronos/datasets/market_event_codec.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/json_fields.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::datasets {
namespace {

namespace json = chronos::canonical_json;

json::Value decimal(std::int64_t value) { return json::Value(std::to_string(value)); }
json::Value decimal(std::uint64_t value) { return json::Value(std::to_string(value)); }

json::Value nullable_identifier(const auto& identifier) {
    return identifier ? json::Value(identifier->value()) : json::Value(nullptr);
}

json::Value quality_json(MarketDataQuality quality) {
    json::Array flags;
    for (const MarketDataQualityFlag flag : kQualityFlags) {
        if (quality.contains(flag)) {
            flags.emplace_back(quality_flag_name(flag));
        }
    }
    return json::Value(std::move(flags));
}

json::Value book_level_json(const std::optional<BookLevel>& level) {
    if (!level) {
        return json::Value(nullptr);
    }
    return json::Value(json::Object{
        {"price_raw", decimal(level->price.raw())},
        {"quantity_raw", decimal(level->quantity.raw())},
    });
}

std::string_view aggressor_side_name(AggressorSide side) {
    switch (side) {
        case AggressorSide::Buy:
            return "buy";
        case AggressorSide::Sell:
            return "sell";
        case AggressorSide::Unknown:
            return "unknown";
    }
    throw std::invalid_argument("unknown aggressor side");
}

json::Object header_members(const MarketEvent& event) {
    const MarketEventHeader& header = market_event_header(event);
    return json::Object{
        {"event_timestamp_ns", decimal(header.event_timestamp.nanoseconds())},
        {"ingest_sequence", decimal(header.ingest_sequence)},
        {"instrument", header.instrument.value()},
        {"quality", quality_json(header.quality)},
        {"receive_timestamp_ns", decimal(header.receive_timestamp.nanoseconds())},
        {"stream", header.stream.value()},
        {"type", event_type_name(event)},
        {"venue", nullable_identifier(header.venue)},
        {"venue_sequence", header.venue_sequence
            ? decimal(*header.venue_sequence)
            : json::Value(nullptr)},
        {"version", kMarketEventSchemaVersion},
    };
}

json::Value event_json(const MarketEvent& event) {
    json::Object members = header_members(event);
    std::visit(
        [&members](const auto& concrete) {
            using Event = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Event, TradeEvent>) {
                members.emplace_back("aggressor_side", aggressor_side_name(concrete.aggressor_side));
                members.emplace_back("price_raw", decimal(concrete.price.raw()));
                members.emplace_back("quantity_raw", decimal(concrete.quantity.raw()));
            } else if constexpr (std::is_same_v<Event, TopOfBookEvent>) {
                members.emplace_back("ask", book_level_json(concrete.ask));
                members.emplace_back("bid", book_level_json(concrete.bid));
            } else if constexpr (std::is_same_v<Event, MarketStatusEvent>) {
                members.emplace_back("reason", concrete.reason);
                members.emplace_back("status", trading_status_name(concrete.status));
            } else {
                static_assert(std::is_same_v<Event, InstrumentDefinitionEvent>);
                members.emplace_back("currency", concrete.currency.value());
                members.emplace_back("display_symbol", concrete.display_symbol);
                members.emplace_back("effective_from_ns", decimal(concrete.effective_from.nanoseconds()));
                members.emplace_back("effective_until_ns", concrete.effective_until
                    ? decimal(concrete.effective_until->nanoseconds())
                    : json::Value(nullptr));
                members.emplace_back(
                    "minimum_price_increment_raw",
                    decimal(concrete.minimum_price_increment.raw())
                );
                members.emplace_back(
                    "minimum_quantity_increment_raw",
                    decimal(concrete.minimum_quantity_increment.raw())
                );
            }
        },
        event
    );
    return json::Value(std::move(members));
}

template <typename Tag>
std::optional<Identifier<Tag>> identifier(ObjectFields& fields, std::string_view key) {
    std::string text = fields.string(key);
    if (!fields.ok()) {
        return std::nullopt;
    }
    if (text.empty()) {
        fields.fail(DatasetErrorCode::InvalidValue, key, "must not be empty");
        return std::nullopt;
    }
    return Identifier<Tag>(std::move(text));
}

template <typename Tag>
std::optional<Identifier<Tag>> nullable_identifier(ObjectFields& fields, std::string_view key) {
    std::optional<std::string> text = fields.nullable_string(key);
    if (!text || !fields.ok()) {
        return std::nullopt;
    }
    if (text->empty()) {
        fields.fail(DatasetErrorCode::InvalidValue, key, "must be null or not empty");
        return std::nullopt;
    }
    return Identifier<Tag>(std::move(*text));
}

MarketDataQuality decode_quality(ObjectFields& fields) {
    std::uint32_t bits = 0;
    for (const std::string& name : fields.string_array("quality")) {
        bool known = false;
        for (const MarketDataQualityFlag flag : kQualityFlags) {
            if (name == quality_flag_name(flag)) {
                bits |= static_cast<std::uint32_t>(flag);
                known = true;
            }
        }
        if (!known) {
            fields.fail(DatasetErrorCode::InvalidValue, "quality", "unknown quality flag " + name);
        }
    }
    return MarketDataQuality::from_raw(bits);
}

std::optional<BookLevel> decode_book_level(
    ObjectFields& fields,
    std::string_view key,
    std::optional<std::uint64_t> line
) {
    const json::Value* value = fields.value(key);
    if (value == nullptr || value->is_null()) {
        return std::nullopt;
    }
    ObjectFields level(*value, fields.field(key), line);
    level.require_keys({"price_raw", "quantity_raw"});
    const std::int64_t price = level.signed_integer("price_raw");
    const std::int64_t quantity = level.signed_integer("quantity_raw");
    if (!level.ok()) {
        fields.fail(level.error()->code, key, level.error()->message);
        return std::nullopt;
    }
    return BookLevel{
        .price = Price::from_raw(price),
        .quantity = Quantity::from_raw(quantity),
    };
}

std::optional<AggressorSide> decode_aggressor_side(ObjectFields& fields) {
    const std::string name = fields.string("aggressor_side");
    for (const AggressorSide side : {AggressorSide::Buy, AggressorSide::Sell, AggressorSide::Unknown}) {
        if (name == aggressor_side_name(side)) {
            return side;
        }
    }
    fields.fail(DatasetErrorCode::InvalidValue, "aggressor_side", "unknown aggressor side " + name);
    return std::nullopt;
}

std::optional<TradingStatus> decode_trading_status(ObjectFields& fields) {
    const std::string name = fields.string("status");
    for (const TradingStatus status : {
             TradingStatus::Unknown,
             TradingStatus::PreOpen,
             TradingStatus::Open,
             TradingStatus::Halted,
             TradingStatus::Closed,
         }) {
        if (name == trading_status_name(status)) {
            return status;
        }
    }
    fields.fail(DatasetErrorCode::InvalidValue, "status", "unknown trading status " + name);
    return std::nullopt;
}

constexpr std::array<std::string_view, 10> kHeaderKeys = {
    "event_timestamp_ns", "ingest_sequence", "instrument", "quality",
    "receive_timestamp_ns", "stream", "type", "venue", "venue_sequence", "version",
};

std::optional<MarketEvent> decode_event(
    const json::Value& value,
    std::optional<std::uint64_t> line,
    std::optional<DatasetError>& error
) {
    ObjectFields fields(value, "", line);
    // The version and type decide which fields are known, so they are
    // checked before the key set.
    if (fields.ok()) {
        const json::Value* version = nullptr;
        const json::Value* type = nullptr;
        for (const auto& member : *value.if_object()) {
            if (member.first == "version") {
                version = &member.second;
            } else if (member.first == "type") {
                type = &member.second;
            }
        }
        if (version == nullptr || version->if_string() == nullptr) {
            fields.fail(DatasetErrorCode::MissingField, "version", "is required and must be a string");
        } else if (*version->if_string() != kMarketEventSchemaVersion) {
            fields.fail(
                DatasetErrorCode::UnsupportedVersion,
                "version",
                "market-event record version " + *version->if_string() + " is not supported"
            );
        } else if (type == nullptr || type->if_string() == nullptr) {
            fields.fail(DatasetErrorCode::MissingField, "type", "is required and must be a string");
        }
    }
    if (!fields.ok()) {
        error = fields.error();
        return std::nullopt;
    }

    std::vector<std::string_view> keys(kHeaderKeys.begin(), kHeaderKeys.end());
    const std::string type = fields.string("type");
    std::vector<std::string_view> type_keys;
    if (type == "trade") {
        type_keys = {"aggressor_side", "price_raw", "quantity_raw"};
    } else if (type == "top_of_book") {
        type_keys = {"ask", "bid"};
    } else if (type == "market_status") {
        type_keys = {"reason", "status"};
    } else if (type == "instrument_definition") {
        type_keys = {
            "currency", "display_symbol", "effective_from_ns", "effective_until_ns",
            "minimum_price_increment_raw", "minimum_quantity_increment_raw",
        };
    } else {
        error = dataset_error(
            DatasetErrorCode::UnknownEventType,
            "type",
            "unknown market-event type " + type,
            line
        );
        return std::nullopt;
    }
    keys.insert(keys.end(), type_keys.begin(), type_keys.end());
    for (const auto& member : *value.if_object()) {
        if (std::find(keys.begin(), keys.end(), member.first) == keys.end()) {
            fields.fail(DatasetErrorCode::UnknownField, member.first, "is not a field of a " + type + " record");
            break;
        }
    }
    for (const std::string_view key : keys) {
        static_cast<void>(fields.value(key));
    }

    auto instrument = identifier<InstrumentIdTag>(fields, "instrument");
    auto stream = identifier<MarketDataStreamIdTag>(fields, "stream");
    auto venue = nullable_identifier<VenueIdTag>(fields, "venue");
    const Timestamp event_timestamp = fields.timestamp("event_timestamp_ns");
    const Timestamp receive_timestamp = fields.timestamp("receive_timestamp_ns");
    const std::uint64_t ingest_sequence = fields.unsigned_integer("ingest_sequence");
    const auto venue_sequence = fields.nullable_unsigned_integer("venue_sequence");
    const MarketDataQuality quality = decode_quality(fields);
    if (!fields.ok()) {
        error = fields.error();
        return std::nullopt;
    }
    MarketEventHeader header{
        .instrument = std::move(*instrument),
        .stream = std::move(*stream),
        .venue = std::move(venue),
        .event_timestamp = event_timestamp,
        .receive_timestamp = receive_timestamp,
        .ingest_sequence = ingest_sequence,
        .venue_sequence = venue_sequence,
        .quality = quality,
    };

    std::optional<MarketEvent> event;
    if (type == "trade") {
        const auto side = decode_aggressor_side(fields);
        const std::int64_t price = fields.signed_integer("price_raw");
        const std::int64_t quantity = fields.signed_integer("quantity_raw");
        if (fields.ok()) {
            event = TradeEvent{
                .header = std::move(header),
                .price = Price::from_raw(price),
                .quantity = Quantity::from_raw(quantity),
                .aggressor_side = *side,
            };
        }
    } else if (type == "top_of_book") {
        auto bid = decode_book_level(fields, "bid", line);
        auto ask = decode_book_level(fields, "ask", line);
        if (fields.ok()) {
            event = TopOfBookEvent{.header = std::move(header), .bid = bid, .ask = ask};
        }
    } else if (type == "market_status") {
        const auto status = decode_trading_status(fields);
        std::string reason = fields.string("reason");
        if (fields.ok()) {
            event = MarketStatusEvent{
                .header = std::move(header),
                .status = *status,
                .reason = std::move(reason),
            };
        }
    } else {
        auto currency = identifier<CurrencyCodeTag>(fields, "currency");
        std::string display_symbol = fields.string("display_symbol");
        const Timestamp effective_from = fields.timestamp("effective_from_ns");
        const auto effective_until = fields.nullable_timestamp("effective_until_ns");
        const std::int64_t price_increment = fields.signed_integer("minimum_price_increment_raw");
        const std::int64_t quantity_increment = fields.signed_integer("minimum_quantity_increment_raw");
        if (fields.ok()) {
            event = InstrumentDefinitionEvent{
                .header = std::move(header),
                .display_symbol = std::move(display_symbol),
                .currency = std::move(*currency),
                .minimum_price_increment = Price::from_raw(price_increment),
                .minimum_quantity_increment = Quantity::from_raw(quantity_increment),
                .effective_from = effective_from,
                .effective_until = effective_until,
            };
        }
    }
    if (!fields.ok()) {
        error = fields.error();
        return std::nullopt;
    }
    return event;
}

std::string describe(const MarketEventValidationFailure& failure) {
    std::string message = "event fails Chronos market-event validation:";
    for (const auto& validation : failure.errors) {
        message += " " + validation.field + " (" + validation.message + ");";
    }
    message.pop_back();
    return message;
}

}  // namespace

std::string_view quality_flag_name(MarketDataQualityFlag flag) noexcept {
    switch (flag) {
        case MarketDataQualityFlag::Snapshot:
            return "snapshot";
        case MarketDataQualityFlag::Replay:
            return "replay";
        case MarketDataQualityFlag::QuestionableReceiveTimestamp:
            return "questionable_receive_timestamp";
        case MarketDataQualityFlag::PotentiallyInvalidBook:
            return "potentially_invalid_book";
        case MarketDataQualityFlag::SourceSpecific:
            return "source_specific";
    }
    return "unknown";
}

std::string_view event_type_name(const MarketEvent& event) noexcept {
    return std::visit(
        [](const auto& concrete) -> std::string_view {
            using Event = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Event, TradeEvent>) {
                return "trade";
            } else if constexpr (std::is_same_v<Event, TopOfBookEvent>) {
                return "top_of_book";
            } else if constexpr (std::is_same_v<Event, MarketStatusEvent>) {
                return "market_status";
            } else {
                static_assert(std::is_same_v<Event, InstrumentDefinitionEvent>);
                return "instrument_definition";
            }
        },
        event
    );
}

std::string_view trading_status_name(TradingStatus status) noexcept {
    switch (status) {
        case TradingStatus::Unknown:
            return "unknown";
        case TradingStatus::PreOpen:
            return "pre_open";
        case TradingStatus::Open:
            return "open";
        case TradingStatus::Halted:
            return "halted";
        case TradingStatus::Closed:
            return "closed";
    }
    return "invalid";
}

std::variant<std::string, DatasetError> encode_market_event(
    const ValidatedMarketEvent& event
) {
    try {
        return json::serialize(event_json(event.value()));
    } catch (const std::invalid_argument&) {
        return dataset_error(
            DatasetErrorCode::UnrepresentableText,
            "event",
            "a text field is not valid UTF-8 and cannot be stored exactly"
        );
    }
}

std::variant<ValidatedMarketEvent, DatasetError> decode_market_event(
    std::string_view record,
    std::optional<std::uint64_t> line
) {
    auto parsed = json::parse(record);
    if (auto* error = std::get_if<json::ParseError>(&parsed)) {
        return dataset_error(
            DatasetErrorCode::MalformedJson,
            "event",
            "invalid JSON at byte " + std::to_string(error->offset) + ": " + error->message,
            line
        );
    }
    std::optional<DatasetError> error;
    auto event = decode_event(std::get<json::Value>(parsed), line, error);
    if (!event) {
        return std::move(*error);
    }
    auto validated = validate_market_event(std::move(*event));
    if (auto* failure = std::get_if<MarketEventValidationFailure>(&validated)) {
        return dataset_error(DatasetErrorCode::InvalidEvent, "event", describe(*failure), line);
    }
    auto& accepted = std::get<ValidatedMarketEvent>(validated);
    auto canonical = encode_market_event(accepted);
    if (auto* encode_error = std::get_if<DatasetError>(&canonical)) {
        encode_error->line = line;
        return std::move(*encode_error);
    }
    if (std::get<std::string>(canonical) != record) {
        return dataset_error(
            DatasetErrorCode::NotCanonical,
            "event",
            "record is not in canonical form",
            line
        );
    }
    return std::move(accepted);
}

}  // namespace chronos::datasets
