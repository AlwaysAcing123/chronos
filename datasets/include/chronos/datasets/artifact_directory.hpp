#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/sha256.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Immutable, atomically published artifact directories: a normalized
// dataset or a feature artifact is written into a private staging
// directory beside its destination, made read-only, and renamed into place
// only if nothing already has its name. A reader therefore sees either a
// complete artifact or none; a crash leaves only a staging directory, which
// is never mistaken for an artifact. Nothing here ever replaces, follows,
// or deletes an entry it did not create.
namespace chronos::datasets {

// Staging directories start with this prefix, so a catalog can tell an
// interrupted write from an artifact.
inline constexpr std::string_view kStagingPrefix = ".chronos-staging-";

enum class EntryKind { Missing, Directory, RegularFile, Symlink, Other };

// What is at path, without following a symlink there.
[[nodiscard]] std::variant<EntryKind, DatasetError> inspect_entry(
    const std::filesystem::path& path
);

// The names in a directory in byte order, excluding "." and "..".
[[nodiscard]] std::variant<std::vector<std::string>, DatasetError> directory_entries(
    const std::filesystem::path& directory
);

// A whole regular file of at most maximum_bytes, never through a symlink.
[[nodiscard]] std::variant<std::string, DatasetError> read_small_file(
    const std::filesystem::path& path,
    std::uint64_t maximum_bytes,
    std::string_view field
);

// A file being written into a staging directory, hashed and counted as it
// is written so it never needs to be read back.
class StagedFile final {
public:
    StagedFile(int descriptor, std::string name);
    ~StagedFile();

    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;

    [[nodiscard]] std::optional<DatasetError> write(std::string_view bytes);
    // Flushes, synchronizes, and closes the file.
    [[nodiscard]] std::variant<FileDigest, DatasetError> finish();

private:
    [[nodiscard]] std::optional<DatasetError> flush();

    int descriptor_;
    std::string name_;
    std::string buffer_;
    Sha256 hash_;
    std::uint64_t size_bytes_{0};
};

enum class PublicationResult {
    Published,
    // Something already has the destination's name; it was not touched.
    DestinationExists,
};

class StagingDirectory final {
public:
    // Creates a unique staging directory inside root, which must be an
    // existing directory.
    [[nodiscard]] static std::variant<std::unique_ptr<StagingDirectory>, DatasetError>
    create(const std::filesystem::path& root);

    // Removes the staging directory and the files it created, unless it was
    // published.
    ~StagingDirectory();

    StagingDirectory(const StagingDirectory&) = delete;
    StagingDirectory& operator=(const StagingDirectory&) = delete;

    [[nodiscard]] std::variant<std::unique_ptr<StagedFile>, DatasetError> create_file(
        std::string_view name
    );
    [[nodiscard]] std::variant<FileDigest, DatasetError> write_file(
        std::string_view name,
        std::string_view contents
    );

    // Makes every file read-only, synchronizes the files and the directory,
    // renames the directory to root/name without replacing anything there,
    // and then makes the directory read-only too. Once the rename succeeds
    // the artifact is published: a failure of the later steps is reported by
    // warning(), never as an error.
    [[nodiscard]] std::variant<PublicationResult, DatasetError> publish(std::string_view name);

    // Why the published directory may not be read-only or durable yet.
    [[nodiscard]] const std::optional<DatasetError>& warning() const noexcept { return warning_; }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    StagingDirectory(std::filesystem::path root, std::filesystem::path path);

    std::filesystem::path root_;
    std::filesystem::path path_;
    std::vector<std::string> files_;
    bool published_{false};
    std::optional<DatasetError> warning_;
};

}  // namespace chronos::datasets
