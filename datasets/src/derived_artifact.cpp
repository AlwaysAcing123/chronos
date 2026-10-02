#include "chronos/datasets/derived_artifact.hpp"

#include "chronos/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::datasets {

namespace json = chronos::canonical_json;

std::variant<DerivedArtifactFiles, DatasetError> open_derived_artifact(
    const std::filesystem::path& given,
    const DerivedArtifactLayout& layout
) {
    // "root/<id>/" names the same directory as "root/<id>".
    const std::filesystem::path directory = given.filename().empty() ? given.parent_path() : given;
    auto kind = inspect_entry(directory);
    if (auto* error = std::get_if<DatasetError>(&kind)) {
        return std::move(*error);
    }
    const auto entry = std::get<EntryKind>(kind);
    if (entry == EntryKind::Symlink) {
        return dataset_error(DatasetErrorCode::SymlinkRejected, "artifact", "is a symlink, which is never followed");
    }
    if (entry != EntryKind::Directory) {
        return dataset_error(
            entry == EntryKind::Missing ? DatasetErrorCode::NotFound : DatasetErrorCode::UnexpectedEntry,
            "artifact",
            "is not a " + std::string(layout.kind) + " directory"
        );
    }
    std::string name = directory.filename().string();
    if (!is_sha256_hex(name)) {
        return dataset_error(
            DatasetErrorCode::UnexpectedEntry,
            "artifact",
            "a " + std::string(layout.kind) + " directory is named by its 64-digit lowercase artifact ID"
        );
    }
    auto entries = directory_entries(directory);
    if (auto* error = std::get_if<DatasetError>(&entries)) {
        return std::move(*error);
    }
    const std::vector<std::string_view> expected = {layout.rows_file, kArtifactManifestFileName};
    for (const std::string& file : std::get<std::vector<std::string>>(entries)) {
        if (std::find(expected.begin(), expected.end(), file) == expected.end()) {
            return dataset_error(DatasetErrorCode::UnexpectedEntry, file, "is not part of a " + std::string(layout.kind));
        }
    }
    for (const std::string_view file : expected) {
        auto file_kind = inspect_entry(directory / file);
        if (auto* error = std::get_if<DatasetError>(&file_kind)) {
            return std::move(*error);
        }
        const auto found = std::get<EntryKind>(file_kind);
        if (found != EntryKind::RegularFile) {
            return dataset_error(
                found == EntryKind::Symlink ? DatasetErrorCode::SymlinkRejected
                    : found == EntryKind::Missing ? DatasetErrorCode::NotFound
                    : DatasetErrorCode::UnexpectedEntry,
                std::string(file),
                "must be a regular file"
            );
        }
    }
    auto manifest = read_small_file(directory / kArtifactManifestFileName, kMaximumManifestBytes, kArtifactManifestFileName);
    if (auto* error = std::get_if<DatasetError>(&manifest)) {
        return std::move(*error);
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(directory / layout.rows_file, size_error);
    if (size_error) {
        return dataset_error(DatasetErrorCode::ReadFailed, std::string(layout.rows_file), "could not read its size");
    }
    return DerivedArtifactFiles{
        .directory = directory,
        .id = std::move(name),
        .manifest = std::get<std::string>(std::move(manifest)),
        .rows_size_bytes = size,
    };
}

std::optional<DatasetError> check_derived_artifact_manifest(
    const DerivedArtifactFiles& files,
    const DerivedArtifactLayout& layout,
    std::string_view artifact_id,
    const FileDigest& rows
) {
    if (artifact_id != files.id) {
        return dataset_error(
            DatasetErrorCode::IdentityMismatch,
            "artifact",
            "directory name differs from the manifest's artifact ID"
        );
    }
    if (files.rows_size_bytes != rows.size_bytes) {
        return dataset_error(DatasetErrorCode::SizeMismatch, std::string(layout.rows_file), "size differs from the manifest");
    }
    return std::nullopt;
}

DerivedArtifactRows::DerivedArtifactRows(
    std::unique_ptr<NdjsonFileReader> file,
    std::string_view rows_file,
    std::uint64_t rows,
    std::string sha256
)
    : file_(std::move(file)), rows_file_(rows_file), expected_rows_(rows), expected_sha256_(std::move(sha256)) {}

std::variant<std::unique_ptr<DerivedArtifactRows>, DatasetError> DerivedArtifactRows::open(
    const std::filesystem::path& directory,
    const DerivedArtifactLayout& layout,
    std::uint64_t rows,
    const FileDigest& digest
) {
    auto file = NdjsonFileReader::open(directory / layout.rows_file, std::string(layout.rows_file), kMaximumRecordBytes);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    return std::unique_ptr<DerivedArtifactRows>(new DerivedArtifactRows(
        std::get<std::unique_ptr<NdjsonFileReader>>(std::move(file)),
        layout.rows_file,
        rows,
        digest.sha256
    ));
}

std::variant<std::string_view, EndOfRows, DatasetError> DerivedArtifactRows::next() {
    auto next = file_->next();
    if (auto* error = std::get_if<DatasetError>(&next)) {
        return std::move(*error);
    }
    if (const auto* record = std::get_if<std::string_view>(&next)) {
        return *record;
    }
    if (file_->line_number() != expected_rows_) {
        return dataset_error(DatasetErrorCode::CountMismatch, rows_file_, "row count differs from the manifest");
    }
    if (file_->hex_digest() != expected_sha256_) {
        return dataset_error(DatasetErrorCode::ChecksumMismatch, rows_file_, "checksum differs from the manifest");
    }
    return EndOfRows{};
}

std::optional<DatasetError> write_record(
    StagedFile& rows,
    std::string_view record,
    std::string_view kind,
    std::uint64_t line
) {
    if (record.size() > kMaximumRecordBytes) {
        return dataset_error(
            DatasetErrorCode::LineTooLong,
            "row",
            "a " + std::string(kind) + " row exceeds the format's maximum length",
            line
        );
    }
    if (auto error = rows.write(record)) {
        return error;
    }
    return rows.write("\n");
}

std::variant<PublishedArtifactDirectory, DatasetError> publish_derived_artifact(
    StagingDirectory& staging,
    const DerivedArtifactLayout& layout,
    const std::string& id,
    const std::string& manifest,
    const VerifiedManifestBytes& verified_manifest
) {
    if (manifest.size() > kMaximumManifestBytes) {
        return dataset_error(
            DatasetErrorCode::SizeMismatch,
            std::string(kArtifactManifestFileName),
            "the manifest exceeds the format's maximum size"
        );
    }
    auto written = staging.write_file(kArtifactManifestFileName, manifest);
    if (auto* error = std::get_if<DatasetError>(&written)) {
        return std::move(*error);
    }
    const std::filesystem::path destination = staging.root() / id;
    auto published = staging.publish(id);
    if (auto* error = std::get_if<DatasetError>(&published)) {
        return std::move(*error);
    }
    if (std::get<PublicationResult>(published) == PublicationResult::Published) {
        return PublishedArtifactDirectory{
            .directory = destination,
            .outcome = PublicationOutcome::Published,
            .warning = staging.warning(),
        };
    }
    auto existing = verified_manifest(destination);
    if (auto* error = std::get_if<DatasetError>(&existing)) {
        return dataset_error(
            DatasetErrorCode::Collision,
            "artifact",
            "an entry named " + id + " already exists and is not a valid " + std::string(layout.kind) + " (" +
                std::string(error_code_name(error->code)) + ": " + error->message + "); it was left untouched"
        );
    }
    if (std::get<std::string>(existing) != manifest) {
        return dataset_error(
            DatasetErrorCode::Collision,
            "artifact",
            "a different " + std::string(layout.kind) + " is already published as " + id
        );
    }
    return PublishedArtifactDirectory{
        .directory = destination,
        .outcome = PublicationOutcome::AlreadyPresent,
        .warning = std::nullopt,
    };
}

}  // namespace chronos::datasets
