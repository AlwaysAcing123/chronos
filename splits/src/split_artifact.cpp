#include "chronos/splits/split_artifact.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/labels/label_artifact.hpp"
#include "chronos/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::splits {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

std::string nanoseconds(Timestamp timestamp) { return std::to_string(timestamp.nanoseconds()); }

// The definition of version 1, with its one parameter: the percentages.
json::Object configuration(const SplitPercentages& percentages) {
    return json::Object{
        {"boundaries", "floor(rows * cumulative_percentage / 100)"},
        {"boundary_ties", "a_boundary_moves_later_past_rows_sharing_the_previous_rows_prediction_at"},
        {"order", "label_rows_in_ingest_order_with_prediction_at_never_decreasing"},
        {"percentages", json::Object{
            {"test", std::to_string(percentages.test)},
            {"train", std::to_string(percentages.train)},
            {"validation", std::to_string(percentages.validation)},
        }},
        {"purge", "a_train_or_validation_label_must_be_available_strictly_before_the_next_period_begins"},
        {"usable", "label_has_a_value"},
    };
}

json::Value by_period_json(const auto& values, const auto& to_json) {
    return json::Value(json::Object{
        {"test", to_json(values.test)},
        {"train", to_json(values.train)},
        {"validation", to_json(values.validation)},
    });
}

json::Value period_json(const PeriodSummary& period) {
    return json::Value(json::Object{
        {"first_prediction_at_ns", nanoseconds(period.first_prediction_at)},
        {"last_prediction_at_ns", nanoseconds(period.last_prediction_at)},
        {"rows", std::to_string(period.rows)},
    });
}

json::Value usable_json(const UsableSummary& usable) {
    return json::Value(json::Object{
        {"first_prediction_at_ns", nanoseconds(usable.first_prediction_at)},
        {"last_prediction_at_ns", nanoseconds(usable.last_prediction_at)},
        {"max_label_available_at_ns", nanoseconds(usable.max_label_available_at)},
        {"rows", std::to_string(usable.rows)},
    });
}

json::Value manifest_body(const SplitArtifactManifest& manifest) {
    return json::Value(json::Object{
        {"boundaries", json::Object{
            {"test_start", json::Object{
                {"prediction_at_ns", nanoseconds(manifest.boundaries.test_start_at)},
                {"row", std::to_string(manifest.boundaries.test_start)},
            }},
            {"validation_start", json::Object{
                {"prediction_at_ns", nanoseconds(manifest.boundaries.validation_start_at)},
                {"row", std::to_string(manifest.boundaries.validation_start)},
            }},
        }},
        {"configuration", configuration(manifest.percentages)},
        {"counts", json::Object{
            {"excluded", datasets::count_map_json(manifest.excluded)},
            {"rows", std::to_string(manifest.rows)},
        }},
        {"files", json::Object{
            {"splits", datasets::file_digest_json(kSplitRowsFileName, manifest.splits)},
        }},
        {"generator", datasets::name_version_json(kSplitGeneratorName, kSplitGeneratorVersion)},
        {"periods", by_period_json(manifest.periods, period_json)},
        {"row", datasets::name_version_json(kSplitRowRecordName, kSplitRowRecordVersion)},
        {"schema", datasets::name_version_json(kSplitArtifactSchemaName, kSplitArtifactSchemaVersion)},
        {"source", json::Object{
            {"dataset_id", manifest.dataset_id},
            {"feature_artifact_id", manifest.feature_artifact_id},
            {"horizon_ns", std::to_string(manifest.horizon_ns)},
            {"label_artifact_id", manifest.label_artifact_id},
            {"label_set", datasets::name_version_json(labels::kFutureMidpointReturnName, labels::kFutureMidpointReturnVersion)},
            {"labels_sha256", manifest.labels_sha256},
            {"stream", manifest.stream.value()},
        }},
        {"split_set", datasets::name_version_json(kChronologicalSplitName, kChronologicalSplitVersion)},
        {"usable", by_period_json(manifest.usable, usable_json)},
    });
}

// The summaries a sequence of rows produces, for writing a manifest and for
// checking one. Rows arrive in order, so the first row of a period or split
// sets its first time and every row its last.
class SplitSummaries final {
public:
    void add(const SplitRow& row) {
        ++rows;
        PeriodSummary& period = periods[row.period];
        if (period.rows++ == 0) {
            period.first_prediction_at = row.prediction_at;
        }
        period.last_prediction_at = row.prediction_at;
        if (row.exclusion) {
            ++excluded[std::string(exclusion_name(*row.exclusion))];
            return;
        }
        UsableSummary& split = usable[row.period];
        if (split.rows++ == 0) {
            split.first_prediction_at = row.prediction_at;
            split.max_label_available_at = *row.label_available_at;
        }
        split.last_prediction_at = row.prediction_at;
        split.max_label_available_at = std::max(split.max_label_available_at, *row.label_available_at);
    }

    [[nodiscard]] bool matches(const SplitArtifactManifest& manifest) const {
        return periods == manifest.periods && usable == manifest.usable && excluded == manifest.excluded;
    }

    std::uint64_t rows{0};
    ByPeriod<PeriodSummary> periods;
    ByPeriod<UsableSummary> usable;
    std::map<std::string, std::uint64_t> excluded;
};

PeriodSummary parse_period(ObjectFields& parent, std::string_view key) {
    datasets::NestedFields fields(parent, key, {"first_prediction_at_ns", "last_prediction_at_ns", "rows"});
    PeriodSummary summary{
        .rows = fields.fields().unsigned_integer("rows"),
        .first_prediction_at = fields.fields().timestamp("first_prediction_at_ns"),
        .last_prediction_at = fields.fields().timestamp("last_prediction_at_ns"),
    };
    fields.finish();
    return summary;
}

UsableSummary parse_usable(ObjectFields& parent, std::string_view key) {
    datasets::NestedFields fields(parent, key, {
        "first_prediction_at_ns", "last_prediction_at_ns", "max_label_available_at_ns", "rows",
    });
    UsableSummary summary{
        .rows = fields.fields().unsigned_integer("rows"),
        .first_prediction_at = fields.fields().timestamp("first_prediction_at_ns"),
        .last_prediction_at = fields.fields().timestamp("last_prediction_at_ns"),
        .max_label_available_at = fields.fields().timestamp("max_label_available_at_ns"),
    };
    fields.finish();
    return summary;
}

template <typename Value, typename Parse>
ByPeriod<Value> parse_by_period(ObjectFields& parent, std::string_view key, Parse parse) {
    datasets::NestedFields fields(parent, key, {"test", "train", "validation"});
    ByPeriod<Value> values;
    for (const SplitPeriod period : kSplitPeriods) {
        values[period] = parse(fields.fields(), period_name(period));
    }
    fields.finish();
    return values;
}

// The invariants every split manifest must keep, whatever its rows say.
std::optional<DatasetError> check_summaries(const SplitArtifactManifest& manifest) {
    const auto mismatch = [](std::string field, std::string message) {
        return datasets::dataset_error(DatasetErrorCode::SummaryMismatch, "manifest." + field, std::move(message));
    };
    if (auto error = check_percentages(manifest.percentages)) {
        return datasets::dataset_error(DatasetErrorCode::InvalidValue, "manifest.configuration.percentages", error->message);
    }
    const SplitBoundaries& boundaries = manifest.boundaries;
    if (boundaries.validation_start == 0 || boundaries.test_start <= boundaries.validation_start ||
        boundaries.test_start >= manifest.rows) {
        return mismatch("boundaries", "every period must hold at least one row");
    }
    if (manifest.periods.train.rows != boundaries.validation_start ||
        manifest.periods.validation.rows != boundaries.test_start - boundaries.validation_start ||
        manifest.periods.test.rows != manifest.rows - boundaries.test_start) {
        return mismatch("periods", "period rows must match the boundaries");
    }
    for (const SplitPeriod period : kSplitPeriods) {
        const PeriodSummary& rows = manifest.periods[period];
        const UsableSummary& usable = manifest.usable[period];
        const std::string name(period_name(period));
        if (rows.first_prediction_at > rows.last_prediction_at) {
            return mismatch("periods." + name, "first prediction must not be after the last");
        }
        if (usable.rows == 0 || usable.rows > rows.rows || usable.first_prediction_at < rows.first_prediction_at ||
            usable.first_prediction_at > usable.last_prediction_at || usable.last_prediction_at > rows.last_prediction_at) {
            return mismatch("usable." + name, "must be a nonempty part of its period");
        }
    }
    if (manifest.periods.validation.first_prediction_at != boundaries.validation_start_at ||
        manifest.periods.test.first_prediction_at != boundaries.test_start_at ||
        manifest.periods.train.last_prediction_at >= boundaries.validation_start_at ||
        manifest.periods.validation.last_prediction_at >= boundaries.test_start_at) {
        return mismatch("periods", "periods must follow one another in time, never sharing a prediction time");
    }
    if (manifest.usable.train.max_label_available_at >= boundaries.validation_start_at) {
        return datasets::dataset_error(
            DatasetErrorCode::LabelLeakage,
            "manifest.usable.train.max_label_available_at_ns",
            "a usable training label is known at or after validation begins"
        );
    }
    if (manifest.usable.validation.max_label_available_at >= boundaries.test_start_at) {
        return datasets::dataset_error(
            DatasetErrorCode::LabelLeakage,
            "manifest.usable.validation.max_label_available_at_ns",
            "a usable validation label is known at or after test begins"
        );
    }
    std::uint64_t accounted = 0;
    bool representable = true;
    for (const SplitPeriod period : kSplitPeriods) {
        representable = representable && datasets::add_count(accounted, manifest.usable[period].rows);
    }
    for (const auto& [reason, count] : manifest.excluded) {
        if (!exclusion_from_name(reason)) {
            return datasets::dataset_error(DatasetErrorCode::InvalidValue, "manifest.counts.excluded", "unknown exclusion " + reason);
        }
        representable = representable && datasets::add_count(accounted, count);
    }
    if (!representable || accounted != manifest.rows) {
        return mismatch("counts", "usable and excluded rows must add up to counts.rows");
    }
    return std::nullopt;
}

std::variant<datasets::OpenedArtifact<SplitArtifactManifest>, DatasetError> open_artifact(
    const std::filesystem::path& directory
) {
    return datasets::open_artifact_manifest(
        directory,
        kSplitArtifactLayout,
        parse_split_manifest,
        &SplitArtifactManifest::splits
    );
}

struct SplitSource {
    labels::LabelArtifactManifest manifest;
    std::vector<SplitInput> inputs;
};

// Every row of a label artifact, as the splitter sees it, once the whole
// artifact has verified.
std::variant<SplitSource, DatasetError> read_source(const std::filesystem::path& label_artifact_directory) {
    auto opened = labels::LabelArtifactReader::open(label_artifact_directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& reader = std::get<std::unique_ptr<labels::LabelArtifactReader>>(opened);
    std::vector<SplitInput> inputs;
    while (true) {
        auto next = reader->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            return SplitSource{.manifest = reader->manifest(), .inputs = std::move(inputs)};
        }
        auto& row = std::get<labels::FutureMidpointReturnRow>(next);
        inputs.push_back(SplitInput{
            .instrument = std::move(row.instrument),
            .ingest_sequence = row.ingest_sequence,
            .prediction_at = row.prediction_at,
            .label_usable = std::holds_alternative<labels::FutureMidpointReturn>(row.label),
            .label_available_at = row.future ? std::optional(row.future->available_at) : std::nullopt,
        });
    }
}

bool same_source(const SplitArtifactManifest& split, const labels::LabelArtifactManifest& label) {
    return split.label_artifact_id == label.artifact_id && split.labels_sha256 == label.labels.sha256 &&
        split.feature_artifact_id == label.feature_artifact_id && split.dataset_id == label.dataset_id &&
        split.stream == label.stream && split.horizon_ns == label.horizon_ns;
}

}  // namespace

std::string split_artifact_identity(const SplitArtifactManifest& manifest) {
    return Sha256::hex(json::serialize(manifest_body(manifest)));
}

json::Value split_manifest_json(const SplitArtifactManifest& manifest) {
    json::Object members = *manifest_body(manifest).if_object();
    members.emplace_back("artifact_id", manifest.artifact_id);
    return json::Value(std::move(members));
}

std::string serialize_split_manifest(const SplitArtifactManifest& manifest) {
    return datasets::canonical_document(split_manifest_json(manifest));
}

std::variant<SplitArtifactManifest, DatasetError> parse_split_manifest(std::string_view bytes) {
    auto document = datasets::parse_canonical_document(bytes, datasets::kArtifactManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&document)) {
        return std::move(*error);
    }
    ObjectFields fields(std::get<json::Value>(document), "manifest");
    if (fields.ok()) {
        datasets::require_name_version(fields, "schema", kSplitArtifactSchemaName, kSplitArtifactSchemaVersion);
        if (!fields.ok()) {
            return *fields.error();
        }
    }
    fields.require_keys({
        "artifact_id", "boundaries", "configuration", "counts", "files", "generator", "periods", "row", "schema",
        "source", "split_set", "usable",
    });
    datasets::require_name_version(fields, "split_set", kChronologicalSplitName, kChronologicalSplitVersion);
    datasets::require_name_version(fields, "row", kSplitRowRecordName, kSplitRowRecordVersion);
    datasets::require_name_version(fields, "generator", kSplitGeneratorName, kSplitGeneratorVersion);
    SplitArtifactManifest manifest{
        .artifact_id = fields.sha256("artifact_id"),
        .dataset_id = {},
        .feature_artifact_id = {},
        .label_artifact_id = {},
        .labels_sha256 = {},
        .stream = MarketDataStreamId("unset"),
        .horizon_ns = 0,
        .percentages = {},
        .boundaries = {},
        .rows = 0,
        .periods = {},
        .usable = {},
        .excluded = {},
        .splits = {},
    };
    // The percentages are the definition's one parameter; everything else
    // in the configuration is fixed by the version.
    if (const json::Value* config = fields.value("configuration")) {
        ObjectFields parameters(*config, fields.field("configuration"));
        if (const json::Value* percentages = parameters.value("percentages")) {
            ObjectFields shares(*percentages, parameters.field("percentages"));
            manifest.percentages = SplitPercentages{
                .train = shares.unsigned_integer("train"),
                .validation = shares.unsigned_integer("validation"),
                .test = shares.unsigned_integer("test"),
            };
            parameters.adopt(shares.error());
        }
        fields.adopt(parameters.error());
        if (fields.ok() && json::serialize(*config) != json::serialize(json::Value(configuration(manifest.percentages)))) {
            fields.fail(DatasetErrorCode::UnsupportedVersion, "configuration", "is not the configuration of this split version");
        }
    }
    {
        datasets::NestedFields source(fields, "source", {
            "dataset_id", "feature_artifact_id", "horizon_ns", "label_artifact_id", "label_set", "labels_sha256", "stream",
        });
        manifest.dataset_id = source.fields().sha256("dataset_id");
        manifest.feature_artifact_id = source.fields().sha256("feature_artifact_id");
        manifest.label_artifact_id = source.fields().sha256("label_artifact_id");
        manifest.labels_sha256 = source.fields().sha256("labels_sha256");
        manifest.horizon_ns = source.fields().signed_integer("horizon_ns");
        datasets::require_name_version(
            source.fields(),
            "label_set",
            labels::kFutureMidpointReturnName,
            labels::kFutureMidpointReturnVersion
        );
        std::string stream = source.fields().string("stream");
        if (source.fields().ok() && (stream.empty() || manifest.horizon_ns <= 0)) {
            source.fields().fail(DatasetErrorCode::InvalidValue, "stream", "stream must not be empty, and horizon_ns must be positive");
        }
        source.finish();
        if (fields.ok()) {
            manifest.stream = MarketDataStreamId(std::move(stream));
        }
    }
    {
        datasets::NestedFields boundaries(fields, "boundaries", {"test_start", "validation_start"});
        for (const auto& [key, row, at] : {
                 std::tuple{"test_start", &manifest.boundaries.test_start, &manifest.boundaries.test_start_at},
                 std::tuple{"validation_start", &manifest.boundaries.validation_start, &manifest.boundaries.validation_start_at},
             }) {
            datasets::NestedFields boundary(boundaries.fields(), key, {"prediction_at_ns", "row"});
            *row = boundary.fields().unsigned_integer("row");
            *at = boundary.fields().timestamp("prediction_at_ns");
            boundary.finish();
        }
        boundaries.finish();
    }
    {
        datasets::NestedFields counts(fields, "counts", {"excluded", "rows"});
        manifest.rows = counts.fields().unsigned_integer("rows");
        manifest.excluded = datasets::parse_count_map(counts.fields(), "excluded");
        counts.finish();
    }
    manifest.periods = parse_by_period<PeriodSummary>(fields, "periods", parse_period);
    manifest.usable = parse_by_period<UsableSummary>(fields, "usable", parse_usable);
    {
        datasets::NestedFields files(fields, "files", {"splits"});
        manifest.splits = datasets::parse_file_digest(files.fields(), "splits", kSplitRowsFileName);
        files.finish();
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    if (split_artifact_identity(manifest) != manifest.artifact_id) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "manifest.artifact_id",
            "does not match the manifest's content"
        );
    }
    if (auto error = check_summaries(manifest)) {
        return std::move(*error);
    }
    return manifest;
}

std::variant<PublishedSplitArtifact, DatasetError> create_chronological_split(
    const std::filesystem::path& label_artifact_directory,
    const std::filesystem::path& output_root,
    const SplitPercentages& percentages
) {
    if (auto error = check_percentages(percentages)) {
        return std::move(*error);
    }
    auto source = read_source(label_artifact_directory);
    if (auto* error = std::get_if<DatasetError>(&source)) {
        return std::move(*error);
    }
    const auto& [labels_manifest, inputs] = std::get<SplitSource>(source);
    SplitSummaries summaries;
    std::optional<SplitBoundaries> boundaries;
    return datasets::stage_and_publish<SplitArtifactManifest>(
        output_root,
        kSplitArtifactLayout,
        [&](datasets::StagedFile& rows) -> std::optional<DatasetError> {
            auto split = split_chronologically(
                labels_manifest.stream,
                inputs,
                percentages,
                [&](const SplitRow& row) -> std::optional<DatasetError> {
                    summaries.add(row);
                    return datasets::write_record(rows, encode_split_row(row), "split", summaries.rows);
                }
            );
            if (auto* error = std::get_if<DatasetError>(&split)) {
                return std::move(*error);
            }
            boundaries = std::get<SplitBoundaries>(split);
            return std::nullopt;
        },
        [&](datasets::FileDigest digest) -> std::variant<SplitArtifactManifest, DatasetError> {
            SplitArtifactManifest manifest{
                .artifact_id = {},
                .dataset_id = labels_manifest.dataset_id,
                .feature_artifact_id = labels_manifest.feature_artifact_id,
                .label_artifact_id = labels_manifest.artifact_id,
                .labels_sha256 = labels_manifest.labels.sha256,
                .stream = labels_manifest.stream,
                .horizon_ns = labels_manifest.horizon_ns,
                .percentages = percentages,
                .boundaries = *boundaries,
                .rows = summaries.rows,
                .periods = summaries.periods,
                .usable = summaries.usable,
                .excluded = summaries.excluded,
                .splits = std::move(digest),
            };
            manifest.artifact_id = split_artifact_identity(manifest);
            // Never publish what the reader would refuse.
            if (auto error = check_summaries(manifest)) {
                return std::move(*error);
            }
            return manifest;
        },
        serialize_split_manifest,
        verify_split_artifact
    );
}

std::variant<SplitArtifactManifest, DatasetError> verify_split_artifact(const std::filesystem::path& directory) {
    auto opened = open_artifact(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    const auto& [manifest, path] = std::get<datasets::OpenedArtifact<SplitArtifactManifest>>(opened);
    auto file = datasets::DerivedArtifactRows::open(path, kSplitArtifactLayout, manifest.rows, manifest.splits);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    auto& rows = std::get<std::unique_ptr<datasets::DerivedArtifactRows>>(file);
    const SplitBoundaries& boundaries = manifest.boundaries;
    SplitSummaries summaries;
    std::vector<Timestamp> times;
    std::optional<std::uint64_t> previous_sequence;
    while (true) {
        auto next = rows->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            break;
        }
        const std::uint64_t line = rows->line_number();
        auto decoded = decode_split_row(std::get<std::string_view>(next), line);
        if (auto* error = std::get_if<DatasetError>(&decoded)) {
            return std::move(*error);
        }
        const auto& row = std::get<SplitRow>(decoded);
        const auto invalid = [line](std::string field, std::string message) {
            return datasets::dataset_error(DatasetErrorCode::InvalidValue, std::move(field), std::move(message), line);
        };
        if (row.stream != manifest.stream) {
            return datasets::dataset_error(DatasetErrorCode::StreamMismatch, "stream", "differs from the manifest", line);
        }
        if (previous_sequence && row.ingest_sequence <= *previous_sequence) {
            return datasets::dataset_error(
                DatasetErrorCode::IngestSequenceMovedBackward,
                "ingest_sequence",
                "rows must follow ingest order",
                line
            );
        }
        previous_sequence = row.ingest_sequence;
        if (!times.empty() && row.prediction_at < times.back()) {
            return datasets::dataset_error(
                DatasetErrorCode::ReceiveTimeMovedBackward,
                "prediction_at_ns",
                "prediction times must never decrease in row order",
                line
            );
        }
        if (row.period != period_of(line - 1, boundaries)) {
            return invalid("period", "is not the period the row's position and the manifest's boundaries give");
        }
        if (row.label_usable && !row.label_available_at) {
            return invalid("label_available_at_ns", "a usable label must have an availability time");
        }
        std::int64_t horizon_end = 0;
        if (row.label_available_at &&
            (__builtin_add_overflow(row.prediction_at.nanoseconds(), manifest.horizon_ns, &horizon_end) ||
             row.label_available_at->nanoseconds() < horizon_end)) {
            return invalid("label_available_at_ns", "a label cannot be known before one horizon after its prediction");
        }
        if (!row.exclusion) {
            // The invariants themselves, before the rule that implies them.
            if (!row.label_usable) {
                return invalid("exclusion", "a row in a split must have a usable label");
            }
            if ((row.period == SplitPeriod::Train && *row.label_available_at >= boundaries.validation_start_at) ||
                (row.period == SplitPeriod::Validation && *row.label_available_at >= boundaries.test_start_at)) {
                return datasets::dataset_error(
                    DatasetErrorCode::LabelLeakage,
                    "label_available_at_ns",
                    "a usable label is known at or after the next period begins",
                    line
                );
            }
        }
        if (row.exclusion != exclusion_of(row.period, row.label_usable, row.label_available_at, boundaries)) {
            return invalid("exclusion", "is not the exclusion the row's label and the manifest's boundaries give");
        }
        summaries.add(row);
        times.push_back(row.prediction_at);
    }
    if (!summaries.matches(manifest)) {
        return datasets::dataset_error(
            DatasetErrorCode::SummaryMismatch,
            std::string(datasets::kArtifactManifestFileName),
            "the rows do not match the manifest's summaries"
        );
    }
    const auto recomputed = chronological_boundaries(times, manifest.percentages);
    if (!std::holds_alternative<SplitBoundaries>(recomputed) || std::get<SplitBoundaries>(recomputed) != boundaries) {
        return datasets::dataset_error(
            DatasetErrorCode::InvalidValue,
            "manifest.boundaries",
            "are not the boundaries the percentages give for these prediction times"
        );
    }
    return manifest;
}

std::optional<DatasetError> reproduce_split_artifact(
    const SplitArtifactManifest& manifest,
    const std::filesystem::path& label_artifact_directory
) {
    auto source = read_source(label_artifact_directory);
    if (auto* error = std::get_if<DatasetError>(&source)) {
        return std::move(*error);
    }
    const auto& [labels_manifest, inputs] = std::get<SplitSource>(source);
    if (!same_source(manifest, labels_manifest)) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "source",
            "the label artifact is not the split artifact's source"
        );
    }
    Sha256 hash;
    std::uint64_t rows = 0;
    auto split = split_chronologically(
        manifest.stream,
        inputs,
        manifest.percentages,
        [&](const SplitRow& row) -> std::optional<DatasetError> {
            hash.update(encode_split_row(row) + "\n");
            ++rows;
            return std::nullopt;
        }
    );
    if (auto* error = std::get_if<DatasetError>(&split)) {
        return std::move(*error);
    }
    if (rows != manifest.rows || hash.hex_digest() != manifest.splits.sha256) {
        return datasets::dataset_error(
            DatasetErrorCode::ChecksumMismatch,
            std::string(kSplitRowsFileName),
            "recomputing the split from the label artifact gives different bytes"
        );
    }
    return std::nullopt;
}

std::variant<std::vector<SplitArtifactEntry>, DatasetError> list_split_artifacts(const std::filesystem::path& root) {
    return datasets::list_derived_artifacts(
        root,
        "output",
        kSplitArtifactLayout,
        parse_split_manifest,
        &SplitArtifactManifest::splits
    );
}

}  // namespace chronos::splits
