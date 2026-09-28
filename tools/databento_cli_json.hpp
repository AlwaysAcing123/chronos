#pragma once

#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace chronos::databento_cli {

inline nlohmann::json recording_error_json(
    const databento_adapter::EquitiesMiniMbp1FileError& error
) {
    nlohmann::json document = {
        {"field", error.field},
        {"message", error.message},
    };
    if (error.source_record_number) {
        document["source_record_number"] = *error.source_record_number;
    }
    if (error.normalization_error) {
        const auto& normalization = *error.normalization_error;
        nlohmann::json validation_errors = nlohmann::json::array();
        for (const auto& validation : normalization.validation_errors) {
            validation_errors.push_back({
                {"field", validation.field},
                {"message", validation.message},
            });
        }
        document["normalization_error"] = {
            {"field", normalization.field},
            {"message", normalization.message},
            {"validation_errors", std::move(validation_errors)},
        };
    }
    if (error.stream_error) {
        document["stream_error"] = {
            {"field", error.stream_error->field},
            {"message", error.stream_error->message},
        };
    }
    return document;
}

}  // namespace chronos::databento_cli
