#include "chronos/databento/historical_recording_acquisition.hpp"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <unistd.h>

namespace chronos::databento_adapter {
namespace {

constexpr std::size_t kMaximumSymbolsPerRequest = 2'000;

HistoricalRecordingError acquisition_error(
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

std::string system_error_message(int error_number) {
    return std::error_code(error_number, std::generic_category()).message();
}

std::optional<HistoricalRecordingError> validate_query(
    const HistoricalRecordingQuery& query
) {
    if (query.start.empty()) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidQuery,
            "start",
            "start timestamp must not be empty"
        );
    }
    if (query.end.empty()) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidQuery,
            "end",
            "end timestamp must not be empty"
        );
    }
    if (query.start == query.end) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidQuery,
            "end",
            "end timestamp must differ from start timestamp"
        );
    }
    if (query.symbols.empty()) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidQuery,
            "symbols",
            "at least one symbol is required"
        );
    }
    if (query.symbols.size() > kMaximumSymbolsPerRequest) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidQuery,
            "symbols",
            "at most 2000 symbols are allowed per request"
        );
    }

    std::unordered_set<std::string> unique_symbols;
    for (const auto& symbol : query.symbols) {
        if (symbol.empty()) {
            return acquisition_error(
                HistoricalRecordingErrorCode::InvalidQuery,
                "symbols",
                "symbols must not be empty"
            );
        }
        if (!unique_symbols.insert(symbol).second) {
            return acquisition_error(
                HistoricalRecordingErrorCode::InvalidQuery,
                "symbols",
                "symbols must not contain duplicates"
            );
        }
    }
    return std::nullopt;
}

std::filesystem::path manifest_path_for(
    const std::filesystem::path& recording_path
) {
    return recording_path.string() + ".manifest.json";
}

std::optional<HistoricalRecordingError> validate_acquisition_request(
    const HistoricalRecordingAcquisitionRequest& request
) {
    if (const auto query_error = validate_query(request.query)) {
        return query_error;
    }
    if (!std::isfinite(request.maximum_cost_usd) ||
        request.maximum_cost_usd < 0.0) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidQuery,
            "maximum_cost_usd",
            "maximum cost must be a finite nonnegative number"
        );
    }
    if (request.destination.empty() ||
        request.destination.filename().empty()) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidDestination,
            "destination",
            "recording destination must name a file"
        );
    }

    const std::string destination_name =
        request.destination.filename().string();
    const bool has_dbn_suffix = destination_name.ends_with(".dbn");
    const bool has_compressed_dbn_suffix =
        destination_name.ends_with(".dbn.zst");
    if (!has_dbn_suffix && !has_compressed_dbn_suffix) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidDestination,
            "destination",
            "recording destination must end in .dbn or .dbn.zst"
        );
    }

    const auto parent = request.destination.has_parent_path()
        ? request.destination.parent_path()
        : std::filesystem::path(".");
    std::error_code status_error;
    const auto parent_status = std::filesystem::status(parent, status_error);
    if (status_error || !std::filesystem::is_directory(parent_status)) {
        return acquisition_error(
            HistoricalRecordingErrorCode::InvalidDestination,
            "destination",
            "recording destination parent must be an existing directory"
        );
    }

    std::error_code recording_status_error;
    const auto recording_status = std::filesystem::symlink_status(
        request.destination,
        recording_status_error
    );
    if (!recording_status_error &&
        recording_status.type() != std::filesystem::file_type::not_found) {
        return acquisition_error(
            HistoricalRecordingErrorCode::DestinationAlreadyExists,
            "destination",
            "recording destination already exists"
        );
    }

    const auto manifest_path = manifest_path_for(request.destination);
    std::error_code manifest_status_error;
    const auto manifest_status = std::filesystem::symlink_status(
        manifest_path,
        manifest_status_error
    );
    if (!manifest_status_error &&
        manifest_status.type() != std::filesystem::file_type::not_found) {
        return acquisition_error(
            HistoricalRecordingErrorCode::DestinationAlreadyExists,
            "manifest_destination",
            "recording manifest destination already exists"
        );
    }
    return std::nullopt;
}

class TemporaryRecordingFile final {
public:
    explicit TemporaryRecordingFile(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryRecordingFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryRecordingFile(const TemporaryRecordingFile&) = delete;
    TemporaryRecordingFile& operator=(const TemporaryRecordingFile&) = delete;
    TemporaryRecordingFile(TemporaryRecordingFile&& other) noexcept
        : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    TemporaryRecordingFile& operator=(TemporaryRecordingFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::variant<TemporaryRecordingFile, HistoricalRecordingError>
create_temporary_recording_file(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> next_id{0};
    constexpr std::uint64_t kMaximumAttempts = 100;
    for (std::uint64_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        const auto path = std::filesystem::path(
            destination.string() + ".download." +
            std::to_string(static_cast<std::uint64_t>(::getpid())) + "." +
            std::to_string(next_id.fetch_add(1, std::memory_order_relaxed))
        );
        const int descriptor = ::open(
            path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL,
            0600
        );
        if (descriptor >= 0) {
            if (::close(descriptor) == 0) {
                return TemporaryRecordingFile(path);
            }
            const int close_error = errno;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return acquisition_error(
                HistoricalRecordingErrorCode::TemporaryFileCreationFailed,
                "destination",
                "could not close temporary recording: " +
                    system_error_message(close_error)
            );
        }
        if (errno != EEXIST) {
            return acquisition_error(
                HistoricalRecordingErrorCode::TemporaryFileCreationFailed,
                "destination",
                "could not create temporary recording: " +
                    system_error_message(errno)
            );
        }
    }
    return acquisition_error(
        HistoricalRecordingErrorCode::TemporaryFileCreationFailed,
        "destination",
        "could not allocate a unique temporary recording name"
    );
}

class PublishedRecordingGuard final {
public:
    explicit PublishedRecordingGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~PublishedRecordingGuard() {
        if (!committed_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    PublishedRecordingGuard(const PublishedRecordingGuard&) = delete;
    PublishedRecordingGuard& operator=(const PublishedRecordingGuard&) = delete;

    void commit() noexcept { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_{false};
};

std::optional<HistoricalRecordingError> publish_recording(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& destination
) {
    if (::link(temporary_path.c_str(), destination.c_str()) != 0) {
        const int link_error = errno;
        const auto code = link_error == EEXIST
            ? HistoricalRecordingErrorCode::DestinationAlreadyExists
            : HistoricalRecordingErrorCode::PublishFailed;
        return acquisition_error(
            code,
            "destination",
            "could not publish recording: " +
                system_error_message(link_error)
        );
    }

    std::error_code permission_error;
    std::filesystem::permissions(
        destination,
        std::filesystem::perms::owner_write |
            std::filesystem::perms::group_write |
            std::filesystem::perms::others_write,
        std::filesystem::perm_options::remove,
        permission_error
    );
    if (permission_error) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        return acquisition_error(
            HistoricalRecordingErrorCode::PublishFailed,
            "destination",
            "could not make published recording read-only: " +
                permission_error.message()
        );
    }
    return std::nullopt;
}

std::variant<RecordingManifest, HistoricalRecordingError> validate_recording(
    const std::filesystem::path& path,
    const MarketDataStreamId& stream
) {
    auto open_result = EquitiesMiniMbp1FileReader::open(path, stream);
    if (std::holds_alternative<EquitiesMiniMbp1FileError>(open_result)) {
        auto provider_error = std::get<EquitiesMiniMbp1FileError>(
            std::move(open_result)
        );
        auto error = acquisition_error(
            HistoricalRecordingErrorCode::RecordingValidationFailed,
            provider_error.field,
            "downloaded recording failed validation: " +
                provider_error.message
        );
        error.recording_error = std::move(provider_error);
        return error;
    }

    auto reader = std::get<std::unique_ptr<EquitiesMiniMbp1FileReader>>(
        std::move(open_result)
    );
    while (true) {
        auto result = reader->read_next();
        if (std::holds_alternative<ValidatedMarketEvent>(result)) {
            continue;
        }
        if (std::holds_alternative<EquitiesMiniMbp1FileError>(result)) {
            auto provider_error = std::get<EquitiesMiniMbp1FileError>(
                std::move(result)
            );
            auto error = acquisition_error(
                HistoricalRecordingErrorCode::RecordingValidationFailed,
                provider_error.field,
                "downloaded recording failed validation: " +
                    provider_error.message
            );
            error.recording_error = std::move(provider_error);
            return error;
        }
        break;
    }

    if (!reader->manifest()) {
        return acquisition_error(
            HistoricalRecordingErrorCode::RecordingValidationFailed,
            "manifest",
            "validated recording did not produce a manifest"
        );
    }
    return *reader->manifest();
}

}  // namespace

std::string_view to_string(HistoricalRecordingErrorCode code) noexcept {
    switch (code) {
        case HistoricalRecordingErrorCode::InvalidQuery:
            return "invalid_query";
        case HistoricalRecordingErrorCode::MissingApiKey:
            return "missing_api_key";
        case HistoricalRecordingErrorCode::ProviderConnectionFailed:
            return "provider_connection_failed";
        case HistoricalRecordingErrorCode::ProviderQuoteFailed:
            return "provider_quote_failed";
        case HistoricalRecordingErrorCode::InvalidDestination:
            return "invalid_destination";
        case HistoricalRecordingErrorCode::DestinationAlreadyExists:
            return "destination_already_exists";
        case HistoricalRecordingErrorCode::CostLimitExceeded:
            return "cost_limit_exceeded";
        case HistoricalRecordingErrorCode::TemporaryFileCreationFailed:
            return "temporary_file_creation_failed";
        case HistoricalRecordingErrorCode::ProviderDownloadFailed:
            return "provider_download_failed";
        case HistoricalRecordingErrorCode::PublishFailed:
            return "publish_failed";
        case HistoricalRecordingErrorCode::RecordingValidationFailed:
            return "recording_validation_failed";
        case HistoricalRecordingErrorCode::ManifestWriteFailed:
            return "manifest_write_failed";
    }
    return "unknown";
}

HistoricalRecordingQuoteResult quote_historical_recording(
    HistoricalRecordingProvider& provider,
    const HistoricalRecordingQuery& query
) {
    if (const auto error = validate_query(query)) {
        return *error;
    }
    return provider.quote(query);
}

HistoricalRecordingAcquisitionResult acquire_historical_recording(
    HistoricalRecordingProvider& provider,
    const HistoricalRecordingAcquisitionRequest& request
) {
    if (const auto error = validate_acquisition_request(request)) {
        return *error;
    }

    auto quote_result = provider.quote(request.query);
    if (std::holds_alternative<HistoricalRecordingError>(quote_result)) {
        return std::get<HistoricalRecordingError>(std::move(quote_result));
    }
    auto quote = std::get<HistoricalRecordingQuote>(quote_result);
    if (quote.estimated_cost_usd > request.maximum_cost_usd) {
        return acquisition_error(
            HistoricalRecordingErrorCode::CostLimitExceeded,
            "maximum_cost_usd",
            "estimated cost exceeds the explicit maximum cost"
        );
    }

    auto temporary_result = create_temporary_recording_file(
        request.destination
    );
    if (std::holds_alternative<HistoricalRecordingError>(temporary_result)) {
        return std::get<HistoricalRecordingError>(
            std::move(temporary_result)
        );
    }
    auto temporary_file = std::get<TemporaryRecordingFile>(
        std::move(temporary_result)
    );

    auto download_result = provider.download_to_file(
        request.query,
        temporary_file.path()
    );
    if (std::holds_alternative<HistoricalRecordingError>(download_result)) {
        return std::get<HistoricalRecordingError>(
            std::move(download_result)
        );
    }

    if (const auto publish_error = publish_recording(
            temporary_file.path(),
            request.destination
        )) {
        return *publish_error;
    }
    PublishedRecordingGuard recording_guard(request.destination);

    auto validation_result = validate_recording(
        request.destination,
        request.stream
    );
    if (std::holds_alternative<HistoricalRecordingError>(validation_result)) {
        return std::get<HistoricalRecordingError>(
            std::move(validation_result)
        );
    }
    auto manifest = std::get<RecordingManifest>(
        std::move(validation_result)
    );

    const auto manifest_path = manifest_path_for(request.destination);
    auto manifest_result = write_recording_manifest(manifest_path, manifest);
    if (std::holds_alternative<RecordingManifestWriteError>(manifest_result)) {
        auto manifest_error = std::get<RecordingManifestWriteError>(
            std::move(manifest_result)
        );
        auto error = acquisition_error(
            HistoricalRecordingErrorCode::ManifestWriteFailed,
            "manifest_destination",
            "could not publish recording manifest: " +
                manifest_error.message
        );
        error.manifest_error = std::move(manifest_error);
        return error;
    }

    recording_guard.commit();
    return HistoricalRecordingAcquisitionSuccess{
        .quote = quote,
        .recording_path = request.destination,
        .manifest_path = manifest_path,
        .manifest = std::move(manifest),
    };
}

}  // namespace chronos::databento_adapter
