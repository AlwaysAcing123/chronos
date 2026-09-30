#include "chronos/datasets/json_fields.hpp"

#include "chronos/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::datasets {

namespace json = chronos::canonical_json;

ObjectFields::ObjectFields(
    const json::Value& value,
    std::string path,
    std::optional<std::uint64_t> line
)
    : object_(value.if_object()), path_(std::move(path)), line_(line) {
    if (object_ == nullptr) {
        fail(DatasetErrorCode::InvalidValue, "", "must be a JSON object");
    }
}

std::string ObjectFields::field(std::string_view key) const {
    if (key.empty()) {
        return path_;
    }
    return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
}

void ObjectFields::fail(
    DatasetErrorCode code,
    std::string_view key,
    std::string message
) {
    if (!error_) {
        error_ = dataset_error(code, field(key), std::move(message), line_);
    }
}

void ObjectFields::adopt(const std::optional<DatasetError>& error) {
    if (!error_ && error) {
        error_ = error;
    }
}

void ObjectFields::require_keys(std::initializer_list<std::string_view> keys) {
    if (!ok()) {
        return;
    }
    for (const auto& member : *object_) {
        if (std::find(keys.begin(), keys.end(), member.first) == keys.end()) {
            fail(DatasetErrorCode::UnknownField, member.first, "is not a known field");
            return;
        }
    }
    for (const std::string_view key : keys) {
        const bool present = std::any_of(
            object_->begin(),
            object_->end(),
            [key](const json::Member& member) { return member.first == key; }
        );
        if (!present) {
            fail(DatasetErrorCode::MissingField, key, "is required");
            return;
        }
    }
}

const json::Value* ObjectFields::value(std::string_view key) {
    if (!ok()) {
        return nullptr;
    }
    for (const auto& member : *object_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    fail(DatasetErrorCode::MissingField, key, "is required");
    return nullptr;
}

std::string ObjectFields::string(std::string_view key) {
    const json::Value* found = value(key);
    if (found == nullptr) {
        return {};
    }
    if (const std::string* text = found->if_string()) {
        return *text;
    }
    fail(DatasetErrorCode::InvalidValue, key, "must be a string");
    return {};
}

std::optional<std::string> ObjectFields::nullable_string(std::string_view key) {
    const json::Value* found = value(key);
    if (found == nullptr || found->is_null()) {
        return std::nullopt;
    }
    if (const std::string* text = found->if_string()) {
        return *text;
    }
    fail(DatasetErrorCode::InvalidValue, key, "must be a string or null");
    return std::nullopt;
}

std::uint64_t ObjectFields::unsigned_integer(std::string_view key) {
    const std::string text = string(key);
    if (!ok()) {
        return 0;
    }
    const auto parsed = json::parse_canonical_unsigned(text);
    if (!parsed) {
        fail(
            DatasetErrorCode::InvalidValue,
            key,
            "must be a canonical unsigned decimal integer"
        );
        return 0;
    }
    return *parsed;
}

std::optional<std::uint64_t> ObjectFields::nullable_unsigned_integer(
    std::string_view key
) {
    const auto text = nullable_string(key);
    if (!text || !ok()) {
        return std::nullopt;
    }
    const auto parsed = json::parse_canonical_unsigned(*text);
    if (!parsed) {
        fail(
            DatasetErrorCode::InvalidValue,
            key,
            "must be a canonical unsigned decimal integer or null"
        );
    }
    return parsed;
}

std::int64_t ObjectFields::signed_integer(std::string_view key) {
    const std::string text = string(key);
    if (!ok()) {
        return 0;
    }
    const auto parsed = json::parse_canonical_signed(text);
    if (!parsed) {
        fail(
            DatasetErrorCode::InvalidValue,
            key,
            "must be a canonical signed 64-bit decimal integer"
        );
        return 0;
    }
    return *parsed;
}

Timestamp ObjectFields::timestamp(std::string_view key) {
    return Timestamp::from_nanoseconds(signed_integer(key));
}

std::optional<Timestamp> ObjectFields::nullable_timestamp(std::string_view key) {
    const auto text = nullable_string(key);
    if (!text || !ok()) {
        return std::nullopt;
    }
    const auto parsed = json::parse_canonical_signed(*text);
    if (!parsed) {
        fail(
            DatasetErrorCode::InvalidValue,
            key,
            "must be a canonical signed 64-bit decimal integer or null"
        );
        return std::nullopt;
    }
    return Timestamp::from_nanoseconds(*parsed);
}

std::string ObjectFields::sha256(std::string_view key) {
    std::string text = string(key);
    if (ok() && !is_sha256_hex(text)) {
        fail(
            DatasetErrorCode::InvalidValue,
            key,
            "must be 64 lowercase hexadecimal digits"
        );
    }
    return text;
}

void ObjectFields::constant(std::string_view key, std::string_view expected) {
    const std::string text = string(key);
    if (ok() && text != expected) {
        fail(
            DatasetErrorCode::UnsupportedVersion,
            key,
            "must be " + std::string(expected) + ", not " + text
        );
    }
}

const json::Object* ObjectFields::object(std::string_view key) {
    const json::Value* found = value(key);
    if (found == nullptr) {
        return nullptr;
    }
    if (const json::Object* nested = found->if_object()) {
        return nested;
    }
    fail(DatasetErrorCode::InvalidValue, key, "must be a JSON object");
    return nullptr;
}

const json::Array* ObjectFields::array(std::string_view key) {
    const json::Value* found = value(key);
    if (found == nullptr) {
        return nullptr;
    }
    if (const json::Array* elements = found->if_array()) {
        return elements;
    }
    fail(DatasetErrorCode::InvalidValue, key, "must be a JSON array");
    return nullptr;
}

std::vector<std::string> ObjectFields::string_array(std::string_view key) {
    std::vector<std::string> strings;
    const json::Array* elements = array(key);
    if (elements == nullptr) {
        return strings;
    }
    for (const auto& element : *elements) {
        const std::string* text = element.if_string();
        if (text == nullptr) {
            fail(DatasetErrorCode::InvalidValue, key, "must contain only strings");
            return {};
        }
        strings.push_back(*text);
    }
    return strings;
}

std::variant<json::Value, DatasetError> parse_canonical_document(
    std::string_view bytes,
    std::string_view field
) {
    if (bytes.empty() || bytes.back() != '\n') {
        return dataset_error(
            DatasetErrorCode::MissingFinalNewline,
            std::string(field),
            "document must end with exactly one newline"
        );
    }
    auto parsed = json::parse(bytes.substr(0, bytes.size() - 1));
    if (auto* error = std::get_if<json::ParseError>(&parsed)) {
        return dataset_error(
            DatasetErrorCode::MalformedJson,
            std::string(field),
            "invalid JSON at byte " + std::to_string(error->offset) + ": " +
                error->message
        );
    }
    auto value = std::get<json::Value>(std::move(parsed));
    if (canonical_document(value) != bytes) {
        return dataset_error(
            DatasetErrorCode::NotCanonical,
            std::string(field),
            "document is not in canonical form"
        );
    }
    return value;
}

std::string canonical_document(const json::Value& value) {
    return json::serialize(value) + '\n';
}

}  // namespace chronos::datasets
