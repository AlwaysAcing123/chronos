#include "chronos/strategy/inference_schedule.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/sha256.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::strategy {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

constexpr std::array<std::string_view, 3> kClassOrder = {"down", "flat", "up"};

// What the runner relies on a schedule's rows to mean. A schedule that
// defines them differently is a different contract and is refused. Only
// the class rule differs by version: a registered model's is its runtime's.
json::Value inference_definition_with_class(std::string_view class_rule) {
    return json::Value(json::Object{
        {"class", class_rule},
        {"confidence_ppb", "floor(1e9 x the predicted class's probability), exact from its binary64 value"},
        {"rows", "every feature row of the instrument available in the window, labelled or not"},
        {"unavailable", "a row whose model inputs are unavailable has no prediction from any model"},
        {"window", "start_at_ns <= available_at_ns, and available_at_ns < end_at_ns when the window has an end"},
    });
}

const json::Value& inference_definition(bool registered) {
    static const json::Value experiment = inference_definition_with_class(
        "the model's predicted class: the largest logit, ties to the earliest class in class_order"
    );
    static const json::Value version =
        inference_definition_with_class("the model version's own class rule, stated in model.runtime.class");
    return registered ? version : experiment;
}

const json::Value& model_feature_set() {
    static const json::Value feature_set = [] {
        const auto input = [](std::string_view name, std::string_view definition, std::int64_t scale) {
            return json::Value(json::Object{
                {"definition", definition},
                {"name", name},
                {"scale", std::to_string(scale)},
            });
        };
        return json::Value(json::Object{
            {"inputs", json::Array{
                input("spread", "feature spread", Price::scale()),
                input("imbalance", "feature imbalance", features::Ratio::scale()),
                input("microprice_minus_midpoint", "feature microprice minus feature midpoint, exact", Price::scale()),
                input("bid_size", "feature bid_size", Quantity::scale()),
                input("ask_size", "feature ask_size", Quantity::scale()),
            }},
            {"name", "top_of_book_baseline"},
            {"version", "1"},
        });
    }();
    return feature_set;
}

std::optional<PredictedClass> class_from_name(std::string_view name) {
    for (std::size_t index = 0; index < kClassOrder.size(); ++index) {
        if (kClassOrder[index] == name) {
            return static_cast<PredictedClass>(index);
        }
    }
    return std::nullopt;
}

void require_value(ObjectFields& fields, std::string_view key, const json::Value& expected, std::string message) {
    const json::Value* value = fields.value(key);
    if (fields.ok() && (value == nullptr || *value != expected)) {
        fields.fail(DatasetErrorCode::UnsupportedVersion, key, std::move(message));
    }
}

ExperimentScheduleModel experiment_model(ObjectFields& entry) {
    ExperimentScheduleModel model{
        .experiment_id = entry.sha256("experiment_id"),
        .name = entry.string("name"),
        .parameters_sha256 = entry.sha256("parameters_sha256"),
        .training_labels_available_through = {},
    };
    if (entry.ok() && model.name != "logistic" && model.name != "majority") {
        entry.fail(DatasetErrorCode::UnsupportedVersion, "name", "must be logistic or majority");
    }
    return model;
}

RegisteredScheduleModel registered_model(ObjectFields& entry) {
    RegisteredScheduleModel model{
        .model_version_id = entry.sha256("model_version_id"),
        .family = entry.string("family"),
        .payload_sha256 = entry.sha256("payload_sha256"),
        .information_cutoff = {},
        .cutoff_basis = {},
    };
    if (entry.ok() && model.family.empty()) {
        entry.fail(DatasetErrorCode::InvalidValue, "family", "must not be empty");
    }
    datasets::NestedFields runtime(entry, "runtime", {"class", "name", "version"});
    for (std::string_view key : {"class", "name", "version"}) {
        if (runtime.fields().string(key).empty() && runtime.fields().ok()) {
            runtime.fields().fail(DatasetErrorCode::InvalidValue, key, "must not be empty");
        }
    }
    runtime.finish();
    return model;
}

// The model block's instrument and feature set, which both versions share.
InstrumentId model_instrument(ObjectFields& entry) {
    require_value(entry, "model_feature_set", model_feature_set(), "is not top_of_book_baseline version 1");
    const std::string instrument = entry.string("instrument");
    if (entry.ok() && instrument.empty()) {
        entry.fail(DatasetErrorCode::InvalidValue, "instrument", "must not be empty");
    }
    return InstrumentId(instrument.empty() ? std::string("unset") : instrument);
}

// The stream a source block names.
void parse_stream(ObjectFields& entry, ScheduleManifest& manifest) {
    const std::string stream = entry.string("stream");
    if (entry.ok() && !stream.empty()) {
        manifest.stream = MarketDataStreamId(stream);
    } else if (entry.ok()) {
        entry.fail(DatasetErrorCode::InvalidValue, "stream", "must not be empty");
    }
}

// Versions 1 and 2: the verified split's sources, model, window (the split's
// validation or test window), and the model's training bound.
void parse_split_provenance(ObjectFields& fields, ScheduleManifest& manifest, bool registered) {
    Timestamp validation_start;
    Timestamp test_start;
    {
        datasets::NestedFields source(fields, "source", {
            "dataset_id", "feature_artifact_id", "feature_set", "features_sha256", "horizon_ns", "label_artifact_id",
            "label_set", "labels_sha256", "split_artifact_id", "split_set", "splits_sha256", "stream",
            "test_start_at_ns", "validation_start_at_ns",
        });
        ObjectFields& entry = source.fields();
        manifest.dataset_id = entry.sha256("dataset_id");
        manifest.feature_artifact_id = entry.sha256("feature_artifact_id");
        manifest.features_sha256 = entry.sha256("features_sha256");
        manifest.label_artifact_id = entry.sha256("label_artifact_id");
        manifest.split_artifact_id = entry.sha256("split_artifact_id");
        (void)entry.sha256("labels_sha256");
        (void)entry.sha256("splits_sha256");
        entry.constant("feature_set", "top_of_book_v1");
        entry.constant("label_set", "future_midpoint_return_v1");
        entry.constant("split_set", "chronological_purged_v1");
        manifest.horizon_ns = entry.signed_integer("horizon_ns");
        validation_start = entry.timestamp("validation_start_at_ns");
        test_start = entry.timestamp("test_start_at_ns");
        parse_stream(entry, manifest);
        source.finish();
    }
    if (registered) {
        datasets::NestedFields model(fields, "model", {
            "family", "instrument", "model_feature_set", "model_version_id", "payload_sha256", "runtime",
        });
        manifest.model = registered_model(model.fields());
        manifest.instrument = model_instrument(model.fields());
        model.finish();
    } else {
        datasets::NestedFields model(fields, "model", {
            "experiment_id", "instrument", "model_feature_set", "name", "parameters_sha256",
        });
        manifest.model = experiment_model(model.fields());
        manifest.instrument = model_instrument(model.fields());
        model.finish();
    }
    {
        datasets::NestedFields window(fields, "window", {"end_at_ns", "name", "start_at_ns"});
        ObjectFields& entry = window.fields();
        manifest.window = ScheduleWindow{
            .name = entry.string("name"),
            .start_at = entry.timestamp("start_at_ns"),
            .end_at = entry.nullable_timestamp("end_at_ns"),
        };
        if (entry.ok()) {
            const bool validation = manifest.window.name == "validation" &&
                manifest.window.start_at == validation_start && manifest.window.end_at == test_start;
            const bool test = manifest.window.name == "test" && manifest.window.start_at == test_start &&
                !manifest.window.end_at;
            if (!validation && !test) {
                entry.fail(
                    DatasetErrorCode::InvalidValue,
                    "name",
                    "must be the split's validation window [validation start, test start) or its test window "
                    "[test start, end of dataset)"
                );
            }
        }
        window.finish();
    }
    if (auto* experiment_model = std::get_if<ExperimentScheduleModel>(&manifest.model)) {
        datasets::NestedFields training(fields, "training", {"label_available_through_ns", "split"});
        training.fields().constant("split", "train");
        experiment_model->training_labels_available_through = training.fields().timestamp("label_available_through_ns");
        training.finish();
        if (fields.ok() && experiment_model->training_labels_available_through >= manifest.window.start_at) {
            fields.fail(
                DatasetErrorCode::LabelLeakage,
                "training",
                "a training label becomes available at or after the window begins"
            );
        }
    } else {
        auto& registered_model = std::get<RegisteredScheduleModel>(manifest.model);
        datasets::NestedFields training(fields, "training", {"basis", "information_cutoff_ns"});
        registered_model.cutoff_basis = training.fields().string("basis");
        registered_model.information_cutoff = training.fields().timestamp("information_cutoff_ns");
        if (training.fields().ok() && registered_model.cutoff_basis != "verified" &&
            registered_model.cutoff_basis != "declared") {
            training.fields().fail(DatasetErrorCode::InvalidValue, "basis", "must be verified or declared");
        }
        training.finish();
        if (fields.ok() && registered_model.information_cutoff > manifest.window.start_at) {
            fields.fail(
                DatasetErrorCode::LabelLeakage,
                "training",
                "the model's information cutoff is after the window begins"
            );
        }
    }
}

// Version 3: a walk-forward fold model on one whole evaluation recording.
// The source is the collection and the recording's dataset and features; it
// has no labels and no split. The window is explicit and has an end. The
// boundary may not be after the window's start, and every training
// prediction and label time is strictly before the boundary.
void parse_fold_provenance(ObjectFields& fields, ScheduleManifest& manifest) {
    FoldScheduleModel model;
    {
        datasets::NestedFields source(fields, "source", {
            "collection_id", "dataset_id", "feature_artifact_id", "feature_set", "features_sha256", "horizon_ns",
            "stream",
        });
        ObjectFields& entry = source.fields();
        model.collection_id = entry.sha256("collection_id");
        manifest.dataset_id = entry.sha256("dataset_id");
        manifest.feature_artifact_id = entry.sha256("feature_artifact_id");
        manifest.features_sha256 = entry.sha256("features_sha256");
        entry.constant("feature_set", "top_of_book_v1");
        manifest.horizon_ns = entry.signed_integer("horizon_ns");
        if (entry.ok() && manifest.horizon_ns <= 0) {
            entry.fail(DatasetErrorCode::InvalidValue, "horizon_ns", "must be positive");
        }
        parse_stream(entry, manifest);
        source.finish();
    }
    {
        datasets::NestedFields block(fields, "model", {
            "fold", "fold_model_id", "instrument", "model_feature_set", "name", "parameters_sha256", "plan_id",
        });
        ObjectFields& entry = block.fields();
        model.plan_id = entry.sha256("plan_id");
        model.fold_model_id = entry.sha256("fold_model_id");
        model.parameters_sha256 = entry.sha256("parameters_sha256");
        model.fold = entry.unsigned_integer("fold");
        model.name = entry.string("name");
        if (entry.ok() && (model.fold == 0 || (model.name != "logistic" && model.name != "majority"))) {
            entry.fail(DatasetErrorCode::InvalidValue, "name", "a fold is numbered from 1 and its model is logistic or majority");
        }
        manifest.instrument = model_instrument(entry);
        block.finish();
    }
    {
        datasets::NestedFields window(fields, "window", {"end_at_ns", "name", "start_at_ns"});
        ObjectFields& entry = window.fields();
        manifest.window = ScheduleWindow{
            .name = entry.string("name"),
            .start_at = entry.timestamp("start_at_ns"),
            .end_at = entry.timestamp("end_at_ns"),
        };
        if (entry.ok() && (manifest.window.name.empty() || *manifest.window.end_at <= manifest.window.start_at)) {
            entry.fail(DatasetErrorCode::InvalidValue, "window", "must be named and end after it starts");
        }
        window.finish();
    }
    {
        datasets::NestedFields training(fields, "training", {
            "information_boundary_ns", "label_available_through_ns", "prediction_through_ns",
        });
        ObjectFields& entry = training.fields();
        model.information_boundary = entry.timestamp("information_boundary_ns");
        model.training_labels_available_through = entry.timestamp("label_available_through_ns");
        model.training_predictions_through = entry.timestamp("prediction_through_ns");
        training.finish();
    }
    if (fields.ok() && (model.information_boundary > manifest.window.start_at ||
                        model.training_labels_available_through >= model.information_boundary ||
                        model.training_predictions_through >= model.information_boundary)) {
        fields.fail(
            DatasetErrorCode::LabelLeakage,
            "training",
            "the information boundary is after the window begins, or a training prediction or label is not "
            "strictly before it"
        );
    }
    manifest.model = std::move(model);
}

struct ParsedManifest {
    ScheduleManifest manifest;
    std::map<std::string, std::uint64_t> classes;
    std::map<std::string, std::uint64_t> unavailable;
};

std::variant<ParsedManifest, DatasetError> parse_manifest(std::string_view bytes) {
    auto document = datasets::parse_canonical_document(bytes, datasets::kArtifactManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&document)) {
        return std::move(*error);
    }
    const json::Value& value = std::get<json::Value>(document);
    ObjectFields fields(value, "manifest");
    const std::string version = datasets::require_name_and_one_of_versions(
        fields, "schema", kScheduleSchemaName,
        {kExperimentScheduleVersion, kRegisteredScheduleVersion, kFoldScheduleVersion}
    );
    if (!fields.ok()) {
        return *fields.error();
    }
    const bool registered = version == kRegisteredScheduleVersion;
    const bool fold = version == kFoldScheduleVersion;
    fields.require_keys({
        "artifact_id", "class_order", "counts", "environment", "files", "inference", "model", "row", "schema",
        "scope", "source", "training", "window",
    });
    datasets::require_name_version(fields, "row", kScheduleRowRecordName, kScheduleRowRecordVersion);
    require_value(fields, "inference", inference_definition(registered), "is not the inference definition this runner reads");
    if (fields.ok() && fields.string_array("class_order") != std::vector<std::string>(kClassOrder.begin(), kClassOrder.end())) {
        fields.fail(DatasetErrorCode::UnsupportedVersion, "class_order", "must be down, flat, up");
    }
    if (fields.ok() && fields.string("scope").empty()) {
        fields.fail(DatasetErrorCode::InvalidValue, "scope", "must not be empty");
    }
    if (const json::Object* environment = fields.object("environment"); environment != nullptr) {
        for (const auto& [name, entry] : *environment) {
            if (entry.if_string() == nullptr) {
                fields.fail(DatasetErrorCode::InvalidValue, "environment", "every entry must be a string");
                break;
            }
        }
    }
    ParsedManifest parsed{
        .manifest = ScheduleManifest{
            .artifact_id = fields.sha256("artifact_id"),
            .dataset_id = {},
            .stream = MarketDataStreamId("unset"),
            .feature_artifact_id = {},
            .features_sha256 = {},
            .label_artifact_id = {},
            .split_artifact_id = {},
            .horizon_ns = 0,
            .instrument = InstrumentId("unset"),
            .model = {},
            .window = {},
            .rows = 0,
            .predicted = 0,
            .schedule = {},
        },
        .classes = {},
        .unavailable = {},
    };
    ScheduleManifest& manifest = parsed.manifest;

    if (fold) {
        parse_fold_provenance(fields, manifest);
    } else {
        parse_split_provenance(fields, manifest, registered);
    }
    {
        datasets::NestedFields counts(fields, "counts", {"classes", "predicted", "rows", "unavailable"});
        ObjectFields& entry = counts.fields();
        manifest.rows = entry.unsigned_integer("rows");
        manifest.predicted = entry.unsigned_integer("predicted");
        {
            datasets::NestedFields classes(entry, "classes", {"down", "flat", "up"});
            for (std::string_view name : kClassOrder) {
                parsed.classes[std::string(name)] = classes.fields().unsigned_integer(name);
            }
            classes.finish();
        }
        parsed.unavailable = datasets::parse_count_map(entry, "unavailable");
        if (entry.ok() && manifest.rows == 0) {
            entry.fail(DatasetErrorCode::EmptyDataset, "rows", "a schedule has at least one row");
        }
        counts.finish();
    }
    {
        datasets::NestedFields files(fields, "files", {"schedule"});
        manifest.schedule = datasets::parse_file_digest(files.fields(), "schedule", kScheduleRowsFileName);
        files.finish();
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    json::Object body;
    for (const auto& member : *value.if_object()) {
        if (member.first != "artifact_id") {
            body.push_back(member);
        }
    }
    if (Sha256::hex(datasets::canonical_document(json::Value(std::move(body)))) != manifest.artifact_id) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "manifest.artifact_id",
            "does not match the manifest's content"
        );
    }
    return parsed;
}

std::variant<ScheduleRow, DatasetError> decode_row(
    std::string_view record,
    const ScheduleManifest& manifest,
    std::uint64_t line
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
    if (json::serialize(value) != record) {
        return datasets::dataset_error(DatasetErrorCode::NotCanonical, "row", "record is not in canonical form", line);
    }
    ObjectFields fields(value, "", line);
    fields.require_keys({"available_at_ns", "identity", "inputs", "prediction", "record"});
    datasets::require_name_version(fields, "record", kScheduleRowRecordName, kScheduleRowRecordVersion);
    ScheduleRow row{.ingest_sequence = 0, .available_at = fields.timestamp("available_at_ns"), .inputs = {}, .prediction = {}};
    {
        datasets::NestedFields identity(fields, "identity", {"ingest_sequence", "instrument", "stream"});
        row.ingest_sequence = identity.fields().unsigned_integer("ingest_sequence");
        if (identity.fields().ok() && (identity.fields().string("stream") != manifest.stream.value() ||
                                       identity.fields().string("instrument") != manifest.instrument.value())) {
            identity.fields().fail(
                DatasetErrorCode::StreamMismatch,
                "stream",
                "the row is not of the schedule's stream and instrument"
            );
        }
        identity.finish();
    }
    std::optional<std::string> unavailable;
    if (const json::Value* inputs = fields.value("inputs"); inputs != nullptr && fields.ok()) {
        ObjectFields entry(*inputs, fields.field("inputs"), line);
        const json::Object* object = inputs->if_object();
        if (object != nullptr && object->size() == 1 && object->front().first == "unavailable") {
            entry.require_keys({"unavailable"});
            unavailable = entry.string("unavailable");
            const auto reason = features::unavailability_from_name(*unavailable);
            if (entry.ok() && !reason) {
                entry.fail(DatasetErrorCode::InvalidValue, "unavailable", "is not a feature unavailability reason");
            }
            row.inputs = reason.value_or(features::Unavailability::MissingBothSides);
        } else {
            entry.require_keys({"raw"});
            const std::vector<std::string> raw = entry.string_array("raw");
            std::array<std::int64_t, kModelInputs> values{};
            if (entry.ok() && raw.size() != kModelInputs) {
                entry.fail(DatasetErrorCode::InvalidValue, "raw", "must hold the five model inputs");
            }
            for (std::size_t index = 0; entry.ok() && index < raw.size(); ++index) {
                const auto parsed_value = json::parse_canonical_signed(raw[index]);
                if (!parsed_value) {
                    entry.fail(DatasetErrorCode::InvalidValue, "raw", "must hold canonical signed integers");
                } else {
                    values[index] = *parsed_value;
                }
            }
            row.inputs = values;
        }
        fields.adopt(entry.error());
    }
    if (const json::Value* prediction = fields.value("prediction"); prediction != nullptr && fields.ok()) {
        ObjectFields entry(*prediction, fields.field("prediction"), line);
        if (unavailable) {
            entry.require_keys({"unavailable"});
            if (entry.ok() && entry.string("unavailable") != "feature_unavailable:" + *unavailable) {
                entry.fail(DatasetErrorCode::InvalidValue, "unavailable", "must name the unavailable model input");
            }
        } else {
            entry.require_keys({"class", "confidence_ppb", "probabilities"});
            const auto predicted = class_from_name(entry.string("class"));
            const std::uint64_t confidence = entry.unsigned_integer("confidence_ppb");
            const std::vector<std::string> probabilities = entry.string_array("probabilities");
            if (entry.ok() && (!predicted || confidence > kConfidenceScale || probabilities.size() != kClassOrder.size())) {
                entry.fail(
                    DatasetErrorCode::InvalidValue,
                    "class",
                    "must be a known class with a confidence of at most 1e9 and one probability per class"
                );
            }
            row.prediction = SchedulePrediction{
                .predicted_class = predicted.value_or(PredictedClass::Flat),
                .confidence_ppb = confidence,
            };
        }
        fields.adopt(entry.error());
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    if (!manifest.window.contains(row.available_at)) {
        return datasets::dataset_error(
            DatasetErrorCode::InvalidValue,
            "available_at_ns",
            "is outside the schedule's window",
            line
        );
    }
    return row;
}

}  // namespace

std::string_view predicted_class_name(PredictedClass value) noexcept {
    return kClassOrder[static_cast<std::size_t>(value)];
}

std::variant<ScheduleManifest, DatasetError> parse_schedule_manifest(std::string_view bytes) {
    auto parsed = parse_manifest(bytes);
    if (auto* error = std::get_if<DatasetError>(&parsed)) {
        return std::move(*error);
    }
    return std::get<ParsedManifest>(std::move(parsed)).manifest;
}

ModelInputs model_inputs(const features::TopOfBookFeatureRow& row) {
    // The order in which the first unavailable input is reported.
    for (const auto* reason : {
             std::get_if<features::Unavailability>(&row.spread),
             std::get_if<features::Unavailability>(&row.imbalance),
             std::get_if<features::Unavailability>(&row.microprice),
             std::get_if<features::Unavailability>(&row.midpoint),
             std::get_if<features::Unavailability>(&row.bid_size),
             std::get_if<features::Unavailability>(&row.ask_size),
         }) {
        if (reason != nullptr) {
            return *reason;
        }
    }
    std::int64_t microprice_minus_midpoint = 0;
    if (__builtin_sub_overflow(
            std::get<Price>(row.microprice).raw(),
            std::get<Price>(row.midpoint).raw(),
            &microprice_minus_midpoint
        )) {
        return features::Unavailability::ArithmeticOverflow;
    }
    return std::array<std::int64_t, kModelInputs>{
        std::get<Price>(row.spread).raw(),
        std::get<features::Ratio>(row.imbalance).raw(),
        microprice_minus_midpoint,
        std::get<Quantity>(row.bid_size).raw(),
        std::get<Quantity>(row.ask_size).raw(),
    };
}

class ScheduleReader::State final {
public:
    ScheduleManifest manifest;
    std::map<std::string, std::uint64_t> expected_classes;
    std::map<std::string, std::uint64_t> expected_unavailable;
    std::unique_ptr<datasets::DerivedArtifactRows> rows;
    std::map<std::string, std::uint64_t> classes;
    std::map<std::string, std::uint64_t> unavailable;
    std::uint64_t predicted{0};
    std::optional<std::uint64_t> previous_sequence;
    std::optional<Timestamp> previous_at;
};

ScheduleReader::ScheduleReader(std::unique_ptr<State> state) : state_(std::move(state)) {}
ScheduleReader::~ScheduleReader() = default;

const ScheduleManifest& ScheduleReader::manifest() const noexcept { return state_->manifest; }

std::variant<std::unique_ptr<ScheduleReader>, DatasetError> ScheduleReader::open(
    const std::filesystem::path& directory
) {
    auto files = datasets::open_derived_artifact(directory, kScheduleLayout);
    if (auto* error = std::get_if<DatasetError>(&files)) {
        return std::move(*error);
    }
    const auto& opened = std::get<datasets::DerivedArtifactFiles>(files);
    auto parsed = parse_manifest(opened.manifest);
    if (auto* error = std::get_if<DatasetError>(&parsed)) {
        return std::move(*error);
    }
    auto& manifest = std::get<ParsedManifest>(parsed);
    if (auto error = datasets::check_derived_artifact_manifest(
            opened, kScheduleLayout, manifest.manifest.artifact_id, manifest.manifest.schedule
        )) {
        return std::move(*error);
    }
    auto rows = datasets::DerivedArtifactRows::open(
        opened.directory, kScheduleLayout, manifest.manifest.rows, manifest.manifest.schedule
    );
    if (auto* error = std::get_if<DatasetError>(&rows)) {
        return std::move(*error);
    }
    auto state = std::make_unique<State>(State{
        .manifest = std::move(manifest.manifest),
        .expected_classes = std::move(manifest.classes),
        .expected_unavailable = std::move(manifest.unavailable),
        .rows = std::get<std::unique_ptr<datasets::DerivedArtifactRows>>(std::move(rows)),
        .classes = {},
        .unavailable = {},
        .predicted = 0,
        .previous_sequence = std::nullopt,
        .previous_at = std::nullopt,
    });
    for (std::string_view name : kClassOrder) {
        state->classes[std::string(name)] = 0;
    }
    return std::unique_ptr<ScheduleReader>(new ScheduleReader(std::move(state)));
}

std::variant<ScheduleRow, datasets::EndOfRows, DatasetError> ScheduleReader::next() {
    State& state = *state_;
    auto next = state.rows->next();
    if (auto* error = std::get_if<DatasetError>(&next)) {
        return std::move(*error);
    }
    if (std::holds_alternative<datasets::EndOfRows>(next)) {
        if (state.classes != state.expected_classes || state.unavailable != state.expected_unavailable ||
            state.predicted != state.manifest.predicted) {
            return datasets::dataset_error(
                DatasetErrorCode::SummaryMismatch,
                std::string(datasets::kArtifactManifestFileName),
                "the rows do not match the manifest's counts"
            );
        }
        return datasets::EndOfRows{};
    }
    const std::uint64_t line = state.rows->line_number();
    auto decoded = decode_row(std::get<std::string_view>(next), state.manifest, line);
    if (auto* error = std::get_if<DatasetError>(&decoded)) {
        return std::move(*error);
    }
    auto& row = std::get<ScheduleRow>(decoded);
    if ((state.previous_sequence && row.ingest_sequence <= *state.previous_sequence) ||
        (state.previous_at && row.available_at < *state.previous_at)) {
        return datasets::dataset_error(
            DatasetErrorCode::IngestSequenceMovedBackward,
            "ingest_sequence",
            "rows must follow ingest order with times that never decrease",
            line
        );
    }
    state.previous_sequence = row.ingest_sequence;
    state.previous_at = row.available_at;
    if (row.prediction) {
        ++state.predicted;
        ++state.classes[std::string(predicted_class_name(row.prediction->predicted_class))];
    } else {
        ++state.unavailable[
            "feature_unavailable:" +
            std::string(features::unavailability_name(std::get<features::Unavailability>(row.inputs)))
        ];
    }
    return std::move(row);
}

}  // namespace chronos::strategy
