#pragma once

#include "chronos/market_data_types.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::databento_adapter {

class EquitiesMiniMbp1FileReader;

class RecordingManifest final {
public:
    [[nodiscard]] std::uint32_t manifest_version() const noexcept {
        return manifest_version_;
    }

    [[nodiscard]] std::string_view source_file_name() const noexcept {
        return source_file_name_;
    }

    [[nodiscard]] std::uint64_t source_size_bytes() const noexcept {
        return source_size_bytes_;
    }

    [[nodiscard]] std::string_view source_sha256() const noexcept {
        return source_sha256_;
    }

    [[nodiscard]] std::uint8_t dbn_version() const noexcept {
        return dbn_version_;
    }

    [[nodiscard]] std::string_view dataset() const noexcept {
        return dataset_;
    }

    [[nodiscard]] std::string_view schema() const noexcept { return schema_; }

    [[nodiscard]] const std::optional<std::string>& input_symbology() const {
        return input_symbology_;
    }

    [[nodiscard]] std::string_view output_symbology() const noexcept {
        return output_symbology_;
    }

    [[nodiscard]] std::uint64_t query_start_nanoseconds() const noexcept {
        return query_start_nanoseconds_;
    }

    [[nodiscard]] std::uint64_t query_end_nanoseconds() const noexcept {
        return query_end_nanoseconds_;
    }

    [[nodiscard]] bool includes_send_timestamps() const noexcept {
        return includes_send_timestamps_;
    }

    [[nodiscard]] const std::vector<std::string>& requested_symbols() const {
        return requested_symbols_;
    }

    [[nodiscard]] const std::vector<std::string>& partially_resolved_symbols()
        const {
        return partially_resolved_symbols_;
    }

    [[nodiscard]] const std::vector<std::string>& unresolved_symbols() const {
        return unresolved_symbols_;
    }

    [[nodiscard]] const MarketDataStreamId& stream() const noexcept {
        return stream_;
    }

    [[nodiscard]] std::uint64_t first_ingest_sequence() const noexcept {
        return first_ingest_sequence_;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    last_ingest_sequence() const noexcept {
        return last_ingest_sequence_;
    }

    [[nodiscard]] std::uint64_t provider_record_count() const noexcept {
        return provider_record_count_;
    }

    [[nodiscard]] std::uint64_t normalized_event_count() const noexcept {
        return normalized_event_count_;
    }

    [[nodiscard]] std::string to_json() const;

private:
    RecordingManifest(
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
    );

    static constexpr std::uint32_t manifest_version_{1};
    std::string source_file_name_;
    std::uint64_t source_size_bytes_;
    std::string source_sha256_;
    std::uint8_t dbn_version_;
    std::string dataset_;
    std::string schema_;
    std::optional<std::string> input_symbology_;
    std::string output_symbology_;
    std::uint64_t query_start_nanoseconds_;
    std::uint64_t query_end_nanoseconds_;
    bool includes_send_timestamps_;
    std::vector<std::string> requested_symbols_;
    std::vector<std::string> partially_resolved_symbols_;
    std::vector<std::string> unresolved_symbols_;
    MarketDataStreamId stream_;
    std::uint64_t first_ingest_sequence_;
    std::optional<std::uint64_t> last_ingest_sequence_;
    std::uint64_t provider_record_count_;
    std::uint64_t normalized_event_count_;

    friend class EquitiesMiniMbp1FileReader;
};

enum class RecordingManifestWriteErrorCode {
    InvalidDestination,
    DestinationAlreadyExists,
    SerializationFailed,
    TemporaryFileCreationFailed,
    WriteFailed,
    PublishFailed,
};

struct RecordingManifestWriteError {
    RecordingManifestWriteErrorCode code;
    std::string message;
};

struct RecordingManifestWriteSuccess {
    std::uint64_t bytes_written;
};

using RecordingManifestWriteResult = std::variant<
    RecordingManifestWriteSuccess,
    RecordingManifestWriteError
>;

[[nodiscard]] RecordingManifestWriteResult write_recording_manifest(
    const std::filesystem::path& destination,
    const RecordingManifest& manifest
);

}  // namespace chronos::databento_adapter
