#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace chronos::datasets {

// A catalog is a directory of dataset directories named by their IDs. There
// is no index to keep consistent: the valid entries are the catalog.
enum class CatalogCheck {
    // Structure, manifest, identity, and the quality file's checksum.
    Manifests,
    // Also every event, as verify_dataset does.
    Events,
};

struct CatalogEntry {
    std::string name;
    // The dataset, or why the entry is not a valid one. A staging directory
    // from an unfinished or interrupted write is reported, never listed as
    // a dataset.
    std::variant<DatasetManifest, DatasetError> status;
};

// Every entry of the catalog in byte order of name.
[[nodiscard]] std::variant<std::vector<CatalogEntry>, DatasetError> list_catalog(
    const std::filesystem::path& catalog,
    CatalogCheck check = CatalogCheck::Manifests
);

}  // namespace chronos::datasets
