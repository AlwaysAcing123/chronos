#pragma once

#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/features/feature_artifact.hpp"
#include "chronos/labels/label_artifact.hpp"
#include "chronos/sha256.hpp"
#include "dataset_test_support.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Builders for the derived-artifact tests: published feature and label
// artifacts from synthetic events, and forged ones with arbitrary content
// that the code under test must judge by itself.
namespace chronos::test {

template <typename Value>
Value succeeded(std::variant<Value, datasets::DatasetError> result, std::string_view what) {
    if (auto* error = std::get_if<datasets::DatasetError>(&result)) {
        throw TestFailure(std::string(what) + " failed: " + error->field + ": " + error->message);
    }
    return std::get<Value>(std::move(result));
}

template <typename Value>
datasets::DatasetError failed(std::variant<Value, datasets::DatasetError> result) {
    REQUIRE(std::holds_alternative<datasets::DatasetError>(result));
    return std::get<datasets::DatasetError>(std::move(result));
}

inline std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t position = text.find(from);
    REQUIRE(position != std::string::npos);
    return text.replace(position, from.size(), to);
}

inline std::size_t entries_in(const std::filesystem::path& directory) {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator(directory),
        std::filesystem::directory_iterator()
    ));
}

// A two-sided quote of size 1 on each side, received when it happened.
inline ValidatedMarketEvent two_sided(
    std::string instrument,
    std::uint64_t sequence,
    std::int64_t receive_ns,
    std::string_view bid,
    std::string_view ask
) {
    return quote(header(std::move(instrument), sequence, receive_ns, receive_ns), level(bid, "1"), level(ask, "1"));
}

inline std::vector<features::TopOfBookFeatureRow> feature_rows_of(const std::vector<ValidatedMarketEvent>& events) {
    features::TopOfBookFeatureEngine engine;
    std::vector<features::TopOfBookFeatureRow> rows;
    for (const auto& event : events) {
        if (auto row = engine.apply(event)) {
            rows.push_back(std::move(*row));
        }
    }
    return rows;
}

// A published dataset and its feature artifact, in scratch directories that
// live as long as this object.
class FeatureFixture final {
public:
    explicit FeatureFixture(const std::vector<ValidatedMarketEvent>& events)
        : dataset_(published(write_dataset(catalog_.path(), events))),
          artifact_(succeeded(
              features::extract_top_of_book_features(dataset_.directory, features_.path()),
              "feature extraction"
          )) {}

    [[nodiscard]] const std::filesystem::path& root() const { return features_.path(); }
    [[nodiscard]] const features::PublishedFeatureArtifact& artifact() const { return artifact_; }
    [[nodiscard]] const std::filesystem::path& directory() const { return artifact_.directory; }

private:
    TemporaryDirectory catalog_;
    TemporaryDirectory features_;
    datasets::PublishedDataset dataset_;
    features::PublishedFeatureArtifact artifact_;
};

inline labels::PublishedLabelArtifact extracted_labels(
    const std::filesystem::path& feature_artifact,
    const std::filesystem::path& root,
    std::int64_t horizon_ns
) {
    return succeeded(
        labels::extract_future_midpoint_return_labels(feature_artifact, root, horizon_ns),
        "label extraction"
    );
}

// A feature row with only a midpoint, for forging artifacts whose values a
// valid dataset could never produce.
inline features::TopOfBookFeatureRow midpoint_row(
    std::string instrument,
    std::uint64_t sequence,
    std::int64_t available_ns,
    features::Feature<Price> midpoint
) {
    const auto missing = features::Unavailability::MissingBothSides;
    const Timestamp at = Timestamp::from_nanoseconds(available_ns);
    return features::TopOfBookFeatureRow{
        .stream = MarketDataStreamId(std::string(kDatasetStream)),
        .instrument = InstrumentId(std::move(instrument)),
        .ingest_sequence = sequence,
        .event_timestamp = at,
        .receive_timestamp = at,
        .available_at = at,
        .quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay),
        .trading_status = std::nullopt,
        .best_bid = missing,
        .best_ask = missing,
        .bid_size = missing,
        .ask_size = missing,
        .spread = missing,
        .midpoint = midpoint,
        .imbalance = missing,
        .microprice = missing,
    };
}

// Writes a structurally valid feature artifact holding exactly these rows,
// with a self-consistent manifest, and returns its directory.
inline std::filesystem::path forge_feature_artifact(
    const std::filesystem::path& root,
    const std::vector<features::TopOfBookFeatureRow>& rows
) {
    features::FeatureArtifactManifest manifest{
        .artifact_id = {},
        .dataset_id = std::string(64, 'd'),
        .dataset_events_sha256 = std::string(64, 'e'),
        .stream = MarketDataStreamId(std::string(kDatasetStream)),
        .rows = rows.size(),
        .rows_by_instrument = {},
        .unavailable = {},
        .features = {},
    };
    std::string bytes;
    for (const auto& row : rows) {
        bytes += features::encode_feature_row(row) + "\n";
        ++manifest.rows_by_instrument[row.instrument.value()];
        const auto count = [&](std::string_view name, const auto& feature) {
            if (const auto* reason = std::get_if<features::Unavailability>(&feature)) {
                ++manifest.unavailable[std::string(name)][std::string(features::unavailability_name(*reason))];
            }
        };
        count("ask_size", row.ask_size);
        count("best_ask", row.best_ask);
        count("best_bid", row.best_bid);
        count("bid_size", row.bid_size);
        count("imbalance", row.imbalance);
        count("microprice", row.microprice);
        count("midpoint", row.midpoint);
        count("spread", row.spread);
    }
    manifest.features = datasets::FileDigest{.sha256 = Sha256::hex(bytes), .size_bytes = bytes.size()};
    manifest.artifact_id = features::feature_artifact_identity(manifest);
    const std::filesystem::path directory = root / manifest.artifact_id;
    std::filesystem::create_directory(directory);
    write_file(directory / features::kFeatureRowsFileName, bytes);
    write_file(directory / features::kFeatureManifestFileName, features::serialize_feature_manifest(manifest));
    return directory;
}

// Writes rows under a label manifest whose digest, size, and ID match them
// (unless the caller keeps the manifest's digest), so only the rows' content
// can be at fault.
inline std::filesystem::path forge_label_artifact(
    const std::filesystem::path& root,
    labels::LabelArtifactManifest manifest,
    std::string_view rows,
    bool keep_digest = false
) {
    if (!keep_digest) {
        manifest.labels = datasets::FileDigest{.sha256 = Sha256::hex(rows), .size_bytes = rows.size()};
    }
    manifest.artifact_id = labels::label_artifact_identity(manifest);
    const std::filesystem::path directory = root / manifest.artifact_id;
    std::filesystem::create_directory(directory);
    write_file(directory / labels::kLabelRowsFileName, rows);
    write_file(directory / datasets::kArtifactManifestFileName, labels::serialize_label_manifest(manifest));
    return directory;
}

}  // namespace chronos::test
