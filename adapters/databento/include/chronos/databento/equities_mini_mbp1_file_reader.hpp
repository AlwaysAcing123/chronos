#pragma once

#include "chronos/databento/equities_mini_mbp1_normalizer.hpp"
#include "chronos/databento/recording_manifest.hpp"
#include "chronos/market_data_stream_validator.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace chronos::databento_adapter {

enum class EquitiesMiniMbp1FileErrorCode {
    SourcePathNotRegularFile,
    SourceFileInspectionFailed,
    SourceFileOpenFailed,
    SourceFileChanged,
    SourceHashFailed,
    MetadataDecodeFailed,
    UnexpectedDataset,
    MissingSchema,
    UnexpectedSchema,
    UnexpectedOutputSymbology,
    SymbolMapCreationFailed,
    RecordDecodeFailed,
    UnexpectedRecordType,
    SymbolResolutionFailed,
    RecordNormalizationFailed,
    StreamValidationFailed,
    CounterOverflow,
};

struct EquitiesMiniMbp1FileError {
    EquitiesMiniMbp1FileErrorCode code;
    std::string field;
    std::string message;
    std::optional<std::uint64_t> source_record_number;
    std::optional<EquitiesMiniMbp1Error> normalization_error;
    std::optional<MarketDataStreamValidationError> stream_error;
};

struct EndOfRecording {};

class EquitiesMiniMbp1FileReader;

using EquitiesMiniMbp1FileOpenResult = std::variant<
    std::unique_ptr<EquitiesMiniMbp1FileReader>,
    EquitiesMiniMbp1FileError
>;

using EquitiesMiniMbp1FileReadResult = std::variant<
    ValidatedMarketEvent,
    EndOfRecording,
    EquitiesMiniMbp1FileError
>;

class EquitiesMiniMbp1FileReader final {
public:
    [[nodiscard]] static EquitiesMiniMbp1FileOpenResult open(
        const std::filesystem::path& source_path,
        MarketDataStreamId stream,
        std::uint64_t initial_ingest_sequence = 0
    );

    ~EquitiesMiniMbp1FileReader();

    EquitiesMiniMbp1FileReader(const EquitiesMiniMbp1FileReader&) = delete;
    EquitiesMiniMbp1FileReader& operator=(
        const EquitiesMiniMbp1FileReader&
    ) = delete;
    EquitiesMiniMbp1FileReader(EquitiesMiniMbp1FileReader&&) = delete;
    EquitiesMiniMbp1FileReader& operator=(
        EquitiesMiniMbp1FileReader&&
    ) = delete;

    [[nodiscard]] EquitiesMiniMbp1FileReadResult read_next();

    [[nodiscard]] const std::optional<RecordingManifest>& manifest()
        const noexcept;

private:
    class Impl;

    explicit EquitiesMiniMbp1FileReader(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

}  // namespace chronos::databento_adapter
