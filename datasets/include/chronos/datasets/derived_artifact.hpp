#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/datasets/json_fields.hpp"
#include "chronos/datasets/ndjson_file.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The shape every artifact derived from a normalized dataset shares:
// feature, label, and split artifacts. Each is a directory named by its
// artifact ID, the SHA-256 of its canonical manifest without the ID, that
// holds exactly two regular files:
//
//     <root>/<artifact-id>/manifest.json
//     <root>/<artifact-id>/<rows-file>.ndjson
//
// Each kind defines its own rows and manifest; this layer checks the
// directory, streams the rows against the manifest's count and checksum,
// and publishes atomically without ever replacing an entry.
namespace chronos::datasets {

inline constexpr std::string_view kArtifactManifestFileName = "manifest.json";

struct DerivedArtifactLayout {
    // For messages, such as "feature artifact".
    std::string_view kind;
    // Such as "features.ndjson".
    std::string_view rows_file;
};

// An artifact directory whose structure is valid: named by a 64-digit
// lowercase ID and holding exactly its two regular files, never through a
// symlink.
struct DerivedArtifactFiles {
    // Without a trailing slash.
    std::filesystem::path directory;
    // The directory's name.
    std::string id;
    std::string manifest;
    std::uint64_t rows_size_bytes{0};
};

[[nodiscard]] std::variant<DerivedArtifactFiles, DatasetError> open_derived_artifact(
    const std::filesystem::path& directory,
    const DerivedArtifactLayout& layout
);

// Requires a parsed manifest to name its directory and its rows file's
// size.
[[nodiscard]] std::optional<DatasetError> check_derived_artifact_manifest(
    const DerivedArtifactFiles& files,
    const DerivedArtifactLayout& layout,
    std::string_view artifact_id,
    const FileDigest& rows
);

template <typename Manifest>
struct OpenedArtifact {
    Manifest manifest;
    // Without a trailing slash.
    std::filesystem::path directory;
};

// Opens an artifact directory and parses its manifest with `parse`, which
// returns std::variant<Manifest, DatasetError>. The manifest's artifact_id
// must name the directory, and its `rows` digest must give the rows file's
// size; the rows themselves are not read.
template <typename Manifest, typename Parse>
[[nodiscard]] std::variant<OpenedArtifact<Manifest>, DatasetError> open_artifact_manifest(
    const std::filesystem::path& directory,
    const DerivedArtifactLayout& layout,
    Parse parse,
    FileDigest Manifest::*rows
) {
    auto opened = open_derived_artifact(directory, layout);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    auto& files = std::get<DerivedArtifactFiles>(opened);
    std::variant<Manifest, DatasetError> parsed = parse(files.manifest);
    if (auto* error = std::get_if<DatasetError>(&parsed)) {
        return std::move(*error);
    }
    auto& manifest = std::get<Manifest>(parsed);
    if (auto error = check_derived_artifact_manifest(files, layout, manifest.artifact_id, manifest.*rows)) {
        return std::move(*error);
    }
    return OpenedArtifact<Manifest>{.manifest = std::move(manifest), .directory = std::move(files.directory)};
}

struct EndOfRows {};

// Streams an artifact's rows file in bounded memory. The end is reported
// only once the file held exactly the manifest's row count and checksum.
class DerivedArtifactRows final {
public:
    [[nodiscard]] static std::variant<std::unique_ptr<DerivedArtifactRows>, DatasetError> open(
        const std::filesystem::path& directory,
        const DerivedArtifactLayout& layout,
        std::uint64_t rows,
        const FileDigest& digest
    );

    // The next record without its newline, valid until the next call.
    [[nodiscard]] std::variant<std::string_view, EndOfRows, DatasetError> next();

    // The one-based line of the last record returned.
    [[nodiscard]] std::uint64_t line_number() const noexcept { return file_->line_number(); }

private:
    DerivedArtifactRows(
        std::unique_ptr<NdjsonFileReader> file,
        std::string_view rows_file,
        std::uint64_t rows,
        std::string sha256
    );

    std::unique_ptr<NdjsonFileReader> file_;
    std::string rows_file_;
    std::uint64_t expected_rows_;
    std::string expected_sha256_;
};

struct PublishedArtifactDirectory {
    std::filesystem::path directory;
    PublicationOutcome outcome;
    // Published, but a step after the atomic rename failed.
    std::optional<DatasetError> warning;
};

// Returns the canonical manifest of the fully verified artifact at a path,
// or why it is not one.
using VerifiedManifestBytes =
    std::function<std::variant<std::string, DatasetError>(const std::filesystem::path&)>;

// Writes the manifest into a staging directory that already holds the rows
// file, and publishes it as <root>/<id>. An existing entry with that name is
// never touched: the outcome is AlreadyPresent only when it verifies and
// its manifest is byte-identical, and Collision otherwise.
[[nodiscard]] std::variant<PublishedArtifactDirectory, DatasetError> publish_derived_artifact(
    StagingDirectory& staging,
    const DerivedArtifactLayout& layout,
    const std::string& id,
    const std::string& manifest,
    const VerifiedManifestBytes& verified_manifest
);

template <typename Manifest>
struct PublishedArtifact {
    Manifest manifest;
    std::filesystem::path directory;
    PublicationOutcome outcome;
    // Published, but a step after the atomic rename failed.
    std::optional<DatasetError> warning;
};

// Appends a record and its newline to a rows file, refusing a record longer
// than every reader accepts. `line` is the record's one-based line.
[[nodiscard]] std::optional<DatasetError> write_record(
    StagedFile& rows,
    std::string_view record,
    std::string_view kind,
    std::uint64_t line
);

// Creates an artifact in root: a staged rows file that write_rows(StagedFile&)
// fills, then the manifest that manifest_for(FileDigest) builds from the
// rows' digest, artifact_id included, published together by
// publish_derived_artifact. An existing artifact with the same ID is
// recognized through verify(path), which returns its verified manifest.
// Nothing is published if any step fails.
template <typename Manifest, typename WriteRows, typename ManifestFor, typename Serialize, typename Verify>
[[nodiscard]] std::variant<PublishedArtifact<Manifest>, DatasetError> stage_and_publish(
    const std::filesystem::path& root,
    const DerivedArtifactLayout& layout,
    WriteRows write_rows,
    ManifestFor manifest_for,
    Serialize serialize,
    Verify verify
) {
    auto staging = StagingDirectory::create(root);
    if (auto* error = std::get_if<DatasetError>(&staging)) {
        return std::move(*error);
    }
    auto& directory = std::get<std::unique_ptr<StagingDirectory>>(staging);
    auto file = directory->create_file(layout.rows_file);
    if (auto* error = std::get_if<DatasetError>(&file)) {
        return std::move(*error);
    }
    auto& rows = std::get<std::unique_ptr<StagedFile>>(file);
    if (std::optional<DatasetError> error = write_rows(*rows)) {
        return std::move(*error);
    }
    auto digest = rows->finish();
    if (auto* error = std::get_if<DatasetError>(&digest)) {
        return std::move(*error);
    }
    std::variant<Manifest, DatasetError> built = manifest_for(std::get<FileDigest>(std::move(digest)));
    if (auto* error = std::get_if<DatasetError>(&built)) {
        return std::move(*error);
    }
    Manifest manifest = std::get<Manifest>(std::move(built));
    auto published = publish_derived_artifact(
        *directory,
        layout,
        manifest.artifact_id,
        serialize(manifest),
        [&](const std::filesystem::path& existing) -> std::variant<std::string, DatasetError> {
            std::variant<Manifest, DatasetError> verified = verify(existing);
            if (auto* error = std::get_if<DatasetError>(&verified)) {
                return std::move(*error);
            }
            return serialize(std::get<Manifest>(verified));
        }
    );
    if (auto* error = std::get_if<DatasetError>(&published)) {
        return std::move(*error);
    }
    auto& result = std::get<PublishedArtifactDirectory>(published);
    return PublishedArtifact<Manifest>{
        .manifest = std::move(manifest),
        .directory = std::move(result.directory),
        .outcome = result.outcome,
        .warning = std::move(result.warning),
    };
}

template <typename Manifest>
struct DerivedArtifactEntry {
    std::string name;
    std::variant<Manifest, DatasetError> status;
};

// Every entry of an artifact root in byte order, each with its manifest as
// open_artifact_manifest returns it, or why it is not an artifact. Staging
// directories are reported as unfinished writes, never opened.
template <typename Manifest, typename Parse>
[[nodiscard]] std::variant<std::vector<DerivedArtifactEntry<Manifest>>, DatasetError> list_derived_artifacts(
    const std::filesystem::path& root,
    std::string_view field,
    const DerivedArtifactLayout& layout,
    Parse parse,
    FileDigest Manifest::*rows
) {
    std::error_code inspection_error;
    if (!std::filesystem::is_directory(root, inspection_error)) {
        return dataset_error(DatasetErrorCode::CatalogUnavailable, std::string(field), "must be an existing directory");
    }
    auto names = directory_entries(root);
    if (auto* error = std::get_if<DatasetError>(&names)) {
        return std::move(*error);
    }
    std::vector<DerivedArtifactEntry<Manifest>> entries;
    for (std::string& name : std::get<std::vector<std::string>>(names)) {
        std::variant<Manifest, DatasetError> status = dataset_error(
            DatasetErrorCode::UnexpectedEntry,
            std::string(field),
            "staging directory of an unfinished or interrupted write"
        );
        if (!name.starts_with(kStagingPrefix)) {
            auto opened = open_artifact_manifest(root / name, layout, parse, rows);
            if (auto* error = std::get_if<DatasetError>(&opened)) {
                status = std::move(*error);
            } else {
                status = std::move(std::get<OpenedArtifact<Manifest>>(opened).manifest);
            }
        }
        entries.push_back(DerivedArtifactEntry<Manifest>{.name = std::move(name), .status = std::move(status)});
    }
    return entries;
}

}  // namespace chronos::datasets
