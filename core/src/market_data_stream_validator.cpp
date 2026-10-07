#include "chronos/market_data_stream_validator.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace chronos {
namespace {

MarketDataStreamValidationError stream_error(
    MarketDataStreamValidationErrorCode code,
    std::string field,
    std::string message,
    std::optional<std::uint64_t> expected_ingest_sequence,
    std::uint64_t actual_ingest_sequence
) {
    return MarketDataStreamValidationError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
        .expected_ingest_sequence = expected_ingest_sequence,
        .actual_ingest_sequence = actual_ingest_sequence,
    };
}

}  // namespace

MarketDataStreamValidator::MarketDataStreamValidator(
    MarketDataStreamId stream,
    std::uint64_t initial_ingest_sequence
)
    : stream_(std::move(stream)),
      next_expected_ingest_sequence_(initial_ingest_sequence) {}

MarketDataStreamValidationResult MarketDataStreamValidator::accept(
    const ValidatedMarketEvent& event
) {
    const MarketEventHeader& header = market_event_header(event.value());
    const std::uint64_t actual_sequence = header.ingest_sequence;

    if (header.stream != stream_) {
        return stream_error(
            MarketDataStreamValidationErrorCode::StreamMismatch,
            "header.stream",
            "expected stream " + stream_.value() + ", received " +
                header.stream.value(),
            std::nullopt,
            actual_sequence
        );
    }

    if (!next_expected_ingest_sequence_.has_value()) {
        if (last_accepted_ingest_sequence_ == actual_sequence) {
            return stream_error(
                MarketDataStreamValidationErrorCode::DuplicateIngestSequence,
                "header.ingest_sequence",
                "ingest sequence duplicates the last accepted event",
                std::nullopt,
                actual_sequence
            );
        }
        return stream_error(
            MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward,
            "header.ingest_sequence",
            "ingest sequence moved behind the last accepted value",
            std::nullopt,
            actual_sequence
        );
    }

    const std::uint64_t expected_sequence =
        *next_expected_ingest_sequence_;
    if (actual_sequence < expected_sequence) {
        const bool duplicates_last =
            last_accepted_ingest_sequence_ == actual_sequence;
        return stream_error(
            duplicates_last
                ? MarketDataStreamValidationErrorCode::DuplicateIngestSequence
                : MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward,
            "header.ingest_sequence",
            duplicates_last
                ? "ingest sequence duplicates the last accepted event"
                : "ingest sequence moved behind the next expected value",
            expected_sequence,
            actual_sequence
        );
    }
    if (actual_sequence > expected_sequence) {
        return stream_error(
            MarketDataStreamValidationErrorCode::IngestSequenceGap,
            "header.ingest_sequence",
            "ingest sequence skipped one or more expected events",
            expected_sequence,
            actual_sequence
        );
    }

    last_accepted_ingest_sequence_ = actual_sequence;
    if (actual_sequence == std::numeric_limits<std::uint64_t>::max()) {
        next_expected_ingest_sequence_ = std::nullopt;
    } else {
        next_expected_ingest_sequence_ = actual_sequence + 1;
    }

    return MarketDataStreamAcceptance{.ingest_sequence = actual_sequence};
}

bool MarketDataStreamValidator::continue_at(
    std::uint64_t next_ingest_sequence
) noexcept {
    if (!next_expected_ingest_sequence_ ||
        next_ingest_sequence < *next_expected_ingest_sequence_) {
        return false;
    }
    next_expected_ingest_sequence_ = next_ingest_sequence;
    return true;
}

void MarketDataStreamValidator::reset(
    std::uint64_t next_expected_ingest_sequence
) noexcept {
    last_accepted_ingest_sequence_ = std::nullopt;
    next_expected_ingest_sequence_ = next_expected_ingest_sequence;
}

}  // namespace chronos
