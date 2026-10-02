#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/trading_types.hpp"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::datasets {

// Strict typed access to one JSON object of a persisted document. The key
// set must be exactly the one the schema declares: a missing or an unknown
// key is an error, never a default. Every accessor records the first error
// and returns an empty value afterwards, so a decoder reads all fields and
// checks error() once.
class ObjectFields final {
public:
    ObjectFields(
        const canonical_json::Value& value,
        std::string path,
        std::optional<std::uint64_t> line = std::nullopt
    );

    // Requires exactly these keys. Call once, before reading fields.
    void require_keys(std::initializer_list<std::string_view> keys);

    [[nodiscard]] const canonical_json::Value* value(std::string_view key);
    [[nodiscard]] std::string string(std::string_view key);
    [[nodiscard]] std::optional<std::string> nullable_string(std::string_view key);
    [[nodiscard]] std::uint64_t unsigned_integer(std::string_view key);
    [[nodiscard]] std::optional<std::uint64_t> nullable_unsigned_integer(
        std::string_view key
    );
    [[nodiscard]] std::int64_t signed_integer(std::string_view key);
    [[nodiscard]] Timestamp timestamp(std::string_view key);
    [[nodiscard]] std::optional<Timestamp> nullable_timestamp(std::string_view key);
    [[nodiscard]] std::string sha256(std::string_view key);
    // Requires the string to be exactly `expected`, such as a schema name.
    void constant(std::string_view key, std::string_view expected);
    [[nodiscard]] const canonical_json::Object* object(std::string_view key);
    [[nodiscard]] const canonical_json::Array* array(std::string_view key);
    [[nodiscard]] std::vector<std::string> string_array(std::string_view key);

    [[nodiscard]] std::string field(std::string_view key) const;
    void fail(DatasetErrorCode code, std::string_view key, std::string message);
    // Records another reader's error, such as a nested object's, unless one
    // is already recorded.
    void adopt(const std::optional<DatasetError>& error);

    [[nodiscard]] const std::optional<DatasetError>& error() const noexcept {
        return error_;
    }
    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }

private:
    const canonical_json::Object* object_{nullptr};
    std::string path_;
    std::optional<std::uint64_t> line_;
    std::optional<DatasetError> error_;
};

// An object nested in another, with exactly the given keys. finish() passes
// its first error, with its full path, to the parent, so the caller checks
// one place.
class NestedFields final {
public:
    NestedFields(
        ObjectFields& parent,
        std::string_view key,
        std::initializer_list<std::string_view> keys
    );

    [[nodiscard]] ObjectFields& fields() noexcept { return fields_; }
    void finish() { parent_.adopt(fields_.error()); }

private:
    ObjectFields& parent_;
    ObjectFields fields_;
};

// {"name": name, "version": version}, as manifests name a schema, a record,
// a feature or label set, or a generator.
[[nodiscard]] canonical_json::Value name_version_json(
    std::string_view name,
    std::string_view version
);

// Requires parent[key] to be exactly name_version_json(name, version).
void require_name_version(
    ObjectFields& parent,
    std::string_view key,
    std::string_view name,
    std::string_view version
);

// Adds value to total unless the sum would overflow, in which case it
// returns false and leaves total unchanged.
[[nodiscard]] inline bool add_count(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > UINT64_MAX - total) {
        return false;
    }
    total += value;
    return true;
}

// Names mapped to positive counts as canonical decimal strings; a zero
// count is never written, so every count map has one spelling.
[[nodiscard]] canonical_json::Value count_map_json(
    const std::map<std::string, std::uint64_t>& counts
);
[[nodiscard]] std::map<std::string, std::uint64_t> parse_count_map(
    ObjectFields& fields,
    std::string_view key
);

// Parses a document that must be byte-identical to its canonical spelling
// followed by exactly one newline.
[[nodiscard]] std::variant<canonical_json::Value, DatasetError>
parse_canonical_document(std::string_view bytes, std::string_view field);

// The canonical spelling of a document followed by one newline.
[[nodiscard]] std::string canonical_document(const canonical_json::Value& value);

}  // namespace chronos::datasets
