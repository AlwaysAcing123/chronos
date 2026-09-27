#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"

#include <databento/dbn.hpp>
#include <databento/dbn_store.hpp>
#include <databento/enums.hpp>
#include <databento/log.hpp>
#include <databento/record.hpp>
#include <databento/symbol_map.hpp>
#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace chronos::databento_adapter {
namespace {

constexpr std::string_view kExpectedDataset = "EQUS.MINI";

class StrictDbnLogReceiver final : public ::databento::ILogReceiver {
public:
    void Receive(
        ::databento::LogLevel level,
        const std::string& message
    ) override {
        if (level >= ::databento::LogLevel::Warning && warning_.empty()) {
            warning_ = message;
        }
    }

    [[nodiscard]] bool ShouldLog(
        ::databento::LogLevel level
    ) const override {
        return level >= ::databento::LogLevel::Warning;
    }

    [[nodiscard]] const std::string& warning() const noexcept {
        return warning_;
    }

private:
    std::string warning_;
};

EquitiesMiniMbp1FileError file_error(
    EquitiesMiniMbp1FileErrorCode code,
    std::string field,
    std::string message,
    std::optional<std::uint64_t> source_record_number = std::nullopt
) {
    return EquitiesMiniMbp1FileError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
        .source_record_number = source_record_number,
        .normalization_error = std::nullopt,
        .stream_error = std::nullopt,
    };
}

std::optional<EquitiesMiniMbp1FileError> inspect_source_file(
    const std::filesystem::path& source_path,
    std::uint64_t& size,
    std::filesystem::file_time_type& last_write_time
) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(source_path, error)) {
        return file_error(
            error
                ? EquitiesMiniMbp1FileErrorCode::SourceFileInspectionFailed
                : EquitiesMiniMbp1FileErrorCode::SourcePathNotRegularFile,
            "source_path",
            error
                ? "could not inspect source file: " + error.message()
                : "source path must identify a regular file"
        );
    }

    const std::uintmax_t file_size = std::filesystem::file_size(
        source_path,
        error
    );
    if (error || file_size > std::numeric_limits<std::uint64_t>::max()) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceFileInspectionFailed,
            "source_size_bytes",
            error
                ? "could not read source file size: " + error.message()
                : "source file size exceeds the manifest range"
        );
    }

    const auto observed_write_time = std::filesystem::last_write_time(
        source_path,
        error
    );
    if (error) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceFileInspectionFailed,
            "source_last_write_time",
            "could not read source modification time: " + error.message()
        );
    }

    size = static_cast<std::uint64_t>(file_size);
    last_write_time = observed_write_time;
    return std::nullopt;
}

bool source_snapshot_matches(
    const std::filesystem::path& source_path,
    std::uint64_t expected_size,
    std::filesystem::file_time_type expected_write_time
) {
    std::uint64_t actual_size = 0;
    std::filesystem::file_time_type actual_write_time;
    return !inspect_source_file(
                source_path,
                actual_size,
                actual_write_time
            ).has_value() &&
        actual_size == expected_size &&
        actual_write_time == expected_write_time;
}

using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

std::variant<std::string, EquitiesMiniMbp1FileError> sha256_file(
    const std::filesystem::path& source_path
) {
    std::ifstream input(source_path, std::ios::binary);
    if (!input) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceHashFailed,
            "source_sha256",
            "could not open source file for hashing"
        );
    }

    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceHashFailed,
            "source_sha256",
            "could not initialize SHA-256"
        );
    }

    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes_read = input.gcount();
        if (bytes_read > 0 && EVP_DigestUpdate(
                context.get(),
                buffer.data(),
                static_cast<std::size_t>(bytes_read)
            ) != 1) {
            return file_error(
                EquitiesMiniMbp1FileErrorCode::SourceHashFailed,
                "source_sha256",
                "could not update SHA-256"
            );
        }
    }
    if (!input.eof()) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceHashFailed,
            "source_sha256",
            "could not read the complete source file for hashing"
        );
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(
            context.get(),
            digest.data(),
            &digest_size
        ) != 1 || digest_size != 32) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceHashFailed,
            "source_sha256",
            "could not finalize SHA-256"
        );
    }

    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_size; ++index) {
        encoded << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return encoded.str();
}

std::optional<std::string> input_symbology(
    const ::databento::Metadata& metadata
) {
    if (!metadata.stype_in.has_value()) {
        return std::nullopt;
    }
    return std::string(::databento::ToString(*metadata.stype_in));
}

}  // namespace

class EquitiesMiniMbp1FileReader::Impl final {
public:
    Impl(
        std::filesystem::path source_path,
        std::uint64_t source_size,
        std::filesystem::file_time_type source_write_time,
        ::databento::Metadata metadata,
        ::databento::TsSymbolMap symbol_map,
        std::unique_ptr<StrictDbnLogReceiver> log_receiver,
        std::unique_ptr<::databento::DbnStore> store,
        MarketDataStreamId stream,
        std::uint64_t initial_ingest_sequence
    )
        : source_path_(std::move(source_path)),
          source_size_(source_size),
          source_write_time_(source_write_time),
          metadata_(std::move(metadata)),
          symbol_map_(std::move(symbol_map)),
          log_receiver_(std::move(log_receiver)),
          store_(std::move(store)),
          normalizer_(
              stream,
              FeedMode::HistoricalReplay,
              initial_ingest_sequence
          ),
          stream_validator_(stream, initial_ingest_sequence),
          first_ingest_sequence_(initial_ingest_sequence) {}

    [[nodiscard]] EquitiesMiniMbp1FileReadResult read_next() {
        if (terminal_error_.has_value()) {
            return *terminal_error_;
        }
        if (finished_) {
            return EndOfRecording{};
        }
        if (pending_event_.has_value()) {
            auto event = std::move(*pending_event_);
            pending_event_.reset();
            return emit(std::move(event));
        }

        const ::databento::Record* provider_record = nullptr;
        try {
            provider_record = store_->NextRecord();
        } catch (const std::exception& error) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::RecordDecodeFailed,
                "record",
                "could not decode provider record: " +
                    std::string(error.what()),
                next_source_record_number()
            ));
        }

        if (provider_record == nullptr) {
            return finish();
        }
        if (provider_record_count_ ==
            std::numeric_limits<std::uint64_t>::max()) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::CounterOverflow,
                "provider_record_count",
                "provider record count cannot be represented"
            ));
        }
        ++provider_record_count_;

        const auto* mbp1 = provider_record->GetIf<::databento::Mbp1Msg>();
        if (mbp1 == nullptr) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::UnexpectedRecordType,
                "record.hd.rtype",
                "DBN file contains a record that is not MBP-1",
                provider_record_count_
            ));
        }

        std::string symbol;
        try {
            symbol = symbol_map_.At(*mbp1);
        } catch (const std::exception& error) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::SymbolResolutionFailed,
                "record.hd.instrument_id",
                "could not resolve instrument " +
                    std::to_string(mbp1->hd.instrument_id) + ": " +
                    error.what(),
                provider_record_count_
            ));
        }

        auto normalized = normalizer_.normalize(*mbp1, symbol);
        if (auto* error = std::get_if<EquitiesMiniMbp1Error>(&normalized)) {
            EquitiesMiniMbp1FileError result = file_error(
                EquitiesMiniMbp1FileErrorCode::RecordNormalizationFailed,
                "record",
                "provider record failed normalization",
                provider_record_count_
            );
            result.normalization_error = std::move(*error);
            return fail(std::move(result));
        }

        auto events = std::get<EquitiesMiniMbp1Events>(
            std::move(normalized)
        );
        if (events.trade.has_value()) {
            pending_event_ = std::move(events.top_of_book);
            return emit(std::move(*events.trade));
        }
        return emit(std::move(events.top_of_book));
    }

    [[nodiscard]] const std::optional<RecordingManifest>& manifest()
        const noexcept {
        return manifest_;
    }

private:
    [[nodiscard]] std::uint64_t next_source_record_number() const noexcept {
        if (provider_record_count_ ==
            std::numeric_limits<std::uint64_t>::max()) {
            return provider_record_count_;
        }
        return provider_record_count_ + 1;
    }

    [[nodiscard]] EquitiesMiniMbp1FileReadResult emit(
        ValidatedMarketEvent event
    ) {
        auto stream_result = stream_validator_.accept(event);
        if (auto* error = std::get_if<MarketDataStreamValidationError>(
                &stream_result
            )) {
            EquitiesMiniMbp1FileError result = file_error(
                EquitiesMiniMbp1FileErrorCode::StreamValidationFailed,
                "normalized_event.header",
                "normalized event violated stream invariants",
                provider_record_count_
            );
            result.stream_error = std::move(*error);
            return fail(std::move(result));
        }
        if (normalized_event_count_ ==
            std::numeric_limits<std::uint64_t>::max()) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::CounterOverflow,
                "normalized_event_count",
                "normalized event count cannot be represented",
                provider_record_count_
            ));
        }
        ++normalized_event_count_;
        return event;
    }

    [[nodiscard]] EquitiesMiniMbp1FileReadResult finish() {
        if (!source_snapshot_matches(
                source_path_,
                source_size_,
                source_write_time_
            )) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::SourceFileChanged,
                "source_path",
                "source file changed while it was being decoded"
            ));
        }
        if (!log_receiver_->warning().empty()) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::RecordDecodeFailed,
                "record",
                "provider decoder rejected an incomplete record: " +
                    log_receiver_->warning(),
                next_source_record_number()
            ));
        }

        auto hash_result = sha256_file(source_path_);
        if (auto* error = std::get_if<EquitiesMiniMbp1FileError>(
                &hash_result
            )) {
            return fail(std::move(*error));
        }
        if (!source_snapshot_matches(
                source_path_,
                source_size_,
                source_write_time_
            )) {
            return fail(file_error(
                EquitiesMiniMbp1FileErrorCode::SourceFileChanged,
                "source_path",
                "source file changed while its checksum was calculated"
            ));
        }

        manifest_ = RecordingManifest(
            source_path_.filename().string(),
            source_size_,
            std::get<std::string>(std::move(hash_result)),
            metadata_.version,
            metadata_.dataset,
            ::databento::ToString(*metadata_.schema),
            input_symbology(metadata_),
            ::databento::ToString(metadata_.stype_out),
            metadata_.start.time_since_epoch().count(),
            metadata_.end.time_since_epoch().count(),
            metadata_.ts_out,
            metadata_.symbols,
            metadata_.partial,
            metadata_.not_found,
            stream_validator_.stream(),
            first_ingest_sequence_,
            stream_validator_.last_accepted_ingest_sequence(),
            provider_record_count_,
            normalized_event_count_
        );
        finished_ = true;
        return EndOfRecording{};
    }

    [[nodiscard]] EquitiesMiniMbp1FileReadResult fail(
        EquitiesMiniMbp1FileError error
    ) {
        terminal_error_ = std::move(error);
        pending_event_.reset();
        return *terminal_error_;
    }

    std::filesystem::path source_path_;
    std::uint64_t source_size_;
    std::filesystem::file_time_type source_write_time_;
    ::databento::Metadata metadata_;
    ::databento::TsSymbolMap symbol_map_;
    std::unique_ptr<StrictDbnLogReceiver> log_receiver_;
    std::unique_ptr<::databento::DbnStore> store_;
    EquitiesMiniMbp1Normalizer normalizer_;
    MarketDataStreamValidator stream_validator_;
    std::uint64_t first_ingest_sequence_;
    std::uint64_t provider_record_count_{0};
    std::uint64_t normalized_event_count_{0};
    std::optional<ValidatedMarketEvent> pending_event_;
    std::optional<RecordingManifest> manifest_;
    std::optional<EquitiesMiniMbp1FileError> terminal_error_;
    bool finished_{false};
};

EquitiesMiniMbp1FileOpenResult EquitiesMiniMbp1FileReader::open(
    const std::filesystem::path& source_path,
    MarketDataStreamId stream,
    std::uint64_t initial_ingest_sequence
) {
    std::uint64_t source_size = 0;
    std::filesystem::file_time_type source_write_time;
    if (auto error = inspect_source_file(
            source_path,
            source_size,
            source_write_time
        )) {
        return std::move(*error);
    }

    auto log_receiver = std::make_unique<StrictDbnLogReceiver>();
    std::unique_ptr<::databento::DbnStore> store;
    try {
        store = std::make_unique<::databento::DbnStore>(
            log_receiver.get(),
            source_path,
            ::databento::VersionUpgradePolicy::UpgradeToV3
        );
    } catch (const std::exception& error) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SourceFileOpenFailed,
            "source_path",
            "could not open DBN source file: " + std::string(error.what())
        );
    }

    ::databento::Metadata metadata;
    try {
        metadata = store->GetMetadata();
    } catch (const std::exception& error) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::MetadataDecodeFailed,
            "metadata",
            "could not decode DBN metadata: " + std::string(error.what())
        );
    }

    if (metadata.dataset != kExpectedDataset) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::UnexpectedDataset,
            "metadata.dataset",
            "expected EQUS.MINI metadata, received " + metadata.dataset
        );
    }
    if (!metadata.schema.has_value()) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::MissingSchema,
            "metadata.schema",
            "historical DBN metadata must declare a schema"
        );
    }
    if (*metadata.schema != ::databento::Schema::Mbp1) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::UnexpectedSchema,
            "metadata.schema",
            "expected MBP-1 metadata, received " +
                std::string(::databento::ToString(*metadata.schema))
        );
    }
    if (metadata.stype_out != ::databento::SType::InstrumentId) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::UnexpectedOutputSymbology,
            "metadata.stype_out",
            "timestamp-aware symbol resolution requires instrument_id output"
        );
    }

    std::optional<::databento::TsSymbolMap> symbol_map;
    try {
        symbol_map.emplace(metadata.CreateSymbolMap());
    } catch (const std::exception& error) {
        return file_error(
            EquitiesMiniMbp1FileErrorCode::SymbolMapCreationFailed,
            "metadata.mappings",
            "could not create timestamp-aware symbol map: " +
                std::string(error.what())
        );
    }

    auto implementation = std::make_unique<Impl>(
        source_path,
        source_size,
        source_write_time,
        std::move(metadata),
        std::move(*symbol_map),
        std::move(log_receiver),
        std::move(store),
        std::move(stream),
        initial_ingest_sequence
    );
    return std::unique_ptr<EquitiesMiniMbp1FileReader>(
        new EquitiesMiniMbp1FileReader(std::move(implementation))
    );
}

EquitiesMiniMbp1FileReader::EquitiesMiniMbp1FileReader(
    std::unique_ptr<Impl> implementation
)
    : implementation_(std::move(implementation)) {}

EquitiesMiniMbp1FileReader::~EquitiesMiniMbp1FileReader() = default;

EquitiesMiniMbp1FileReadResult EquitiesMiniMbp1FileReader::read_next() {
    return implementation_->read_next();
}

const std::optional<RecordingManifest>&
EquitiesMiniMbp1FileReader::manifest() const noexcept {
    return implementation_->manifest();
}

}  // namespace chronos::databento_adapter
