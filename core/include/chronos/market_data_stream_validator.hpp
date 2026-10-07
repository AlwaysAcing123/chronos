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

    MarketDataStreamValidator(const MarketDataStreamValidator&) = delete;
    MarketDataStreamValidator& operator=(
        const MarketDataStreamValidator&
    ) = delete;
    MarketDataStreamValidator(MarketDataStreamValidator&&) noexcept = default;
    MarketDataStreamValidator& operator=(
        MarketDataStreamValidator&&
    ) noexcept = default;

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

    // Declares that the stream continues at `next_ingest_sequence`: the
    // sequences before it were issued but will never be delivered. Only
    // forward, and never after the stream is exhausted: returns false, and
    // changes nothing, if `next_ingest_sequence` is behind the next expected
    // sequence. The last accepted sequence is kept, so a later duplicate of
    // it is still reported as one. Every undeclared gap is still rejected.
    [[nodiscard]] bool continue_at(std::uint64_t next_ingest_sequence) noexcept;

    void reset(std::uint64_t next_expected_ingest_sequence = 0) noexcept;

private:
    MarketDataStreamId stream_;
    std::optional<std::uint64_t> last_accepted_ingest_sequence_;
    std::optional<std::uint64_t> next_expected_ingest_sequence_;
};

}  // namespace chronos
