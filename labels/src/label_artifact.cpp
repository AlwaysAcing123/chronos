#include "chronos/labels/label_artifact.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/features/feature_artifact.hpp"
#include "chronos/sha256.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::labels {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

// The definition of version 1, with its one parameter. All of it is part
// of the artifact's identity.
json::Object configuration(std::int64_t horizon_ns) {
    return json::Object{
        {"direction", "sign_of_exact_return_without_deadband"},
        {"future_observation",
         "first_later_row_of_the_same_instrument_available_at_or_after_prediction_at_plus_horizon"},
        {"horizon_ns", std::to_string(horizon_ns)},
        {"label_available_at", "future_observation_available_at"},
        {"prediction_at", "feature_row_available_at"},
        {"return", "(future_midpoint - current_midpoint) / current_midpoint"},
        {"return_rounding", "half_away_from_zero"},
        {"return_scale", std::to_string(Return::scale())},
    };
}

json::Value manifest_body(const LabelArtifactManifest& manifest) {
    return json::Value(json::Object{
        {"configuration", configuration(manifest.horizon_ns)},
        {"counts", json::Object{
            {"directions", json::Object{
                {"down", std::to_string(manifest.directions.down)},
                {"flat", std::to_string(manifest.directions.flat)},
                {"up", std::to_string(manifest.directions.up)},
            }},
            {"rows", std::to_string(manifest.rows)},
            {"rows_by_instrument", datasets::count_map_json(manifest.rows_by_instrument)},
            {"unavailable", datasets::count_map_json(manifest.unavailable)},
            {"usable", std::to_string(usable_labels(manifest))},
        }},
        {"files", json::Object{
            {"labels", datasets::file_digest_json(kLabelRowsFileName, manifest.labels)},
        }},
        {"generator", datasets::name_version_json(kLabelGeneratorName, kLabelGeneratorVersion)},
        {"label_set", datasets::name_version_json(kFutureMidpointReturnName, kFutureMidpointReturnVersion)},
        {"row", datasets::name_version_json(kLabelRowRecordName, kLabelRowRecordVersion)},
        {"schema", datasets::name_version_json(kLabelArtifactSchemaName, kLabelArtifactSchemaVersion)},
        {"source", json::Object{
            {"dataset_id", manifest.dataset_id},
            {"feature_artifact_id", manifest.feature_artifact_id},
            {"feature_set", datasets::name_version_json(
                features::kTopOfBookFeatureSetName,
                features::kTopOfBookFeatureSetVersion
            )},
            {"features_sha256", manifest.features_sha256},
            {"stream", manifest.stream.value()},
        }},
    });
}

// The counts a sequence of rows produces, for writing a manifest and for
// checking one.
class LabelCounts final {
public:
    std::optional<DatasetError> add(const FutureMidpointReturnRow& row) {
        if (rows == std::numeric_limits<std::uint64_t>::max()) {
            return datasets::dataset_error(DatasetErrorCode::CounterOverflow, "counts", "a row count cannot be represented");
        }
        // Every other count is at most `rows`, so none can overflow first.
        ++rows;
        ++by_instrument[row.instrument.value()];
        if (const auto* value = std::get_if<FutureMidpointReturn>(&row.label)) {
            ++(value->direction == Direction::Down ? directions.down
                : value->direction == Direction::Flat ? directions.flat
                : directions.up);
        } else {
            ++unavailable[std::string(label_unavailability_name(std::get<LabelUnavailability>(row.label)))];
        }
        return std::nullopt;
    }

    [[nodiscard]] bool matches(const LabelArtifactManifest& manifest) const {
        return by_instrument == manifest.rows_by_instrument && directions == manifest.directions &&
            unavailable == manifest.unavailable;
    }

    std::uint64_t rows{0};
    std::map<std::string, std::uint64_t> by_instrument;
    DirectionCounts directions;
    std::map<std::string, std::uint64_t> unavailable;
};

std::variant<datasets::OpenedArtifact<LabelArtifactManifest>, DatasetError> open_artifact(
    const std::filesystem::path& directory
) {
    return datasets::open_artifact_manifest(
        directory,
        kLabelArtifactLayout,
        parse_label_manifest,
        &LabelArtifactManifest::labels
    );
}

LabelObservation observation_of(const FutureMidpointReturnRow& row) {
    return LabelObservation{
        .instrument = row.instrument,
        .ingest_sequence = row.ingest_sequence,
        .available_at = row.prediction_at,
        .midpoint = row.current_midpoint,
    };
}

struct LabelSource {
    features::FeatureArtifactManifest manifest;
    std::vector<LabelObservation> observations;
};

// Every row of a feature artifact, as the labeler sees it, once the whole
// artifact has verified.
std::variant<LabelSource, DatasetError> read_source(const std::filesystem::path& feature_artifact_directory) {
    auto opened = features::FeatureArtifactReader::open(feature_artifact_directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& reader = std::get<std::unique_ptr<features::FeatureArtifactReader>>(opened);
    std::vector<LabelObservation> observations;
    while (true) {
        auto next = reader->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            return LabelSource{.manifest = reader->manifest(), .observations = std::move(observations)};
        }
        auto& row = std::get<features::TopOfBookFeatureRow>(next);
        observations.push_back(LabelObservation{
            .instrument = std::move(row.instrument),
            .ingest_sequence = row.ingest_sequence,
            .available_at = row.available_at,
            .midpoint = row.midpoint,
        });
    }
}

// What a single row must satisfy on its own, beyond its canonical form.
std::optional<DatasetError> check_row(
    const FutureMidpointReturnRow& row,
    const LabelArtifactManifest& manifest,
    std::uint64_t line
) {
    if (row.stream != manifest.stream) {
        return datasets::dataset_error(DatasetErrorCode::StreamMismatch, "stream", "differs from the manifest", line);
    }
    if (row.horizon_ns != manifest.horizon_ns) {
        return datasets::dataset_error(DatasetErrorCode::InvalidValue, "horizon_ns", "differs from the manifest", line);
    }
    if (row.future) {
        std::int64_t end = 0;
        std::int64_t elapsed = 0;
        if (row.future->ingest_sequence <= row.ingest_sequence) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidValue,
                "future.ingest_sequence",
                "the future observation must come later in ingest order",
                line
            );
        }
        if (__builtin_add_overflow(row.prediction_at.nanoseconds(), row.horizon_ns, &end) ||
            row.future->available_at.nanoseconds() < end) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidValue,
                "label_available_at_ns",
                "the future observation is earlier than one horizon after the prediction",
                line
            );
        }
        if (__builtin_sub_overflow(row.future->available_at.nanoseconds(), row.prediction_at.nanoseconds(), &elapsed) ||
            elapsed != row.future->elapsed_ns) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidValue,
                "future.elapsed_ns",
                "must be the label's availability time minus the prediction time",
                line
            );
        }
    }
    if (row.label != label_of(row)) {
        return datasets::dataset_error(
            DatasetErrorCode::InvalidValue,
            "label",
            "differs from the label the row's midpoints and future observation determine",
            line
        );
    }
    return std::nullopt;
}

// The second pass: the definition applied to the artifact's own prediction
// times and current midpoints must select the same future observations and
// so give every row's exact bytes. The first pass cannot see a row whose
// future observation is plausible on its own but is not the one selected.
std::optional<DatasetError> check_rederived(
    const datasets::OpenedArtifact<LabelArtifactManifest>& artifact,
    const std::vector<LabelObservation>& observations
) {
    const LabelArtifactManifest& manifest = artifact.manifest;
    auto file = datasets::DerivedArtifactRows::open(artifact.directory, kLabelArtifactLayout, manifest.rows, manifest.labels);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    auto& rows = std::get<std::unique_ptr<datasets::DerivedArtifactRows>>(file);
    return label_future_midpoint_returns(
        manifest.stream,
        observations,
        manifest.horizon_ns,
        [&](const FutureMidpointReturnRow& row) -> std::optional<DatasetError> {
            auto next = rows->next();
            if (auto* error = std::get_if<DatasetError>(&next)) {
                return std::move(*error);
            }
            const auto* record = std::get_if<std::string_view>(&next);
            if (record == nullptr || *record != encode_label_row(row)) {
                return datasets::dataset_error(
                    DatasetErrorCode::InvalidValue,
                    "future",
                    "is not the future observation the definition selects from the artifact's own rows",
                    rows->line_number()
                );
            }
            return std::nullopt;
        }
    );
}

}  // namespace

std::uint64_t usable_labels(const LabelArtifactManifest& manifest) noexcept {
    return manifest.directions.down + manifest.directions.flat + manifest.directions.up;
}

std::string label_artifact_identity(const LabelArtifactManifest& manifest) {
    return Sha256::hex(json::serialize(manifest_body(manifest)));
}

std::string serialize_label_manifest(const LabelArtifactManifest& manifest) {
    json::Object members = *manifest_body(manifest).if_object();
    members.emplace_back("artifact_id", manifest.artifact_id);
    return datasets::canonical_document(json::Value(std::move(members)));
}

std::variant<LabelArtifactManifest, DatasetError> parse_label_manifest(std::string_view bytes) {
    auto document = datasets::parse_canonical_document(bytes, datasets::kArtifactManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&document)) {
        return std::move(*error);
    }
    ObjectFields fields(std::get<json::Value>(document), "manifest");
    if (fields.ok()) {
        datasets::require_name_version(fields, "schema", kLabelArtifactSchemaName, kLabelArtifactSchemaVersion);
        if (!fields.ok()) {
            return *fields.error();
        }
    }
    fields.require_keys({
        "artifact_id", "configuration", "counts", "files", "generator", "label_set", "row", "schema", "source",
    });
    datasets::require_name_version(fields, "label_set", kFutureMidpointReturnName, kFutureMidpointReturnVersion);
    datasets::require_name_version(fields, "row", kLabelRowRecordName, kLabelRowRecordVersion);
    datasets::require_name_version(fields, "generator", kLabelGeneratorName, kLabelGeneratorVersion);
    LabelArtifactManifest manifest{
        .artifact_id = fields.sha256("artifact_id"),
        .dataset_id = {},
        .feature_artifact_id = {},
        .features_sha256 = {},
        .stream = MarketDataStreamId("unset"),
        .horizon_ns = 0,
        .rows = 0,
        .rows_by_instrument = {},
        .directions = {},
        .unavailable = {},
        .labels = {},
    };
    // The horizon is the definition's one parameter; everything else in the
    // configuration is fixed by the version.
    if (const json::Value* config = fields.value("configuration")) {
        ObjectFields parameters(*config, fields.field("configuration"));
        const std::uint64_t horizon = parameters.unsigned_integer("horizon_ns");
        fields.adopt(parameters.error());
        if (fields.ok() && (horizon == 0 || horizon > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
            fields.fail(DatasetErrorCode::InvalidValue, "configuration", "horizon_ns must be a positive int64");
        }
        if (fields.ok()) {
            manifest.horizon_ns = static_cast<std::int64_t>(horizon);
            if (json::serialize(*config) != json::serialize(json::Value(configuration(manifest.horizon_ns)))) {
                fields.fail(DatasetErrorCode::UnsupportedVersion, "configuration", "is not the configuration of this label version");
            }
        }
    }
    {
        datasets::NestedFields source(fields, "source", {
            "dataset_id", "feature_artifact_id", "feature_set", "features_sha256", "stream",
        });
        manifest.dataset_id = source.fields().sha256("dataset_id");
        manifest.feature_artifact_id = source.fields().sha256("feature_artifact_id");
        manifest.features_sha256 = source.fields().sha256("features_sha256");
        datasets::require_name_version(
            source.fields(),
            "feature_set",
            features::kTopOfBookFeatureSetName,
            features::kTopOfBookFeatureSetVersion
        );
        std::string stream = source.fields().string("stream");
        if (source.fields().ok() && stream.empty()) {
            source.fields().fail(DatasetErrorCode::InvalidValue, "stream", "must not be empty");
        }
        source.finish();
        if (fields.ok()) {
            manifest.stream = MarketDataStreamId(std::move(stream));
        }
    }
    std::uint64_t usable = 0;
    {
        datasets::NestedFields counts(fields, "counts", {"directions", "rows", "rows_by_instrument", "unavailable", "usable"});
        {
            datasets::NestedFields directions(counts.fields(), "directions", {"down", "flat", "up"});
            manifest.directions.down = directions.fields().unsigned_integer("down");
            manifest.directions.flat = directions.fields().unsigned_integer("flat");
            manifest.directions.up = directions.fields().unsigned_integer("up");
            directions.finish();
        }
        manifest.rows = counts.fields().unsigned_integer("rows");
        manifest.rows_by_instrument = datasets::parse_count_map(counts.fields(), "rows_by_instrument");
        manifest.unavailable = datasets::parse_count_map(counts.fields(), "unavailable");
        usable = counts.fields().unsigned_integer("usable");
        for (const auto& [reason, count] : manifest.unavailable) {
            if (counts.fields().ok() && !label_unavailability_from_name(reason)) {
                counts.fields().fail(DatasetErrorCode::InvalidValue, "unavailable", "unknown label unavailability reason " + reason);
            }
        }
        counts.finish();
    }
    {
        datasets::NestedFields files(fields, "files", {"labels"});
        manifest.labels = datasets::parse_file_digest(files.fields(), "labels", kLabelRowsFileName);
        files.finish();
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    if (label_artifact_identity(manifest) != manifest.artifact_id) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "manifest.artifact_id",
            "does not match the manifest's content"
        );
    }

    std::uint64_t instruments = 0;
    std::uint64_t directions = 0;
    std::uint64_t accounted = 0;
    bool representable = true;
    for (const auto& entry : manifest.rows_by_instrument) {
        representable = representable && datasets::add_count(instruments, entry.second);
    }
    for (const std::uint64_t count : {manifest.directions.down, manifest.directions.flat, manifest.directions.up}) {
        representable = representable && datasets::add_count(directions, count);
    }
    accounted = directions;
    for (const auto& entry : manifest.unavailable) {
        representable = representable && datasets::add_count(accounted, entry.second);
    }
    if (!representable || instruments != manifest.rows || directions != usable || accounted != manifest.rows) {
        return datasets::dataset_error(
            DatasetErrorCode::SummaryMismatch,
            "manifest.counts",
            "rows by instrument, and usable plus unavailable rows, must each add up to counts.rows, and directions to "
            "counts.usable"
        );
    }
    if ((manifest.rows == 0) != (manifest.labels.size_bytes == 0)) {
        return datasets::dataset_error(
            DatasetErrorCode::SummaryMismatch,
            "manifest.files.labels.size_bytes",
            "must be zero exactly when there are no rows"
        );
    }
    return manifest;
}

std::variant<PublishedLabelArtifact, DatasetError> extract_future_midpoint_return_labels(
    const std::filesystem::path& feature_artifact_directory,
    const std::filesystem::path& output_root,
    std::int64_t horizon_ns
) {
    if (horizon_ns <= 0) {
        return datasets::dataset_error(
            DatasetErrorCode::InvalidArgument,
            "horizon_ns",
            "must be a positive number of nanoseconds"
        );
    }
    auto source = read_source(feature_artifact_directory);
    if (auto* error = std::get_if<DatasetError>(&source)) {
        return std::move(*error);
    }
    const auto& [features_manifest, observations] = std::get<LabelSource>(source);

    LabelCounts counts;
    return datasets::stage_and_publish<LabelArtifactManifest>(
        output_root,
        kLabelArtifactLayout,
        [&](datasets::StagedFile& rows) {
            return label_future_midpoint_returns(
                features_manifest.stream,
                observations,
                horizon_ns,
                [&](const FutureMidpointReturnRow& row) -> std::optional<DatasetError> {
                    if (auto error = datasets::write_record(rows, encode_label_row(row), "label", counts.rows + 1)) {
                        return error;
                    }
                    return counts.add(row);
                }
            );
        },
        [&](datasets::FileDigest digest) -> std::variant<LabelArtifactManifest, DatasetError> {
            LabelArtifactManifest manifest{
                .artifact_id = {},
                .dataset_id = features_manifest.dataset_id,
                .feature_artifact_id = features_manifest.artifact_id,
                .features_sha256 = features_manifest.features.sha256,
                .stream = features_manifest.stream,
                .horizon_ns = horizon_ns,
                .rows = counts.rows,
                .rows_by_instrument = counts.by_instrument,
                .directions = counts.directions,
                .unavailable = counts.unavailable,
                .labels = std::move(digest),
            };
            manifest.artifact_id = label_artifact_identity(manifest);
            return manifest;
        },
        serialize_label_manifest,
        verify_label_artifact
    );
}

class LabelArtifactReader::State final {
public:
    State(datasets::OpenedArtifact<LabelArtifactManifest> artifact, std::unique_ptr<datasets::DerivedArtifactRows> rows)
        : artifact_(std::move(artifact)), rows_(std::move(rows)) {}

    const LabelArtifactManifest& manifest() const noexcept { return artifact_.manifest; }
    const std::filesystem::path& directory() const noexcept { return artifact_.directory; }

    std::variant<FutureMidpointReturnRow, datasets::EndOfRows, DatasetError> next() {
        auto next = rows_->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            if (!counts_.matches(artifact_.manifest)) {
                return datasets::dataset_error(
                    DatasetErrorCode::SummaryMismatch,
                    std::string(datasets::kArtifactManifestFileName),
                    "the rows do not match the manifest's counts"
                );
            }
            if (auto error = check_rederived(artifact_, observations_)) {
                return std::move(*error);
            }
            return datasets::EndOfRows{};
        }
        const std::uint64_t line = rows_->line_number();
        auto decoded = decode_label_row(std::get<std::string_view>(next), line);
        if (auto* error = std::get_if<DatasetError>(&decoded)) {
            return std::move(*error);
        }
        auto& row = std::get<FutureMidpointReturnRow>(decoded);
        if (previous_sequence_ && row.ingest_sequence <= *previous_sequence_) {
            return datasets::dataset_error(
                DatasetErrorCode::IngestSequenceMovedBackward,
                "ingest_sequence",
                "rows must follow ingest order",
                line
            );
        }
        previous_sequence_ = row.ingest_sequence;
        if (auto error = check_row(row, artifact_.manifest, line)) {
            return std::move(*error);
        }
        if (auto error = counts_.add(row)) {
            return std::move(*error);
        }
        observations_.push_back(observation_of(row));
        return std::move(row);
    }

private:
    datasets::OpenedArtifact<LabelArtifactManifest> artifact_;
    std::unique_ptr<datasets::DerivedArtifactRows> rows_;
    LabelCounts counts_;
    std::optional<std::uint64_t> previous_sequence_;
    // Every row so far, compactly, for the second pass.
    std::vector<LabelObservation> observations_;
};

LabelArtifactReader::LabelArtifactReader(std::unique_ptr<State> state) : state_(std::move(state)) {}
LabelArtifactReader::~LabelArtifactReader() = default;

std::variant<std::unique_ptr<LabelArtifactReader>, DatasetError> LabelArtifactReader::open(
    const std::filesystem::path& directory
) {
    auto opened = open_artifact(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& artifact = std::get<datasets::OpenedArtifact<LabelArtifactManifest>>(opened);
    auto rows = datasets::DerivedArtifactRows::open(
        artifact.directory,
        kLabelArtifactLayout,
        artifact.manifest.rows,
        artifact.manifest.labels
    );
    if (auto* error = std::get_if<DatasetError>(&rows)) {
        return std::move(*error);
    }
    return std::unique_ptr<LabelArtifactReader>(new LabelArtifactReader(std::make_unique<State>(
        std::move(artifact),
        std::get<std::unique_ptr<datasets::DerivedArtifactRows>>(std::move(rows))
    )));
}

const LabelArtifactManifest& LabelArtifactReader::manifest() const noexcept { return state_->manifest(); }
const std::filesystem::path& LabelArtifactReader::directory() const noexcept { return state_->directory(); }

std::variant<FutureMidpointReturnRow, datasets::EndOfRows, DatasetError> LabelArtifactReader::next() {
    return state_->next();
}

std::variant<LabelArtifactManifest, DatasetError> verify_label_artifact(const std::filesystem::path& directory) {
    auto opened = LabelArtifactReader::open(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& reader = std::get<std::unique_ptr<LabelArtifactReader>>(opened);
    while (true) {
        auto next = reader->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            return reader->manifest();
        }
    }
}

std::optional<DatasetError> reproduce_label_artifact(
    const LabelArtifactManifest& manifest,
    const std::filesystem::path& feature_artifact_directory
) {
    auto source = read_source(feature_artifact_directory);
    if (auto* error = std::get_if<DatasetError>(&source)) {
        return std::move(*error);
    }
    const auto& [features_manifest, observations] = std::get<LabelSource>(source);
    if (features_manifest.artifact_id != manifest.feature_artifact_id ||
        features_manifest.features.sha256 != manifest.features_sha256 ||
        features_manifest.dataset_id != manifest.dataset_id || features_manifest.stream != manifest.stream) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "source",
            "the feature artifact is not the label artifact's source"
        );
    }
    Sha256 hash;
    std::uint64_t rows = 0;
    auto labelled = label_future_midpoint_returns(
        manifest.stream,
        observations,
        manifest.horizon_ns,
        [&](const FutureMidpointReturnRow& row) -> std::optional<DatasetError> {
            hash.update(encode_label_row(row) + "\n");
            ++rows;
            return std::nullopt;
        }
    );
    if (labelled) {
        return std::move(*labelled);
    }
    if (rows != manifest.rows || hash.hex_digest() != manifest.labels.sha256) {
        return datasets::dataset_error(
            DatasetErrorCode::ChecksumMismatch,
            std::string(kLabelRowsFileName),
            "recomputing the labels from the feature artifact gives different bytes"
        );
    }
    return std::nullopt;
}

std::variant<std::vector<LabelArtifactEntry>, DatasetError> list_label_artifacts(const std::filesystem::path& root) {
    return datasets::list_derived_artifacts(
        root,
        "output",
        kLabelArtifactLayout,
        parse_label_manifest,
        &LabelArtifactManifest::labels
    );
}

}  // namespace chronos::labels
