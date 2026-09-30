#include "chronos/datasets/dataset_error.hpp"

#include <string_view>

namespace chronos::datasets {

std::string_view error_code_name(DatasetErrorCode code) noexcept {
    switch (code) {
        case DatasetErrorCode::InvalidArgument:
            return "invalid_argument";
        case DatasetErrorCode::CatalogUnavailable:
            return "catalog_unavailable";
        case DatasetErrorCode::NotFound:
            return "not_found";
        case DatasetErrorCode::SymlinkRejected:
            return "symlink_rejected";
        case DatasetErrorCode::UnexpectedEntry:
            return "unexpected_entry";
        case DatasetErrorCode::MalformedJson:
            return "malformed_json";
        case DatasetErrorCode::NotCanonical:
            return "not_canonical";
        case DatasetErrorCode::UnsupportedVersion:
            return "unsupported_version";
        case DatasetErrorCode::UnknownEventType:
            return "unknown_event_type";
        case DatasetErrorCode::UnknownField:
            return "unknown_field";
        case DatasetErrorCode::MissingField:
            return "missing_field";
        case DatasetErrorCode::InvalidValue:
            return "invalid_value";
        case DatasetErrorCode::InvalidEvent:
            return "invalid_event";
        case DatasetErrorCode::LineTooLong:
            return "line_too_long";
        case DatasetErrorCode::MissingFinalNewline:
            return "missing_final_newline";
        case DatasetErrorCode::StreamMismatch:
            return "stream_mismatch";
        case DatasetErrorCode::IngestSequenceGap:
            return "ingest_sequence_gap";
        case DatasetErrorCode::DuplicateIngestSequence:
            return "duplicate_ingest_sequence";
        case DatasetErrorCode::IngestSequenceMovedBackward:
            return "ingest_sequence_moved_backward";
        case DatasetErrorCode::ReceiveTimeMovedBackward:
            return "receive_time_moved_backward";
        case DatasetErrorCode::UnknownInstrument:
            return "unknown_instrument";
        case DatasetErrorCode::IdentityMismatch:
            return "identity_mismatch";
        case DatasetErrorCode::ChecksumMismatch:
            return "checksum_mismatch";
        case DatasetErrorCode::SizeMismatch:
            return "size_mismatch";
        case DatasetErrorCode::CountMismatch:
            return "count_mismatch";
        case DatasetErrorCode::SummaryMismatch:
            return "summary_mismatch";
        case DatasetErrorCode::QualityMismatch:
            return "quality_mismatch";
        case DatasetErrorCode::EmptyDataset:
            return "empty_dataset";
        case DatasetErrorCode::UnrepresentableText:
            return "unrepresentable_text";
        case DatasetErrorCode::CounterOverflow:
            return "counter_overflow";
        case DatasetErrorCode::ReadFailed:
            return "read_failed";
        case DatasetErrorCode::WriteFailed:
            return "write_failed";
        case DatasetErrorCode::PublishFailed:
            return "publish_failed";
        case DatasetErrorCode::Collision:
            return "collision";
    }
    return "unknown";
}

}  // namespace chronos::datasets
