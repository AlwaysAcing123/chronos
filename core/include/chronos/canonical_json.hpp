#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// A strict subset of JSON for Chronos's persisted artifacts.
//
// Values are null, booleans, strings, arrays, and objects. JSON numbers are
// deliberately absent: every exact quantity is a decimal string, so no value
// ever passes through binary floating point in any reader. The canonical
// spelling of a value is unique: object keys in byte order, no insignificant
// whitespace, and one escape for each character that needs one. A reader
// accepts a document only when it is byte-identical to the canonical
// spelling of what it parsed, so equal values always have equal bytes.
namespace chronos::canonical_json {

class Value;

using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;
// Members in any order; serialization sorts them by key.
using Object = std::vector<Member>;

class Value final {
public:
    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool value) : data_(value) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(std::string_view value) : data_(std::string(value)) {}
    Value(const char* value) : data_(std::string(value)) {}
    Value(Array value) : data_(std::move(value)) {}
    Value(Object value) : data_(std::move(value)) {}

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::nullptr_t>(data_);
    }
    [[nodiscard]] const bool* if_bool() const noexcept {
        return std::get_if<bool>(&data_);
    }
    [[nodiscard]] const std::string* if_string() const noexcept {
        return std::get_if<std::string>(&data_);
    }
    [[nodiscard]] const Array* if_array() const noexcept {
        return std::get_if<Array>(&data_);
    }
    [[nodiscard]] const Object* if_object() const noexcept {
        return std::get_if<Object>(&data_);
    }

    friend bool operator==(const Value&, const Value&) = default;

private:
    std::variant<std::nullptr_t, bool, std::string, Array, Object> data_{};
};

struct ParseError {
    // Byte offset of the first unacceptable input.
    std::size_t offset;
    std::string message;
};

// Parses exactly one value spanning the whole text. Rejects numbers,
// duplicate keys, invalid UTF-8, raw control characters in strings, lone
// surrogates, nesting deeper than maximum_depth, and trailing data. It does
// not by itself require canonical spelling; compare serialize(parse(text))
// with text for that.
[[nodiscard]] std::variant<Value, ParseError> parse(
    std::string_view text,
    std::size_t maximum_depth = 32
);

// The canonical spelling of a value. Throws std::invalid_argument when a
// string is not valid UTF-8 or an object repeats a key.
[[nodiscard]] std::string serialize(const Value& value);

// An indented rendering for people, with keys in the same order. It is not
// the canonical spelling and is never persisted.
[[nodiscard]] std::string serialize_indented(const Value& value);

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

// Canonical decimal integers: "0" or digits without a leading zero, and for
// signed values an optional "-" that never precedes zero. Nullopt when the
// text is not canonical or does not fit.
[[nodiscard]] std::optional<std::uint64_t> parse_canonical_unsigned(
    std::string_view text
) noexcept;
[[nodiscard]] std::optional<std::int64_t> parse_canonical_signed(
    std::string_view text
) noexcept;

}  // namespace chronos::canonical_json
