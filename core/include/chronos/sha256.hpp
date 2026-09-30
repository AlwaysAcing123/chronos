#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace chronos {

// SHA-256 (FIPS 180-4), computed incrementally so a file can be hashed while
// it is streamed. Digests are rendered as 64 lowercase hexadecimal digits.
//
// The default build has no cryptography dependency; this is used only for
// content identity and integrity, never for secrecy.
class Sha256 final {
public:
    Sha256() noexcept;

    // Appends bytes to the message. Throws std::logic_error after
    // hex_digest().
    void update(std::string_view bytes);

    // Finishes the message and returns its digest. The object cannot be
    // updated afterwards.
    [[nodiscard]] std::string hex_digest();

    [[nodiscard]] static std::string hex(std::string_view bytes);

private:
    void compress(const unsigned char* block) noexcept;

    std::array<std::uint32_t, 8> state_;
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffered_{0};
    std::uint64_t length_bytes_{0};
    bool finished_{false};
};

// Whether text is exactly 64 lowercase hexadecimal digits.
[[nodiscard]] bool is_sha256_hex(std::string_view text) noexcept;

}  // namespace chronos
