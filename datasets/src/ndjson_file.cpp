#include "chronos/datasets/ndjson_file.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace chronos::datasets {
namespace {

constexpr std::size_t kReadChunkBytes = 1U << 16U;

std::string system_message(int error_number) {
    return std::error_code(error_number, std::generic_category()).message();
}

}  // namespace

NdjsonFileReader::NdjsonFileReader(int descriptor, std::string field, std::size_t maximum_line_bytes)
    : descriptor_(descriptor), field_(std::move(field)), maximum_line_bytes_(maximum_line_bytes) {}

NdjsonFileReader::~NdjsonFileReader() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

std::variant<std::unique_ptr<NdjsonFileReader>, DatasetError> NdjsonFileReader::open(
    const std::filesystem::path& path,
    std::string field,
    std::size_t maximum_line_bytes
) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        const int error_number = errno;
        if (error_number == ELOOP) {
            return dataset_error(DatasetErrorCode::SymlinkRejected, std::move(field), "is a symlink");
        }
        if (error_number == ENOENT) {
            return dataset_error(DatasetErrorCode::NotFound, std::move(field), "does not exist");
        }
        return dataset_error(
            DatasetErrorCode::ReadFailed,
            std::move(field),
            "could not open file: " + system_message(error_number)
        );
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return dataset_error(DatasetErrorCode::UnexpectedEntry, std::move(field), "is not a regular file");
    }
    return std::unique_ptr<NdjsonFileReader>(
        new NdjsonFileReader(descriptor, std::move(field), maximum_line_bytes)
    );
}

std::variant<std::string_view, EndOfLines, DatasetError> NdjsonFileReader::next() {
    std::size_t searched = position_;
    while (true) {
        const std::size_t newline = buffer_.find('\n', searched);
        if (newline != std::string::npos) {
            if (newline - position_ > maximum_line_bytes_) {
                return dataset_error(
                    DatasetErrorCode::LineTooLong,
                    field_,
                    "a line exceeds the format's maximum length",
                    line_number_ + 1
                );
            }
            ++line_number_;
            const std::string_view line(buffer_.data() + position_, newline - position_);
            position_ = newline + 1;
            return line;
        }
        if (buffer_.size() - position_ > maximum_line_bytes_) {
            return dataset_error(
                DatasetErrorCode::LineTooLong,
                field_,
                "a line exceeds the format's maximum length",
                line_number_ + 1
            );
        }
        if (end_of_file_) {
            if (position_ < buffer_.size()) {
                return dataset_error(
                    DatasetErrorCode::MissingFinalNewline,
                    field_,
                    "the file ends inside a line: it is truncated or lacks its final newline",
                    line_number_ + 1
                );
            }
            return EndOfLines{};
        }
        // Keep only the unfinished line before reading more, so a line is
        // moved at most once per read.
        if (position_ > 0) {
            buffer_.erase(0, position_);
            position_ = 0;
        }
        searched = buffer_.size();
        char chunk[kReadChunkBytes];
        const ssize_t read = ::read(descriptor_, chunk, sizeof(chunk));
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return dataset_error(
                DatasetErrorCode::ReadFailed,
                field_,
                "could not read file: " + system_message(errno)
            );
        }
        if (read == 0) {
            end_of_file_ = true;
            continue;
        }
        const std::string_view bytes(chunk, static_cast<std::size_t>(read));
        hash_.update(bytes);
        bytes_read_ += bytes.size();
        buffer_.append(bytes);
    }
}

std::string NdjsonFileReader::hex_digest() {
    return hash_.hex_digest();
}

}  // namespace chronos::datasets
