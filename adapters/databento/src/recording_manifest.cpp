#include "chronos/databento/recording_manifest.hpp"

#include "chronos/trading_types.hpp"

#include <databento/version.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <unistd.h>

namespace chronos::databento_adapter {
namespace {

constexpr std::string_view kDatabentoSdkCommit =
    "4e0cc2eded5ee3cff280ca5949f0ba251010afbf";

RecordingManifestWriteError manifest_write_error(
    RecordingManifestWriteErrorCode code,
    std::string message
) {
    return RecordingManifestWriteError{
        .code = code,
        .message = std::move(message),
    };
}

std::string system_error_message(int error_number) {
    return std::error_code(error_number, std::generic_category()).message();
}

class FileDescriptor final {
public:
    explicit FileDescriptor(int descriptor) : descriptor_(descriptor) {}

    ~FileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor_(other.release()) {}
    FileDescriptor& operator=(FileDescriptor&&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    [[nodiscard]] int release() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor;
    }

private:
    int descriptor_;
};

class TemporaryManifestFile final {
public:
    explicit TemporaryManifestFile(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryManifestFile() {
        if (!published_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    TemporaryManifestFile(const TemporaryManifestFile&) = delete;
    TemporaryManifestFile& operator=(const TemporaryManifestFile&) = delete;
    TemporaryManifestFile(TemporaryManifestFile&& other) noexcept
        : path_(std::move(other.path_)), published_(other.published_) {
        other.published_ = true;
    }
    TemporaryManifestFile& operator=(TemporaryManifestFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    void mark_published() noexcept { published_ = true; }

private:
    std::filesystem::path path_;
    bool published_{false};
};

std::variant<std::pair<TemporaryManifestFile, FileDescriptor>,
             RecordingManifestWriteError>
create_temporary_manifest_file(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> next_id{0};
    constexpr std::uint64_t kMaximumAttempts = 100;
    for (std::uint64_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        const std::filesystem::path temporary_path =
            destination.string() + ".tmp." +
            std::to_string(static_cast<std::uint64_t>(::getpid())) + "." +
            std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
        const int descriptor = ::open(
            temporary_path.c_str(),
            O_WRONLY | O_CREAT | O_EXCL,
            0444
        );
        if (descriptor >= 0) {
            return std::pair<TemporaryManifestFile, FileDescriptor>(
                TemporaryManifestFile(temporary_path),
                FileDescriptor(descriptor)
            );
        }
        if (errno != EEXIST) {
            return manifest_write_error(
                RecordingManifestWriteErrorCode::TemporaryFileCreationFailed,
                "could not create temporary manifest: " +
                    system_error_message(errno)
            );
        }
    }
    return manifest_write_error(
        RecordingManifestWriteErrorCode::TemporaryFileCreationFailed,
        "could not allocate a unique temporary manifest name"
    );
}

std::optional<RecordingManifestWriteError> write_all(
    int descriptor,
    std::string_view contents
) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t bytes_written = ::write(
            descriptor,
            contents.data() + offset,
            contents.size() - offset
        );
        if (bytes_written > 0) {
            offset += static_cast<std::size_t>(bytes_written);
            continue;
        }
        if (bytes_written < 0 && errno == EINTR) {
            continue;
        }
        return manifest_write_error(
            RecordingManifestWriteErrorCode::WriteFailed,
            "could not write complete manifest: " +
                system_error_message(errno)
        );
    }
    if (::fsync(descriptor) != 0) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::WriteFailed,
            "could not synchronize manifest contents: " +
                system_error_message(errno)
        );
    }
    return std::nullopt;
}

}  // namespace

RecordingManifest::RecordingManifest(
    std::string source_file_name,
    std::uint64_t source_size_bytes,
    std::string source_sha256,
    std::uint8_t dbn_version,
    std::string dataset,
    std::string schema,
    std::optional<std::string> input_symbology,
    std::string output_symbology,
    std::uint64_t query_start_nanoseconds,
    std::uint64_t query_end_nanoseconds,
    bool includes_send_timestamps,
    std::vector<std::string> requested_symbols,
    std::vector<std::string> partially_resolved_symbols,
    std::vector<std::string> unresolved_symbols,
    MarketDataStreamId stream,
    std::uint64_t first_ingest_sequence,
    std::optional<std::uint64_t> last_ingest_sequence,
    std::uint64_t provider_record_count,
    std::uint64_t normalized_event_count
)
    : source_file_name_(std::move(source_file_name)),
      source_size_bytes_(source_size_bytes),
      source_sha256_(std::move(source_sha256)),
      dbn_version_(dbn_version),
      dataset_(std::move(dataset)),
      schema_(std::move(schema)),
      input_symbology_(std::move(input_symbology)),
      output_symbology_(std::move(output_symbology)),
      query_start_nanoseconds_(query_start_nanoseconds),
      query_end_nanoseconds_(query_end_nanoseconds),
      includes_send_timestamps_(includes_send_timestamps),
      requested_symbols_(std::move(requested_symbols)),
      partially_resolved_symbols_(std::move(partially_resolved_symbols)),
      unresolved_symbols_(std::move(unresolved_symbols)),
      stream_(std::move(stream)),
      first_ingest_sequence_(first_ingest_sequence),
      last_ingest_sequence_(last_ingest_sequence),
      provider_record_count_(provider_record_count),
      normalized_event_count_(normalized_event_count) {}

std::string RecordingManifest::to_json() const {
    const nlohmann::json document = {
        {"counts",
         {
             {"normalized_events", normalized_event_count_},
             {"provider_records", provider_record_count_},
         }},
        {"manifest_version", manifest_version_},
        {"normalization",
         {
             {"contract_version", 1},
             {"first_ingest_sequence", first_ingest_sequence_},
             {"last_ingest_sequence", last_ingest_sequence_},
             {"price_scale", Price::scale()},
             {"quantity_scale", Quantity::scale()},
             {"stream", stream_.value()},
         }},
        {"provider",
         {
             {"dataset", dataset_},
             {"dbn_version", dbn_version_},
             {"name", "databento"},
             {"schema", schema_},
             {"sdk_source_commit", kDatabentoSdkCommit},
             {"sdk_version", DATABENTO_VERSION},
         }},
        {"query",
         {
             {"end_nanoseconds", query_end_nanoseconds_},
             {"includes_send_timestamps", includes_send_timestamps_},
             {"input_symbology", input_symbology_},
             {"output_symbology", output_symbology_},
             {"partially_resolved_symbols", partially_resolved_symbols_},
             {"requested_symbols", requested_symbols_},
             {"start_nanoseconds", query_start_nanoseconds_},
             {"unresolved_symbols", unresolved_symbols_},
         }},
        {"source",
         {
             {"file_name", source_file_name_},
             {"sha256", source_sha256_},
             {"size_bytes", source_size_bytes_},
         }},
    };
    return document.dump(2) + '\n';
}

RecordingManifestWriteResult write_recording_manifest(
    const std::filesystem::path& destination,
    const RecordingManifest& manifest
) {
    if (destination.empty() || destination.filename().empty()) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::InvalidDestination,
            "manifest destination must include a file name"
        );
    }

    std::string contents;
    try {
        contents = manifest.to_json();
    } catch (const std::exception& error) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::SerializationFailed,
            "could not serialize recording manifest: " +
                std::string(error.what())
        );
    }

    std::error_code inspection_error;
    const bool destination_exists = std::filesystem::exists(
        destination,
        inspection_error
    );
    if (inspection_error) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::InvalidDestination,
            "could not inspect manifest destination: " +
                inspection_error.message()
        );
    }
    if (destination_exists) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::DestinationAlreadyExists,
            "manifest destination already exists and will not be overwritten"
        );
    }

    auto temporary_result = create_temporary_manifest_file(destination);
    if (auto* error = std::get_if<RecordingManifestWriteError>(
            &temporary_result
        )) {
        return std::move(*error);
    }
    auto [temporary_file, descriptor] =
        std::get<std::pair<TemporaryManifestFile, FileDescriptor>>(
            std::move(temporary_result)
        );
    if (auto error = write_all(descriptor.get(), contents)) {
        return std::move(*error);
    }
    const int raw_descriptor = descriptor.release();
    if (::close(raw_descriptor) != 0) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::WriteFailed,
            "could not close temporary manifest: " +
                system_error_message(errno)
        );
    }

    if (::link(temporary_file.path().c_str(), destination.c_str()) != 0) {
        if (errno == EEXIST) {
            return manifest_write_error(
                RecordingManifestWriteErrorCode::DestinationAlreadyExists,
                "manifest destination already exists and will not be overwritten"
            );
        }
        return manifest_write_error(
            RecordingManifestWriteErrorCode::PublishFailed,
            "could not atomically publish manifest: " +
                system_error_message(errno)
        );
    }

    std::error_code removal_error;
    std::filesystem::remove(temporary_file.path(), removal_error);
    temporary_file.mark_published();
    if (removal_error) {
        return manifest_write_error(
            RecordingManifestWriteErrorCode::PublishFailed,
            "manifest was published but its temporary link remains: " +
                removal_error.message()
        );
    }
    return RecordingManifestWriteSuccess{
        .bytes_written = static_cast<std::uint64_t>(contents.size()),
    };
}

}  // namespace chronos::databento_adapter
