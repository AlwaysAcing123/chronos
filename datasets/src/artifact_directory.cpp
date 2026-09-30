#include "chronos/datasets/artifact_directory.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chronos::datasets {
namespace {

constexpr std::size_t kWriteBufferBytes = 1U << 20U;

std::string system_message(int error_number) {
    return std::error_code(error_number, std::generic_category()).message();
}

DatasetError file_error(DatasetErrorCode code, std::string_view field, std::string_view action) {
    const int error_number = errno;
    return dataset_error(
        code,
        std::string(field),
        std::string(action) + ": " + system_message(error_number)
    );
}

class Descriptor final {
public:
    explicit Descriptor(int descriptor) : descriptor_(descriptor) {}
    ~Descriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

std::optional<DatasetError> synchronize_directory(const std::filesystem::path& directory) {
    const Descriptor descriptor(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (descriptor.get() < 0) {
        return file_error(DatasetErrorCode::WriteFailed, directory.filename().string(), "could not open directory");
    }
    if (::fsync(descriptor.get()) != 0) {
        return file_error(DatasetErrorCode::WriteFailed, directory.filename().string(), "could not synchronize directory");
    }
    return std::nullopt;
}

bool is_single_component(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
        name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

// rename(2) that fails with EEXIST instead of replacing the destination,
// including an empty directory or a symlink. There is no portable POSIX
// call; a platform without one refuses to publish rather than risk a
// replacement.
int rename_without_replacing(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__APPLE__)
    return ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL);
#elif defined(__linux__)
    return ::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE);
#else
    static_cast<void>(from);
    static_cast<void>(to);
    errno = ENOSYS;
    return -1;
#endif
}

std::uint64_t random_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32U) ^
        static_cast<std::uint64_t>(device()) ^
        counter.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

std::variant<EntryKind, DatasetError> inspect_entry(const std::filesystem::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) {
            return EntryKind::Missing;
        }
        return file_error(DatasetErrorCode::ReadFailed, path.filename().string(), "could not inspect entry");
    }
    if (S_ISLNK(status.st_mode)) {
        return EntryKind::Symlink;
    }
    if (S_ISDIR(status.st_mode)) {
        return EntryKind::Directory;
    }
    if (S_ISREG(status.st_mode)) {
        return EntryKind::RegularFile;
    }
    return EntryKind::Other;
}

std::variant<std::vector<std::string>, DatasetError> directory_entries(
    const std::filesystem::path& directory
) {
    // A catalog root may be reached through a symlink; the entries it holds
    // are checked with inspect_entry and never followed.
    const int descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return file_error(DatasetErrorCode::ReadFailed, directory.filename().string(), "could not open directory");
    }
    DIR* stream = ::fdopendir(descriptor);
    if (stream == nullptr) {
        auto error = file_error(DatasetErrorCode::ReadFailed, directory.filename().string(), "could not read directory");
        ::close(descriptor);
        return error;
    }
    std::vector<std::string> names;
    errno = 0;
    while (const dirent* entry = ::readdir(stream)) {
        const std::string_view name = entry->d_name;
        if (name != "." && name != "..") {
            names.emplace_back(name);
        }
        errno = 0;
    }
    const int read_error = errno;
    ::closedir(stream);
    if (read_error != 0) {
        errno = read_error;
        return file_error(DatasetErrorCode::ReadFailed, directory.filename().string(), "could not read directory");
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::variant<std::string, DatasetError> read_small_file(
    const std::filesystem::path& path,
    std::uint64_t maximum_bytes,
    std::string_view field
) {
    const Descriptor descriptor(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (descriptor.get() < 0) {
        if (errno == ELOOP) {
            return dataset_error(DatasetErrorCode::SymlinkRejected, std::string(field), "is a symlink");
        }
        if (errno == ENOENT) {
            return dataset_error(DatasetErrorCode::NotFound, std::string(field), "does not exist");
        }
        return file_error(DatasetErrorCode::ReadFailed, field, "could not open file");
    }
    struct stat status {};
    if (::fstat(descriptor.get(), &status) != 0) {
        return file_error(DatasetErrorCode::ReadFailed, field, "could not inspect file");
    }
    if (!S_ISREG(status.st_mode)) {
        return dataset_error(DatasetErrorCode::UnexpectedEntry, std::string(field), "is not a regular file");
    }
    if (static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
        return dataset_error(DatasetErrorCode::SizeMismatch, std::string(field), "is larger than its format allows");
    }
    std::string contents;
    char buffer[1U << 16U];
    while (true) {
        const ssize_t read = ::read(descriptor.get(), buffer, sizeof(buffer));
        if (read == 0) {
            break;
        }
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return file_error(DatasetErrorCode::ReadFailed, field, "could not read file");
        }
        contents.append(buffer, static_cast<std::size_t>(read));
        if (contents.size() > maximum_bytes) {
            return dataset_error(DatasetErrorCode::SizeMismatch, std::string(field), "is larger than its format allows");
        }
    }
    return contents;
}

StagedFile::StagedFile(int descriptor, std::string name)
    : descriptor_(descriptor), name_(std::move(name)) {
    buffer_.reserve(kWriteBufferBytes);
}

StagedFile::~StagedFile() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

std::optional<DatasetError> StagedFile::flush() {
    std::size_t offset = 0;
    while (offset < buffer_.size()) {
        const ssize_t written = ::write(descriptor_, buffer_.data() + offset, buffer_.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return file_error(DatasetErrorCode::WriteFailed, name_, "could not write file");
    }
    buffer_.clear();
    return std::nullopt;
}

std::optional<DatasetError> StagedFile::write(std::string_view bytes) {
    if (descriptor_ < 0) {
        return dataset_error(DatasetErrorCode::WriteFailed, name_, "file is already finished");
    }
    try {
        hash_.update(bytes);
    } catch (const std::length_error&) {
        return dataset_error(DatasetErrorCode::CounterOverflow, name_, "file is too large to hash");
    }
    size_bytes_ += bytes.size();
    buffer_.append(bytes);
    if (buffer_.size() >= kWriteBufferBytes) {
        return flush();
    }
    return std::nullopt;
}

std::variant<FileDigest, DatasetError> StagedFile::finish() {
    if (descriptor_ < 0) {
        return dataset_error(DatasetErrorCode::WriteFailed, name_, "file is already finished");
    }
    if (auto error = flush()) {
        return std::move(*error);
    }
    if (::fsync(descriptor_) != 0) {
        return file_error(DatasetErrorCode::WriteFailed, name_, "could not synchronize file");
    }
    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (::close(descriptor) != 0) {
        return file_error(DatasetErrorCode::WriteFailed, name_, "could not close file");
    }
    return FileDigest{.sha256 = hash_.hex_digest(), .size_bytes = size_bytes_};
}

StagingDirectory::StagingDirectory(std::filesystem::path root, std::filesystem::path path)
    : root_(std::move(root)), path_(std::move(path)) {}

std::variant<std::unique_ptr<StagingDirectory>, DatasetError> StagingDirectory::create(
    const std::filesystem::path& root
) {
    struct stat status {};
    if (::stat(root.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return dataset_error(
            DatasetErrorCode::CatalogUnavailable,
            "catalog",
            "catalog root must be an existing directory"
        );
    }
    constexpr int kAttempts = 16;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const std::filesystem::path path = root /
            (std::string(kStagingPrefix) + std::to_string(::getpid()) + "-" +
             std::to_string(random_suffix()));
        if (::mkdir(path.c_str(), 0700) == 0) {
            return std::unique_ptr<StagingDirectory>(new StagingDirectory(root, path));
        }
        if (errno != EEXIST) {
            return file_error(DatasetErrorCode::WriteFailed, "catalog", "could not create a staging directory");
        }
    }
    return dataset_error(
        DatasetErrorCode::WriteFailed,
        "catalog",
        "could not allocate a unique staging directory"
    );
}

StagingDirectory::~StagingDirectory() {
    if (published_) {
        return;
    }
    // Only what this object created: its files, then its directory.
    ::chmod(path_.c_str(), 0700);
    for (const std::string& name : files_) {
        ::unlink((path_ / name).c_str());
    }
    ::rmdir(path_.c_str());
}

std::variant<std::unique_ptr<StagedFile>, DatasetError> StagingDirectory::create_file(
    std::string_view name
) {
    if (published_ || !is_single_component(name) ||
        std::find(files_.begin(), files_.end(), name) != files_.end()) {
        return dataset_error(DatasetErrorCode::InvalidArgument, std::string(name), "cannot be created here");
    }
    const int descriptor = ::open(
        (path_ / std::string(name)).c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
        0600
    );
    if (descriptor < 0) {
        return file_error(DatasetErrorCode::WriteFailed, name, "could not create file");
    }
    files_.emplace_back(name);
    return std::make_unique<StagedFile>(descriptor, std::string(name));
}

std::variant<FileDigest, DatasetError> StagingDirectory::write_file(
    std::string_view name,
    std::string_view contents
) {
    auto created = create_file(name);
    if (auto* error = std::get_if<DatasetError>(&created)) {
        return std::move(*error);
    }
    auto& file = std::get<std::unique_ptr<StagedFile>>(created);
    if (auto error = file->write(contents)) {
        return std::move(*error);
    }
    return file->finish();
}

std::variant<PublicationResult, DatasetError> StagingDirectory::publish(std::string_view name) {
    if (published_ || !is_single_component(name)) {
        return dataset_error(DatasetErrorCode::InvalidArgument, std::string(name), "cannot be published");
    }
    for (const std::string& file : files_) {
        if (::chmod((path_ / file).c_str(), 0444) != 0) {
            return file_error(DatasetErrorCode::PublishFailed, file, "could not make file read-only");
        }
    }
    if (auto error = synchronize_directory(path_)) {
        return std::move(*error);
    }
    // Renaming a directory needs write permission on it on some file
    // systems, so it becomes read-only just after it is published. Its files
    // already are, and the directory has been owner-only since creation.
    const std::filesystem::path destination = root_ / std::string(name);
    if (rename_without_replacing(path_, destination) != 0) {
        const int error_number = errno;
        if (error_number == EEXIST || error_number == ENOTEMPTY) {
            return PublicationResult::DestinationExists;
        }
        errno = error_number;
        return file_error(DatasetErrorCode::PublishFailed, std::string(name), "could not publish directory");
    }
    published_ = true;
    // Through a descriptor opened without following a symlink, so a swapped
    // entry is refused rather than followed.
    const Descriptor published(::open(destination.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (published.get() < 0 || ::fchmod(published.get(), 0555) != 0) {
        warning_ = file_error(DatasetErrorCode::PublishFailed, std::string(name), "published, but could not make the directory read-only");
    } else if (auto error = synchronize_directory(root_)) {
        warning_ = dataset_error(
            DatasetErrorCode::PublishFailed,
            std::string(name),
            "published, but the catalog directory could not be synchronized: " + error->message
        );
    }
    return PublicationResult::Published;
}

}  // namespace chronos::datasets
