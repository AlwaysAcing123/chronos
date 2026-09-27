#pragma once

#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::databento_adapter {

struct HistoricalRecordingQuery {
    std::string start;
    std::string end;
    std::vector<std::string> symbols;
};

struct HistoricalRecordingQuote {
    std::uint64_t estimated_record_count;
    std::uint64_t estimated_billable_size_bytes;
    double estimated_cost_usd;
};

enum class HistoricalRecordingErrorCode {
    InvalidQuery,
    MissingApiKey,
    ProviderConnectionFailed,
    ProviderQuoteFailed,
    InvalidDestination,
    DestinationAlreadyExists,
    CostLimitExceeded,
    TemporaryFileCreationFailed,
    ProviderDownloadFailed,
    PublishFailed,
    RecordingValidationFailed,
    ManifestWriteFailed,
};

struct HistoricalRecordingError {
    HistoricalRecordingErrorCode code;
    std::string field;
    std::string message;
    std::optional<EquitiesMiniMbp1FileError> recording_error;
    std::optional<RecordingManifestWriteError> manifest_error;
};

[[nodiscard]] std::string_view to_string(
    HistoricalRecordingErrorCode code
) noexcept;

using HistoricalRecordingQuoteResult = std::variant<
    HistoricalRecordingQuote,
    HistoricalRecordingError
>;

using HistoricalRecordingDownloadResult = std::variant<
    std::monostate,
    HistoricalRecordingError
>;

class HistoricalRecordingProvider {
public:
    virtual ~HistoricalRecordingProvider() = default;

    [[nodiscard]] virtual HistoricalRecordingQuoteResult quote(
        const HistoricalRecordingQuery& query
    ) = 0;

    [[nodiscard]] virtual HistoricalRecordingDownloadResult download_to_file(
        const HistoricalRecordingQuery& query,
        const std::filesystem::path& destination
    ) = 0;
};

class DatabentoHistoricalRecordingProvider final
    : public HistoricalRecordingProvider {
public:
    using ConnectionResult = std::variant<
        std::unique_ptr<DatabentoHistoricalRecordingProvider>,
        HistoricalRecordingError
    >;

    [[nodiscard]] static ConnectionResult connect_from_environment();

    ~DatabentoHistoricalRecordingProvider() override;

    DatabentoHistoricalRecordingProvider(
        const DatabentoHistoricalRecordingProvider&
    ) = delete;
    DatabentoHistoricalRecordingProvider& operator=(
        const DatabentoHistoricalRecordingProvider&
    ) = delete;
    DatabentoHistoricalRecordingProvider(
        DatabentoHistoricalRecordingProvider&&
    ) = delete;
    DatabentoHistoricalRecordingProvider& operator=(
        DatabentoHistoricalRecordingProvider&&
    ) = delete;

    [[nodiscard]] HistoricalRecordingQuoteResult quote(
        const HistoricalRecordingQuery& query
    ) override;

    [[nodiscard]] HistoricalRecordingDownloadResult download_to_file(
        const HistoricalRecordingQuery& query,
        const std::filesystem::path& destination
    ) override;

private:
    class Impl;

    explicit DatabentoHistoricalRecordingProvider(
        std::unique_ptr<Impl> implementation
    );

    std::unique_ptr<Impl> implementation_;
};

struct HistoricalRecordingAcquisitionRequest {
    HistoricalRecordingQuery query;
    std::filesystem::path destination;
    MarketDataStreamId stream;
    double maximum_cost_usd;
};

struct HistoricalRecordingAcquisitionSuccess {
    HistoricalRecordingQuote quote;
    std::filesystem::path recording_path;
    std::filesystem::path manifest_path;
    RecordingManifest manifest;
};

using HistoricalRecordingAcquisitionResult = std::variant<
    HistoricalRecordingAcquisitionSuccess,
    HistoricalRecordingError
>;

[[nodiscard]] HistoricalRecordingQuoteResult quote_historical_recording(
    HistoricalRecordingProvider& provider,
    const HistoricalRecordingQuery& query
);

[[nodiscard]] HistoricalRecordingAcquisitionResult acquire_historical_recording(
    HistoricalRecordingProvider& provider,
    const HistoricalRecordingAcquisitionRequest& request
);

}  // namespace chronos::databento_adapter
