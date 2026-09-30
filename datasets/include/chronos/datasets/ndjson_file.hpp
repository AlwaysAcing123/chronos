#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace chronos::datasets {

struct EndOfLines {};

// Reads a newline-delimited file one line at a time in bounded memory,
// hashing every byte as it goes. Every line, including the last, must end
// with "\n" and be at most maximum_line_bytes long; a file that ends inside
// a line is truncated, not a shorter file. Never follows a symlink.
class NdjsonFileReader final {
public:
    [[nodiscard]] static std::variant<std::unique_ptr<NdjsonFileReader>, DatasetError> open(
        const std::filesystem::path& path,
        std::string field,
        std::size_t maximum_line_bytes
    );

    ~NdjsonFileReader();

    NdjsonFileReader(const NdjsonFileReader&) = delete;
    NdjsonFileReader& operator=(const NdjsonFileReader&) = delete;

    // The next line without its newline. The view is valid until the next
    // call.
    [[nodiscard]] std::variant<std::string_view, EndOfLines, DatasetError> next();

    // The one-based number of the last line returned.
    [[nodiscard]] std::uint64_t line_number() const noexcept { return line_number_; }
    [[nodiscard]] std::uint64_t bytes_read() const noexcept { return bytes_read_; }
    // The SHA-256 of the whole file, after EndOfLines.
    [[nodiscard]] std::string hex_digest();

private:
    NdjsonFileReader(int descriptor, std::string field, std::size_t maximum_line_bytes);

    int descriptor_;
    std::string field_;
    std::size_t maximum_line_bytes_;
    std::string buffer_;
    // Start of the data not yet returned.
    std::size_t position_{0};
    bool end_of_file_{false};
    std::uint64_t line_number_{0};
    std::uint64_t bytes_read_{0};
    Sha256 hash_;
};

}  // namespace chronos::datasets
