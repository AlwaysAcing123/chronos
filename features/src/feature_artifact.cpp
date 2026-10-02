#include "chronos/features/feature_artifact.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::features {
namespace {

namespace json = chronos::canonical_json;
using datasets::DatasetError;
using datasets::DatasetErrorCode;
using datasets::ObjectFields;

// The configuration that fixes what version 1's values mean. None of it is
// adjustable yet, but all of it is part of the artifact's identity.
const json::Object& configuration() {
    static const json::Object config{
        {"available_at", "receive_timestamp"},
        {"price_rounding", "half_away_from_zero"},
        {"ratio_rounding", "half_away_from_zero"},
        {"ratio_scale", std::to_string(Ratio::scale())},
        {"rows", "one_per_top_of_book_event"},
        {"trading_status", "latest_market_status_event_per_instrument"},
        {"unavailability", "typed_reason_per_feature"},
    };
    return config;
}

const json::Object& feature_scales() {
    static const json::Object scales = [] {
        const auto scale = [](std::string_view kind, std::int64_t value) {
            return json::Value(json::Object{{"kind", kind}, {"scale", std::to_string(value)}});
        };
        return json::Object{
            {"ask_size", scale("quantity_raw", Quantity::scale())},
            {"best_ask", scale("price_raw", Price::scale())},
            {"best_bid", scale("price_raw", Price::scale())},
            {"bid_size", scale("quantity_raw", Quantity::scale())},
            {"imbalance", scale("ratio_raw", Ratio::scale())},
            {"microprice", scale("price_raw", Price::scale())},
            {"midpoint", scale("price_raw", Price::scale())},
            {"spread", scale("price_raw", Price::scale())},
        };
    }();
    return scales;
}

constexpr std::array<std::string_view, 8> kFeatureNames = {
    "ask_size", "best_ask", "best_bid", "bid_size", "imbalance", "microprice", "midpoint", "spread",
};

json::Value manifest_body(const FeatureArtifactManifest& manifest) {
    json::Object unavailable;
    for (const auto& [feature, reasons] : manifest.unavailable) {
        unavailable.emplace_back(feature, datasets::count_map_json(reasons));
    }
    return json::Value(json::Object{
        {"configuration", configuration()},
        {"counts", json::Object{
            {"rows", std::to_string(manifest.rows)},
            {"rows_by_instrument", datasets::count_map_json(manifest.rows_by_instrument)},
            {"unavailable", std::move(unavailable)},
        }},
        {"feature_set", datasets::name_version_json(kTopOfBookFeatureSetName, kTopOfBookFeatureSetVersion)},
        {"features", feature_scales()},
        {"files", json::Object{
            {"features", datasets::file_digest_json(kFeatureRowsFileName, manifest.features)},
        }},
        {"row", datasets::name_version_json(kFeatureRowRecordName, kFeatureRowRecordVersion)},
        {"schema", datasets::name_version_json(kFeatureArtifactSchemaName, kFeatureArtifactSchemaVersion)},
        {"source", json::Object{
            {"dataset_id", manifest.dataset_id},
            {"events_sha256", manifest.dataset_events_sha256},
            {"stream", manifest.stream.value()},
        }},
    });
}

// The counts a sequence of rows produces, for writing a manifest and for
// checking one.
class RowCounts final {
public:
    std::optional<DatasetError> add(const TopOfBookFeatureRow& row) {
        if (rows == std::numeric_limits<std::uint64_t>::max()) {
            return overflow();
        }
        ++rows;
        ++by_instrument[row.instrument.value()];
        const auto count = [this](std::string_view feature, const auto& value) {
            if (const auto* reason = std::get_if<Unavailability>(&value)) {
                ++unavailable[std::string(feature)][std::string(unavailability_name(*reason))];
            }
        };
        count("ask_size", row.ask_size);
        count("best_ask", row.best_ask);
        count("best_bid", row.best_bid);
        count("bid_size", row.bid_size);
        count("imbalance", row.imbalance);
        count("microprice", row.microprice);
        count("midpoint", row.midpoint);
        count("spread", row.spread);
        return std::nullopt;
    }

    static DatasetError overflow() {
        return datasets::dataset_error(DatasetErrorCode::CounterOverflow, "counts", "a row count cannot be represented");
    }

    // Every count is at most `rows`, so none can overflow first.
    std::uint64_t rows{0};
    std::map<std::string, std::uint64_t> by_instrument;
    std::map<std::string, std::map<std::string, std::uint64_t>> unavailable;
};

// A feature artifact's directory, with a valid manifest that names it.
std::variant<datasets::OpenedArtifact<FeatureArtifactManifest>, DatasetError> open_artifact(
    const std::filesystem::path& directory
) {
    return datasets::open_artifact_manifest(
        directory,
        kFeatureArtifactLayout,
        parse_feature_manifest,
        &FeatureArtifactManifest::features
    );
}

// Computes the rows of a verified dataset and passes each, with its
// canonical record (without a newline), to sink.
std::variant<datasets::DatasetManifest, DatasetError> compute_rows(
    const std::filesystem::path& dataset_directory,
    const std::function<std::optional<DatasetError>(const TopOfBookFeatureRow&, const std::string&)>& sink
) {
    auto opened = datasets::NormalizedMarketEventReader::open(dataset_directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& reader = std::get<std::unique_ptr<datasets::NormalizedMarketEventReader>>(opened);
    TopOfBookFeatureEngine engine;
    while (true) {
        MarketEventRead read = reader->read_next();
        if (std::holds_alternative<EndOfMarketEvents>(read)) {
            return reader->manifest();
        }
        if (std::holds_alternative<MarketEventReadError>(read)) {
            return *reader->error();
        }
        if (auto row = engine.apply(std::get<ValidatedMarketEvent>(read))) {
            if (auto error = sink(*row, encode_feature_row(*row))) {
                return std::move(*error);
            }
        }
    }
}

}  // namespace

std::string feature_artifact_identity(const FeatureArtifactManifest& manifest) {
    return Sha256::hex(json::serialize(manifest_body(manifest)));
}

std::string serialize_feature_manifest(const FeatureArtifactManifest& manifest) {
    json::Object members = *manifest_body(manifest).if_object();
    members.emplace_back("artifact_id", manifest.artifact_id);
    return datasets::canonical_document(json::Value(std::move(members)));
}

std::variant<FeatureArtifactManifest, DatasetError> parse_feature_manifest(std::string_view bytes) {
    auto document = datasets::parse_canonical_document(bytes, kFeatureManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&document)) {
        return std::move(*error);
    }
    ObjectFields fields(std::get<json::Value>(document), "manifest");
    if (fields.ok()) {
        datasets::require_name_version(fields, "schema", kFeatureArtifactSchemaName, kFeatureArtifactSchemaVersion);
        if (!fields.ok()) {
            return *fields.error();
        }
    }
    fields.require_keys({
        "artifact_id", "configuration", "counts", "feature_set", "features", "files", "row", "schema", "source",
    });
    datasets::require_name_version(fields, "feature_set", kTopOfBookFeatureSetName, kTopOfBookFeatureSetVersion);
    datasets::require_name_version(fields, "row", kFeatureRowRecordName, kFeatureRowRecordVersion);
    // Configuration and scales are fixed by the version: anything else is a
    // different meaning this reader does not know.
    for (const auto& [key, expected] : {
             std::pair<std::string_view, const json::Object*>{"configuration", &configuration()},
             {"features", &feature_scales()},
         }) {
        const json::Value* value = fields.value(key);
        if (value != nullptr && json::serialize(*value) != json::serialize(json::Value(*expected))) {
            fields.fail(DatasetErrorCode::UnsupportedVersion, key, "is not the configuration of this feature version");
        }
    }
    FeatureArtifactManifest manifest{
        .artifact_id = fields.sha256("artifact_id"),
        .dataset_id = {},
        .dataset_events_sha256 = {},
        .stream = MarketDataStreamId("unset"),
        .rows = 0,
        .rows_by_instrument = {},
        .unavailable = {},
        .features = {},
    };
    {
        datasets::NestedFields source(fields, "source", {"dataset_id", "events_sha256", "stream"});
        manifest.dataset_id = source.fields().sha256("dataset_id");
        manifest.dataset_events_sha256 = source.fields().sha256("events_sha256");
        std::string stream = source.fields().string("stream");
        if (source.fields().ok() && stream.empty()) {
            source.fields().fail(DatasetErrorCode::InvalidValue, "stream", "must not be empty");
        }
        source.finish();
        if (fields.ok()) {
            manifest.stream = MarketDataStreamId(std::move(stream));
        }
    }
    {
        datasets::NestedFields counts(fields, "counts", {"rows", "rows_by_instrument", "unavailable"});
        manifest.rows = counts.fields().unsigned_integer("rows");
        manifest.rows_by_instrument = datasets::parse_count_map(counts.fields(), "rows_by_instrument");
        const json::Value* unavailable = counts.fields().value("unavailable");
        if (unavailable != nullptr && unavailable->if_object() != nullptr) {
            ObjectFields by_feature(*unavailable, counts.fields().field("unavailable"));
            for (const auto& entry : *unavailable->if_object()) {
                if (std::find(kFeatureNames.begin(), kFeatureNames.end(), entry.first) == kFeatureNames.end()) {
                    counts.fields().fail(DatasetErrorCode::UnknownField, "unavailable", "unknown feature " + entry.first);
                    break;
                }
                auto reasons = datasets::parse_count_map(by_feature, entry.first);
                for (const auto& reason : reasons) {
                    if (!unavailability_from_name(reason.first)) {
                        counts.fields().fail(
                            DatasetErrorCode::InvalidValue,
                            "unavailable",
                            "unknown unavailability reason " + reason.first
                        );
                        break;
                    }
                }
                manifest.unavailable.emplace(entry.first, std::move(reasons));
            }
            counts.fields().adopt(by_feature.error());
        }
        counts.finish();
    }
    {
        datasets::NestedFields files(fields, "files", {"features"});
        manifest.features = datasets::parse_file_digest(files.fields(), "features", kFeatureRowsFileName);
        files.finish();
    }
    if (!fields.ok()) {
        return *fields.error();
    }
    if (feature_artifact_identity(manifest) != manifest.artifact_id) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "manifest.artifact_id",
            "does not match the manifest's content"
        );
    }
    std::uint64_t total = 0;
    bool representable = true;
    for (const auto& entry : manifest.rows_by_instrument) {
        representable = representable && datasets::add_count(total, entry.second);
    }
    if (!representable || total != manifest.rows) {
        return datasets::dataset_error(
            DatasetErrorCode::SummaryMismatch,
            "manifest.counts.rows_by_instrument",
            "must add up to counts.rows"
        );
    }
    for (const auto& [feature, reasons] : manifest.unavailable) {
        std::uint64_t unavailable = 0;
        for (const auto& entry : reasons) {
            representable = representable && datasets::add_count(unavailable, entry.second);
        }
        if (!representable || unavailable > manifest.rows) {
            return datasets::dataset_error(
                DatasetErrorCode::SummaryMismatch,
                "manifest.counts.unavailable." + feature,
                "cannot exceed counts.rows"
            );
        }
    }
    if ((manifest.rows == 0) != (manifest.features.size_bytes == 0)) {
        return datasets::dataset_error(
            DatasetErrorCode::SummaryMismatch,
            "manifest.files.features.size_bytes",
            "must be zero exactly when there are no rows"
        );
    }
    return manifest;
}

std::variant<PublishedFeatureArtifact, DatasetError> extract_top_of_book_features(
    const std::filesystem::path& dataset_directory,
    const std::filesystem::path& output_root
) {
    RowCounts counts;
    std::optional<datasets::DatasetManifest> dataset;
    return datasets::stage_and_publish<FeatureArtifactManifest>(
        output_root,
        kFeatureArtifactLayout,
        [&](datasets::StagedFile& rows) -> std::optional<DatasetError> {
            auto source = compute_rows(
                dataset_directory,
                [&](const TopOfBookFeatureRow& row, const std::string& record) -> std::optional<DatasetError> {
                    if (auto error = datasets::write_record(rows, record, "feature", counts.rows + 1)) {
                        return error;
                    }
                    return counts.add(row);
                }
            );
            if (auto* error = std::get_if<DatasetError>(&source)) {
                return std::move(*error);
            }
            dataset = std::get<datasets::DatasetManifest>(std::move(source));
            return std::nullopt;
        },
        [&](datasets::FileDigest digest) -> std::variant<FeatureArtifactManifest, DatasetError> {
            FeatureArtifactManifest manifest{
                .artifact_id = {},
                .dataset_id = dataset->dataset_id,
                .dataset_events_sha256 = dataset->events.sha256,
                .stream = dataset->stream,
                .rows = counts.rows,
                .rows_by_instrument = counts.by_instrument,
                .unavailable = counts.unavailable,
                .features = std::move(digest),
            };
            manifest.artifact_id = feature_artifact_identity(manifest);
            return manifest;
        },
        serialize_feature_manifest,
        verify_feature_artifact
    );
}

class FeatureArtifactReader::State final {
public:
    State(FeatureArtifactManifest manifest, std::unique_ptr<datasets::DerivedArtifactRows> rows)
        : manifest_(std::move(manifest)), rows_(std::move(rows)) {}

    const FeatureArtifactManifest& manifest() const noexcept { return manifest_; }

    std::variant<TopOfBookFeatureRow, datasets::EndOfRows, DatasetError> next() {
        auto next = rows_->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfRows>(next)) {
            if (counts_.by_instrument != manifest_.rows_by_instrument || counts_.unavailable != manifest_.unavailable) {
                return datasets::dataset_error(
                    DatasetErrorCode::SummaryMismatch,
                    std::string(kFeatureManifestFileName),
                    "the rows do not match the manifest's counts"
                );
            }
            return datasets::EndOfRows{};
        }
        const std::uint64_t line = rows_->line_number();
        auto decoded = decode_feature_row(std::get<std::string_view>(next), line);
        if (auto* error = std::get_if<DatasetError>(&decoded)) {
            return std::move(*error);
        }
        const auto& row = std::get<TopOfBookFeatureRow>(decoded);
        if (row.stream != manifest_.stream) {
            return datasets::dataset_error(DatasetErrorCode::StreamMismatch, "stream", "differs from the manifest", line);
        }
        if (previous_sequence_ && row.ingest_sequence <= *previous_sequence_) {
            return datasets::dataset_error(
                DatasetErrorCode::IngestSequenceMovedBackward,
                "ingest_sequence",
                "rows must follow ingest order",
                line
            );
        }
        previous_sequence_ = row.ingest_sequence;
        if (row.available_at != row.receive_timestamp) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidValue,
                "available_at_ns",
                "must be the triggering event's receive time",
                line
            );
        }
        if (auto error = counts_.add(row)) {
            return std::move(*error);
        }
        return std::move(std::get<TopOfBookFeatureRow>(decoded));
    }

private:
    FeatureArtifactManifest manifest_;
    std::unique_ptr<datasets::DerivedArtifactRows> rows_;
    RowCounts counts_;
    std::optional<std::uint64_t> previous_sequence_;
};

FeatureArtifactReader::FeatureArtifactReader(std::unique_ptr<State> state) : state_(std::move(state)) {}
FeatureArtifactReader::~FeatureArtifactReader() = default;

std::variant<std::unique_ptr<FeatureArtifactReader>, DatasetError> FeatureArtifactReader::open(
    const std::filesystem::path& directory
) {
    auto opened = open_artifact(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& [manifest, path] = std::get<datasets::OpenedArtifact<FeatureArtifactManifest>>(opened);
    auto rows = datasets::DerivedArtifactRows::open(path, kFeatureArtifactLayout, manifest.rows, manifest.features);
    if (auto* error = std::get_if<DatasetError>(&rows)) {
        return std::move(*error);
    }
    return std::unique_ptr<FeatureArtifactReader>(new FeatureArtifactReader(std::make_unique<State>(
        std::move(manifest),
        std::get<std::unique_ptr<datasets::DerivedArtifactRows>>(std::move(rows))
    )));
}

const FeatureArtifactManifest& FeatureArtifactReader::manifest() const noexcept { return state_->manifest(); }

std::variant<TopOfBookFeatureRow, datasets::EndOfRows, DatasetError> FeatureArtifactReader::next() {
    return state_->next();
}

std::variant<FeatureArtifactManifest, DatasetError> verify_feature_artifact(const std::filesystem::path& directory) {
    auto opened = FeatureArtifactReader::open(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& reader = std::get<std::unique_ptr<FeatureArtifactReader>>(opened);
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

std::optional<DatasetError> reproduce_feature_artifact(
    const FeatureArtifactManifest& manifest,
    const std::filesystem::path& dataset_directory
) {
    Sha256 hash;
    std::uint64_t rows = 0;
    auto source = compute_rows(
        dataset_directory,
        [&](const TopOfBookFeatureRow&, const std::string& record) -> std::optional<DatasetError> {
            hash.update(record);
            hash.update("\n");
            ++rows;
            return std::nullopt;
        }
    );
    if (auto* error = std::get_if<DatasetError>(&source)) {
        return std::move(*error);
    }
    const auto& dataset = std::get<datasets::DatasetManifest>(source);
    if (dataset.dataset_id != manifest.dataset_id || dataset.events.sha256 != manifest.dataset_events_sha256 ||
        dataset.stream != manifest.stream) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "source",
            "the dataset is not the artifact's source"
        );
    }
    if (rows != manifest.rows || hash.hex_digest() != manifest.features.sha256) {
        return datasets::dataset_error(
            DatasetErrorCode::ChecksumMismatch,
            std::string(kFeatureRowsFileName),
            "recomputing the rows from the dataset gives different bytes"
        );
    }
    return std::nullopt;
}

std::variant<std::vector<FeatureArtifactEntry>, DatasetError> list_feature_artifacts(const std::filesystem::path& root) {
    return datasets::list_derived_artifacts(
        root,
        "output",
        kFeatureArtifactLayout,
        parse_feature_manifest,
        &FeatureArtifactManifest::features
    );
}

}  // namespace chronos::features
