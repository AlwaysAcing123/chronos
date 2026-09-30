#include "chronos/datasets/dataset_catalog.hpp"

#include "chronos/datasets/artifact_directory.hpp"
#include "chronos/datasets/dataset_reader.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::datasets {
namespace {

std::variant<DatasetManifest, DatasetError> entry_status(
    const std::filesystem::path& catalog,
    const std::string& name,
    CatalogCheck check
) {
    if (name.starts_with(kStagingPrefix)) {
        return dataset_error(
            DatasetErrorCode::UnexpectedEntry,
            "catalog",
            "staging directory of an unfinished or interrupted write"
        );
    }
    if (check == CatalogCheck::Events) {
        return verify_dataset(catalog / name);
    }
    auto opened = open_dataset(catalog / name);
    if (auto* error = std::get_if<DatasetError>(&opened)) {
        return std::move(*error);
    }
    return std::get<OpenedDataset>(std::move(opened)).manifest;
}

}  // namespace

std::variant<std::vector<CatalogEntry>, DatasetError> list_catalog(
    const std::filesystem::path& catalog,
    CatalogCheck check
) {
    std::error_code inspection_error;
    if (!std::filesystem::is_directory(catalog, inspection_error)) {
        return dataset_error(
            DatasetErrorCode::CatalogUnavailable,
            "catalog",
            "catalog root must be an existing directory"
        );
    }
    auto names = directory_entries(catalog);
    if (auto* error = std::get_if<DatasetError>(&names)) {
        return std::move(*error);
    }
    std::vector<CatalogEntry> entries;
    for (const std::string& name : std::get<std::vector<std::string>>(names)) {
        entries.push_back(CatalogEntry{.name = name, .status = entry_status(catalog, name, check)});
    }
    return entries;
}

}  // namespace chronos::datasets
