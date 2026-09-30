#include "chronos/canonical_json.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/features/feature_artifact.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::features {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

constexpr std::array<TradingStatus, 5> kStatuses = {
    TradingStatus::Unknown, TradingStatus::PreOpen, TradingStatus::Open,
    TradingStatus::Halted, TradingStatus::Closed,
};

template <typename Value>
json::Value feature_json(const Feature<Value>& feature) {
    if (const auto* value = std::get_if<Value>(&feature)) {
        return json::Value(json::Object{{"raw", std::to_string(value->raw())}});
    }
    return json::Value(json::Object{
        {"unavailable", unavailability_name(std::get<Unavailability>(feature))},
    });
}

std::string_view status_name(const std::optional<TradingStatus>& status) {
    return status ? datasets::trading_status_name(*status) : "not_reported";
}

template <typename Value>
Feature<Value> decode_feature(ObjectFields& features, std::string_view key, std::optional<std::uint64_t> line) {
    const json::Value* value = features.value(key);
    if (value == nullptr) {
        return Unavailability::MissingBothSides;
    }
    ObjectFields feature(*value, features.field(key), line);
    const json::Object* members = value->if_object();
    if (members != nullptr && members->size() == 1 && members->front().first == "raw") {
        const std::int64_t raw = feature.signed_integer("raw");
        features.adopt(feature.error());
        return Value::from_raw(raw);
    }
    feature.require_keys({"unavailable"});
    const std::string reason = feature.string("unavailable");
    features.adopt(feature.error());
    if (const auto known = unavailability_from_name(reason)) {
        return *known;
    }
    features.fail(DatasetErrorCode::InvalidValue, key, "unknown unavailability reason " + reason);
    return Unavailability::MissingBothSides;
}

}  // namespace

std::string encode_feature_row(const TopOfBookFeatureRow& row) {
    json::Array flags;
    for (const MarketDataQualityFlag flag : datasets::kQualityFlags) {
        if (row.quality.contains(flag)) {
            flags.emplace_back(datasets::quality_flag_name(flag));
        }
    }
    return json::serialize(json::Value(json::Object{
        {"available_at_ns", std::to_string(row.available_at.nanoseconds())},
        {"event_timestamp_ns", std::to_string(row.event_timestamp.nanoseconds())},
        {"feature_set", kTopOfBookFeatureSetName},
        {"features", json::Object{
            {"ask_size", feature_json(row.ask_size)},
            {"best_ask", feature_json(row.best_ask)},
            {"best_bid", feature_json(row.best_bid)},
            {"bid_size", feature_json(row.bid_size)},
            {"imbalance", feature_json(row.imbalance)},
            {"microprice", feature_json(row.microprice)},
            {"midpoint", feature_json(row.midpoint)},
            {"spread", feature_json(row.spread)},
        }},
        {"ingest_sequence", std::to_string(row.ingest_sequence)},
        {"instrument", row.instrument.value()},
        {"quality", std::move(flags)},
        {"receive_timestamp_ns", std::to_string(row.receive_timestamp.nanoseconds())},
        {"stream", row.stream.value()},
        {"trading_status", status_name(row.trading_status)},
        {"version", kTopOfBookFeatureSetVersion},
    }));
}

std::variant<TopOfBookFeatureRow, DatasetError> decode_feature_row(
    std::string_view record,
    std::optional<std::uint64_t> line
) {
    auto parsed = json::parse(record);
    if (auto* error = std::get_if<json::ParseError>(&parsed)) {
        return datasets::dataset_error(
            DatasetErrorCode::MalformedJson,
            "row",
            "invalid JSON at byte " + std::to_string(error->offset) + ": " + error->message,
            line
        );
    }
    const json::Value& value = std::get<json::Value>(parsed);
    ObjectFields fields(value, "", line);
    fields.require_keys({
        "available_at_ns", "event_timestamp_ns", "feature_set", "features", "ingest_sequence",
        "instrument", "quality", "receive_timestamp_ns", "stream", "trading_status", "version",
    });
    fields.constant("feature_set", kTopOfBookFeatureSetName);
    fields.constant("version", kTopOfBookFeatureSetVersion);
    std::string instrument = fields.string("instrument");
    std::string stream = fields.string("stream");
    const Timestamp available_at = fields.timestamp("available_at_ns");
    const Timestamp event_timestamp = fields.timestamp("event_timestamp_ns");
    const Timestamp receive_timestamp = fields.timestamp("receive_timestamp_ns");
    const std::uint64_t ingest_sequence = fields.unsigned_integer("ingest_sequence");
    std::uint32_t quality_bits = 0;
    for (const std::string& name : fields.string_array("quality")) {
        bool known = false;
        for (const MarketDataQualityFlag flag : datasets::kQualityFlags) {
            if (name == datasets::quality_flag_name(flag)) {
                quality_bits |= static_cast<std::uint32_t>(flag);
                known = true;
            }
        }
        if (!known) {
            fields.fail(DatasetErrorCode::InvalidValue, "quality", "unknown quality flag " + name);
        }
    }
    std::optional<TradingStatus> trading_status;
    const std::string status = fields.string("trading_status");
    if (fields.ok() && status != "not_reported") {
        for (const TradingStatus known : kStatuses) {
            if (status == datasets::trading_status_name(known)) {
                trading_status = known;
            }
        }
        if (!trading_status) {
            fields.fail(DatasetErrorCode::InvalidValue, "trading_status", "unknown trading status " + status);
        }
    }
    if (fields.ok() && (instrument.empty() || stream.empty())) {
        fields.fail(DatasetErrorCode::InvalidValue, "instrument", "instrument and stream must not be empty");
    }

    const json::Value* features_value = fields.value("features");
    const json::Value empty_features{json::Object{}};
    ObjectFields features(features_value ? *features_value : empty_features, "features", line);
    features.require_keys({
        "ask_size", "best_ask", "best_bid", "bid_size", "imbalance", "microprice", "midpoint", "spread",
    });
    auto best_bid = decode_feature<Price>(features, "best_bid", line);
    auto best_ask = decode_feature<Price>(features, "best_ask", line);
    auto bid_size = decode_feature<Quantity>(features, "bid_size", line);
    auto ask_size = decode_feature<Quantity>(features, "ask_size", line);
    auto spread_value = decode_feature<Price>(features, "spread", line);
    auto midpoint_value = decode_feature<Price>(features, "midpoint", line);
    auto imbalance_value = decode_feature<Ratio>(features, "imbalance", line);
    auto microprice_value = decode_feature<Price>(features, "microprice", line);
    fields.adopt(features.error());
    if (!fields.ok()) {
        return *fields.error();
    }

    TopOfBookFeatureRow row{
        .stream = MarketDataStreamId(std::move(stream)),
        .instrument = InstrumentId(std::move(instrument)),
        .ingest_sequence = ingest_sequence,
        .event_timestamp = event_timestamp,
        .receive_timestamp = receive_timestamp,
        .available_at = available_at,
        .quality = MarketDataQuality::from_raw(quality_bits),
        .trading_status = trading_status,
        .best_bid = best_bid,
        .best_ask = best_ask,
        .bid_size = bid_size,
        .ask_size = ask_size,
        .spread = spread_value,
        .midpoint = midpoint_value,
        .imbalance = imbalance_value,
        .microprice = microprice_value,
    };
    if (encode_feature_row(row) != record) {
        return datasets::dataset_error(DatasetErrorCode::NotCanonical, "row", "record is not in canonical form", line);
    }
    return row;
}

}  // namespace chronos::features
