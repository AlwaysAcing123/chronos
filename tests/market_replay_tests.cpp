#include "chronos/market_replay.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::AggressorSide;
using chronos::BookLevel;
using chronos::CurrencyCode;
using chronos::CurrentMarketView;
using chronos::InstrumentDefinitionEvent;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataStreamId;
using chronos::MarketEvent;
using chronos::MarketEventHeader;
using chronos::MarketEventValidationFailure;
using chronos::MarketReplay;
using chronos::MarketReplayError;
using chronos::MarketReplayErrorCode;
using chronos::MarketReplayResult;
using chronos::MarketReplayStep;
using chronos::MarketStatusEvent;
using chronos::MarketViewUpdate;
using chronos::Price;
using chronos::Quantity;
using chronos::ReplayClock;
using chronos::ReplayClockAdvance;
using chronos::ReplayClockError;
using chronos::ReplayClockErrorCode;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::TradingStatus;
using chronos::ValidatedMarketEvent;

using ReplayAcceptFunction = MarketReplayResult (MarketReplay::*)(
    const ValidatedMarketEvent&
);

static_assert(std::is_invocable_v<
    ReplayAcceptFunction,
    MarketReplay&,
    const ValidatedMarketEvent&
>);
static_assert(!std::is_invocable_v<
    ReplayAcceptFunction,
    MarketReplay&,
    const MarketEvent&
>);
static_assert(!std::is_copy_constructible_v<MarketReplay>);
static_assert(std::is_move_constructible_v<MarketReplay>);

Price price(std::string_view value) { return Price::parse(value); }

Quantity quantity(std::string_view value) {
    return Quantity::parse(value);
}

MarketEventHeader header(
    std::string stream,
    std::uint64_t ingest_sequence,
    std::int64_t event_time,
    std::int64_t receive_time,
    std::string instrument = "AAPL"
) {
    return MarketEventHeader{
        .instrument = InstrumentId(std::move(instrument)),
        .stream = MarketDataStreamId(std::move(stream)),
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(event_time),
        .receive_timestamp = Timestamp::from_nanoseconds(receive_time),
        .ingest_sequence = ingest_sequence,
        .venue_sequence = std::nullopt,
        .quality = MarketDataQuality{},
    };
}

ValidatedMarketEvent require_valid(MarketEvent event) {
    auto result = chronos::validate_market_event(std::move(event));
    REQUIRE(!std::holds_alternative<MarketEventValidationFailure>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

ValidatedMarketEvent trade(
    std::string stream,
    std::uint64_t ingest_sequence,
    std::int64_t event_time,
    std::int64_t receive_time,
    std::string instrument = "AAPL",
    std::string_view trade_price = "100"
) {
    return require_valid(TradeEvent{
        .header = header(
            std::move(stream),
            ingest_sequence,
            event_time,
            receive_time,
            std::move(instrument)
        ),
        .price = price(trade_price),
        .quantity = quantity("2"),
        .aggressor_side = AggressorSide::Buy,
    });
}

ValidatedMarketEvent top_of_book(
    std::string stream,
    std::uint64_t ingest_sequence,
    std::int64_t event_time,
    std::int64_t receive_time,
    std::string instrument = "AAPL"
) {
    return require_valid(TopOfBookEvent{
        .header = header(
            std::move(stream),
            ingest_sequence,
            event_time,
            receive_time,
            std::move(instrument)
        ),
        .bid = BookLevel{
            .price = price("99.99"),
            .quantity = quantity("5"),
        },
        .ask = BookLevel{
            .price = price("100.01"),
            .quantity = quantity("6"),
        },
    });
}

ValidatedMarketEvent market_status(
    std::string stream,
    std::uint64_t ingest_sequence,
    std::int64_t event_time,
    std::int64_t receive_time
) {
    return require_valid(MarketStatusEvent{
        .header = header(
            std::move(stream),
            ingest_sequence,
            event_time,
            receive_time
        ),
        .status = TradingStatus::Open,
        .reason = "regular session",
    });
}

ValidatedMarketEvent definition(
    std::string stream,
    std::uint64_t ingest_sequence,
    std::int64_t event_time,
    std::int64_t receive_time
) {
    return require_valid(InstrumentDefinitionEvent{
        .header = header(
            std::move(stream),
            ingest_sequence,
            event_time,
            receive_time
        ),
        .display_symbol = "AAPL",
        .currency = CurrencyCode("USD"),
        .minimum_price_increment = price("0.01"),
        .minimum_quantity_increment = quantity("1"),
        .effective_from = Timestamp::from_nanoseconds(event_time),
        .effective_until = std::nullopt,
    });
}

ReplayClockAdvance require_clock_advance(
    chronos::ReplayClockAdvanceResult result
) {
    REQUIRE(std::holds_alternative<ReplayClockAdvance>(result));
    return std::get<ReplayClockAdvance>(std::move(result));
}

ReplayClockError require_clock_error(
    chronos::ReplayClockAdvanceResult result,
    ReplayClockErrorCode code
) {
    REQUIRE(std::holds_alternative<ReplayClockError>(result));
    auto error = std::get<ReplayClockError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.message.empty());
    return error;
}

MarketReplayStep require_replay_step(MarketReplayResult result) {
    REQUIRE(std::holds_alternative<MarketReplayStep>(result));
    return std::get<MarketReplayStep>(std::move(result));
}

MarketReplayError require_replay_error(
    MarketReplayResult result,
    MarketReplayErrorCode code
) {
    REQUIRE(std::holds_alternative<MarketReplayError>(result));
    auto error = std::get<MarketReplayError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

void test_replay_clock_is_monotonic_and_resettable() {
    ReplayClock clock;
    REQUIRE(!clock.current_time().has_value());

    const auto first = require_clock_advance(
        clock.advance_to(Timestamp::from_nanoseconds(100))
    );
    REQUIRE(first.current_time == Timestamp::from_nanoseconds(100));
    REQUIRE(first.time_changed);

    const auto equal = require_clock_advance(
        clock.advance_to(Timestamp::from_nanoseconds(100))
    );
    REQUIRE(!equal.time_changed);

    const auto backward = require_clock_error(
        clock.advance_to(Timestamp::from_nanoseconds(99)),
        ReplayClockErrorCode::TimeMovedBackward
    );
    REQUIRE(backward.current_time == Timestamp::from_nanoseconds(100));
    REQUIRE(clock.current_time() == Timestamp::from_nanoseconds(100));

    clock.reset();
    REQUIRE(!clock.current_time().has_value());
    require_clock_error(
        clock.advance_to(Timestamp::from_nanoseconds(-1)),
        ReplayClockErrorCode::NegativeTimestamp
    );
    REQUIRE(!clock.current_time().has_value());
}

void test_current_market_view_retains_each_event_kind() {
    CurrentMarketView view;
    REQUIRE(view.empty());

    REQUIRE(view.apply(trade("stream", 0, 90, 100)) ==
        MarketViewUpdate::LastTrade);
    REQUIRE(view.apply(top_of_book("stream", 1, 91, 101)) ==
        MarketViewUpdate::TopOfBook);
    REQUIRE(view.apply(market_status("stream", 2, 92, 102)) ==
        MarketViewUpdate::TradingStatus);
    REQUIRE(view.apply(definition("stream", 3, 93, 103)) ==
        MarketViewUpdate::InstrumentDefinition);
    REQUIRE(view.apply(trade("stream", 4, 94, 104, "SPY", "500")) ==
        MarketViewUpdate::LastTrade);

    REQUIRE(view.size() == 2);
    const auto* aapl = view.find(InstrumentId("AAPL"));
    REQUIRE(aapl != nullptr);
    REQUIRE(aapl->last_trade.has_value());
    REQUIRE(aapl->top_of_book.has_value());
    REQUIRE(aapl->trading_status.has_value());
    REQUIRE(aapl->definition.has_value());
    REQUIRE(aapl->last_trade->price == price("100"));

    const auto* spy = view.find(InstrumentId("SPY"));
    REQUIRE(spy != nullptr);
    REQUIRE(spy->last_trade->price == price("500"));
    REQUIRE(!spy->top_of_book.has_value());

    view.reset();
    REQUIRE(view.empty());
    REQUIRE(view.find(InstrumentId("AAPL")) == nullptr);
}

void test_replay_uses_receive_time_and_stable_ingest_order() {
    MarketReplay replay(MarketDataStreamId("stream"));

    const auto first = require_replay_step(
        replay.accept(trade("stream", 0, 95, 100))
    );
    const auto second = require_replay_step(
        replay.accept(top_of_book("stream", 1, 90, 100))
    );

    REQUIRE(first.replay_time == Timestamp::from_nanoseconds(100));
    REQUIRE(second.replay_time == Timestamp::from_nanoseconds(100));
    REQUIRE(first.ingest_sequence == 0);
    REQUIRE(second.ingest_sequence == 1);
    REQUIRE(first.market_view_update == MarketViewUpdate::LastTrade);
    REQUIRE(second.market_view_update == MarketViewUpdate::TopOfBook);
    REQUIRE(replay.last_accepted_ingest_sequence() == 1);
    REQUIRE(replay.market_view().find(InstrumentId("AAPL")) != nullptr);
}

void test_receive_time_regression_is_rejected_atomically() {
    MarketReplay replay(MarketDataStreamId("stream"));
    require_replay_step(replay.accept(trade("stream", 0, 90, 100)));
    const CurrentMarketView view_before = replay.market_view();

    const auto error = require_replay_error(
        replay.accept(top_of_book("stream", 1, 89, 99)),
        MarketReplayErrorCode::ReceiveTimeMovedBackward
    );
    REQUIRE(error.clock_error.has_value());
    REQUIRE(
        error.clock_error->code == ReplayClockErrorCode::TimeMovedBackward
    );
    REQUIRE(replay.last_accepted_ingest_sequence() == 0);
    REQUIRE(replay.clock().current_time() == Timestamp::from_nanoseconds(100));
    REQUIRE(replay.market_view() == view_before);

    require_replay_step(replay.accept(top_of_book("stream", 1, 91, 101)));
}

void test_stream_error_does_not_change_clock_or_market_view() {
    MarketReplay replay(MarketDataStreamId("stream"));
    require_replay_step(replay.accept(trade("stream", 0, 90, 100)));
    const CurrentMarketView view_before = replay.market_view();

    const auto error = require_replay_error(
        replay.accept(top_of_book("other", 1, 91, 200)),
        MarketReplayErrorCode::StreamValidationFailed
    );
    REQUIRE(error.stream_error.has_value());
    REQUIRE(replay.last_accepted_ingest_sequence() == 0);
    REQUIRE(replay.clock().current_time() == Timestamp::from_nanoseconds(100));
    REQUIRE(replay.market_view() == view_before);
}

void test_repeated_replay_after_reset_is_identical() {
    const std::vector<ValidatedMarketEvent> events = {
        trade("stream", 5, 90, 100),
        top_of_book("stream", 6, 91, 100),
        trade("stream", 7, 92, 101, "SPY", "500"),
    };
    MarketReplay replay(MarketDataStreamId("stream"), 5);

    std::vector<MarketReplayStep> first_steps;
    for (const auto& event : events) {
        first_steps.push_back(require_replay_step(replay.accept(event)));
    }
    const CurrentMarketView first_view = replay.market_view();
    const auto first_time = replay.clock().current_time();

    replay.reset(5);
    REQUIRE(replay.market_view().empty());
    REQUIRE(!replay.clock().current_time().has_value());
    REQUIRE(!replay.last_accepted_ingest_sequence().has_value());

    std::vector<MarketReplayStep> second_steps;
    for (const auto& event : events) {
        second_steps.push_back(require_replay_step(replay.accept(event)));
    }
    REQUIRE(second_steps == first_steps);
    REQUIRE(replay.market_view() == first_view);
    REQUIRE(replay.clock().current_time() == first_time);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "monotonic replay clock",
            test_replay_clock_is_monotonic_and_resettable,
        },
        {
            "complete current market view",
            test_current_market_view_retains_each_event_kind,
        },
        {
            "receive-time replay ordering",
            test_replay_uses_receive_time_and_stable_ingest_order,
        },
        {
            "atomic receive-time rejection",
            test_receive_time_regression_is_rejected_atomically,
        },
        {
            "atomic stream rejection",
            test_stream_error_does_not_change_clock_or_market_view,
        },
        {
            "deterministic replay reset",
            test_repeated_replay_after_reset_is_identical,
        },
    });
}
