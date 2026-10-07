#include "chronos/market_state.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::AggressorSide;
using chronos::BookLevel;
using chronos::ExecutionQuote;
using chronos::ExecutionQuoteResult;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketDataStreamValidationErrorCode;
using chronos::MarketEvent;
using chronos::MarketEventHeader;
using chronos::MarketEventValidationFailure;
using chronos::MarketQuotes;
using chronos::MarketReplayErrorCode;
using chronos::MarketReplayStep;
using chronos::MarketState;
using chronos::MarketStateUnavailable;
using chronos::MarketStateUnavailableCode;
using chronos::MarketStateUpdateError;
using chronos::MarketStateUpdateErrorCode;
using chronos::MarketStateUpdateResult;
using chronos::Price;
using chronos::Quantity;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::ValidatedMarketEvent;

constexpr std::string_view kStream = "market-state-stream";

MarketEventHeader header(
    std::uint64_t ingest_sequence,
    std::int64_t receive_time,
    std::string instrument = "AAPL",
    std::string stream = std::string(kStream)
) {
    return MarketEventHeader{
        .instrument = InstrumentId(std::move(instrument)),
        .stream = MarketDataStreamId(std::move(stream)),
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(receive_time - 1),
        .receive_timestamp = Timestamp::from_nanoseconds(receive_time),
        .ingest_sequence = ingest_sequence,
        .venue_sequence = std::nullopt,
        .quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay),
    };
}

ValidatedMarketEvent require_valid(MarketEvent event) {
    auto result = chronos::validate_market_event(std::move(event));
    REQUIRE(!std::holds_alternative<MarketEventValidationFailure>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

TopOfBookEvent top_of_book(
    std::uint64_t ingest_sequence,
    std::int64_t receive_time,
    std::string instrument = "AAPL",
    std::string_view ask_quantity = "5"
) {
    return TopOfBookEvent{
        .header = header(ingest_sequence, receive_time, std::move(instrument)),
        .bid = BookLevel{
            .price = Price::parse("99.5"),
            .quantity = Quantity::parse("3"),
        },
        .ask = BookLevel{
            .price = Price::parse("100.25"),
            .quantity = Quantity::parse(ask_quantity),
        },
    };
}

ValidatedMarketEvent trade(std::uint64_t ingest_sequence, std::int64_t time) {
    return require_valid(TradeEvent{
        .header = header(ingest_sequence, time),
        .price = Price::parse("100"),
        .quantity = Quantity::parse("1"),
        .aggressor_side = AggressorSide::Buy,
    });
}

void require_step(MarketStateUpdateResult result) {
    REQUIRE(std::holds_alternative<MarketReplayStep>(result));
}

ExecutionQuote require_quote(const ExecutionQuoteResult& result) {
    REQUIRE(std::holds_alternative<ExecutionQuote>(result));
    return std::get<ExecutionQuote>(result);
}

void require_unavailable(
    const ExecutionQuoteResult& result,
    MarketStateUnavailableCode code
) {
    REQUIRE(std::holds_alternative<MarketStateUnavailable>(result));
    const auto& unavailable = std::get<MarketStateUnavailable>(result);
    REQUIRE(unavailable.code == code);
    REQUIRE(!unavailable.message.empty());
}

const MarketStateUpdateError& require_update_error(
    const MarketStateUpdateResult& result,
    MarketStateUpdateErrorCode code
) {
    REQUIRE(std::holds_alternative<MarketStateUpdateError>(result));
    const auto& error = std::get<MarketStateUpdateError>(result);
    REQUIRE(error.code == code);
    REQUIRE(!error.message.empty());
    return error;
}

void test_empty_state_has_no_quote_or_time() {
    const MarketState state{MarketDataStreamId(std::string(kStream))};
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::NoTopOfBook
    );
    const auto status = state.status();
    REQUIRE(status.stream == MarketDataStreamId(std::string(kStream)));
    REQUIRE(!status.last_ingest_sequence.has_value());
    REQUIRE(!status.market_time.has_value());
    REQUIRE(!status.interruption.has_value());
}

void test_trade_alone_is_not_an_execution_quote() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(trade(0, 100)));
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::NoTopOfBook
    );
    REQUIRE(state.status().market_time == Timestamp::from_nanoseconds(100));
}

void test_quote_is_returned_exactly_with_market_time() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    const TopOfBookEvent quote = top_of_book(0, 100);
    require_step(state.apply(require_valid(quote)));

    const ExecutionQuote execution_quote = require_quote(
        state.execution_quote(InstrumentId("AAPL"))
    );
    REQUIRE(execution_quote.top_of_book == quote);
    REQUIRE(execution_quote.market_time == Timestamp::from_nanoseconds(100));
    require_unavailable(
        state.execution_quote(InstrumentId("SPY")),
        MarketStateUnavailableCode::NoTopOfBook
    );
}

void test_quote_ages_as_other_instruments_advance_market_time() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    const TopOfBookEvent aapl = top_of_book(0, 100);
    require_step(state.apply(require_valid(aapl)));
    require_step(state.apply(require_valid(top_of_book(1, 5'000, "SPY"))));

    const ExecutionQuote quote = require_quote(
        state.execution_quote(InstrumentId("AAPL"))
    );
    REQUIRE(quote.top_of_book == aapl);
    REQUIRE(quote.market_time == Timestamp::from_nanoseconds(5'000));
    REQUIRE(state.status().last_ingest_sequence == 1U);
}

void test_quality_flags_are_preserved_for_risk_to_judge() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    TopOfBookEvent crossed = top_of_book(0, 100);
    crossed.bid->price = Price::parse("101");
    crossed.header.quality = crossed.header.quality.with(
        MarketDataQualityFlag::PotentiallyInvalidBook
    );
    require_step(state.apply(require_valid(crossed)));

    const ExecutionQuote quote = require_quote(
        state.execution_quote(InstrumentId("AAPL"))
    );
    REQUIRE(quote.top_of_book == crossed);
    REQUIRE(quote.top_of_book.header.quality.contains(
        MarketDataQualityFlag::PotentiallyInvalidBook
    ));
}

void test_sequence_gap_interrupts_every_quote_permanently() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 100))));
    require_step(state.apply(require_valid(top_of_book(1, 110, "SPY"))));

    const auto gap_result = state.apply(require_valid(top_of_book(3, 120)));
    const auto& gap = require_update_error(
        gap_result,
        MarketStateUpdateErrorCode::ReplayRejected
    );
    REQUIRE(gap.replay_error.has_value());
    REQUIRE(
        gap.replay_error->code == MarketReplayErrorCode::StreamValidationFailed
    );
    REQUIRE(
        gap.replay_error->stream_error->code ==
        MarketDataStreamValidationErrorCode::IngestSequenceGap
    );

    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::StreamInterrupted
    );
    require_unavailable(
        state.execution_quote(InstrumentId("SPY")),
        MarketStateUnavailableCode::StreamInterrupted
    );
    const auto late_result = state.apply(require_valid(top_of_book(2, 115)));
    const auto& late = require_update_error(
        late_result,
        MarketStateUpdateErrorCode::StreamInterrupted
    );
    REQUIRE(!late.replay_error.has_value());

    const auto status = state.status();
    REQUIRE(status.last_ingest_sequence == 1U);
    REQUIRE(status.market_time == Timestamp::from_nanoseconds(110));
    REQUIRE(status.interruption.has_value());
    REQUIRE(status.interruption == gap.replay_error);
}

void test_receive_time_regression_interrupts_the_stream() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 100))));
    const auto regression_result = state.apply(
        require_valid(top_of_book(1, 99))
    );
    const auto& regression = require_update_error(
        regression_result,
        MarketStateUpdateErrorCode::ReplayRejected
    );
    REQUIRE(
        regression.replay_error->code ==
        MarketReplayErrorCode::ReceiveTimeMovedBackward
    );
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::StreamInterrupted
    );
}

void test_event_from_another_stream_interrupts_the_stream() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    TopOfBookEvent foreign = top_of_book(0, 100);
    foreign.header.stream = MarketDataStreamId("another-stream");
    const auto mismatch_result = state.apply(require_valid(foreign));
    const auto& mismatch = require_update_error(
        mismatch_result,
        MarketStateUpdateErrorCode::ReplayRejected
    );
    REQUIRE(
        mismatch.replay_error->stream_error->code ==
        MarketDataStreamValidationErrorCode::StreamMismatch
    );
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::StreamInterrupted
    );
}

void test_concurrent_readers_observe_consistent_snapshots() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 1'000))));
    constexpr std::uint64_t kUpdates = 2'000;

    std::atomic<bool> writer_done{false};
    std::atomic<bool> readers_consistent{true};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 3; ++reader) {
        readers.emplace_back([&] {
            std::uint64_t previous_sequence = 0;
            while (!writer_done.load()) {
                const auto result = state.execution_quote(InstrumentId("AAPL"));
                const auto* quote = std::get_if<ExecutionQuote>(&result);
                if (quote == nullptr ||
                    quote->market_time <
                        quote->top_of_book.header.receive_timestamp ||
                    quote->top_of_book.header.ingest_sequence <
                        previous_sequence) {
                    readers_consistent = false;
                    return;
                }
                previous_sequence = quote->top_of_book.header.ingest_sequence;
            }
        });
    }

    bool writer_accepted_every_update = true;
    for (std::uint64_t sequence = 1; sequence <= kUpdates; ++sequence) {
        const auto result = state.apply(require_valid(top_of_book(
            sequence,
            static_cast<std::int64_t>(1'000 + sequence),
            "AAPL",
            std::to_string(1 + sequence % 7)
        )));
        writer_accepted_every_update = writer_accepted_every_update &&
            std::holds_alternative<MarketReplayStep>(result);
    }
    writer_done = true;
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(writer_accepted_every_update);
    REQUIRE(readers_consistent.load());
    REQUIRE(state.status().last_ingest_sequence == kUpdates);
}

const TopOfBookEvent& require_top_of_book(
    const MarketQuotes& read,
    std::string_view instrument
) {
    const auto& quote = read.quotes.at(InstrumentId(std::string(instrument)));
    REQUIRE(std::holds_alternative<TopOfBookEvent>(quote));
    return std::get<TopOfBookEvent>(quote);
}

void require_unavailable_quote(
    const MarketQuotes& read,
    std::string_view instrument,
    MarketStateUnavailableCode code
) {
    const auto& quote = read.quotes.at(InstrumentId(std::string(instrument)));
    REQUIRE(std::holds_alternative<MarketStateUnavailable>(quote));
    REQUIRE(std::get<MarketStateUnavailable>(quote).code == code);
    REQUIRE(!std::get<MarketStateUnavailable>(quote).message.empty());
}

void test_quotes_reads_several_instruments_together() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    const std::vector<InstrumentId> watched{
        InstrumentId("SPY"),
        InstrumentId("AAPL"),
        InstrumentId("MSFT"),
    };
    const MarketQuotes empty = state.quotes(watched);
    REQUIRE(empty.stream == MarketDataStreamId(std::string(kStream)));
    REQUIRE(!empty.market_time.has_value());
    REQUIRE(empty.quotes.size() == 3);
    require_unavailable_quote(empty, "AAPL", MarketStateUnavailableCode::NoTopOfBook);
    REQUIRE(state.quotes({}).quotes.empty());

    const TopOfBookEvent aapl = top_of_book(0, 1'000, "AAPL");
    const TopOfBookEvent spy = top_of_book(1, 1'500, "SPY");
    require_step(state.apply(require_valid(aapl)));
    require_step(state.apply(require_valid(spy)));
    const MarketQuotes read = state.quotes(watched);
    REQUIRE(read.market_time == Timestamp::from_nanoseconds(1'500));
    REQUIRE(require_top_of_book(read, "AAPL") == aapl);
    REQUIRE(require_top_of_book(read, "SPY") == spy);
    // Only the requested instruments are read, each with its own reason.
    require_unavailable_quote(read, "MSFT", MarketStateUnavailableCode::NoTopOfBook);
    REQUIRE(state.quotes({InstrumentId("SPY")}).quotes.size() == 1);

    state.suspend();
    state.resume();
    require_step(state.apply(require_valid(top_of_book(2, 1'600, "SPY"))));
    const MarketQuotes resumed = state.quotes(watched);
    require_unavailable_quote(resumed, "AAPL", MarketStateUnavailableCode::AwaitingFreshQuote);
    REQUIRE(require_top_of_book(resumed, "SPY").header.ingest_sequence == 2U);
    require_unavailable_quote(resumed, "MSFT", MarketStateUnavailableCode::NoTopOfBook);

    state.suspend();
    for (const auto& [instrument, quote] : state.quotes(watched).quotes) {
        REQUIRE(std::get<MarketStateUnavailable>(quote).code ==
            MarketStateUnavailableCode::SourceUnavailable);
    }
    state.resume();
    require_update_error(
        state.apply(require_valid(top_of_book(9, 1'700, "SPY"))),
        MarketStateUpdateErrorCode::ReplayRejected
    );
    for (const auto& [instrument, quote] : state.quotes(watched).quotes) {
        REQUIRE(std::get<MarketStateUnavailable>(quote).code ==
            MarketStateUnavailableCode::StreamInterrupted);
    }
}

// Every quote in one read, and the market time, come from the same instant:
// with only quotes applied, market time is the newer quote's receive time.
void test_concurrent_quote_reads_are_one_instant() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 1'000, "AAPL"))));
    require_step(state.apply(require_valid(top_of_book(1, 1'001, "SPY"))));
    constexpr std::uint64_t kUpdates = 2'000;
    const std::vector<InstrumentId> both{InstrumentId("AAPL"), InstrumentId("SPY")};

    std::atomic<bool> writer_done{false};
    std::atomic<bool> readers_consistent{true};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 3; ++reader) {
        readers.emplace_back([&] {
            while (!writer_done.load()) {
                const MarketQuotes read = state.quotes(both);
                const auto* aapl = std::get_if<TopOfBookEvent>(
                    &read.quotes.at(InstrumentId("AAPL"))
                );
                const auto* spy = std::get_if<TopOfBookEvent>(
                    &read.quotes.at(InstrumentId("SPY"))
                );
                if (aapl == nullptr || spy == nullptr || !read.market_time) {
                    readers_consistent = false;
                    return;
                }
                const auto newest = std::max(
                    aapl->header.receive_timestamp,
                    spy->header.receive_timestamp
                );
                const auto sequence_gap =
                    aapl->header.ingest_sequence > spy->header.ingest_sequence
                        ? aapl->header.ingest_sequence - spy->header.ingest_sequence
                        : spy->header.ingest_sequence - aapl->header.ingest_sequence;
                if (*read.market_time != newest || sequence_gap != 1) {
                    readers_consistent = false;
                    return;
                }
            }
        });
    }

    bool writer_accepted_every_update = true;
    for (std::uint64_t sequence = 2; sequence <= kUpdates; ++sequence) {
        const auto result = state.apply(require_valid(top_of_book(
            sequence,
            static_cast<std::int64_t>(1'000 + sequence),
            sequence % 2 == 0 ? "AAPL" : "SPY"
        )));
        writer_accepted_every_update = writer_accepted_every_update &&
            std::holds_alternative<MarketReplayStep>(result);
    }
    writer_done = true;
    for (auto& reader : readers) {
        reader.join();
    }

    REQUIRE(writer_accepted_every_update);
    REQUIRE(readers_consistent.load());
}

void test_suspension_makes_every_quote_unavailable() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 1'000))));
    require_quote(state.execution_quote(InstrumentId("AAPL")));

    state.suspend();
    state.suspend();
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::SourceUnavailable
    );
    require_unavailable(
        state.execution_quote(InstrumentId("SPY")),
        MarketStateUnavailableCode::SourceUnavailable
    );
    // Suspension is not corruption: the stream still reports accepting.
    REQUIRE(!state.status().interruption.has_value());
    REQUIRE(state.status().last_ingest_sequence == 0U);
}

void test_resume_requires_a_fresh_quote_per_instrument() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 1'000, "AAPL"))));
    require_step(state.apply(require_valid(top_of_book(1, 1'001, "SPY"))));
    state.suspend();
    // An event applied while suspended does not count as fresh.
    require_step(state.apply(require_valid(top_of_book(2, 1'002, "AAPL"))));
    state.resume();

    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::AwaitingFreshQuote
    );
    require_unavailable(
        state.execution_quote(InstrumentId("SPY")),
        MarketStateUnavailableCode::AwaitingFreshQuote
    );
    require_unavailable(
        state.execution_quote(InstrumentId("MSFT")),
        MarketStateUnavailableCode::NoTopOfBook
    );

    // A trade is not a quote.
    require_step(state.apply(trade(3, 1'003)));
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::AwaitingFreshQuote
    );

    require_step(state.apply(require_valid(top_of_book(4, 1'004, "AAPL"))));
    const ExecutionQuote aapl = require_quote(
        state.execution_quote(InstrumentId("AAPL"))
    );
    REQUIRE(aapl.top_of_book.header.ingest_sequence == 4U);
    require_unavailable(
        state.execution_quote(InstrumentId("SPY")),
        MarketStateUnavailableCode::AwaitingFreshQuote
    );

    // Every suspension starts a new epoch.
    state.suspend();
    state.resume();
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::AwaitingFreshQuote
    );
}

void test_interruption_outranks_suspension() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 1'000))));
    state.suspend();
    require_update_error(
        state.apply(require_valid(top_of_book(5, 1'001))),
        MarketStateUpdateErrorCode::ReplayRejected
    );
    require_unavailable(
        state.execution_quote(InstrumentId("AAPL")),
        MarketStateUnavailableCode::StreamInterrupted
    );
}

void test_advanced_time_ages_a_silent_quote() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    require_step(state.apply(require_valid(top_of_book(0, 1'000))));

    REQUIRE(!state.advance_time(Timestamp::from_nanoseconds(1'000)));
    REQUIRE(!state.advance_time(Timestamp::from_nanoseconds(9'000)));
    const ExecutionQuote quote = require_quote(
        state.execution_quote(InstrumentId("AAPL"))
    );
    REQUIRE(quote.market_time == Timestamp::from_nanoseconds(9'000));
    REQUIRE(quote.top_of_book.header.receive_timestamp ==
        Timestamp::from_nanoseconds(1'000));
    REQUIRE(state.status().market_time == Timestamp::from_nanoseconds(9'000));
    REQUIRE(state.status().last_ingest_sequence == 0U);

    const auto backward = state.advance_time(Timestamp::from_nanoseconds(8'999));
    REQUIRE(backward.has_value());
    REQUIRE(backward->code == MarketStateUpdateErrorCode::TimeMovedBackward);
    // A rejected advance changes nothing and does not interrupt.
    REQUIRE(state.status().market_time == Timestamp::from_nanoseconds(9'000));
    REQUIRE(!state.status().interruption.has_value());

    // An event received before the advanced time is a regression.
    require_update_error(
        state.apply(require_valid(top_of_book(1, 8'000))),
        MarketStateUpdateErrorCode::ReplayRejected
    );
    const auto interrupted = state.advance_time(Timestamp::from_nanoseconds(10'000));
    REQUIRE(interrupted.has_value());
    REQUIRE(interrupted->code == MarketStateUpdateErrorCode::StreamInterrupted);
}

void test_time_can_advance_before_any_event() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    REQUIRE(!state.advance_time(Timestamp::from_nanoseconds(500)));
    REQUIRE(state.status().market_time == Timestamp::from_nanoseconds(500));
    REQUIRE(!state.status().last_ingest_sequence.has_value());
    REQUIRE(state.advance_time(Timestamp::from_nanoseconds(-1)).has_value());
    require_step(state.apply(require_valid(top_of_book(0, 500))));
}

// A live stream continued by a new process or after a refused publication
// (ADR 0020): the jump is declared while suspended, only forward, and a
// fresh quote is required after it. Undeclared gaps still interrupt.
void test_a_declared_continuation_moves_only_forward_while_suspended() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    // Never while quotes are available.
    const auto available = state.continue_at(1'000);
    REQUIRE(available.has_value());
    REQUIRE(available->code == MarketStateUpdateErrorCode::ContinuationRefused);

    state.suspend();
    REQUIRE(!state.continue_at(1'000));
    state.resume();
    require_step(state.apply(require_valid(top_of_book(1'000, 100))));
    require_step(state.apply(require_valid(top_of_book(1'001, 200, "SPY"))));
    REQUIRE(state.status().last_ingest_sequence == std::optional<std::uint64_t>(1'001));

    // A new run of the same process jumps further; the quote from before
    // it is never executable after it.
    state.suspend();
    const auto backward = state.continue_at(1'001);
    REQUIRE(backward.has_value());
    REQUIRE(backward->code == MarketStateUpdateErrorCode::ContinuationRefused);
    REQUIRE(!state.continue_at(1'002));  // where it already was: no change
    REQUIRE(!state.continue_at(5'000));
    state.resume();
    require_unavailable(state.execution_quote(InstrumentId("AAPL")), MarketStateUnavailableCode::AwaitingFreshQuote);
    // The last accepted event cannot come back, even as a duplicate.
    require_update_error(state.apply(require_valid(top_of_book(1'001, 300, "SPY"))), MarketStateUpdateErrorCode::ReplayRejected);
}

// Codex's review of ADR 0020, finding 4: what a process began its stream
// with is kept, so a reset that later "catches up" past the durable history
// is still visible after the fact.
void test_the_first_accepted_sequence_is_kept_for_the_states_lifetime() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    REQUIRE(!state.status().first_ingest_sequence.has_value());
    state.suspend();
    REQUIRE(!state.continue_at(1'000));
    // A declared jump is not an accepted event.
    REQUIRE(!state.status().first_ingest_sequence.has_value());
    state.resume();
    require_step(state.apply(require_valid(top_of_book(1'000, 100))));
    require_step(state.apply(require_valid(top_of_book(1'001, 200))));
    state.suspend();
    REQUIRE(!state.continue_at(9'000));
    state.resume();
    require_step(state.apply(require_valid(top_of_book(9'000, 300))));
    const auto status = state.status();
    REQUIRE(status.first_ingest_sequence == std::optional<std::uint64_t>(1'000));
    REQUIRE(status.last_ingest_sequence == std::optional<std::uint64_t>(9'000));
    // A rejected event leaves it alone.
    require_update_error(state.apply(require_valid(top_of_book(9'005, 400))), MarketStateUpdateErrorCode::ReplayRejected);
    REQUIRE(state.status().first_ingest_sequence == std::optional<std::uint64_t>(1'000));
}

void test_an_undeclared_gap_after_a_continuation_still_interrupts() {
    MarketState state{MarketDataStreamId(std::string(kStream))};
    state.suspend();
    REQUIRE(!state.continue_at(7));
    state.resume();
    require_step(state.apply(require_valid(top_of_book(7, 100))));
    require_update_error(state.apply(require_valid(top_of_book(9, 200))), MarketStateUpdateErrorCode::ReplayRejected);
    REQUIRE(state.status().interruption.has_value());
    // An interrupted state stays interrupted; no continuation repairs it.
    state.suspend();
    const auto refused = state.continue_at(100);
    REQUIRE(refused.has_value());
    REQUIRE(refused->code == MarketStateUpdateErrorCode::StreamInterrupted);
}

void test_an_exhausted_stream_cannot_continue() {
    constexpr std::uint64_t kLast = std::numeric_limits<std::uint64_t>::max();
    MarketState state{MarketDataStreamId(std::string(kStream)), kLast};
    require_step(state.apply(require_valid(top_of_book(kLast, 100))));
    state.suspend();
    const auto refused = state.continue_at(kLast);
    REQUIRE(refused.has_value());
    REQUIRE(refused->code == MarketStateUpdateErrorCode::ContinuationRefused);
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "a declared continuation moves only forward while suspended",
            test_a_declared_continuation_moves_only_forward_while_suspended,
        },
        {
            "an undeclared gap after a continuation still interrupts",
            test_an_undeclared_gap_after_a_continuation_still_interrupts,
        },
        {"an exhausted stream cannot continue", test_an_exhausted_stream_cannot_continue},
        {"the first accepted sequence is kept for the state's lifetime", test_the_first_accepted_sequence_is_kept_for_the_states_lifetime},
        {"empty state has no quote", test_empty_state_has_no_quote_or_time},
        {
            "trade alone is not an execution quote",
            test_trade_alone_is_not_an_execution_quote,
        },
        {
            "quote is returned exactly with market time",
            test_quote_is_returned_exactly_with_market_time,
        },
        {
            "quote ages as market time advances",
            test_quote_ages_as_other_instruments_advance_market_time,
        },
        {
            "quality flags are preserved",
            test_quality_flags_are_preserved_for_risk_to_judge,
        },
        {
            "sequence gap interrupts every quote",
            test_sequence_gap_interrupts_every_quote_permanently,
        },
        {
            "receive time regression interrupts",
            test_receive_time_regression_interrupts_the_stream,
        },
        {
            "foreign stream interrupts",
            test_event_from_another_stream_interrupts_the_stream,
        },
        {
            "concurrent readers observe consistent snapshots",
            test_concurrent_readers_observe_consistent_snapshots,
        },
        {
            "quotes reads several instruments together",
            test_quotes_reads_several_instruments_together,
        },
        {
            "concurrent quote reads are one instant",
            test_concurrent_quote_reads_are_one_instant,
        },
        {
            "suspension makes every quote unavailable",
            test_suspension_makes_every_quote_unavailable,
        },
        {
            "resume requires a fresh quote per instrument",
            test_resume_requires_a_fresh_quote_per_instrument,
        },
        {
            "interruption outranks suspension",
            test_interruption_outranks_suspension,
        },
        {
            "advanced time ages a silent quote",
            test_advanced_time_ages_a_silent_quote,
        },
        {
            "time can advance before any event",
            test_time_can_advance_before_any_event,
        },
    });
}
