#pragma once

#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace chronos {

enum class MarketDataStreamValidationErrorCode {
    StreamMismatch,
    DuplicateIngestSequence,
    IngestSequenceMovedBackward,
    IngestSequenceGap,
};

struct MarketDataStreamValidationError {
    MarketDataStreamValidationErrorCode code;
    std::string field;
    std::string message;
    std::optional<std::uint64_t> expected_ingest_sequence;
    std::uint64_t actual_ingest_sequence;

    friend bool operator==(
        const MarketDataStreamValidationError&,
        const MarketDataStreamValidationError&
    ) = default;
};

struct MarketDataStreamAcceptance {
    std::uint64_t ingest_sequence;

    friend bool operator==(
        const MarketDataStreamAcceptance&,
        const MarketDataStreamAcceptance&
    ) = default;
};

using MarketDataStreamValidationResult = std::variant<
    MarketDataStreamAcceptance,
    MarketDataStreamValidationError
>;

class MarketDataStreamValidator final {
public:
    explicit MarketDataStreamValidator(
        MarketDataStreamId stream,
        std::uint64_t initial_ingest_sequence = 0
    );

    [[nodiscard]] const MarketDataStreamId& stream() const noexcept {
        return stream_;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    last_accepted_ingest_sequence() const noexcept {
        return last_accepted_ingest_sequence_;
    }

    [[nodiscard]] std::optional<std::uint64_t>
    next_expected_ingest_sequence() const noexcept {
        return next_expected_ingest_sequence_;
    }

    [[nodiscard]] MarketDataStreamValidationResult accept(
        const ValidatedMarketEvent& event
    );

    void reset(std::uint64_t next_expected_ingest_sequence = 0) noexcept;

private:
    MarketDataStreamId stream_;
    std::optional<std::uint64_t> last_accepted_ingest_sequence_;
    std::optional<std::uint64_t> next_expected_ingest_sequence_;
};

}  // namespace chronos
