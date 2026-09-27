#include "chronos/databento/historical_recording_acquisition.hpp"

#include <databento/enums.hpp>
#include <databento/historical.hpp>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos::databento_adapter {
namespace {

constexpr std::string_view kDataset = "EQUS.MINI";
constexpr std::uint64_t kNoRecordLimit = 0;

HistoricalRecordingError provider_error(
    HistoricalRecordingErrorCode code,
    std::string field,
    std::string message
) {
    return HistoricalRecordingError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
        .recording_error = std::nullopt,
        .manifest_error = std::nullopt,
    };
}

}  // namespace

class DatabentoHistoricalRecordingProvider::Impl final {
public:
    Impl()
        : client_(
              ::databento::Historical::Builder()
                  .SetKeyFromEnv()
                  .SetUpgradePolicy(
                      ::databento::VersionUpgradePolicy::UpgradeToV3
                  )
                  .ExtendUserAgent("chronos")
                  .Build()
          ) {}

    ::databento::Historical client_;
};

DatabentoHistoricalRecordingProvider::DatabentoHistoricalRecordingProvider(
    std::unique_ptr<Impl> implementation
) : implementation_(std::move(implementation)) {}

DatabentoHistoricalRecordingProvider::~DatabentoHistoricalRecordingProvider() =
    default;

DatabentoHistoricalRecordingProvider::ConnectionResult
DatabentoHistoricalRecordingProvider::connect_from_environment() {
    const char* api_key = std::getenv("DATABENTO_API_KEY");
    if (api_key == nullptr || *api_key == '\0') {
        return provider_error(
            HistoricalRecordingErrorCode::MissingApiKey,
            "DATABENTO_API_KEY",
            "DATABENTO_API_KEY is not set"
        );
    }
    try {
        return std::unique_ptr<DatabentoHistoricalRecordingProvider>(
            new DatabentoHistoricalRecordingProvider(
                std::make_unique<Impl>()
            )
        );
    } catch (const std::exception& exception) {
        return provider_error(
            HistoricalRecordingErrorCode::ProviderConnectionFailed,
            "DATABENTO_API_KEY",
            "could not create Databento historical client: " +
                std::string(exception.what())
        );
    }
}

HistoricalRecordingQuoteResult
DatabentoHistoricalRecordingProvider::quote(
    const HistoricalRecordingQuery& query
) {
    try {
        const ::databento::DateTimeRange<std::string> range{
            query.start,
            query.end,
        };
        const auto count = implementation_->client_.MetadataGetRecordCount(
            std::string(kDataset),
            range,
            query.symbols,
            ::databento::Schema::Mbp1,
            ::databento::SType::RawSymbol,
            kNoRecordLimit
        );
        const auto billable_size =
            implementation_->client_.MetadataGetBillableSize(
                std::string(kDataset),
                range,
                query.symbols,
                ::databento::Schema::Mbp1,
                ::databento::SType::RawSymbol,
                kNoRecordLimit
            );
        const auto cost = implementation_->client_.MetadataGetCost(
            std::string(kDataset),
            range,
            query.symbols,
            ::databento::Schema::Mbp1,
            ::databento::SType::RawSymbol,
            kNoRecordLimit
        );
        if (!std::isfinite(cost) || cost < 0.0) {
            return provider_error(
                HistoricalRecordingErrorCode::ProviderQuoteFailed,
                "estimated_cost_usd",
                "provider returned an invalid cost estimate"
            );
        }
        return HistoricalRecordingQuote{
            .estimated_record_count = count,
            .estimated_billable_size_bytes = billable_size,
            .estimated_cost_usd = cost,
        };
    } catch (const std::exception& exception) {
        return provider_error(
            HistoricalRecordingErrorCode::ProviderQuoteFailed,
            "query",
            "Databento quote request failed: " +
                std::string(exception.what())
        );
    }
}

HistoricalRecordingDownloadResult
DatabentoHistoricalRecordingProvider::download_to_file(
    const HistoricalRecordingQuery& query,
    const std::filesystem::path& destination
) {
    try {
        implementation_->client_.TimeseriesGetRangeToFile(
            std::string(kDataset),
            {query.start, query.end},
            query.symbols,
            ::databento::Schema::Mbp1,
            ::databento::SType::RawSymbol,
            ::databento::SType::InstrumentId,
            kNoRecordLimit,
            destination
        );
        return std::monostate{};
    } catch (const std::exception& exception) {
        return provider_error(
            HistoricalRecordingErrorCode::ProviderDownloadFailed,
            "query",
            "Databento download request failed: " +
                std::string(exception.what())
        );
    }
}

}  // namespace chronos::databento_adapter
