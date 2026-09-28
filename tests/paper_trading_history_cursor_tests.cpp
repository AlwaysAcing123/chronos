#include "chronos/paper_trading/history_cursor.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

using chronos::paper_trading::decode_history_cursor;
using chronos::paper_trading::encode_history_cursor;
using chronos::paper_trading::HistoryCursorKind;

constexpr auto kOrders = HistoryCursorKind::Orders;
constexpr auto kFills = HistoryCursorKind::Fills;

// Independent vectors: Python's base64.urlsafe_b64encode of the payload,
// with padding removed.
constexpr std::string_view kOrdersAccount1Before42 =
    "djF8b3JkZXJzfGFjY3QtMXw0Mg";
constexpr std::string_view kFillsAccount1Before42 =
    "djF8ZmlsbHN8YWNjdC0xfDQy";

void test_cursor_version_one_encoding_is_pinned() {
    REQUIRE(encode_history_cursor(kOrders, "acct-1", 42) ==
            kOrdersAccount1Before42);
    REQUIRE(encode_history_cursor(kFills, "acct-1", 42) ==
            kFillsAccount1Before42);
    REQUIRE(decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-1") ==
            42);
    REQUIRE(decode_history_cursor(kFillsAccount1Before42, kFills, "acct-1") ==
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
                record_id
            );
            REQUIRE(cursor.size() <=
                    chronos::paper_trading::kMaximumHistoryCursorLength);
            REQUIRE(
                decode_history_cursor(cursor, kind, longest_account) ==
                record_id
            );
        }
    }
}

void test_cursors_are_bound_to_one_kind_and_account() {
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kFills, "acct-1"));
    REQUIRE(!decode_history_cursor(kFillsAccount1Before42, kOrders, "acct-1"));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-2"));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, "acct-"));
    REQUIRE(!decode_history_cursor(kOrdersAccount1Before42, kOrders, ""));
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
        REQUIRE(!decode_history_cursor(cursor, kOrders, "acct-1"));
    }
    REQUIRE(
        decode_history_cursor(
            "djF8b3JkZXJzfGFjY3QtMXw5MjIzMzcyMDM2ODU0Nzc1ODA3",
            kOrders,
            "acct-1"
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
        REQUIRE(!decode_history_cursor(cursor, kOrders, "acct-1"));
    }
    const std::string oversized(
        chronos::paper_trading::kMaximumHistoryCursorLength + 1,
        'A'
    );
    REQUIRE(!decode_history_cursor(oversized, kOrders, "acct-1"));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
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
    });
}
