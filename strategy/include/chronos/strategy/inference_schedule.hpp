#pragma once

#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_manifest.hpp"
#include "chronos/datasets/derived_artifact.hpp"
#include "chronos/features/top_of_book_features.hpp"
#include "chronos/market_data_types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// The inference schedules the Python research package publishes (see
// python/chronos_research/schedule.py and docs/strategy-evaluation.md): one
// fitted model's predictions on every feature row of one instrument in one
// evaluation window, without any label, label time, or future observation.
//
//     <schedule-root>/<schedule-id>/manifest.json
//     <schedule-root>/<schedule-id>/schedule.ndjson
//
// The strategy runner never trusts a schedule's claims about the market: it
// recomputes each row's features from the verified dataset and requires the
// schedule's identity, time, and model inputs to match them exactly.
namespace chronos::strategy {

inline constexpr std::string_view kScheduleSchemaName = "chronos.inference_schedule";
// Version 1 schedules a verified experiment's model; version 2 a registered
// model version (ADR 0021). Both stay readable, with their own meaning.
inline constexpr std::string_view kExperimentScheduleVersion = "1";
inline constexpr std::string_view kRegisteredScheduleVersion = "2";
inline constexpr std::string_view kScheduleRowRecordName = "chronos.inference_row";
inline constexpr std::string_view kScheduleRowRecordVersion = "1";
inline constexpr std::string_view kScheduleRowsFileName = "schedule.ndjson";
inline constexpr datasets::DerivedArtifactLayout kScheduleLayout{
    .kind = "inference schedule",
    .rows_file = kScheduleRowsFileName,
};
inline constexpr std::uint64_t kConfidenceScale = 1'000'000'000;
inline constexpr std::size_t kModelInputs = 5;

enum class PredictedClass {
    Down,
    Flat,
    Up,
};

[[nodiscard]] std::string_view predicted_class_name(PredictedClass value) noexcept;

// [start_at, end_at), or through the end of the dataset without an end.
struct ScheduleWindow {
    std::string name;
    Timestamp start_at;
    std::optional<Timestamp> end_at;

    [[nodiscard]] bool contains(Timestamp at) const noexcept {
        return start_at <= at && (!end_at || at < *end_at);
    }
    [[nodiscard]] bool ended_by(Timestamp at) const noexcept { return end_at && at >= *end_at; }
    friend bool operator==(const ScheduleWindow&, const ScheduleWindow&) = default;
};

// The model of a version 1 schedule: a verified experiment's fitted model,
// trained on labels all known before the window.
struct ExperimentScheduleModel {
    std::string experiment_id;
    std::string name;
    std::string parameters_sha256;
    Timestamp training_labels_available_through;
};

// The model of a version 2 schedule: a registered model version, whose
// information cutoff is at or before the window's start. A declared cutoff
// is the uploader's claim, which the run records as such.
struct RegisteredScheduleModel {
    std::string model_version_id;
    std::string family;
    std::string payload_sha256;
    Timestamp information_cutoff;
    // "verified" or "declared".
    std::string cutoff_basis;
};

using ScheduleModel = std::variant<ExperimentScheduleModel, RegisteredScheduleModel>;

struct ScheduleManifest {
    std::string artifact_id;
    // The verified sources the model was trained and evaluated on.
    std::string dataset_id;
    MarketDataStreamId stream;
    std::string feature_artifact_id;
    std::string features_sha256;
    std::string label_artifact_id;
    std::string split_artifact_id;
    std::int64_t horizon_ns{0};
    InstrumentId instrument;
    ScheduleModel model;
    ScheduleWindow window;
    std::uint64_t rows{0};
    std::uint64_t predicted{0};
    datasets::FileDigest schedule;
};

// The schedule ID: the SHA-256 of the canonical manifest document, with its
// newline, without artifact_id, as the Python package defines it.
[[nodiscard]] std::variant<ScheduleManifest, datasets::DatasetError> parse_schedule_manifest(
    std::string_view bytes
);

// The model inputs of the model feature set top_of_book_baseline version 1,
// as raw integers: spread, imbalance, microprice - midpoint, bid_size,
// ask_size. Unavailable with the first unavailable source feature, in the
// order spread, imbalance, microprice, midpoint, bid_size, ask_size.
using ModelInputs = std::variant<std::array<std::int64_t, kModelInputs>, features::Unavailability>;

[[nodiscard]] ModelInputs model_inputs(const features::TopOfBookFeatureRow& row);

struct SchedulePrediction {
    PredictedClass predicted_class{PredictedClass::Flat};
    // floor(1e9 x the predicted class's probability).
    std::uint64_t confidence_ppb{0};
    friend bool operator==(const SchedulePrediction&, const SchedulePrediction&) = default;
};

struct ScheduleRow {
    std::uint64_t ingest_sequence{0};
    Timestamp available_at;
    ModelInputs inputs;
    // Absent exactly when the inputs are unavailable.
    std::optional<SchedulePrediction> prediction;
};

// Reads a schedule's rows in order, checking each as it goes: canonical, of
// the manifest's stream and instrument, inside its window, after the
// previous row in ingest order, with consistent inputs and prediction. The
// end is reported only once the count, checksum, and counts match the
// manifest.
class ScheduleReader final {
public:
    [[nodiscard]] static std::variant<std::unique_ptr<ScheduleReader>, datasets::DatasetError> open(
        const std::filesystem::path& directory
    );
    ~ScheduleReader();
    ScheduleReader(const ScheduleReader&) = delete;
    ScheduleReader& operator=(const ScheduleReader&) = delete;

    [[nodiscard]] const ScheduleManifest& manifest() const noexcept;
    [[nodiscard]] std::variant<ScheduleRow, datasets::EndOfRows, datasets::DatasetError> next();

private:
    class State;
    explicit ScheduleReader(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace chronos::strategy
