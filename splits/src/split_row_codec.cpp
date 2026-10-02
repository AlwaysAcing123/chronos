#include "chronos/canonical_json.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/splits/split_artifact.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::splits {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

}  // namespace

std::string encode_split_row(const SplitRow& row) {
    return json::serialize(json::Value(json::Object{
        {"exclusion", row.exclusion ? json::Value(exclusion_name(*row.exclusion)) : json::Value(nullptr)},
        {"ingest_sequence", std::to_string(row.ingest_sequence)},
        {"instrument", row.instrument.value()},
        {"label_available_at_ns",
         row.label_available_at ? json::Value(std::to_string(row.label_available_at->nanoseconds())) : json::Value(nullptr)},
        {"label_usable", row.label_usable},
        {"period", period_name(row.period)},
        {"prediction_at_ns", std::to_string(row.prediction_at.nanoseconds())},
        // The row's split, spelled out: its period, or excluded.
        {"split", row.exclusion ? std::string_view("excluded") : period_name(row.period)},
        {"split_set", kChronologicalSplitName},
        {"stream", row.stream.value()},
        {"version", kChronologicalSplitVersion},
    }));
}

std::variant<SplitRow, DatasetError> decode_split_row(std::string_view record, std::optional<std::uint64_t> line) {
    auto parsed = json::parse(record);
    if (auto* error = std::get_if<json::ParseError>(&parsed)) {
        return datasets::dataset_error(
            DatasetErrorCode::MalformedJson,
            "row",
            "invalid JSON at byte " + std::to_string(error->offset) + ": " + error->message,
            line
        );
    }
    ObjectFields fields(std::get<json::Value>(parsed), "", line);
    fields.require_keys({
        "exclusion", "ingest_sequence", "instrument", "label_available_at_ns", "label_usable", "period",
        "prediction_at_ns", "split", "split_set", "stream", "version",
    });
    fields.constant("split_set", kChronologicalSplitName);
    fields.constant("version", kChronologicalSplitVersion);
    std::string instrument = fields.string("instrument");
    std::string stream = fields.string("stream");
    const std::uint64_t ingest_sequence = fields.unsigned_integer("ingest_sequence");
    const Timestamp prediction_at = fields.timestamp("prediction_at_ns");
    const auto label_available_at = fields.nullable_timestamp("label_available_at_ns");
    const auto exclusion = fields.nullable_string("exclusion");
    const std::string period = fields.string("period");
    const std::string split = fields.string("split");
    bool label_usable = false;
    if (const json::Value* usable = fields.value("label_usable")) {
        if (const bool* flag = usable->if_bool()) {
            label_usable = *flag;
        } else {
            fields.fail(DatasetErrorCode::InvalidValue, "label_usable", "must be true or false");
        }
    }
    const auto known_period = period_from_name(period);
    if (fields.ok() && !known_period) {
        fields.fail(DatasetErrorCode::InvalidValue, "period", "unknown period " + period);
    }
    std::optional<SplitExclusion> known_exclusion;
    if (fields.ok() && exclusion) {
        known_exclusion = exclusion_from_name(*exclusion);
        if (!known_exclusion) {
            fields.fail(DatasetErrorCode::InvalidValue, "exclusion", "unknown exclusion reason " + *exclusion);
        }
    }
    if (fields.ok() && split != (known_exclusion ? std::string_view("excluded") : period_name(*known_period))) {
        fields.fail(DatasetErrorCode::InvalidValue, "split", "must be the row's period, or excluded when it has an exclusion");
    }
    if (fields.ok() && (instrument.empty() || stream.empty())) {
        fields.fail(DatasetErrorCode::InvalidValue, "instrument", "instrument and stream must not be empty");
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    SplitRow row{
        .stream = MarketDataStreamId(std::move(stream)),
        .instrument = InstrumentId(std::move(instrument)),
        .ingest_sequence = ingest_sequence,
        .prediction_at = prediction_at,
        .label_usable = label_usable,
        .label_available_at = label_available_at,
        .period = *known_period,
        .exclusion = known_exclusion,
    };
    if (encode_split_row(row) != record) {
        return datasets::dataset_error(DatasetErrorCode::NotCanonical, "row", "record is not in canonical form", line);
    }
    return row;
}

}  // namespace chronos::splits
