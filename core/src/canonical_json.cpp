#include "chronos/canonical_json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::canonical_json {
namespace {

// The length of the UTF-8 sequence starting at text[index], or zero when it
// is not a valid, shortest-form encoding of a scalar value.
std::size_t utf8_sequence_length(std::string_view text, std::size_t index) {
    const auto byte = [&](std::size_t offset) {
        return static_cast<unsigned char>(text[index + offset]);
    };
    const auto continuation = [&](std::size_t offset) {
        return index + offset < text.size() && (byte(offset) & 0xC0U) == 0x80U;
    };
    const unsigned char lead = byte(0);
    if (lead < 0x80U) {
        return 1;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return continuation(1) ? 2 : 0;
    }
    if (lead >= 0xE0U && lead <= 0xEFU) {
        if (!continuation(1) || !continuation(2)) {
            return 0;
        }
        // Reject overlong forms and UTF-16 surrogates.
        if (lead == 0xE0U && byte(1) < 0xA0U) {
            return 0;
        }
        if (lead == 0xEDU && byte(1) >= 0xA0U) {
            return 0;
        }
        return 3;
    }
    if (lead >= 0xF0U && lead <= 0xF4U) {
        if (!continuation(1) || !continuation(2) || !continuation(3)) {
            return 0;
        }
        if (lead == 0xF0U && byte(1) < 0x90U) {
            return 0;
        }
        if (lead == 0xF4U && byte(1) >= 0x90U) {
            return 0;
        }
        return 4;
    }
    return 0;
}

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point < 0x80U) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

class Parser final {
public:
    Parser(std::string_view text, std::size_t maximum_depth)
        : text_(text), maximum_depth_(maximum_depth) {}

    std::variant<Value, ParseError> parse_document() {
        skip_whitespace();
        auto value = parse_value(0);
        if (error_) {
            return std::move(*error_);
        }
        skip_whitespace();
        if (position_ != text_.size()) {
            return ParseError{position_, "unexpected data after the value"};
        }
        return value;
    }

private:
    Value fail(std::string message) {
        if (!error_) {
            error_ = ParseError{position_, std::move(message)};
        }
        return Value{};
    }

    [[nodiscard]] bool at_end() const { return position_ >= text_.size(); }

    void skip_whitespace() {
        while (!at_end()) {
            const char character = text_[position_];
            if (character != ' ' && character != '\t' && character != '\n' &&
                character != '\r') {
                return;
            }
            ++position_;
        }
    }

    bool consume_literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    Value parse_value(std::size_t depth) {
        if (at_end()) {
            return fail("expected a value");
        }
        const char character = text_[position_];
        if (character == '{') {
            return parse_object(depth);
        }
        if (character == '[') {
            return parse_array(depth);
        }
        if (character == '"') {
            std::string text;
            if (!parse_string(text)) {
                return Value{};
            }
            return Value(std::move(text));
        }
        if (consume_literal("true")) {
            return Value(true);
        }
        if (consume_literal("false")) {
            return Value(false);
        }
        if (consume_literal("null")) {
            return Value(nullptr);
        }
        if (character == '-' || (character >= '0' && character <= '9')) {
            return fail("JSON numbers are not used; exact values are strings");
        }
        return fail("expected a value");
    }

    Value parse_object(std::size_t depth) {
        if (depth >= maximum_depth_) {
            return fail("values are nested too deeply");
        }
        ++position_;
        Object members;
        skip_whitespace();
        if (!at_end() && text_[position_] == '}') {
            ++position_;
            return Value(std::move(members));
        }
        while (true) {
            skip_whitespace();
            if (at_end() || text_[position_] != '"') {
                return fail("expected an object key");
            }
            const std::size_t key_offset = position_;
            std::string key;
            if (!parse_string(key)) {
                return Value{};
            }
            for (const auto& member : members) {
                if (member.first == key) {
                    position_ = key_offset;
                    return fail("duplicate object key: " + key);
                }
            }
            skip_whitespace();
            if (at_end() || text_[position_] != ':') {
                return fail("expected ':' after an object key");
            }
            ++position_;
            skip_whitespace();
            Value value = parse_value(depth + 1);
            if (error_) {
                return Value{};
            }
            members.emplace_back(std::move(key), std::move(value));
            skip_whitespace();
            if (at_end()) {
                return fail("unterminated object");
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == '}') {
                ++position_;
                return Value(std::move(members));
            }
            return fail("expected ',' or '}' in an object");
        }
    }

    Value parse_array(std::size_t depth) {
        if (depth >= maximum_depth_) {
            return fail("values are nested too deeply");
        }
        ++position_;
        Array elements;
        skip_whitespace();
        if (!at_end() && text_[position_] == ']') {
            ++position_;
            return Value(std::move(elements));
        }
        while (true) {
            skip_whitespace();
            Value value = parse_value(depth + 1);
            if (error_) {
                return Value{};
            }
            elements.push_back(std::move(value));
            skip_whitespace();
            if (at_end()) {
                return fail("unterminated array");
            }
            if (text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (text_[position_] == ']') {
                ++position_;
                return Value(std::move(elements));
            }
            return fail("expected ',' or ']' in an array");
        }
    }

    std::optional<std::uint32_t> parse_hex4() {
        if (text_.size() - position_ < 4) {
            fail("incomplete \\u escape");
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char character = text_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') {
                value |= static_cast<std::uint32_t>(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= static_cast<std::uint32_t>(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= static_cast<std::uint32_t>(character - 'A' + 10);
            } else {
                --position_;
                fail("invalid \\u escape");
                return std::nullopt;
            }
        }
        return value;
    }

    bool parse_string(std::string& output) {
        ++position_;
        while (true) {
            if (at_end()) {
                fail("unterminated string");
                return false;
            }
            const auto character = static_cast<unsigned char>(text_[position_]);
            if (character == '"') {
                ++position_;
                return true;
            }
            if (character < 0x20U) {
                fail("raw control character in a string");
                return false;
            }
            if (character != '\\') {
                const std::size_t length = utf8_sequence_length(text_, position_);
                if (length == 0) {
                    fail("invalid UTF-8 in a string");
                    return false;
                }
                output.append(text_.substr(position_, length));
                position_ += length;
                continue;
            }
            ++position_;
            if (at_end()) {
                fail("unterminated escape");
                return false;
            }
            const char escape = text_[position_++];
            switch (escape) {
                case '"':
                    output.push_back('"');
                    break;
                case '\\':
                    output.push_back('\\');
                    break;
                case '/':
                    output.push_back('/');
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u': {
                    const auto high = parse_hex4();
                    if (!high) {
                        return false;
                    }
                    std::uint32_t code_point = *high;
                    if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                        fail("lone low surrogate in a string");
                        return false;
                    }
                    if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                        if (!consume_literal("\\u")) {
                            fail("lone high surrogate in a string");
                            return false;
                        }
                        const auto low = parse_hex4();
                        if (!low) {
                            return false;
                        }
                        if (*low < 0xDC00U || *low > 0xDFFFU) {
                            fail("invalid surrogate pair in a string");
                            return false;
                        }
                        code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                            (*low - 0xDC00U);
                    }
                    append_utf8(output, code_point);
                    break;
                }
                default:
                    --position_;
                    fail("invalid escape in a string");
                    return false;
            }
        }
    }

    std::string_view text_;
    std::size_t maximum_depth_;
    std::size_t position_{0};
    std::optional<ParseError> error_;
};

void write_string(std::string& output, std::string_view text) {
    if (!is_valid_utf8(text)) {
        throw std::invalid_argument("a JSON string must be valid UTF-8");
    }
    constexpr std::string_view kHex = "0123456789abcdef";
    output.push_back('"');
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(kHex[byte >> 4U]);
                    output.push_back(kHex[byte & 0xFU]);
                } else {
                    output.push_back(character);
                }
        }
    }
    output.push_back('"');
}

std::vector<const Member*> sorted_members(const Object& object) {
    std::vector<const Member*> members;
    members.reserve(object.size());
    for (const auto& member : object) {
        members.push_back(&member);
    }
    std::sort(members.begin(), members.end(), [](const Member* left, const Member* right) {
        return left->first < right->first;
    });
    for (std::size_t index = 1; index < members.size(); ++index) {
        if (members[index - 1]->first == members[index]->first) {
            throw std::invalid_argument(
                "a JSON object repeats the key " + members[index]->first
            );
        }
    }
    return members;
}

void write_value(
    std::string& output,
    const Value& value,
    std::optional<std::size_t> indent_depth
) {
    const auto newline = [&](std::size_t depth) {
        if (indent_depth) {
            output.push_back('\n');
            output.append(depth * 2, ' ');
        }
    };
    const std::size_t depth = indent_depth.value_or(0);
    const auto child_depth = indent_depth
        ? std::optional<std::size_t>(depth + 1)
        : std::nullopt;

    if (value.is_null()) {
        output += "null";
    } else if (const bool* boolean = value.if_bool()) {
        output += *boolean ? "true" : "false";
    } else if (const std::string* text = value.if_string()) {
        write_string(output, *text);
    } else if (const Array* array = value.if_array()) {
        output.push_back('[');
        for (std::size_t index = 0; index < array->size(); ++index) {
            if (index > 0) {
                output.push_back(',');
            }
            newline(depth + 1);
            write_value(output, (*array)[index], child_depth);
        }
        if (!array->empty()) {
            newline(depth);
        }
        output.push_back(']');
    } else {
        const auto members = sorted_members(*value.if_object());
        output.push_back('{');
        for (std::size_t index = 0; index < members.size(); ++index) {
            if (index > 0) {
                output.push_back(',');
            }
            newline(depth + 1);
            write_string(output, members[index]->first);
            output.push_back(':');
            if (indent_depth) {
                output.push_back(' ');
            }
            write_value(output, members[index]->second, child_depth);
        }
        if (!members.empty()) {
            newline(depth);
        }
        output.push_back('}');
    }
}

}  // namespace

std::variant<Value, ParseError> parse(
    std::string_view text,
    std::size_t maximum_depth
) {
    return Parser(text, maximum_depth).parse_document();
}

std::string serialize(const Value& value) {
    std::string output;
    write_value(output, value, std::nullopt);
    return output;
}

std::string serialize_indented(const Value& value) {
    std::string output;
    write_value(output, value, std::size_t{0});
    return output;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t length = utf8_sequence_length(text, index);
        if (length == 0) {
            return false;
        }
        index += length;
    }
    return true;
}

std::optional<std::uint64_t> parse_canonical_unsigned(
    std::string_view text
) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return value;
}

std::optional<std::int64_t> parse_canonical_signed(
    std::string_view text
) noexcept {
    const bool negative = !text.empty() && text.front() == '-';
    const auto magnitude = parse_canonical_unsigned(
        negative ? text.substr(1) : text
    );
    if (!magnitude) {
        return std::nullopt;
    }
    constexpr auto kMaximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!negative) {
        if (*magnitude > kMaximum) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(*magnitude);
    }
    if (*magnitude == 0 || *magnitude > kMaximum + 1) {
        return std::nullopt;
    }
    if (*magnitude == kMaximum + 1) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(*magnitude);
}

}  // namespace chronos::canonical_json
