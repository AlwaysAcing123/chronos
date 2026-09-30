#include "chronos/datasets/dataset_reader.hpp"

#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_quality.hpp"
#include "chronos/datasets/market_event_codec.hpp"
#include "chronos/datasets/ndjson_file.hpp"
#include "chronos/sha256.hpp"
#include "stream_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::datasets {
namespace {

std::optional<DatasetError> require_entry(
    const std::filesystem::path& path,
    EntryKind expected,
    std::string_view field
) {
    auto inspected = inspect_entry(path);
    if (auto* error = std::get_if<DatasetError>(&inspected)) {
        return std::move(*error);
    }
    const EntryKind kind = std::get<EntryKind>(inspected);
    if (kind == expected) {
        return std::nullopt;
    }
    if (kind == EntryKind::Missing) {
        return dataset_error(DatasetErrorCode::NotFound, std::string(field), "does not exist");
    }
    if (kind == EntryKind::Symlink) {
        return dataset_error(DatasetErrorCode::SymlinkRejected, std::string(field), "is a symlink, which is never followed");
    }
    return dataset_error(
        DatasetErrorCode::UnexpectedEntry,
        std::string(field),
        expected == EntryKind::Directory ? "is not a directory" : "is not a regular file"
    );
}

}  // namespace

std::variant<OpenedDataset, DatasetError> open_dataset(const std::filesystem::path& given) {
    // "catalog/<id>/" names the same directory as "catalog/<id>".
    const std::filesystem::path directory = given.filename().empty() ? given.parent_path() : given;
    if (auto error = require_entry(directory, EntryKind::Directory, "dataset")) {
        return std::move(*error);
    }
    const std::string name = directory.filename().string();
    if (!is_sha256_hex(name)) {
        return dataset_error(
            DatasetErrorCode::UnexpectedEntry,
            "dataset",
            "a dataset directory is named by its 64-digit lowercase dataset ID"
        );
    }
    auto entries = directory_entries(directory);
    if (auto* error = std::get_if<DatasetError>(&entries)) {
        return std::move(*error);
    }
    constexpr std::array<std::string_view, 3> kFiles = {
        kEventsFileName, kManifestFileName, kQualityFileName,
    };
    for (const std::string& entry : std::get<std::vector<std::string>>(entries)) {
        if (std::find(kFiles.begin(), kFiles.end(), entry) == kFiles.end()) {
            return dataset_error(
                DatasetErrorCode::UnexpectedEntry,
                entry,
                "is not part of a normalized dataset"
            );
        }
    }
    for (const std::string_view file : kFiles) {
        if (auto error = require_entry(directory / file, EntryKind::RegularFile, file)) {
            return std::move(*error);
        }
    }

    auto manifest_bytes = read_small_file(directory / kManifestFileName, kMaximumManifestBytes, kManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&manifest_bytes)) {
        return std::move(*error);
    }
    auto parsed = parse_manifest(std::get<std::string>(manifest_bytes));
    if (auto* error = std::get_if<DatasetError>(&parsed)) {
        return std::move(*error);
    }
    DatasetManifest manifest = std::get<DatasetManifest>(std::move(parsed));
    if (manifest.dataset_id != name) {
        return dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "dataset",
            "directory name differs from the manifest's dataset ID"
        );
    }

    auto quality = read_small_file(directory / kQualityFileName, kMaximumQualityBytes, kQualityFileName);
    if (auto* error = std::get_if<DatasetError>(&quality)) {
        return std::move(*error);
    }
    const std::string& quality_bytes = std::get<std::string>(quality);
    if (quality_bytes.size() != manifest.quality.size_bytes) {
        return dataset_error(DatasetErrorCode::SizeMismatch, std::string(kQualityFileName), "size differs from the manifest");
    }
    if (Sha256::hex(quality_bytes) != manifest.quality.sha256) {
        return dataset_error(DatasetErrorCode::ChecksumMismatch, std::string(kQualityFileName), "checksum differs from the manifest");
    }

    std::error_code size_error;
    const auto events_size = std::filesystem::file_size(directory / kEventsFileName, size_error);
    if (size_error) {
        return dataset_error(DatasetErrorCode::ReadFailed, std::string(kEventsFileName), "could not inspect file");
    }
    if (events_size != manifest.events.size_bytes) {
        return dataset_error(DatasetErrorCode::SizeMismatch, std::string(kEventsFileName), "size differs from the manifest");
    }
    return OpenedDataset{.manifest = std::move(manifest), .quality_report = quality_bytes};
}

class NormalizedMarketEventReader::Impl final {
public:
    Impl(OpenedDataset opened, std::unique_ptr<NdjsonFileReader> events)
        : manifest(std::move(opened.manifest)),
          quality_report(std::move(opened.quality_report)),
          events(std::move(events)),
          contract(manifest.stream, manifest.summary.first_ingest_sequence) {
        for (const auto& entry : manifest.summary.by_instrument) {
            instruments.insert(entry.first);
        }
    }

    MarketEventRead fail(DatasetError failure) {
        MarketEventReadError read_error{.message = std::string(kEventsFileName) + ": " + failure.message};
        if (failure.line) {
            read_error.message = std::string(kEventsFileName) + " line " +
                std::to_string(*failure.line) + ": " + failure.message;
        }
        error = std::move(failure);
        done = true;
        return read_error;
    }

    MarketEventRead finish() {
        const DatasetSummary& expected = manifest.summary;
        if (events->line_number() != expected.events) {
            return fail(dataset_error(
                DatasetErrorCode::CountMismatch,
                std::string(kEventsFileName),
                "holds " + std::to_string(events->line_number()) + " events; the manifest records " +
                    std::to_string(expected.events)
            ));
        }
        if (events->bytes_read() != manifest.events.size_bytes ||
            events->hex_digest() != manifest.events.sha256) {
            return fail(dataset_error(
                DatasetErrorCode::ChecksumMismatch,
                std::string(kEventsFileName),
                "checksum differs from the manifest"
            ));
        }
        if (!summary.summary() || *summary.summary() != expected) {
            return fail(dataset_error(
                DatasetErrorCode::SummaryMismatch,
                std::string(kManifestFileName),
                "the events do not match the manifest's counts, sequences, or time range"
            ));
        }
        if (quality.serialize() != quality_report) {
            return fail(dataset_error(
                DatasetErrorCode::QualityMismatch,
                std::string(kQualityFileName),
                "differs from the report the events produce"
            ));
        }
        done = true;
        completed = true;
        return EndOfMarketEvents{.source_sha256 = manifest.events.sha256};
    }

    MarketEventRead read_next() {
        if (done) {
            return MarketEventReadError{.message = "the dataset reader has already finished"};
        }
        auto next = events->next();
        if (auto* failure = std::get_if<DatasetError>(&next)) {
            return fail(std::move(*failure));
        }
        if (std::holds_alternative<EndOfLines>(next)) {
            return finish();
        }
        const std::uint64_t line = events->line_number();
        if (line > manifest.summary.events) {
            return fail(dataset_error(
                DatasetErrorCode::CountMismatch,
                std::string(kEventsFileName),
                "holds more events than the manifest records",
                line
            ));
        }
        auto decoded = decode_market_event(std::get<std::string_view>(next), line);
        if (auto* failure = std::get_if<DatasetError>(&decoded)) {
            return fail(std::move(*failure));
        }
        ValidatedMarketEvent event = std::get<ValidatedMarketEvent>(std::move(decoded));
        if (auto failure = contract.accept(event, line)) {
            return fail(std::move(*failure));
        }
        if (!instruments.contains(market_event_header(event.value()).instrument.value())) {
            return fail(dataset_error(
                DatasetErrorCode::UnknownInstrument,
                "instrument",
                "is not listed in the manifest",
                line
            ));
        }
        if (auto failure = summary.add(event)) {
            return fail(std::move(*failure));
        }
        if (auto failure = quality.observe(event)) {
            return fail(std::move(*failure));
        }
        return event;
    }

    DatasetManifest manifest;
    std::string quality_report;
    std::unique_ptr<NdjsonFileReader> events;
    detail::StreamContract contract;
    std::set<std::string> instruments;
    DatasetSummaryBuilder summary;
    DatasetQualityReport quality;
    std::optional<DatasetError> error;
    bool done{false};
    bool completed{false};
};

NormalizedMarketEventReader::NormalizedMarketEventReader(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

NormalizedMarketEventReader::~NormalizedMarketEventReader() = default;

std::variant<std::unique_ptr<NormalizedMarketEventReader>, DatasetError>
NormalizedMarketEventReader::open(const std::filesystem::path& directory) {
    auto opened = open_dataset(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto events = NdjsonFileReader::open(
        directory / kEventsFileName,
        std::string(kEventsFileName),
        kMaximumRecordBytes
    );
    if (auto* error = std::get_if<DatasetError>(&events)) {
        return std::move(*error);
    }
    return std::unique_ptr<NormalizedMarketEventReader>(new NormalizedMarketEventReader(
        std::make_unique<Impl>(
            std::get<OpenedDataset>(std::move(opened)),
            std::get<std::unique_ptr<NdjsonFileReader>>(std::move(events))
        )
    ));
}

MarketEventRead NormalizedMarketEventReader::read_next() {
    return implementation_->read_next();
}

const DatasetManifest& NormalizedMarketEventReader::manifest() const noexcept {
    return implementation_->manifest;
}

const std::optional<DatasetError>& NormalizedMarketEventReader::error() const noexcept {
    return implementation_->error;
}

std::variant<DatasetManifest, DatasetError> verify_dataset(const std::filesystem::path& directory) {
    auto opened = NormalizedMarketEventReader::open(directory);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& reader = std::get<std::unique_ptr<NormalizedMarketEventReader>>(opened);
    while (true) {
        const MarketEventRead read = reader->read_next();
        if (std::holds_alternative<EndOfMarketEvents>(read)) {
            return reader->manifest();
        }
        if (std::holds_alternative<MarketEventReadError>(read)) {
            return *reader->error();
        }
    }
}

}  // namespace chronos::datasets
