#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/sha256.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Argument parsing and JSON output shared by the dataset command-line tools.
// Every document goes to stdout on success and to stderr as {"error": ...}
// on failure, with stable codes and no provider or system internals.
namespace chronos::dataset_cli {

namespace json = chronos::canonical_json;

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Options of the form --name value, each given at most once, with a value
// that is neither empty nor another option.
class Options final {
public:
    Options(int argc, char* argv[], int first, std::set<std::string> known, std::set<std::string> flags = {}) {
        for (int index = first; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help" || option == "-h") {
                throw UsageError("");
            }
            if (!option.starts_with("--") || (!known.contains(option) && !flags.contains(option))) {
                throw UsageError("unknown option: " + option);
            }
            if (values_.contains(option)) {
                throw UsageError(option + " may be provided only once");
            }
            if (flags.contains(option)) {
                values_.emplace(option, "");
                continue;
            }
            const std::string value = index + 1 < argc ? argv[++index] : "";
            if (value.empty() || value.starts_with("--")) {
                throw UsageError(option + " requires a value");
            }
            values_.emplace(option, value);
        }
    }

    [[nodiscard]] std::string required(const std::string& option) const {
        const auto found = values_.find(option);
        if (found == values_.end() || found->second.empty()) {
            throw UsageError(option + " is required");
        }
        return found->second;
    }

    [[nodiscard]] std::optional<std::string> optional(const std::string& option) const {
        const auto found = values_.find(option);
        return found == values_.end() ? std::nullopt : std::optional(found->second);
    }

    [[nodiscard]] bool flag(const std::string& option) const { return values_.contains(option); }

private:
    std::map<std::string, std::string> values_;
};

// Text for output: valid UTF-8 unchanged, and each byte that is not part
// of a valid sequence as \xNN. File names and paths are bytes on some
// systems, and JSON can only carry UTF-8.
inline std::string printable(std::string_view text) {
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string output;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::size_t length = lead < 0x80U ? 1 : lead < 0xE0U ? 2 : lead < 0xF0U ? 3 : 4;
        if (index + length <= text.size() && json::is_valid_utf8(text.substr(index, length))) {
            output.append(text.substr(index, length));
            index += length;
        } else {
            output += "\\x";
            output.push_back(kHex[lead >> 4U]);
            output.push_back(kHex[lead & 0xFU]);
            ++index;
        }
    }
    return output;
}

inline std::string printable_path(const std::filesystem::path& path) {
    return printable(std::string_view(path.native()));
}

inline json::Value decimal(std::uint64_t value) { return json::Value(std::to_string(value)); }
inline json::Value decimal(std::int64_t value) { return json::Value(std::to_string(value)); }

inline json::Value error_json(const datasets::DatasetError& error) {
    return json::Value(json::Object{
        {"code", datasets::error_code_name(error.code)},
        {"field", printable(error.field)},
        {"line", error.line ? decimal(*error.line) : json::Value(nullptr)},
        {"message", printable(error.message)},
    });
}

inline json::Value warning_json(const std::optional<datasets::DatasetError>& warning) {
    return warning ? error_json(*warning) : json::Value(nullptr);
}

// The last resort for main(): even an unexpected failure is one JSON
// document.
inline int fail_unexpectedly(std::string_view what) {
    std::cerr << json::serialize_indented(json::Value(json::Object{{"error", json::Object{
        {"code", "internal_error"},
        {"field", ""},
        {"line", nullptr},
        {"message", printable(what)},
    }}})) << '\n';
    return 1;
}

inline int print(const json::Value& document) {
    std::cout << json::serialize_indented(document) << '\n';
    return 0;
}

inline int fail(json::Value error) {
    std::cerr << json::serialize_indented(json::Value(json::Object{{"error", std::move(error)}})) << '\n';
    return 1;
}

inline int fail(const datasets::DatasetError& error) {
    return fail(error_json(error));
}

// The facts about a dataset a person or script usually needs.
inline json::Value dataset_summary_json(
    const datasets::DatasetManifest& manifest,
    const std::filesystem::path& directory
) {
    const datasets::DatasetSummary& summary = manifest.summary;
    json::Array instruments;
    json::Object by_instrument;
    for (const auto& [instrument, count] : summary.by_instrument) {
        instruments.emplace_back(instrument);
        by_instrument.emplace_back(instrument, decimal(count));
    }
    return json::Value(json::Object{
        {"checksums", json::Object{
            {"events_sha256", manifest.events.sha256},
            {"quality_sha256", manifest.quality.sha256},
            {"recording_sha256", manifest.source.recording_sha256},
        }},
        {"counts", json::Object{
            {"by_event_type", datasets::event_type_counts_json(summary.by_type)},
            {"by_instrument", std::move(by_instrument)},
            {"events", decimal(summary.events)},
        }},
        {"dataset_id", manifest.dataset_id},
        {"directory", printable_path(directory)},
        {"instruments", std::move(instruments)},
        {"provider", manifest.source.provider},
        {"sizes_bytes", json::Object{
            {"events", decimal(manifest.events.size_bytes)},
            {"quality", decimal(manifest.quality.size_bytes)},
        }},
        {"stream", manifest.stream.value()},
        {"time_range", json::Object{
            {"event_timestamp_ns", json::Object{
                {"earliest", decimal(summary.earliest_event_timestamp.nanoseconds())},
                {"latest", decimal(summary.latest_event_timestamp.nanoseconds())},
            }},
            {"receive_timestamp_ns", json::Object{
                {"earliest", decimal(summary.earliest_receive_timestamp.nanoseconds())},
                {"latest", decimal(summary.latest_receive_timestamp.nanoseconds())},
            }},
        }},
    });
}

// <root option>/<id option>, once the ID is a 64-digit lowercase ID. The
// options are read in this order, so a missing one is always reported the
// same way.
inline std::filesystem::path id_path(
    const Options& options,
    const std::string& root_option,
    const std::string& id_option
) {
    const std::filesystem::path root = options.required(root_option);
    const std::string id = options.required(id_option);
    if (!is_sha256_hex(id)) {
        throw UsageError(id_option + " must be a 64-digit lowercase ID");
    }
    return root / id;
}

// A canonical decimal integer in [1, maximum]: no sign, no leading zero, no
// exponent or unit.
inline std::uint64_t positive_integer(const std::string& text, std::string_view option, std::uint64_t maximum) {
    const auto value = json::parse_canonical_unsigned(text);
    if (!value || *value == 0 || *value > maximum) {
        throw UsageError(
            std::string(option) + " must be a whole number from 1 to " + std::to_string(maximum) +
            ", written without a sign, leading zeros, or a unit"
        );
    }
    return *value;
}

// The artifacts of a root and every entry that is not one.
template <typename Manifest, typename Summary>
json::Value artifact_list_json(
    const std::filesystem::path& root,
    const std::vector<datasets::DerivedArtifactEntry<Manifest>>& entries,
    Summary summary
) {
    json::Array artifacts;
    json::Array invalid;
    for (const auto& entry : entries) {
        if (const auto* manifest = std::get_if<Manifest>(&entry.status)) {
            artifacts.push_back(summary(*manifest, root / entry.name));
        } else {
            invalid.push_back(json::Value(json::Object{
                {"error", error_json(std::get<datasets::DatasetError>(entry.status))},
                {"name", printable(entry.name)},
            }));
        }
    }
    return json::Value(json::Object{
        {"artifacts", std::move(artifacts)},
        {"invalid_entries", std::move(invalid)},
        {"output", printable_path(root)},
    });
}

inline std::string_view outcome_name(datasets::PublicationOutcome outcome) {
    return outcome == datasets::PublicationOutcome::Published ? "published" : "already_present";
}

// Creates a catalog or artifact root, which may not exist yet. Its parent
// must.
inline std::optional<datasets::DatasetError> ensure_directory(
    const std::filesystem::path& directory,
    std::string_view field
) {
    std::error_code error;
    std::filesystem::create_directory(directory, error);
    if (error || !std::filesystem::is_directory(directory, error)) {
        return datasets::dataset_error(
            datasets::DatasetErrorCode::CatalogUnavailable,
            std::string(field),
            "must be a directory, or a new one in an existing directory"
        );
    }
    return std::nullopt;
}

}  // namespace chronos::dataset_cli
