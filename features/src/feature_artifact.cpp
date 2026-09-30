#include "chronos/features/feature_artifact.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/datasets/ndjson_file.hpp"
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

using datasets::kMaximumManifestBytes;
using datasets::kMaximumRecordBytes;

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

json::Value decimal(std::uint64_t value) { return json::Value(std::to_string(value)); }

json::Value manifest_body(const FeatureArtifactManifest& manifest) {
    json::Object by_instrument;
    for (const auto& [instrument, count] : manifest.rows_by_instrument) {
        by_instrument.emplace_back(instrument, decimal(count));
    }
    json::Object unavailable;
    for (const auto& [feature, reasons] : manifest.unavailable) {
        json::Object counts;
        for (const auto& [reason, count] : reasons) {
            counts.emplace_back(reason, decimal(count));
        }
        unavailable.emplace_back(feature, std::move(counts));
    }
    return json::Value(json::Object{
        {"configuration", configuration()},
        {"counts", json::Object{
            {"rows", decimal(manifest.rows)},
            {"rows_by_instrument", std::move(by_instrument)},
            {"unavailable", std::move(unavailable)},
        }},
        {"feature_set", json::Object{
            {"name", kTopOfBookFeatureSetName},
            {"version", kTopOfBookFeatureSetVersion},
        }},
        {"features", feature_scales()},
        {"files", json::Object{
            {"features", json::Object{
                {"name", kFeatureRowsFileName},
                {"sha256", manifest.features.sha256},
                {"size_bytes", decimal(manifest.features.size_bytes)},
            }},
        }},
        {"row", json::Object{
            {"name", kFeatureRowRecordName},
            {"version", kFeatureRowRecordVersion},
        }},
        {"schema", json::Object{
            {"name", kFeatureArtifactSchemaName},
            {"version", kFeatureArtifactSchemaVersion},
        }},
        {"source", json::Object{
            {"dataset_id", manifest.dataset_id},
            {"events_sha256", manifest.dataset_events_sha256},
            {"stream", manifest.stream.value()},
        }},
    });
}

bool add_to(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
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

class Nested final {
public:
    Nested(ObjectFields& parent, std::string_view key, std::initializer_list<std::string_view> keys)
        : parent_(parent), fields_(value_or_empty(parent, key), parent.field(key)) {
        fields_.require_keys(keys);
    }
    ObjectFields& fields() { return fields_; }
    void finish() { parent_.adopt(fields_.error()); }

private:
    static const json::Value& value_or_empty(ObjectFields& parent, std::string_view key) {
        static const json::Value empty{json::Object{}};
        const json::Value* value = parent.value(key);
        return value != nullptr ? *value : empty;
    }
    ObjectFields& parent_;
    ObjectFields fields_;
};

std::map<std::string, std::uint64_t> positive_counts(ObjectFields& fields, std::string_view key) {
    std::map<std::string, std::uint64_t> counts;
    const json::Object* object = fields.object(key);
    if (object == nullptr) {
        return counts;
    }
    for (const auto& [name, value] : *object) {
        const std::string* text = value.if_string();
        const auto count = text ? json::parse_canonical_unsigned(*text) : std::nullopt;
        if (name.empty() || !count || *count == 0) {
            fields.fail(DatasetErrorCode::InvalidValue, key, "must map names to positive canonical counts");
            return {};
        }
        counts.emplace(name, *count);
    }
    return counts;
}

// A feature artifact's directory: exactly its two regular files, named by
// its ID.
std::variant<FeatureArtifactManifest, DatasetError> open_artifact(const std::filesystem::path& given) {
    // "root/<id>/" names the same directory as "root/<id>".
    const std::filesystem::path directory = given.filename().empty() ? given.parent_path() : given;
    auto kind = datasets::inspect_entry(directory);
    if (auto* error = std::get_if<DatasetError>(&kind)) {
        return std::move(*error);
    }
    const auto entry = std::get<datasets::EntryKind>(kind);
    if (entry == datasets::EntryKind::Symlink) {
        return datasets::dataset_error(DatasetErrorCode::SymlinkRejected, "artifact", "is a symlink, which is never followed");
    }
    if (entry != datasets::EntryKind::Directory) {
        return datasets::dataset_error(
            entry == datasets::EntryKind::Missing ? DatasetErrorCode::NotFound : DatasetErrorCode::UnexpectedEntry,
            "artifact",
            "is not a feature artifact directory"
        );
    }
    const std::string name = directory.filename().string();
    if (!is_sha256_hex(name)) {
        return datasets::dataset_error(
            DatasetErrorCode::UnexpectedEntry,
            "artifact",
            "a feature artifact directory is named by its 64-digit lowercase artifact ID"
        );
    }
    auto entries = datasets::directory_entries(directory);
    if (auto* error = std::get_if<DatasetError>(&entries)) {
        return std::move(*error);
    }
    const std::vector<std::string> expected = {std::string(kFeatureRowsFileName), std::string(kFeatureManifestFileName)};
    for (const std::string& file : std::get<std::vector<std::string>>(entries)) {
        if (std::find(expected.begin(), expected.end(), file) == expected.end()) {
            return datasets::dataset_error(DatasetErrorCode::UnexpectedEntry, file, "is not part of a feature artifact");
        }
    }
    for (const std::string& file : expected) {
        auto file_kind = datasets::inspect_entry(directory / file);
        if (auto* error = std::get_if<DatasetError>(&file_kind)) {
            return std::move(*error);
        }
        const auto found = std::get<datasets::EntryKind>(file_kind);
        if (found != datasets::EntryKind::RegularFile) {
            return datasets::dataset_error(
                found == datasets::EntryKind::Symlink ? DatasetErrorCode::SymlinkRejected
                    : found == datasets::EntryKind::Missing ? DatasetErrorCode::NotFound
                    : DatasetErrorCode::UnexpectedEntry,
                file,
                "must be a regular file"
            );
        }
    }
    auto bytes = datasets::read_small_file(directory / kFeatureManifestFileName, kMaximumManifestBytes, kFeatureManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&bytes)) {
        return std::move(*error);
    }
    auto parsed = parse_feature_manifest(std::get<std::string>(bytes));
    if (auto* error = std::get_if<DatasetError>(&parsed)) {
        return std::move(*error);
    }
    auto manifest = std::get<FeatureArtifactManifest>(std::move(parsed));
    if (manifest.artifact_id != name) {
        return datasets::dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "artifact",
            "directory name differs from the manifest's artifact ID"
        );
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(directory / kFeatureRowsFileName, size_error);
    if (size_error || size != manifest.features.size_bytes) {
        return datasets::dataset_error(
            DatasetErrorCode::SizeMismatch,
            std::string(kFeatureRowsFileName),
            "size differs from the manifest"
        );
    }
    return manifest;
}

// Computes the rows of a verified dataset and passes each, with its
// canonical record, to sink.
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
            if (auto error = sink(*row, encode_feature_row(*row) + "\n")) {
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
        Nested schema(fields, "schema", {"name", "version"});
        schema.fields().constant("name", kFeatureArtifactSchemaName);
        schema.fields().constant("version", kFeatureArtifactSchemaVersion);
        schema.finish();
        if (!fields.ok()) {
            return *fields.error();
        }
    }
    fields.require_keys({
        "artifact_id", "configuration", "counts", "feature_set", "features", "files", "row", "schema", "source",
    });
    {
        Nested feature_set(fields, "feature_set", {"name", "version"});
        feature_set.fields().constant("name", kTopOfBookFeatureSetName);
        feature_set.fields().constant("version", kTopOfBookFeatureSetVersion);
        feature_set.finish();
        Nested row(fields, "row", {"name", "version"});
        row.fields().constant("name", kFeatureRowRecordName);
        row.fields().constant("version", kFeatureRowRecordVersion);
        row.finish();
    }
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
        Nested source(fields, "source", {"dataset_id", "events_sha256", "stream"});
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
        Nested counts(fields, "counts", {"rows", "rows_by_instrument", "unavailable"});
        manifest.rows = counts.fields().unsigned_integer("rows");
        manifest.rows_by_instrument = positive_counts(counts.fields(), "rows_by_instrument");
        const json::Value* unavailable = counts.fields().value("unavailable");
        if (unavailable != nullptr && unavailable->if_object() != nullptr) {
            ObjectFields by_feature(*unavailable, counts.fields().field("unavailable"));
            for (const auto& entry : *unavailable->if_object()) {
                if (std::find(kFeatureNames.begin(), kFeatureNames.end(), entry.first) == kFeatureNames.end()) {
                    counts.fields().fail(DatasetErrorCode::UnknownField, "unavailable", "unknown feature " + entry.first);
                    break;
                }
                auto reasons = positive_counts(by_feature, entry.first);
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
        Nested files(fields, "files", {"features"});
        Nested rows(files.fields(), "features", {"name", "sha256", "size_bytes"});
        rows.fields().constant("name", kFeatureRowsFileName);
        manifest.features.sha256 = rows.fields().sha256("sha256");
        manifest.features.size_bytes = rows.fields().unsigned_integer("size_bytes");
        rows.finish();
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
        representable = representable && add_to(total, entry.second);
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
            representable = representable && add_to(unavailable, entry.second);
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
    auto staging = datasets::StagingDirectory::create(output_root);
    if (auto* error = std::get_if<DatasetError>(&staging)) {
        return std::move(*error);
    }
    auto& directory = std::get<std::unique_ptr<datasets::StagingDirectory>>(staging);
    auto file = directory->create_file(kFeatureRowsFileName);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    auto& rows = std::get<std::unique_ptr<datasets::StagedFile>>(file);
    RowCounts counts;
    auto source = compute_rows(
        dataset_directory,
        [&](const TopOfBookFeatureRow& row, const std::string& record) -> std::optional<DatasetError> {
            // The record ends with its newline.
            if (record.size() - 1 > kMaximumRecordBytes) {
                return datasets::dataset_error(
                    DatasetErrorCode::LineTooLong,
                    "row",
                    "a feature row exceeds the format's maximum length",
                    counts.rows + 1
                );
            }
            if (auto error = counts.add(row)) {
                return error;
            }
            return rows->write(record);
        }
    );
    if (auto* error = std::get_if<DatasetError>(&source)) {
        return std::move(*error);
    }
    auto digest = rows->finish();
    if (auto* error = std::get_if<DatasetError>(&digest)) {
        return std::move(*error);
    }
    const auto& dataset = std::get<datasets::DatasetManifest>(source);
    FeatureArtifactManifest manifest{
        .artifact_id = {},
        .dataset_id = dataset.dataset_id,
        .dataset_events_sha256 = dataset.events.sha256,
        .stream = dataset.stream,
        .rows = counts.rows,
        .rows_by_instrument = counts.by_instrument,
        .unavailable = counts.unavailable,
        .features = std::get<datasets::FileDigest>(std::move(digest)),
    };
    manifest.artifact_id = feature_artifact_identity(manifest);
    const std::string manifest_bytes = serialize_feature_manifest(manifest);
    if (manifest_bytes.size() > kMaximumManifestBytes) {
        return datasets::dataset_error(
            DatasetErrorCode::SizeMismatch,
            std::string(kFeatureManifestFileName),
            "the manifest exceeds the format's maximum size"
        );
    }
    auto written = directory->write_file(kFeatureManifestFileName, manifest_bytes);
    if (auto* error = std::get_if<DatasetError>(&written)) {
        return std::move(*error);
    }
    const std::filesystem::path destination = output_root / manifest.artifact_id;
    auto published = directory->publish(manifest.artifact_id);
    if (auto* error = std::get_if<DatasetError>(&published)) {
        return std::move(*error);
    }
    if (std::get<datasets::PublicationResult>(published) == datasets::PublicationResult::Published) {
        return PublishedFeatureArtifact{
            .manifest = std::move(manifest),
            .directory = destination,
            .outcome = datasets::PublicationOutcome::Published,
            .warning = directory->warning(),
        };
    }
    auto existing = verify_feature_artifact(destination);
    if (auto* error = std::get_if<DatasetError>(&existing)) {
        return datasets::dataset_error(
            DatasetErrorCode::Collision,
            "artifact",
            "an entry named " + manifest.artifact_id + " already exists and is not a valid feature artifact (" +
                std::string(datasets::error_code_name(error->code)) + ": " + error->message + "); it was left untouched"
        );
    }
    if (serialize_feature_manifest(std::get<FeatureArtifactManifest>(existing)) != manifest_bytes) {
        return datasets::dataset_error(
            DatasetErrorCode::Collision,
            "artifact",
            "a different feature artifact is already published as " + manifest.artifact_id
        );
    }
    return PublishedFeatureArtifact{
        .manifest = std::move(manifest),
        .directory = destination,
        .outcome = datasets::PublicationOutcome::AlreadyPresent,
        .warning = std::nullopt,
    };
}

std::variant<FeatureArtifactManifest, DatasetError> verify_feature_artifact(const std::filesystem::path& directory) {
    auto opened = open_artifact(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    const auto& manifest = std::get<FeatureArtifactManifest>(opened);
    auto file = datasets::NdjsonFileReader::open(directory / kFeatureRowsFileName, std::string(kFeatureRowsFileName), kMaximumRecordBytes);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    auto& rows = std::get<std::unique_ptr<datasets::NdjsonFileReader>>(file);
    RowCounts counts;
    std::optional<std::uint64_t> previous_sequence;
    while (true) {
        auto next = rows->next();
        if (auto* error = std::get_if<DatasetError>(&next)) {
            return std::move(*error);
        }
        if (std::holds_alternative<datasets::EndOfLines>(next)) {
            break;
        }
        const std::uint64_t line = rows->line_number();
        auto decoded = decode_feature_row(std::get<std::string_view>(next), line);
        if (auto* error = std::get_if<DatasetError>(&decoded)) {
            return std::move(*error);
        }
        const auto& row = std::get<TopOfBookFeatureRow>(decoded);
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
        if (row.available_at != row.receive_timestamp) {
            return datasets::dataset_error(
                DatasetErrorCode::InvalidValue,
                "available_at_ns",
                "must be the triggering event's receive time",
                line
            );
        }
        if (auto error = counts.add(row)) {
            return std::move(*error);
        }
    }
    if (counts.rows != manifest.rows) {
        return datasets::dataset_error(DatasetErrorCode::CountMismatch, std::string(kFeatureRowsFileName), "row count differs from the manifest");
    }
    if (rows->hex_digest() != manifest.features.sha256) {
        return datasets::dataset_error(DatasetErrorCode::ChecksumMismatch, std::string(kFeatureRowsFileName), "checksum differs from the manifest");
    }
    if (counts.by_instrument != manifest.rows_by_instrument || counts.unavailable != manifest.unavailable) {
        return datasets::dataset_error(DatasetErrorCode::SummaryMismatch, std::string(kFeatureManifestFileName), "the rows do not match the manifest's counts");
    }
    return manifest;
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
    std::error_code inspection_error;
    if (!std::filesystem::is_directory(root, inspection_error)) {
        return datasets::dataset_error(DatasetErrorCode::CatalogUnavailable, "output", "must be an existing directory");
    }
    auto names = datasets::directory_entries(root);
    if (auto* error = std::get_if<DatasetError>(&names)) {
        return std::move(*error);
    }
    std::vector<FeatureArtifactEntry> entries;
    for (std::string& name : std::get<std::vector<std::string>>(names)) {
        std::variant<FeatureArtifactManifest, DatasetError> status = datasets::dataset_error(
            DatasetErrorCode::UnexpectedEntry,
            "output",
            "staging directory of an unfinished or interrupted write"
        );
        if (!name.starts_with(datasets::kStagingPrefix)) {
            status = open_artifact(root / name);
        }
        entries.push_back(FeatureArtifactEntry{.name = std::move(name), .status = std::move(status)});
    }
    return entries;
}

}  // namespace chronos::features
