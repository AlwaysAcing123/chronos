#include "chronos/canonical_json.hpp"
#include "chronos/sha256.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

namespace {

namespace json = chronos::canonical_json;
using chronos::Sha256;

// FIPS 180-4 / NIST CAVS example messages.
void test_sha256_matches_published_vectors() {
    REQUIRE(Sha256::hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(Sha256::hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(Sha256::hex(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    ) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    REQUIRE(Sha256::hex(
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"
    ) == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
    Sha256 million;
    const std::string thousand(1000, 'a');
    for (int chunk = 0; chunk < 1000; ++chunk) {
        million.update(thousand);
    }
    REQUIRE(million.hex_digest() ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Padding changes at 55, 56, and 64 bytes; incremental updates that split
// blocks anywhere must agree with a single update.
void test_sha256_is_independent_of_update_boundaries() {
    std::string message;
    for (int index = 0; index < 200; ++index) {
        message.push_back(static_cast<char>('a' + index % 26));
    }
    for (const std::size_t length : {55U, 56U, 57U, 63U, 64U, 65U, 119U, 128U, 200U}) {
        const std::string_view prefix(message.data(), length);
        const std::string whole = Sha256::hex(prefix);
        for (std::size_t split = 0; split <= length; split += 7) {
            Sha256 pieces;
            pieces.update(prefix.substr(0, split));
            pieces.update(prefix.substr(split));
            REQUIRE(pieces.hex_digest() == whole);
        }
        Sha256 bytes;
        for (const char character : prefix) {
            bytes.update(std::string_view(&character, 1));
        }
        REQUIRE(bytes.hex_digest() == whole);
    }
    REQUIRE(Sha256::hex(std::string(55, 'a')) ==
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    REQUIRE(Sha256::hex(std::string(64, 'a')) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

    Sha256 with_null_empty_view;
    with_null_empty_view.update("a");
    with_null_empty_view.update(std::string_view{});
    with_null_empty_view.update("bc");
    REQUIRE(with_null_empty_view.hex_digest() == Sha256::hex("abc"));
}

void test_sha256_refuses_use_after_finishing() {
    Sha256 hash;
    static_cast<void>(hash.hex_digest());
    REQUIRE_THROWS_AS(hash.update("x"), std::logic_error);
    REQUIRE_THROWS_AS(hash.hex_digest(), std::logic_error);
    REQUIRE(chronos::is_sha256_hex(Sha256::hex("x")));
    REQUIRE(!chronos::is_sha256_hex(
        "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"
    ));
    REQUIRE(!chronos::is_sha256_hex("e3b0"));
}

json::Value parsed(std::string_view text) {
    auto result = json::parse(text);
    REQUIRE(std::holds_alternative<json::Value>(result));
    return std::get<json::Value>(std::move(result));
}

std::string parse_error(std::string_view text) {
    auto result = json::parse(text);
    REQUIRE(std::holds_alternative<json::ParseError>(result));
    return std::get<json::ParseError>(result).message;
}

void test_json_serializes_canonically() {
    const json::Value value(json::Object{
        {"b", json::Array{"x", nullptr, true, false}},
        {"a", json::Object{{"z", ""}, {"y", "\"\\\n\t\x01/é"}}},
    });
    REQUIRE(json::serialize(value) ==
        R"({"a":{"y":"\"\\\n\t\u0001/é","z":""},"b":["x",null,true,false]})");
    REQUIRE(parsed(json::serialize(value)) == json::Value(json::Object{
        {"a", json::Object{{"y", "\"\\\n\t\x01/é"}, {"z", ""}}},
        {"b", json::Array{"x", nullptr, true, false}},
    }));
    REQUIRE(json::serialize(json::Value(json::Object{})) == "{}");
    REQUIRE(json::serialize(json::Value(json::Array{})) == "[]");
    REQUIRE(json::serialize_indented(json::Value(json::Object{
        {"a", json::Array{"x"}},
    })) == "{\n  \"a\": [\n    \"x\"\n  ]\n}");
    REQUIRE_THROWS_AS(
        json::serialize(json::Value(std::string("\xff"))),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        json::serialize(json::Value(json::Object{{"a", "1"}, {"a", "2"}})),
        std::invalid_argument
    );
}

// Parsing accepts any spelling of a supported value; canonical documents
// are those equal to serialize(parse(text)).
void test_json_non_canonical_spellings_round_trip_to_one_form() {
    for (const std::string_view spelling : {
             R"({ "b":"1", "a":"2" })",
             R"({"a":"2","b":"1"})",
             "{\"a\":\"2\",\"b\":\"1\"}\n",
         }) {
        REQUIRE(json::serialize(parsed(spelling)) == R"({"a":"2","b":"1"})");
    }
    REQUIRE(json::serialize(parsed(R"("😀\/")")) == "\"\xF0\x9F\x98\x80/\"");
}

void test_json_rejects_what_is_not_supported() {
    REQUIRE(parse_error("1") == "JSON numbers are not used; exact values are strings");
    REQUIRE(parse_error(R"({"a":-1})") == "JSON numbers are not used; exact values are strings");
    REQUIRE(parse_error(R"({"a":"1","a":"2"})") == "duplicate object key: a");
    REQUIRE(parse_error(R"({"a":"1"} x)") == "unexpected data after the value");
    REQUIRE(parse_error(R"({"a":"1"}{})") == "unexpected data after the value");
    REQUIRE(parse_error("") == "expected a value");
    REQUIRE(parse_error(R"({"a":"1")") == "unterminated object");
    REQUIRE(parse_error(R"({"a":"1)") == "unterminated string");
    REQUIRE(parse_error("\"a\x01\"") == "raw control character in a string");
    REQUIRE(parse_error("\"\xC0\xAF\"") == "invalid UTF-8 in a string");
    REQUIRE(parse_error("\"\xED\xA0\x80\"") == "invalid UTF-8 in a string");
    REQUIRE(parse_error("\"\xF4\x90\x80\x80\"") == "invalid UTF-8 in a string");
    REQUIRE(parse_error(R"("\ud800")") == "lone high surrogate in a string");
    REQUIRE(parse_error(R"("\udc00")") == "lone low surrogate in a string");
    REQUIRE(parse_error(R"("\x")") == "invalid escape in a string");
    REQUIRE(parse_error(R"("\u12g4")") == "invalid \\u escape");
    REQUIRE(parse_error("nul") == "expected a value");
    REQUIRE(parse_error("[\"a\",]") == "expected a value");
    std::string deep(40, '[');
    deep += std::string(40, ']');
    REQUIRE(parse_error(deep) == "values are nested too deeply");
    REQUIRE(json::parse(std::string(32, '[') + std::string(32, ']'), 33).index() == 0);
}

void test_canonical_integers_have_one_spelling() {
    REQUIRE(json::parse_canonical_unsigned("0") == 0U);
    REQUIRE(json::parse_canonical_unsigned("18446744073709551615") ==
        std::numeric_limits<std::uint64_t>::max());
    for (const std::string_view invalid : {
             "", "00", "01", "+1", "-1", "1.0", "1e3", " 1", "18446744073709551616",
         }) {
        REQUIRE(!json::parse_canonical_unsigned(invalid));
    }
    REQUIRE(json::parse_canonical_signed("-9223372036854775808") ==
        std::numeric_limits<std::int64_t>::min());
    REQUIRE(json::parse_canonical_signed("9223372036854775807") ==
        std::numeric_limits<std::int64_t>::max());
    REQUIRE(json::parse_canonical_signed("-1") == -1);
    for (const std::string_view invalid : {
             "-0", "-", "--1", "-01", "9223372036854775808", "-9223372036854775809",
         }) {
        REQUIRE(!json::parse_canonical_signed(invalid));
    }
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"SHA-256 matches published vectors", test_sha256_matches_published_vectors},
        {"SHA-256 is independent of update boundaries",
         test_sha256_is_independent_of_update_boundaries},
        {"SHA-256 refuses use after finishing", test_sha256_refuses_use_after_finishing},
        {"JSON serializes canonically", test_json_serializes_canonically},
        {"non-canonical spellings round trip to one form",
         test_json_non_canonical_spellings_round_trip_to_one_form},
        {"JSON rejects what is not supported", test_json_rejects_what_is_not_supported},
        {"canonical integers have one spelling", test_canonical_integers_have_one_spelling},
    });
}
