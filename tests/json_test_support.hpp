#pragma once

#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace chronos::test {

// The paper-trading API never emits JSON numbers; every financial value,
// integer, and timestamp is a string. This walks a whole document.
inline void require_no_json_numbers(
    const nlohmann::json& value,
    const std::string& path = "$"
) {
    if (value.is_number()) {
        throw TestFailure("JSON number at " + path);
    }
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            require_no_json_numbers(item.value(), path + "." + item.key());
        }
    } else if (value.is_array()) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            require_no_json_numbers(
                value[index],
                path + "[" + std::to_string(index) + "]"
            );
        }
    }
}

}  // namespace chronos::test
