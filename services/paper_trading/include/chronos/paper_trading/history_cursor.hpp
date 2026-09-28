#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chronos::paper_trading {

// The history a cursor pages through. A cursor for one kind is invalid for
// the other.
enum class HistoryCursorKind {
    Orders,
    Fills,
};

// Clients treat cursors as opaque. This version encodes, as unpadded
// base64url, "v1|<kind>|<account_id>|<journal record id>", binding a position
// to one account and one kind so that a cursor cannot be replayed against a
// different history. A cursor is not an authorization token: every query is
// scoped to the requested account whatever the cursor says.
inline constexpr std::size_t kMaximumHistoryCursorLength = 256;

[[nodiscard]] std::string encode_history_cursor(
    HistoryCursorKind kind,
    std::string_view account_id,
    std::int64_t before_record_id
);

// Returns the journal position, or nullopt for anything other than a cursor
// that encode_history_cursor produces for the same kind and account.
[[nodiscard]] std::optional<std::int64_t> decode_history_cursor(
    std::string_view cursor,
    HistoryCursorKind kind,
    std::string_view account_id
);

}  // namespace chronos::paper_trading
