#include "chronos/paper_trading/history_cursor.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using chronos::paper_trading::decode_history_cursor;
using chronos::paper_trading::decode_order_cursor;
using chronos::paper_trading::encode_history_cursor;
using chronos::paper_trading::encode_order_cursor;
using chronos::paper_trading::HistoryCursorKind;
using chronos::paper_trading::OrderCursorKind;

constexpr auto kOrders = HistoryCursorKind::Orders;
constexpr auto kFills = HistoryCursorKind::Fills;

// Independent vectors: Python's base64.urlsafe_b64encode of the payload,
// with padding removed.
constexpr std::string_view kOrdersAccount1Before42 =
    "djF8b3JkZXJzfGFjY3QtMXw0Mg";
constexpr std::string_view kFillsAccount1Before42 =
    "djF8ZmlsbHN8YWNjdC0xfDQy";

void test_cursor_version_one_encoding_is_pinned() {
    REQUIRE(encode_history_cursor(kOrders, "acct-1", std::nullopt, 42) ==
            kOrdersAccount1Before42);
    REQUIRE(encode_history_cursor(kFills, "acct-1", std::nullopt, 42) ==
            kFillsAccount1Before42);
    REQUIRE(decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-1", std::nullopt) ==
            42);
    REQUIRE(decode_history_cursor(kFillsAccount1Before42, kFills, "acct-1", std::nullopt) ==
            42);
}

void test_cursors_round_trip_across_the_record_range() {
    const std::string longest_account(64, 'a');
    for (const std::int64_t record_id : {
             std::int64_t{1},
             std::int64_t{123'456'789},
             std::numeric_limits<std::int64_t>::max(),
         }) {
        for (const auto kind : {kOrders, kFills}) {
            const std::string cursor = encode_history_cursor(
                kind,
                longest_account,
                std::nullopt,
                record_id
            );
            REQUIRE(cursor.size() <=
                    chronos::paper_trading::kMaximumHistoryCursorLength);
            REQUIRE(
                decode_history_cursor(cursor, kind, longest_account, std::nullopt) ==
                record_id
            );
        }
    }
}

void test_cursors_are_bound_to_one_kind_and_account() {
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kFills, "acct-1", std::nullopt));
    REQUIRE(!decode_history_cursor(kFillsAccount1Before42, kOrders, "acct-1", std::nullopt));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-2", std::nullopt));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-", std::nullopt));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, "", std::nullopt));
}

void test_malformed_payloads_are_rejected() {
    for (const std::string_view cursor : {
             "djJ8b3JkZXJzfGFjY3QtMXw0Mg",
             "djF8dHJhZGVzfGFjY3QtMXw0Mg",
             "djF8b3JkZXJzfGFjY3QtMXww",
             "djF8b3JkZXJzfGFjY3QtMXwtMQ",
             "djF8b3JkZXJzfGFjY3QtMXwwNDI",
             "djF8b3JkZXJzfGFjY3QtMXwrNDI",
             "djF8b3JkZXJzfGFjY3QtMXw5MjIzMzcyMDM2ODU0Nzc1ODA4",
             "djF8b3JkZXJzfGFjY3QtMXw0Mnw3",
             "djF8b3JkZXJzfGFjY3QtMQ",
             "djF8b3JkZXJzfGFjY3QtMXw",
         }) {
        REQUIRE(!decode_history_cursor(cursor, kOrders, "acct-1", std::nullopt));
    }
    REQUIRE(
        decode_history_cursor(
            "djF8b3JkZXJzfGFjY3QtMXw5MjIzMzcyMDM2ODU0Nzc1ODA3",
            kOrders,
            "acct-1",
            std::nullopt
        ) == std::numeric_limits<std::int64_t>::max()
    );
}

void test_only_the_canonical_unpadded_spelling_is_accepted() {
    for (const std::string_view cursor : {
             "",
             "djF8b3JkZXJzfGFjY3QtMXw0Mh",
             "djF8b3JkZXJzfGFjY3QtMXw0Mg==",
             "djF8b3JkZXJzfGFjY3QtMXw0M",
             "djF8b3JkZXJzfGFjY3QtMXw0Mg ",
             "djF8b3JkZXJzfGFjY3QtMXw0Mg+",
             "djF8b3JkZXJzfGFjY3QtMXw0/g",
             "42",
         }) {
        REQUIRE(!decode_history_cursor(cursor, kOrders, "acct-1", std::nullopt));
    }
    const std::string oversized(
        chronos::paper_trading::kMaximumHistoryCursorLength + 1,
        'A'
    );
    REQUIRE(!decode_history_cursor(oversized, kOrders, "acct-1", std::nullopt));
}


// Independent vectors, as above, of the v2 payloads
// "v2|orders|acct-1|SPY|42" and "v2|fills|acct-1|SPY|42".
constexpr std::string_view kSpyOrdersAccount1Before42 =
    "djJ8b3JkZXJzfGFjY3QtMXxTUFl8NDI";
constexpr std::string_view kSpyFillsAccount1Before42 =
    "djJ8ZmlsbHN8YWNjdC0xfFNQWXw0Mg";

void test_filtered_cursors_are_bound_to_their_instrument() {
    REQUIRE(encode_history_cursor(kOrders, "acct-1", "SPY", 42) ==
            kSpyOrdersAccount1Before42);
    REQUIRE(encode_history_cursor(kFills, "acct-1", "SPY", 42) ==
            kSpyFillsAccount1Before42);
    REQUIRE(decode_history_cursor(kSpyOrdersAccount1Before42, kOrders, "acct-1", "SPY") ==
            42);
    REQUIRE(decode_history_cursor(kSpyFillsAccount1Before42, kFills, "acct-1", "SPY") ==
            42);

    // A filtered cursor pages only the same filter, and an unfiltered one
    // only unfiltered history.
    REQUIRE(!decode_history_cursor(kSpyOrdersAccount1Before42, kOrders, "acct-1", "AAPL"));
    REQUIRE(!decode_history_cursor(kSpyOrdersAccount1Before42, kOrders, "acct-1", "SP"));
    REQUIRE(!decode_history_cursor(kSpyOrdersAccount1Before42, kOrders, "acct-1", std::nullopt));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-1", "SPY"));
    REQUIRE(!decode_history_cursor(kSpyOrdersAccount1Before42, kFills, "acct-1", "SPY"));
    REQUIRE(!decode_history_cursor(kSpyOrdersAccount1Before42, kOrders, "acct-2", "SPY"));

    for (const std::string_view cursor : {
             "djJ8b3JkZXJzfGFjY3QtMXxTUFk",
             "djJ8b3JkZXJzfGFjY3QtMXxTUFl8NDJ8Nw",
             "djJ8b3JkZXJzfGFjY3QtMXx8NDI",
             "djF8b3JkZXJzfGFjY3QtMXxTUFl8NDI",
         }) {
        REQUIRE(!decode_history_cursor(cursor, kOrders, "acct-1", "SPY"));
    }

    const std::string longest(64, 'a');
    const std::string cursor = encode_history_cursor(
        kOrders,
        longest,
        longest,
        std::numeric_limits<std::int64_t>::max()
    );
    REQUIRE(cursor.size() <=
            chronos::paper_trading::kMaximumHistoryCursorLength);
    REQUIRE(decode_history_cursor(cursor, kOrders, longest, longest) ==
            std::numeric_limits<std::int64_t>::max());
}

// Working orders page forward after a record, in the same two spellings as
// the journal's history; per-order cursors add the order and allow the
// first position, 0.
void test_working_order_and_per_order_cursors_are_pinned_and_bound() {
    constexpr auto kWorking = HistoryCursorKind::WorkingOrders;
    REQUIRE(encode_history_cursor(kWorking, "acct-1", std::nullopt, 42) ==
            "djF8d29ya2luZy1vcmRlcnN8YWNjdC0xfDQy");
    REQUIRE(encode_history_cursor(kWorking, "acct-1", "SPY", 42) ==
            "djJ8d29ya2luZy1vcmRlcnN8YWNjdC0xfFNQWXw0Mg");
    REQUIRE(decode_history_cursor("djF8d29ya2luZy1vcmRlcnN8YWNjdC0xfDQy", kWorking, "acct-1",
                                  std::nullopt) == 42);
    // Not an orders or fills cursor, and not for a filter it was not issued for.
    REQUIRE(!decode_history_cursor("djF8d29ya2luZy1vcmRlcnN8YWNjdC0xfDQy", kOrders, "acct-1",
                                   std::nullopt));
    REQUIRE(!decode_history_cursor("djJ8d29ya2luZy1vcmRlcnN8YWNjdC0xfFNQWXw0Mg", kWorking,
                                   "acct-1", "AAPL"));

    constexpr auto kOrderFills = OrderCursorKind::Fills;
    constexpr auto kOrderEvents = OrderCursorKind::Events;
    REQUIRE(encode_order_cursor(kOrderFills, "acct-1", "order-7", 0) ==
            "bzF8b3JkZXItZmlsbHN8YWNjdC0xfG9yZGVyLTd8MA");
    REQUIRE(encode_order_cursor(kOrderEvents, "acct-1", "order-7", 3) ==
            "bzF8b3JkZXItZXZlbnRzfGFjY3QtMXxvcmRlci03fDM");
    REQUIRE(decode_order_cursor("bzF8b3JkZXItZmlsbHN8YWNjdC0xfG9yZGVyLTd8MA", kOrderFills,
                                "acct-1", "order-7") == 0);
    REQUIRE(decode_order_cursor("bzF8b3JkZXItZXZlbnRzfGFjY3QtMXxvcmRlci03fDM", kOrderEvents,
                                "acct-1", "order-7") == 3);
    for (const std::int64_t position : {std::int64_t{0}, std::int64_t{1},
                                        std::numeric_limits<std::int64_t>::max()}) {
        const std::string cursor = encode_order_cursor(kOrderFills, "a", "o", position);
        REQUIRE(decode_order_cursor(cursor, kOrderFills, "a", "o") == position);
        // Bound to the kind, the account, and the order.
        REQUIRE(!decode_order_cursor(cursor, kOrderEvents, "a", "o"));
        REQUIRE(!decode_order_cursor(cursor, kOrderFills, "b", "o"));
        REQUIRE(!decode_order_cursor(cursor, kOrderFills, "a", "p"));
        // Never a history cursor.
        REQUIRE(!decode_history_cursor(cursor, kFills, "a", "o"));
    }
    // A position is canonical: no sign, no leading zeros, nothing trailing.
    for (const std::string_view payload : {
             "o1|order-fills|a|o|-1",
             "o1|order-fills|a|o|00",
             "o1|order-fills|a|o|01",
             "o1|order-fills|a|o|",
             "o1|order-fills|a|o|1|",
             "o1|order-fills|a|o|9223372036854775808",
             "o2|order-fills|a|o|1",
         }) {
        std::string cursor;
        // Encode the arbitrary payload with the same alphabet.
        const std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::uint32_t buffer = 0;
        int bits = 0;
        for (const char byte : payload) {
            buffer = (buffer << 8) | static_cast<unsigned char>(byte);
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                cursor += alphabet[(buffer >> bits) & 0x3F];
            }
            buffer &= (1U << bits) - 1;
        }
        if (bits > 0) {
            cursor += alphabet[(buffer << (6 - bits)) & 0x3F];
        }
        REQUIRE(!decode_order_cursor(cursor, kOrderFills, "a", "o"));
    }
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "working-order and per-order cursors",
            test_working_order_and_per_order_cursors_are_pinned_and_bound,
        },
        {
            "cursor version one encoding is pinned",
            test_cursor_version_one_encoding_is_pinned,
        },
        {
            "cursors round trip across the record range",
            test_cursors_round_trip_across_the_record_range,
        },
        {
            "cursors are bound to one kind and account",
            test_cursors_are_bound_to_one_kind_and_account,
        },
        {
            "malformed payloads are rejected",
            test_malformed_payloads_are_rejected,
        },
        {
            "only the canonical spelling is accepted",
            test_only_the_canonical_unpadded_spelling_is_accepted,
        },
        {
            "filtered cursors are bound to their instrument",
            test_filtered_cursors_are_bound_to_their_instrument,
        },
    });
}
