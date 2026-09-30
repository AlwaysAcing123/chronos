#pragma once

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/market_data_stream_validator.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace chronos::datasets::detail {

// The logical input contract of a normalized dataset, enforced when it is
// written and again when it is read: one stream, ingest sequences
// contiguous from the first event, and receive time that never decreases,
// which is what replay requires.
class StreamContract final {
public:
    StreamContract(MarketDataStreamId stream, std::optional<std::uint64_t> first_ingest_sequence)
        : stream_(std::move(stream)), first_ingest_sequence_(first_ingest_sequence) {}

    [[nodiscard]] std::optional<DatasetError> accept(
        const ValidatedMarketEvent& event,
        std::optional<std::uint64_t> line
    ) {
        const MarketEventHeader& header = market_event_header(event.value());
        if (!sequences_) {
            sequences_.emplace(stream_, first_ingest_sequence_.value_or(header.ingest_sequence));
        }
        auto accepted = sequences_->accept(event);
        if (const auto* error = std::get_if<MarketDataStreamValidationError>(&accepted)) {
            return dataset_error(code_for(error->code), error->field, error->message, line);
        }
        if (last_receive_ && header.receive_timestamp < *last_receive_) {
            return dataset_error(
                DatasetErrorCode::ReceiveTimeMovedBackward,
                "header.receive_timestamp",
                "receive timestamp moved behind the previous event's",
                line
            );
        }
        last_receive_ = header.receive_timestamp;
        return std::nullopt;
    }

private:
    static DatasetErrorCode code_for(MarketDataStreamValidationErrorCode code) {
        switch (code) {
            case MarketDataStreamValidationErrorCode::StreamMismatch:
                return DatasetErrorCode::StreamMismatch;
            case MarketDataStreamValidationErrorCode::DuplicateIngestSequence:
                return DatasetErrorCode::DuplicateIngestSequence;
            case MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward:
                return DatasetErrorCode::IngestSequenceMovedBackward;
            case MarketDataStreamValidationErrorCode::IngestSequenceGap:
                return DatasetErrorCode::IngestSequenceGap;
        }
        return DatasetErrorCode::InvalidEvent;
    }

    MarketDataStreamId stream_;
    std::optional<std::uint64_t> first_ingest_sequence_;
    std::optional<MarketDataStreamValidator> sequences_;
    std::optional<Timestamp> last_receive_;
};

}  // namespace chronos::datasets::detail
