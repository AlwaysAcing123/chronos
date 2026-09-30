#include "chronos/paper_trading/history_cursor.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace chronos::paper_trading {
namespace {

constexpr std::string_view kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view kUnfilteredVersion = "v1";
constexpr std::string_view kFilteredVersion = "v2";
constexpr char kSeparator = '|';

constexpr std::string_view kOrderVersion = "o1";

std::string_view kind_name(HistoryCursorKind kind) {
    switch (kind) {
        case HistoryCursorKind::Orders:
            return "orders";
        case HistoryCursorKind::Fills:
            return "fills";
        case HistoryCursorKind::WorkingOrders:
            return "working-orders";
    }
    return "";
}

std::string_view kind_name(OrderCursorKind kind) {
    return kind == OrderCursorKind::Fills ? "order-fills" : "order-events";
}

std::string base64url_encode(std::string_view bytes) {
    std::string encoded;
    encoded.reserve((bytes.size() * 4 + 2) / 3);
    std::uint32_t buffer = 0;
    int buffered_bits = 0;
    for (const char byte : bytes) {
        buffer = (buffer << 8) | static_cast<unsigned char>(byte);
        buffered_bits += 8;
        while (buffered_bits >= 6) {
            buffered_bits -= 6;
            encoded += kBase64UrlAlphabet[(buffer >> buffered_bits) & 0x3F];
        }
        buffer &= (1U << buffered_bits) - 1;
    }
    if (buffered_bits > 0) {
        encoded += kBase64UrlAlphabet[(buffer << (6 - buffered_bits)) & 0x3F];
    }
    return encoded;
}

// Accepts only the canonical unpadded encoding: re-encoding the decoded
// bytes must reproduce the text, so every cursor has one spelling.
std::optional<std::string> base64url_decode(std::string_view text) {
    std::string decoded;
    std::uint32_t buffer = 0;
    int buffered_bits = 0;
    for (const char character : text) {
        const std::size_t value = kBase64UrlAlphabet.find(character);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        buffered_bits += 6;
        if (buffered_bits >= 8) {
            buffered_bits -= 8;
            decoded += static_cast<char>((buffer >> buffered_bits) & 0xFF);
        }
        buffer &= (1U << buffered_bits) - 1;
    }
    if (base64url_encode(decoded) != text) {
        return std::nullopt;
    }
    return decoded;
}

// A journal identity is a positive int64 written without sign or leading
// zeros.
std::optional<std::int64_t> parse_record_id(std::string_view text) {
    if (text.empty() || text.front() < '1' || text.front() > '9') {
        return std::nullopt;
    }
    std::int64_t record_id = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        record_id
    );
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return record_id;
}

// A position that may be zero, written without sign or leading zeros.
std::optional<std::int64_t> parse_position(std::string_view text) {
    if (text == "0") {
        return 0;
    }
    return parse_record_id(text);
}

// Removes and returns the text before the next separator.
std::optional<std::string_view> take_field(std::string_view& remaining) {
    const std::size_t separator = remaining.find(kSeparator);
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view field = remaining.substr(0, separator);
    remaining.remove_prefix(separator + 1);
    return field;
}

}  // namespace

std::string encode_history_cursor(
    HistoryCursorKind kind,
    std::string_view account_id,
    std::optional<std::string_view> instrument,
    std::int64_t before_record_id
) {
    std::string payload(instrument ? kFilteredVersion : kUnfilteredVersion);
    payload += kSeparator;
    payload += kind_name(kind);
    payload += kSeparator;
    payload += account_id;
    payload += kSeparator;
    if (instrument) {
        payload += *instrument;
        payload += kSeparator;
    }
    payload += std::to_string(before_record_id);
    return base64url_encode(payload);
}

std::optional<std::int64_t> decode_history_cursor(
    std::string_view cursor,
    HistoryCursorKind kind,
    std::string_view account_id,
    std::optional<std::string_view> instrument
) {
    if (cursor.empty() || cursor.size() > kMaximumHistoryCursorLength) {
        return std::nullopt;
    }
    const std::optional<std::string> payload = base64url_decode(cursor);
    if (!payload) {
        return std::nullopt;
    }

    std::string_view remaining(*payload);
    const auto version = take_field(remaining);
    const auto encoded_kind = take_field(remaining);
    const auto encoded_account = take_field(remaining);
    if (version != (instrument ? kFilteredVersion : kUnfilteredVersion) ||
        encoded_kind != kind_name(kind) || encoded_account != account_id) {
        return std::nullopt;
    }
    if (instrument && take_field(remaining) != *instrument) {
        return std::nullopt;
    }
    return parse_record_id(remaining);
}

std::string encode_order_cursor(
    OrderCursorKind kind,
    std::string_view account_id,
    std::string_view client_order_id,
    std::int64_t after
) {
    std::string payload(kOrderVersion);
    payload += kSeparator;
    payload += kind_name(kind);
    payload += kSeparator;
    payload += account_id;
    payload += kSeparator;
    payload += client_order_id;
    payload += kSeparator;
    payload += std::to_string(after);
    return base64url_encode(payload);
}

std::optional<std::int64_t> decode_order_cursor(
    std::string_view cursor,
    OrderCursorKind kind,
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (cursor.empty() || cursor.size() > kMaximumHistoryCursorLength) {
        return std::nullopt;
    }
    const std::optional<std::string> payload = base64url_decode(cursor);
    if (!payload) {
        return std::nullopt;
    }
    std::string_view remaining(*payload);
    if (take_field(remaining) != kOrderVersion ||
        take_field(remaining) != kind_name(kind) ||
        take_field(remaining) != account_id ||
        take_field(remaining) != client_order_id) {
        return std::nullopt;
    }
    return parse_position(remaining);
}

}  // namespace chronos::paper_trading
