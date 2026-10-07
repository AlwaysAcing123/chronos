#include "chronos/market_data_stream_validator.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using chronos::AggressorSide;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataStreamAcceptance;
using chronos::MarketDataStreamId;
using chronos::MarketDataStreamValidationError;
using chronos::MarketDataStreamValidationErrorCode;
using chronos::MarketDataStreamValidationResult;
using chronos::MarketDataStreamValidator;
using chronos::MarketEvent;
using chronos::MarketEventHeader;
using chronos::MarketEventValidationFailure;
using chronos::Price;
using chronos::Quantity;
using chronos::Timestamp;
using chronos::TradeEvent;
using chronos::ValidatedMarketEvent;

using AcceptFunction = MarketDataStreamValidationResult (
    MarketDataStreamValidator::*
)(const ValidatedMarketEvent&);

static_assert(std::is_invocable_v<
    AcceptFunction,
    MarketDataStreamValidator&,
    const ValidatedMarketEvent&
>);
static_assert(!std::is_invocable_v<
    AcceptFunction,
    MarketDataStreamValidator&,
    const MarketEvent&
>);
static_assert(!std::is_copy_constructible_v<MarketDataStreamValidator>);
static_assert(std::is_move_constructible_v<MarketDataStreamValidator>);

Price price(std::string_view value) {
    return Price::parse(value);
}

Quantity quantity(std::string_view value) {
    return Quantity::parse(value);
}

ValidatedMarketEvent validated_trade(
    std::string stream,
    std::uint64_t ingest_sequence,
    std::int64_t event_timestamp = 1'000,
    std::string instrument = "AAPL"
) {
    MarketEvent event = TradeEvent{
        .header = MarketEventHeader{
            .instrument = InstrumentId(std::move(instrument)),
            .stream = MarketDataStreamId(std::move(stream)),
            .venue = std::nullopt,
            .event_timestamp = Timestamp::from_nanoseconds(event_timestamp),
            .receive_timestamp = Timestamp::from_nanoseconds(2'000),
            .ingest_sequence = ingest_sequence,
            .venue_sequence = std::nullopt,
            .quality = MarketDataQuality{},
        },
        .price = price("100"),
        .quantity = quantity("1"),
        .aggressor_side = AggressorSide::Unknown,
    };

    auto result = chronos::validate_market_event(std::move(event));
    REQUIRE(!std::holds_alternative<MarketEventValidationFailure>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

MarketDataStreamAcceptance require_acceptance(
    MarketDataStreamValidationResult result
) {
    REQUIRE(std::holds_alternative<MarketDataStreamAcceptance>(result));
    return std::get<MarketDataStreamAcceptance>(std::move(result));
}

MarketDataStreamValidationError require_error(
    MarketDataStreamValidationResult result,
    MarketDataStreamValidationErrorCode code
) {
    REQUIRE(std::holds_alternative<MarketDataStreamValidationError>(result));
    auto error = std::get<MarketDataStreamValidationError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

void test_accepts_contiguous_stream_sequence() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));

    REQUIRE(validator.stream() == MarketDataStreamId("stream"));
    REQUIRE(!validator.last_accepted_ingest_sequence().has_value());
    REQUIRE(validator.next_expected_ingest_sequence() == 0);

    const auto first = require_acceptance(
        validator.accept(validated_trade("stream", 0))
    );
    REQUIRE(first.ingest_sequence == 0);
    REQUIRE(validator.last_accepted_ingest_sequence() == 0);
    REQUIRE(validator.next_expected_ingest_sequence() == 1);

    const auto second = require_acceptance(
        validator.accept(validated_trade("stream", 1))
    );
    REQUIRE(second.ingest_sequence == 1);
    REQUIRE(validator.last_accepted_ingest_sequence() == 1);
    REQUIRE(validator.next_expected_ingest_sequence() == 2);
}

void test_rejects_stream_mismatch_without_changing_state() {
    MarketDataStreamValidator validator(MarketDataStreamId("expected"));

    const auto error = require_error(
        validator.accept(validated_trade("other", 0)),
        MarketDataStreamValidationErrorCode::StreamMismatch
    );
    REQUIRE(error.field == "header.stream");
    REQUIRE(error.actual_ingest_sequence == 0);
    REQUIRE(!error.expected_ingest_sequence.has_value());
    REQUIRE(!validator.last_accepted_ingest_sequence().has_value());
    REQUIRE(validator.next_expected_ingest_sequence() == 0);

    require_acceptance(validator.accept(validated_trade("expected", 0)));
}

void test_rejects_gap_without_consuming_sequence() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));
    require_acceptance(validator.accept(validated_trade("stream", 0)));

    const auto error = require_error(
        validator.accept(validated_trade("stream", 2)),
        MarketDataStreamValidationErrorCode::IngestSequenceGap
    );
    REQUIRE(error.field == "header.ingest_sequence");
    REQUIRE(error.expected_ingest_sequence == 1);
    REQUIRE(error.actual_ingest_sequence == 2);
    REQUIRE(validator.last_accepted_ingest_sequence() == 0);
    REQUIRE(validator.next_expected_ingest_sequence() == 1);

    require_acceptance(validator.accept(validated_trade("stream", 1)));
}

void test_distinguishes_duplicate_and_backward_sequence() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));
    require_acceptance(validator.accept(validated_trade("stream", 0)));
    require_acceptance(validator.accept(validated_trade("stream", 1)));

    const auto duplicate = require_error(
        validator.accept(validated_trade("stream", 1)),
        MarketDataStreamValidationErrorCode::DuplicateIngestSequence
    );
    REQUIRE(duplicate.expected_ingest_sequence == 2);
    REQUIRE(duplicate.actual_ingest_sequence == 1);

    const auto backward = require_error(
        validator.accept(validated_trade("stream", 0)),
        MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward
    );
    REQUIRE(backward.expected_ingest_sequence == 2);
    REQUIRE(backward.actual_ingest_sequence == 0);
    REQUIRE(validator.last_accepted_ingest_sequence() == 1);
    REQUIRE(validator.next_expected_ingest_sequence() == 2);
}

void test_allows_market_timestamps_to_move_backward() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));
    require_acceptance(
        validator.accept(validated_trade("stream", 0, 1'500))
    );
    require_acceptance(
        validator.accept(validated_trade("stream", 1, 1'000))
    );

    REQUIRE(validator.last_accepted_ingest_sequence() == 1);
}

void test_one_stream_can_contain_multiple_instruments() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));
    require_acceptance(
        validator.accept(validated_trade("stream", 0, 1'000, "AAPL"))
    );
    require_acceptance(
        validator.accept(validated_trade("stream", 1, 1'001, "SPY"))
    );

    REQUIRE(validator.next_expected_ingest_sequence() == 2);
}

void test_reset_establishes_new_sequence_boundary() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));
    require_acceptance(validator.accept(validated_trade("stream", 0)));

    validator.reset(10);
    REQUIRE(!validator.last_accepted_ingest_sequence().has_value());
    REQUIRE(validator.next_expected_ingest_sequence() == 10);

    require_acceptance(validator.accept(validated_trade("stream", 10)));
    REQUIRE(validator.next_expected_ingest_sequence() == 11);
}

void test_nonzero_initial_boundary_rejects_older_sequence() {
    MarketDataStreamValidator validator(MarketDataStreamId("stream"), 10);

    const auto error = require_error(
        validator.accept(validated_trade("stream", 9)),
        MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward
    );
    REQUIRE(error.expected_ingest_sequence == 10);
    REQUIRE(error.actual_ingest_sequence == 9);
    REQUIRE(!validator.last_accepted_ingest_sequence().has_value());
    REQUIRE(validator.next_expected_ingest_sequence() == 10);

    require_acceptance(validator.accept(validated_trade("stream", 10)));
}

void test_sequence_maximum_does_not_wrap() {
    constexpr std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    MarketDataStreamValidator validator(MarketDataStreamId("stream"), maximum);

    require_acceptance(validator.accept(validated_trade("stream", maximum)));
    REQUIRE(validator.last_accepted_ingest_sequence() == maximum);
    REQUIRE(!validator.next_expected_ingest_sequence().has_value());

    const auto duplicate = require_error(
        validator.accept(validated_trade("stream", maximum)),
        MarketDataStreamValidationErrorCode::DuplicateIngestSequence
    );
    REQUIRE(!duplicate.expected_ingest_sequence.has_value());

    const auto backward = require_error(
        validator.accept(validated_trade("stream", maximum - 1)),
        MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward
    );
    REQUIRE(!backward.expected_ingest_sequence.has_value());
    REQUIRE(!validator.next_expected_ingest_sequence().has_value());
}

void test_a_declared_continuation_is_forward_only_and_keeps_gap_checks() {
    constexpr std::uint64_t maximum =
        std::numeric_limits<std::uint64_t>::max();
    MarketDataStreamValidator validator(MarketDataStreamId("stream"));
    require_acceptance(validator.accept(validated_trade("stream", 0)));

    // Behind the next expected sequence: refused, nothing changes.
    REQUIRE(!validator.continue_at(0));
    REQUIRE(validator.next_expected_ingest_sequence() == 1);
    REQUIRE(validator.continue_at(1));
    REQUIRE(validator.continue_at(1'048'576));
    REQUIRE(validator.next_expected_ingest_sequence() == 1'048'576);
    REQUIRE(validator.last_accepted_ingest_sequence() == 0);

    // The declared sequence is accepted; an undeclared gap after it is not.
    require_error(
        validator.accept(validated_trade("stream", 1)),
        MarketDataStreamValidationErrorCode::IngestSequenceMovedBackward
    );
    require_acceptance(validator.accept(validated_trade("stream", 1'048'576)));
    require_error(
        validator.accept(validated_trade("stream", 1'048'578)),
        MarketDataStreamValidationErrorCode::IngestSequenceGap
    );

    // Up to the last sequence, and no further.
    REQUIRE(validator.continue_at(maximum));
    require_acceptance(validator.accept(validated_trade("stream", maximum)));
    REQUIRE(!validator.continue_at(maximum));
    REQUIRE(!validator.next_expected_ingest_sequence().has_value());
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"contiguous stream sequence", test_accepts_contiguous_stream_sequence},
        {
            "stream mismatch is atomic",
            test_rejects_stream_mismatch_without_changing_state,
        },
        {
            "sequence gap is atomic",
            test_rejects_gap_without_consuming_sequence,
        },
        {
            "duplicate and backward sequence",
            test_distinguishes_duplicate_and_backward_sequence,
        },
        {
            "non-monotonic market timestamps",
            test_allows_market_timestamps_to_move_backward,
        },
        {
            "multi-instrument stream",
            test_one_stream_can_contain_multiple_instruments,
        },
        {"stream reset", test_reset_establishes_new_sequence_boundary},
        {
            "nonzero initial boundary",
            test_nonzero_initial_boundary_rejects_older_sequence,
        },
        {"sequence maximum", test_sequence_maximum_does_not_wrap},
        {
            "declared continuation",
            test_a_declared_continuation_is_forward_only_and_keeps_gap_checks,
        },
    });
}
