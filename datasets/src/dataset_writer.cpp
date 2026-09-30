#include "chronos/datasets/dataset_writer.hpp"

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_quality.hpp"
#include "chronos/datasets/dataset_reader.hpp"
#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/sha256.hpp"
#include "stream_contract.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::datasets {
namespace {

std::optional<DatasetError> check_source(const DatasetSource& source) {
    const auto invalid = [](std::string field, std::string message) {
        return dataset_error(DatasetErrorCode::InvalidArgument, "source." + std::move(field), std::move(message));
    };
    for (const auto& [field, value] : {
             std::pair<std::string_view, const std::string*>{"provider", &source.provider},
             {"provider_dataset", &source.provider_dataset},
             {"provider_schema", &source.provider_schema},
             {"normalizer", &source.normalizer},
             {"normalizer_version", &source.normalizer_version},
         }) {
        if (value->empty()) {
            return invalid(std::string(field), "must not be empty");
        }
    }
    for (const std::string* text : {
             &source.provider, &source.provider_dataset, &source.provider_schema,
             &source.description, &source.normalizer, &source.normalizer_version,
         }) {
        if (!canonical_json::is_valid_utf8(*text)) {
            return invalid("text", "must be valid UTF-8");
        }
    }
    if (!is_sha256_hex(source.recording_sha256)) {
        return invalid("recording_sha256", "must be 64 lowercase hexadecimal digits");
    }
    if (source.coverage) {
        if (source.coverage->end < source.coverage->start) {
            return invalid("coverage", "end must not precede start");
        }
        if (source.coverage->semantics.empty() ||
            !canonical_json::is_valid_utf8(source.coverage->semantics)) {
            return invalid("coverage.semantics", "must be non-empty UTF-8");
        }
    }
    return std::nullopt;
}

}  // namespace

class NormalizedMarketDatasetWriter::Impl final {
public:
    Impl(std::unique_ptr<StagingDirectory> staging, std::unique_ptr<StagedFile> events, MarketDataStreamId stream)
        : staging(std::move(staging)),
          events(std::move(events)),
          stream(stream),
          contract(std::move(stream), std::nullopt) {}

    std::optional<DatasetError> fail(DatasetError error) {
        failure = error;
        return error;
    }

    std::optional<DatasetError> append(const ValidatedMarketEvent& event) {
        if (failure) {
            return failure;
        }
        if (finished) {
            return fail(dataset_error(DatasetErrorCode::InvalidArgument, "writer", "the dataset is already finished"));
        }
        const std::uint64_t line = appended + 1;
        if (auto error = contract.accept(event, line)) {
            return fail(std::move(*error));
        }
        auto record = encode_market_event(event);
        if (auto* error = std::get_if<DatasetError>(&record)) {
            error->line = line;
            return fail(std::move(*error));
        }
        std::string& text = std::get<std::string>(record);
        if (text.size() > kMaximumRecordBytes) {
            return fail(dataset_error(
                DatasetErrorCode::LineTooLong,
                "event",
                "the event's record exceeds the format's maximum length",
                line
            ));
        }
        text.push_back('\n');
        if (auto error = events->write(text)) {
            return fail(std::move(*error));
        }
        if (auto error = summary.add(event)) {
            return fail(std::move(*error));
        }
        if (auto error = quality.observe(event)) {
            return fail(std::move(*error));
        }
        ++appended;
        return std::nullopt;
    }

    std::variant<PublishedDataset, DatasetError> finish(DatasetSource source) {
        if (failure) {
            return *failure;
        }
        if (finished) {
            return *fail(dataset_error(DatasetErrorCode::InvalidArgument, "writer", "the dataset is already finished"));
        }
        finished = true;
        if (auto error = check_source(source)) {
            return *fail(std::move(*error));
        }
        if (!summary.summary()) {
            return *fail(dataset_error(
                DatasetErrorCode::EmptyDataset,
                "events",
                "a dataset must hold at least one event"
            ));
        }
        auto events_digest = events->finish();
        if (auto* error = std::get_if<DatasetError>(&events_digest)) {
            return *fail(std::move(*error));
        }
        const std::string quality_bytes = quality.serialize();
        if (quality_bytes.size() > kMaximumQualityBytes) {
            return *fail(dataset_error(
                DatasetErrorCode::SizeMismatch,
                std::string(kQualityFileName),
                "the quality report exceeds the format's maximum size"
            ));
        }
        auto quality_digest = staging->write_file(kQualityFileName, quality_bytes);
        if (auto* error = std::get_if<DatasetError>(&quality_digest)) {
            return *fail(std::move(*error));
        }
        DatasetManifest manifest{
            .dataset_id = {},
            .stream = stream,
            .source = std::move(source),
            .summary = *summary.summary(),
            .events = std::get<FileDigest>(std::move(events_digest)),
            .quality = std::get<FileDigest>(std::move(quality_digest)),
        };
        manifest.dataset_id = dataset_identity(manifest);
        const std::string manifest_bytes = serialize_manifest(manifest);
        if (manifest_bytes.size() > kMaximumManifestBytes) {
            return *fail(dataset_error(
                DatasetErrorCode::SizeMismatch,
                std::string(kManifestFileName),
                "the manifest exceeds the format's maximum size"
            ));
        }
        auto manifest_digest = staging->write_file(kManifestFileName, manifest_bytes);
        if (auto* error = std::get_if<DatasetError>(&manifest_digest)) {
            return *fail(std::move(*error));
        }

        const std::filesystem::path directory = staging->root() / manifest.dataset_id;
        auto published = staging->publish(manifest.dataset_id);
        if (auto* error = std::get_if<DatasetError>(&published)) {
            return *fail(std::move(*error));
        }
        if (std::get<PublicationResult>(published) == PublicationResult::Published) {
            return PublishedDataset{
                .manifest = std::move(manifest),
                .directory = directory,
                .outcome = PublicationOutcome::Published,
                .warning = staging->warning(),
            };
        }
        // Something already has this ID. It is this dataset only if it
        // verifies completely and its manifest is byte-identical.
        auto existing = verify_dataset(directory);
        if (auto* error = std::get_if<DatasetError>(&existing)) {
            return *fail(dataset_error(
                DatasetErrorCode::Collision,
                "dataset",
                "an entry named " + manifest.dataset_id +
                    " already exists and is not a valid dataset (" +
                    std::string(error_code_name(error->code)) + ": " + error->message + "); it was left untouched"
            ));
        }
        if (serialize_manifest(std::get<DatasetManifest>(existing)) != manifest_bytes) {
            return *fail(dataset_error(
                DatasetErrorCode::Collision,
                "dataset",
                "a different dataset is already published as " + manifest.dataset_id
            ));
        }
        return PublishedDataset{
            .manifest = std::move(manifest),
            .directory = directory,
            .outcome = PublicationOutcome::AlreadyPresent,
            .warning = std::nullopt,
        };
    }

    std::unique_ptr<StagingDirectory> staging;
    std::unique_ptr<StagedFile> events;
    MarketDataStreamId stream;
    detail::StreamContract contract;
    DatasetSummaryBuilder summary;
    DatasetQualityReport quality;
    std::uint64_t appended{0};
    std::optional<DatasetError> failure;
    bool finished{false};
};

NormalizedMarketDatasetWriter::NormalizedMarketDatasetWriter(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

NormalizedMarketDatasetWriter::~NormalizedMarketDatasetWriter() = default;

std::variant<std::unique_ptr<NormalizedMarketDatasetWriter>, DatasetError>
NormalizedMarketDatasetWriter::create(const std::filesystem::path& catalog, MarketDataStreamId stream) {
    if (!canonical_json::is_valid_utf8(stream.value())) {
        return dataset_error(DatasetErrorCode::InvalidArgument, "stream", "must be valid UTF-8");
    }
    auto staging = StagingDirectory::create(catalog);
    if (auto* error = std::get_if<DatasetError>(&staging)) {
        return std::move(*error);
    }
    auto& directory = std::get<std::unique_ptr<StagingDirectory>>(staging);
    auto events = directory->create_file(kEventsFileName);
    if (auto* error = std::get_if<DatasetError>(&events)) {
        return std::move(*error);
    }
    return std::unique_ptr<NormalizedMarketDatasetWriter>(new NormalizedMarketDatasetWriter(
        std::make_unique<Impl>(
            std::move(directory),
            std::get<std::unique_ptr<StagedFile>>(std::move(events)),
            std::move(stream)
        )
    ));
}

std::optional<DatasetError> NormalizedMarketDatasetWriter::append(const ValidatedMarketEvent& event) {
    return implementation_->append(event);
}

std::variant<PublishedDataset, DatasetError> NormalizedMarketDatasetWriter::finish(DatasetSource source) {
    return implementation_->finish(std::move(source));
}

}  // namespace chronos::datasets
