#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/trading_types.hpp"

#include <cstdint>
#include <initializer_list>
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

// Parses a document that must be byte-identical to its canonical spelling
// followed by exactly one newline.
[[nodiscard]] std::variant<canonical_json::Value, DatasetError>
parse_canonical_document(std::string_view bytes, std::string_view field);

// The canonical spelling of a document followed by one newline.
[[nodiscard]] std::string canonical_document(const canonical_json::Value& value);

}  // namespace chronos::datasets
