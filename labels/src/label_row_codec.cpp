#include "chronos/canonical_json.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/features/feature_artifact.hpp"
#include "chronos/labels/label_artifact.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::labels {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

json::Value label_json(const Label& label) {
    if (const auto* value = std::get_if<FutureMidpointReturn>(&label)) {
        return json::Value(json::Object{
            {"direction", direction_name(value->direction)},
            {"return_raw", std::to_string(value->value.raw())},
        });
    }
    return json::Value(json::Object{
        {"unavailable", label_unavailability_name(std::get<LabelUnavailability>(label))},
    });
}

Label decode_label(ObjectFields& fields, std::optional<std::uint64_t> line) {
    const Label kPlaceholder = LabelUnavailability::NoFutureObservation;
    const json::Value* value = fields.value("label");
    if (value == nullptr) {
        return kPlaceholder;
    }
    ObjectFields label(*value, fields.field("label"), line);
    const json::Object* members = value->if_object();
    if (members != nullptr && members->size() == 1) {
        label.require_keys({"unavailable"});
        const std::string reason = label.string("unavailable");
        fields.adopt(label.error());
        if (const auto known = label_unavailability_from_name(reason)) {
            return *known;
        }
        fields.fail(DatasetErrorCode::InvalidValue, "label", "unknown label unavailability reason " + reason);
        return kPlaceholder;
    }
    label.require_keys({"direction", "return_raw"});
    const std::string direction = label.string("direction");
    const std::int64_t raw = label.signed_integer("return_raw");
    fields.adopt(label.error());
    const auto known = direction_from_name(direction);
    if (fields.ok() && !known) {
        fields.fail(DatasetErrorCode::InvalidValue, "label", "unknown direction " + direction);
    }
    if (!fields.ok()) {
        return kPlaceholder;
    }
    return FutureMidpointReturn{.value = Return::from_raw(raw), .direction = *known};
}

}  // namespace

std::string encode_label_row(const FutureMidpointReturnRow& row) {
    json::Value future = nullptr;
    json::Value label_available_at = nullptr;
    if (row.future) {
        future = json::Value(json::Object{
            {"elapsed_ns", std::to_string(row.future->elapsed_ns)},
            {"ingest_sequence", std::to_string(row.future->ingest_sequence)},
            {"midpoint", features::feature_value_json(row.future->midpoint)},
        });
        label_available_at = json::Value(std::to_string(row.future->available_at.nanoseconds()));
    }
    return json::serialize(json::Value(json::Object{
        {"current_midpoint", features::feature_value_json(row.current_midpoint)},
        {"future", std::move(future)},
        {"horizon_ns", std::to_string(row.horizon_ns)},
        {"ingest_sequence", std::to_string(row.ingest_sequence)},
        {"instrument", row.instrument.value()},
        {"label", label_json(row.label)},
        {"label_available_at_ns", std::move(label_available_at)},
        {"label_set", kFutureMidpointReturnName},
        {"prediction_at_ns", std::to_string(row.prediction_at.nanoseconds())},
        {"stream", row.stream.value()},
        {"version", kFutureMidpointReturnVersion},
    }));
}

std::variant<FutureMidpointReturnRow, DatasetError> decode_label_row(
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
    ObjectFields fields(std::get<json::Value>(parsed), "", line);
    fields.require_keys({
        "current_midpoint", "future", "horizon_ns", "ingest_sequence", "instrument", "label",
        "label_available_at_ns", "label_set", "prediction_at_ns", "stream", "version",
    });
    fields.constant("label_set", kFutureMidpointReturnName);
    fields.constant("version", kFutureMidpointReturnVersion);
    std::string instrument = fields.string("instrument");
    std::string stream = fields.string("stream");
    const std::uint64_t ingest_sequence = fields.unsigned_integer("ingest_sequence");
    const Timestamp prediction_at = fields.timestamp("prediction_at_ns");
    const std::int64_t horizon_ns = fields.signed_integer("horizon_ns");
    auto current_midpoint = features::decode_feature_value<Price>(fields, "current_midpoint", line);
    const auto label_available_at = fields.nullable_timestamp("label_available_at_ns");

    std::optional<FutureObservation> future;
    const json::Value* future_value = fields.value("future");
    if (future_value != nullptr && !future_value->is_null()) {
        ObjectFields observed(*future_value, fields.field("future"), line);
        observed.require_keys({"elapsed_ns", "ingest_sequence", "midpoint"});
        future = FutureObservation{
            .ingest_sequence = observed.unsigned_integer("ingest_sequence"),
            .available_at = label_available_at.value_or(Timestamp{}),
            .elapsed_ns = observed.signed_integer("elapsed_ns"),
            .midpoint = features::decode_feature_value<Price>(observed, "midpoint", line),
        };
        fields.adopt(observed.error());
    }
    if (fields.ok() && future.has_value() != label_available_at.has_value()) {
        fields.fail(
            DatasetErrorCode::InvalidValue,
            "label_available_at_ns",
            "must be null exactly when there is no future observation"
        );
    }
    Label label = decode_label(fields, line);
    if (fields.ok() && (instrument.empty() || stream.empty())) {
        fields.fail(DatasetErrorCode::InvalidValue, "instrument", "instrument and stream must not be empty");
    }
    if (!fields.ok()) {
        return *fields.error();
    }

    FutureMidpointReturnRow row{
        .stream = MarketDataStreamId(std::move(stream)),
        .instrument = InstrumentId(std::move(instrument)),
        .ingest_sequence = ingest_sequence,
        .prediction_at = prediction_at,
        .horizon_ns = horizon_ns,
        .current_midpoint = current_midpoint,
        .future = future,
        .label = label,
    };
    if (encode_label_row(row) != record) {
        return datasets::dataset_error(DatasetErrorCode::NotCanonical, "row", "record is not in canonical form", line);
    }
    return row;
}

}  // namespace chronos::labels
