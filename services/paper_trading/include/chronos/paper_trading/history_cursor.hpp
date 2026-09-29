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

// Clients treat cursors as opaque. A cursor is unpadded base64url of
//
//   v1|<kind>|<account_id>|<journal record id>                for all records
//   v2|<kind>|<account_id>|<instrument>|<journal record id>   for one symbol
//
// binding a position to one account, one kind, and one instrument filter, so
// that a cursor cannot be replayed against a different history. Unfiltered
// cursors keep the v1 spelling, so cursors issued before instrument filters
// existed stay valid. A cursor is not an authorization token: every query is
// scoped to the requested account whatever the cursor says.
inline constexpr std::size_t kMaximumHistoryCursorLength = 256;

[[nodiscard]] std::string encode_history_cursor(
    HistoryCursorKind kind,
    std::string_view account_id,
    std::optional<std::string_view> instrument,
    std::int64_t before_record_id
);

// Returns the journal position, or nullopt for anything other than a cursor
// that encode_history_cursor produces for the same kind, account, and
// instrument filter.
[[nodiscard]] std::optional<std::int64_t> decode_history_cursor(
    std::string_view cursor,
    HistoryCursorKind kind,
    std::string_view account_id,
    std::optional<std::string_view> instrument
);

}  // namespace chronos::paper_trading
