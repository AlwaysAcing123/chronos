#include "chronos/market_state.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
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

}  // namespace

int main() {
    return chronos::test::run_tests({
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
