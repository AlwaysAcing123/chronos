#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace chronos::datasets {

// Why a dataset or feature artifact could not be written, read, or
// verified. Each code has a stable snake_case name for machine-readable
// output (see error_code_name).
enum class DatasetErrorCode {
    // Arguments and the catalog.
    InvalidArgument,
    CatalogUnavailable,
    NotFound,
    SymlinkRejected,
    UnexpectedEntry,
    // Document and record structure.
    MalformedJson,
    NotCanonical,
    UnsupportedVersion,
    UnknownEventType,
    UnknownField,
    MissingField,
    InvalidValue,
    InvalidEvent,
    LineTooLong,
    MissingFinalNewline,
    // Stream contract.
    StreamMismatch,
    IngestSequenceGap,
    DuplicateIngestSequence,
    IngestSequenceMovedBackward,
    ReceiveTimeMovedBackward,
    UnknownInstrument,
    // Integrity against the manifest.
    IdentityMismatch,
    ChecksumMismatch,
    SizeMismatch,
    CountMismatch,
    SummaryMismatch,
    QualityMismatch,
    // Content that cannot be published.
    EmptyDataset,
    UnrepresentableText,
    CounterOverflow,
    // Files.
    ReadFailed,
    WriteFailed,
    PublishFailed,
    Collision,
};

struct DatasetError {
    DatasetErrorCode code;
    // The part of the input or document at fault, such as
    // "events.ndjson" or "manifest.counts.events".
    std::string field;
    // A safe diagnostic for people: never raw record bytes or credentials.
    std::string message;
    // The one-based line of events.ndjson or features.ndjson, when known.
    std::optional<std::uint64_t> line;

    friend bool operator==(const DatasetError&, const DatasetError&) = default;
};

[[nodiscard]] std::string_view error_code_name(DatasetErrorCode code) noexcept;

[[nodiscard]] inline DatasetError dataset_error(
    DatasetErrorCode code,
    std::string field,
    std::string message,
    std::optional<std::uint64_t> line = std::nullopt
) {
    return DatasetError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
        .line = line,
    };
}

}  // namespace chronos::datasets
