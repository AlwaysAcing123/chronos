#include "chronos/alpaca/alpaca_live_source.hpp"
#include "alpaca_test_support.hpp"
#include "market_data_test_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::ExecutionQuote;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::MarketState;
using chronos::MarketStateUnavailable;
using chronos::MarketStateUnavailableCode;
using chronos::Price;
using chronos::Timestamp;
using chronos::alpaca_adapter::AlpacaCredentials;
using chronos::alpaca_adapter::AlpacaLiveOptions;
using chronos::alpaca_adapter::AlpacaLiveSource;
using chronos::alpaca_adapter::AlpacaSubscription;
using chronos::market_data::MarketDataMode;
using chronos::market_data::MarketDataNotifier;
using chronos::market_data::MarketDataSourceState;
using chronos::market_data::MarketStateFeed;
using chronos::market_data::MonotonicClock;
using chronos::market_data::MonotonicUtcClock;
using chronos::market_data::ReplayCommand;
using chronos::market_data::ReplayCommandError;
using chronos::market_data::ReplayCommandErrorCode;
using chronos::test::eventually;
using chronos::test::FakeLink;
using chronos::test::FakeTransport;
using chronos::test::ManualClock;
using namespace std::chrono_literals;

constexpr std::string_view kStream = "alpaca-iex-live-test";
// Local receipt starts at 2026-09-28T13:30:01Z.
constexpr std::int64_t kReceiptStart = 1'790'602'201'000'000'000;
// Obviously fake credentials; no real value appears in any test.
constexpr std::string_view kFakeKeyId = "FAKEKEYID000000TEST";
constexpr std::string_view kFakeSecret = "fake-secret-value-for-tests-only";

constexpr std::string_view kConnected = R"([{"T":"success","msg":"connected"}])";
constexpr std::string_view kAuthenticated = R"([{"T":"success","msg":"authenticated"}])";
constexpr std::string_view kSubscribed =
    R"([{"T":"subscription","trades":["AAPL","SPY"],"quotes":["AAPL","SPY"],"bars":[],)"
    R"("updatedBars":[],"dailyBars":[],"statuses":[],"lulds":[],"corrections":["AAPL","SPY"],)"
    R"("cancelErrors":["AAPL","SPY"]}])";
constexpr std::string_view kSubscribeRequest =
    R"({"action":"subscribe","trades":["AAPL","SPY"],"quotes":["AAPL","SPY"]})";

std::string quote_frame(
    std::string_view symbol = "AAPL",
    std::string_view bid = "227.34",
    std::string_view time = "2026-09-28T13:30:00.100000000Z"
) {
    return R"([{"T":"q","S":")" + std::string(symbol) + R"(","bx":"V","bp":)" +
        std::string(bid) + R"(,"bs":300,"ax":"V","ap":227.36,"as":100,"c":["R"],"z":"C","t":")" +
        std::string(time) + R"("}])";
}

std::string auth_request() {
    return R"({"action":"auth","key":")" + std::string(kFakeKeyId) + R"(","secret":")" +
        std::string(kFakeSecret) + R"("})";
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

AlpacaCredentials fake_credentials() {
    auto loaded = AlpacaCredentials::from_environment([](std::string_view name) {
        return std::optional<std::string>(std::string(
            name == "APCA_API_KEY_ID" ? kFakeKeyId : kFakeSecret
        ));
    });
    REQUIRE(std::holds_alternative<AlpacaCredentials>(loaded));
    return std::get<AlpacaCredentials>(std::move(loaded));
}

AlpacaSubscription aapl_and_spy() {
    auto created = AlpacaSubscription::create({InstrumentId("AAPL"), InstrumentId("SPY")});
    REQUIRE(std::holds_alternative<AlpacaSubscription>(created));
    return std::get<AlpacaSubscription>(std::move(created));
}

// A live source over a fake transport, a held manual clock, and a real
// market state, with every log line kept for inspection.
class LiveHarness final {
public:
    explicit LiveHarness(AlpacaLiveOptions options = {})
        : receive_clock_(clock_, Timestamp::from_nanoseconds(kReceiptStart)),
          notifier_(clock_, MarketDataNotifier::Options{
              .maximum_subscribers = 2,
              .minimum_interval = MonotonicClock::duration::zero(),
              .heartbeat_interval = 1h,
          }),
          feed_(market_, notifier_) {
        auto transport = std::make_unique<FakeTransport>();
        transport_ = transport.get();
        first_link_ = transport_->accept();
        source_ = std::make_unique<AlpacaLiveSource>(
            std::move(transport),
            fake_credentials(),
            aapl_and_spy(),
            feed_,
            clock_,
            receive_clock_,
            [](MonotonicClock::duration bound) { return bound; },
            options,
            [this](std::string_view line) {
                const std::lock_guard lock(log_mutex_);
                log_.emplace_back(line);
            }
        );
    }

    ~LiveHarness() { source_->stop(); }

    LiveHarness(const LiveHarness&) = delete;
    LiveHarness& operator=(const LiveHarness&) = delete;

    [[nodiscard]] AlpacaLiveSource& source() { return *source_; }
    [[nodiscard]] FakeTransport& transport() { return *transport_; }
    [[nodiscard]] FakeLink& link() { return *first_link_; }
    [[nodiscard]] ManualClock& clock() { return clock_; }
    [[nodiscard]] MarketState& market() { return market_; }
    [[nodiscard]] MarketDataNotifier& notifier() { return notifier_; }

    [[nodiscard]] bool reaches(MarketDataSourceState state) {
        return eventually([&] { return source_->status().state == state; });
    }

    // Completes the handshake on `link` and waits for Live.
    void go_live(FakeLink& link) {
        const std::size_t sent = link.sent().size();
        link.push(std::string(kConnected));
        REQUIRE(link.await_sent(sent + 1));
        link.push(std::string(kAuthenticated));
        REQUIRE(link.await_sent(sent + 2));
        link.push(std::string(kSubscribed));
        REQUIRE(reaches(MarketDataSourceState::Live));
    }

    [[nodiscard]] std::vector<std::string> log() const {
        const std::lock_guard lock(log_mutex_);
        return log_;
    }

    [[nodiscard]] std::string diagnostic() {
        return source_->status().diagnostic.value_or("");
    }

    [[nodiscard]] std::optional<MarketStateUnavailableCode> unavailable(std::string symbol) {
        const auto quote = market_.execution_quote(InstrumentId(std::move(symbol)));
        if (const auto* unavailable = std::get_if<MarketStateUnavailable>(&quote)) {
            return unavailable->code;
        }
        return std::nullopt;
    }

    [[nodiscard]] ExecutionQuote quote(std::string symbol) {
        const auto quote = market_.execution_quote(InstrumentId(std::move(symbol)));
        REQUIRE(std::holds_alternative<ExecutionQuote>(quote));
        return std::get<ExecutionQuote>(quote);
    }

    // No secret appears in any diagnostic or log line.
    void require_no_secret_leaked() {
        std::vector<std::string> texts = log();
        texts.push_back(diagnostic());
        for (const std::string& text : texts) {
            REQUIRE(!contains(text, kFakeKeyId));
            REQUIRE(!contains(text, kFakeSecret));
        }
    }

private:
    ManualClock clock_{ManualClock::Mode::Hold};
    MonotonicUtcClock receive_clock_;
    MarketState market_{MarketDataStreamId(std::string(kStream))};
    MarketDataNotifier notifier_;
    MarketStateFeed feed_;
    FakeTransport* transport_{nullptr};
    std::shared_ptr<FakeLink> first_link_;
    mutable std::mutex log_mutex_;
    std::vector<std::string> log_;
    std::unique_ptr<AlpacaLiveSource> source_;
};

void test_connects_authenticates_subscribes_and_goes_live() {
    LiveHarness harness;
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);

    harness.link().push(std::string(kConnected));
    REQUIRE(harness.link().await_sent(1));
    REQUIRE(harness.reaches(MarketDataSourceState::Authenticating));
    REQUIRE(harness.link().sent()[0] == auth_request());

    harness.link().push(std::string(kAuthenticated));
    REQUIRE(harness.link().await_sent(2));
    REQUIRE(harness.reaches(MarketDataSourceState::Subscribing));
    REQUIRE(harness.link().sent()[1] == kSubscribeRequest);
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);

    harness.link().push(std::string(kSubscribed));
    REQUIRE(harness.reaches(MarketDataSourceState::Live));
    // Connected and subscribed, but no quote yet.
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::NoTopOfBook);
    REQUIRE(!harness.source().status().diagnostic.has_value());

    harness.link().push(quote_frame());
    REQUIRE(eventually([&] { return !harness.unavailable("AAPL").has_value(); }));
    const ExecutionQuote quote = harness.quote("AAPL");
    REQUIRE(quote.top_of_book.bid->price == Price::parse("227.34"));
    REQUIRE(quote.top_of_book.header.ingest_sequence == 0U);
    REQUIRE(quote.top_of_book.header.receive_timestamp.nanoseconds() == kReceiptStart);
    REQUIRE(harness.source().status().events_applied == 1U);
    REQUIRE(harness.unavailable("SPY") == MarketStateUnavailableCode::NoTopOfBook);

    // Only the authentication and subscription frames were ever sent.
    REQUIRE(harness.link().sent().size() == 2U);
    REQUIRE(harness.transport().connects() == 1U);
    harness.require_no_secret_leaked();
}

void test_describes_itself_as_iex_only_and_refuses_replay_commands() {
    LiveHarness harness;
    const auto status = harness.source().status();
    REQUIRE(status.description.mode == MarketDataMode::Live);
    REQUIRE(status.description.provider == "Alpaca");
    REQUIRE(status.description.feed == "IEX");
    REQUIRE(status.description.label == "Alpaca IEX");
    REQUIRE(status.description.coverage_notice.has_value());
    REQUIRE(contains(*status.description.coverage_notice, "not consolidated SIP"));
    REQUIRE(contains(*status.description.coverage_notice, "not the NBBO"));
    REQUIRE(status.description.coverage_label == std::optional<std::string>("IEX only, not SIP or NBBO"));
    REQUIRE(!status.replay_speed.has_value());
    REQUIRE((status.description.instruments ==
        std::vector<InstrumentId>{InstrumentId("AAPL"), InstrumentId("SPY")}));

    const auto refused = harness.source().control_replay(ReplayCommand::Start);
    REQUIRE(std::holds_alternative<ReplayCommandError>(refused));
    REQUIRE(std::get<ReplayCommandError>(refused).code == ReplayCommandErrorCode::NotReplay);
}

void test_refuses_an_exhausted_ingest_sequence_before_connecting() {
    ManualClock clock(ManualClock::Mode::Hold);
    const MonotonicUtcClock receive_clock(
        clock,
        Timestamp::from_nanoseconds(kReceiptStart)
    );
    MarketState market(
        MarketDataStreamId(std::string(chronos::test::kSourceTestStream)),
        std::numeric_limits<std::uint64_t>::max()
    );
    const auto accepted = market.apply(chronos::test::recorded_quote(
        std::numeric_limits<std::uint64_t>::max(),
        0ns
    ));
    REQUIRE(std::holds_alternative<chronos::MarketReplayStep>(accepted));
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    MarketStateFeed feed(market, notifier);
    auto transport = std::make_unique<FakeTransport>();

    try {
        AlpacaLiveSource source(
            std::move(transport),
            fake_credentials(),
            aapl_and_spy(),
            feed,
            clock,
            receive_clock,
            [](MonotonicClock::duration bound) { return bound; }
        );
        REQUIRE(false);
    } catch (const std::overflow_error& error) {
        REQUIRE(std::string_view(error.what()).find("ingest sequence is exhausted") !=
            std::string_view::npos);
    }
}

void test_rejected_credentials_fail_without_reconnecting() {
    LiveHarness harness;
    harness.link().push(std::string(kConnected));
    REQUIRE(harness.link().await_sent(1));
    // Even an error that echoes a credential is redacted.
    harness.link().push(
        R"([{"T":"error","code":402,"msg":"auth failed for )" + std::string(kFakeKeyId) + R"("}])"
    );
    REQUIRE(harness.reaches(MarketDataSourceState::Failed));
    const std::string diagnostic = harness.diagnostic();
    REQUIRE(contains(diagnostic, "Alpaca error 402"));
    REQUIRE(contains(diagnostic, "APCA_API_KEY_ID"));
    REQUIRE(contains(diagnostic, "[REDACTED]"));
    REQUIRE(harness.transport().connects() == 1U);
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);
    harness.require_no_secret_leaked();
    REQUIRE(eventually([&] { return harness.link().released(); }));
}

void test_insufficient_subscription_is_fatal() {
    LiveHarness harness;
    harness.link().push(std::string(kConnected));
    REQUIRE(harness.link().await_sent(1));
    harness.link().push(R"([{"T":"error","code":409,"msg":"insufficient subscription"}])");
    REQUIRE(harness.reaches(MarketDataSourceState::Failed));
    REQUIRE(contains(harness.diagnostic(), "data plan does not include this feed"));
}

void test_greeting_timeout_reconnects_after_backoff() {
    LiveHarness harness;
    REQUIRE(harness.link().await_drained());
    harness.clock().advance_to(harness.clock().now() + 10s);
    harness.link().push_timeout();
    REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
    REQUIRE(contains(
        harness.diagnostic(),
        "timed out after 10.000 s waiting for Alpaca's connection greeting; "
        "reconnecting in 1.000 s"
    ));
    REQUIRE(harness.clock().await_waits(1));
    REQUIRE(harness.clock().deadlines().back() == harness.clock().now() + 1s);
    REQUIRE(eventually([&] { return harness.link().released(); }));

    // An idle poll before the deadline is not a timeout.
    auto second = harness.transport().accept();
    harness.clock().advance_to(harness.clock().now() + 1s);
    REQUIRE(harness.transport().await_connect_attempts(2));
    REQUIRE(second->await_drained());
    harness.clock().advance_to(harness.clock().now() + 9s);
    second->push_timeout();
    REQUIRE(second->await_drained());
    REQUIRE(harness.source().status().state == MarketDataSourceState::Connecting);
    harness.go_live(*second);
}

void test_authentication_and_subscription_timeouts_are_transient() {
    {
        LiveHarness harness;
        harness.link().push(std::string(kConnected));
        REQUIRE(harness.link().await_sent(1));
        // The deadline is set once the source is reading again.
        REQUIRE(harness.link().await_drained());
        harness.clock().advance_to(harness.clock().now() + 10s);
        harness.link().push_timeout();
        REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
        REQUIRE(contains(harness.diagnostic(), "waiting for authentication"));
    }
    {
        LiveHarness harness;
        harness.link().push(std::string(kConnected));
        REQUIRE(harness.link().await_sent(1));
        harness.link().push(std::string(kAuthenticated));
        REQUIRE(harness.link().await_sent(2));
        REQUIRE(harness.link().await_drained());
        harness.clock().advance_to(harness.clock().now() + 10s);
        harness.link().push_timeout();
        REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
        REQUIRE(contains(harness.diagnostic(), "waiting for the subscription acknowledgement"));
        REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);
    }
}

void test_connection_failures_back_off_exponentially_to_the_bound() {
    LiveHarness harness(AlpacaLiveOptions{
        .backoff = {.initial = 1s, .maximum = 4s},
    });
    // The first connection closes before its greeting.
    harness.link().close("connection reset");
    REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
    REQUIRE(contains(harness.diagnostic(), "connection reset"));

    const std::vector<MonotonicClock::duration> expected{1s, 2s, 4s, 4s};
    for (std::size_t attempt = 0; attempt < expected.size(); ++attempt) {
        REQUIRE(harness.clock().await_waits(attempt + 1));
        const auto deadline = harness.clock().deadlines().back();
        REQUIRE(deadline == harness.clock().now() + expected[attempt]);
        harness.transport().refuse("Could not establish connection");
        harness.clock().advance_to(deadline);
        REQUIRE(harness.transport().await_connect_attempts(attempt + 2));
    }
    REQUIRE(harness.clock().await_waits(expected.size() + 1));
    REQUIRE(contains(harness.diagnostic(), "could not connect to Alpaca: Could not establish connection"));
    REQUIRE(contains(harness.diagnostic(), "reconnecting in 4.000 s"));
}

void test_disconnect_blocks_stale_quotes_until_fresh_ones_arrive() {
    LiveHarness harness;
    harness.go_live(harness.link());
    harness.link().push(quote_frame("AAPL"));
    harness.link().push(quote_frame("SPY"));
    REQUIRE(eventually([&] { return harness.source().status().events_applied == 2U; }));
    REQUIRE(!harness.unavailable("AAPL").has_value());
    REQUIRE(!harness.unavailable("SPY").has_value());

    harness.link().close();
    REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
    // The old quotes are never executable while disconnected.
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);
    REQUIRE(harness.unavailable("SPY") == MarketStateUnavailableCode::SourceUnavailable);
    REQUIRE(contains(harness.diagnostic(), "the connection closed: closed by peer"));
    // A connection that reached Live starts the backoff over.
    REQUIRE(harness.clock().await_waits(1));
    REQUIRE(harness.clock().deadlines().back() == harness.clock().now() + 1s);

    auto second = harness.transport().accept();
    harness.clock().advance_to(harness.clock().now() + 1s);
    harness.go_live(*second);
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::AwaitingFreshQuote);
    REQUIRE(harness.unavailable("SPY") == MarketStateUnavailableCode::AwaitingFreshQuote);

    second->push(quote_frame("AAPL", "227.40", "2026-09-28T13:30:01.500000000Z"));
    REQUIRE(eventually([&] { return !harness.unavailable("AAPL").has_value(); }));
    const ExecutionQuote fresh = harness.quote("AAPL");
    REQUIRE(fresh.top_of_book.bid->price == Price::parse("227.40"));
    // One ingest sequence across reconnects.
    REQUIRE(fresh.top_of_book.header.ingest_sequence == 2U);
    REQUIRE(harness.unavailable("SPY") == MarketStateUnavailableCode::AwaitingFreshQuote);
    REQUIRE(harness.transport().connects() == 2U);
    REQUIRE(harness.link().released());
}

void test_silence_advances_market_time_so_quotes_age() {
    LiveHarness harness(AlpacaLiveOptions{.clock_notice_interval = 1s});
    harness.go_live(harness.link());
    harness.link().push(quote_frame());
    REQUIRE(eventually([&] { return harness.source().status().events_applied == 1U; }));
    REQUIRE(harness.link().await_drained());
    const std::uint64_t revision = harness.notifier().revision();

    harness.clock().advance_to(harness.clock().now() + 6s);
    harness.link().push_timeout();
    REQUIRE(harness.link().await_drained());
    const ExecutionQuote aged = harness.quote("AAPL");
    REQUIRE(aged.market_time.nanoseconds() == kReceiptStart + 6'000'000'000);
    REQUIRE(aged.market_time.nanoseconds() -
        aged.top_of_book.header.receive_timestamp.nanoseconds() == 6'000'000'000);
    REQUIRE(harness.notifier().revision() == revision + 1);

    // Polls within the notice interval advance time without notifying.
    harness.clock().advance_to(harness.clock().now() + 250ms);
    harness.link().push_timeout();
    REQUIRE(harness.link().await_drained());
    REQUIRE(harness.quote("AAPL").market_time.nanoseconds() == kReceiptStart + 6'250'000'000);
    REQUIRE(harness.notifier().revision() == revision + 1);
}

void test_frames_apply_in_order_with_contiguous_sequences() {
    LiveHarness harness;
    harness.go_live(harness.link());
    harness.link().push(
        R"([{"T":"q","S":"AAPL","bx":"V","bp":227.34,"bs":300,"ax":"V","ap":227.36,"as":100,)"
        R"("t":"2026-09-28T13:30:00.1Z"},)"
        R"({"T":"c","S":"AAPL","x":"V","oi":1,"op":1,"os":1,"ci":2,"cp":2,"cs":2,"t":"2026-09-28T13:30:00Z"},)"
        R"({"T":"t","S":"SPY","i":7,"x":"V","p":655.1,"s":5,"t":"2026-09-28T13:30:00.2Z"},)"
        R"({"T":"q","S":"AAPL","bx":"V","bp":227.35,"bs":200,"ax":"V","ap":227.36,"as":100,)"
        R"("t":"2026-09-28T13:30:00.3Z"}])"
    );
    REQUIRE(eventually([&] { return harness.source().status().events_applied == 3U; }));
    const ExecutionQuote latest = harness.quote("AAPL");
    REQUIRE(latest.top_of_book.bid->price == Price::parse("227.35"));
    REQUIRE(latest.top_of_book.header.ingest_sequence == 2U);
    REQUIRE(harness.market().status().last_ingest_sequence == 2U);
    REQUIRE(harness.source().status().state == MarketDataSourceState::Live);
}

void test_malformed_data_fails_closed_and_is_never_applied() {
    LiveHarness harness;
    harness.go_live(harness.link());
    harness.link().push(quote_frame());
    REQUIRE(eventually([&] { return harness.source().status().events_applied == 1U; }));
    // The second message of this frame is inexact; neither is applied.
    harness.link().push(
        R"([{"T":"q","S":"AAPL","bx":"V","bp":227.30,"bs":1,"ax":"V","ap":227.36,"as":1,)"
        R"("t":"2026-09-28T13:30:00.2Z"},)"
        R"({"T":"q","S":"AAPL","bx":"V","bp":227.3400000001,"bs":1,"ax":"V","ap":227.36,)"
        R"("as":1,"t":"2026-09-28T13:30:00.3Z"}])"
    );
    REQUIRE(harness.reaches(MarketDataSourceState::Failed));
    REQUIRE(contains(harness.diagnostic(), "violates the Chronos market-data contract"));
    REQUIRE(contains(harness.diagnostic(), "is not exact at nine decimal places"));
    REQUIRE(harness.source().status().events_applied == 1U);
    REQUIRE(harness.market().status().last_ingest_sequence == 0U);
    REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);
    REQUIRE(harness.transport().connects() == 1U);
    // The log line follows the status change.
    REQUIRE(eventually([&] {
        const auto log = harness.log();
        return !log.empty() && contains(log.back(), "Alpaca IEX failed after 1 events");
    }));
}

void test_protocol_violations_fail_closed() {
    struct Case {
        std::string frame;
        std::string_view diagnostic;
    };
    const std::vector<Case> live_cases{
        {R"([{"T":"n","S":"AAPL"}])", "unknown type \"n\""},
        {"{not json", "cannot decode"},
        {R"([{"T":"success","msg":"authenticated"}])", "unexpected success message while live"},
        {R"([{"T":"error","code":400,"msg":"invalid syntax"},{"T":"c","S":"AAPL"}])",
         "together with other messages"},
        {quote_frame("MSFT"), "the symbol is not subscribed"},
        {R"([{"T":"error","code":999,"msg":"new"}])", "not documented"},
    };
    for (const Case& violation : live_cases) {
        LiveHarness harness;
        harness.go_live(harness.link());
        harness.link().push(violation.frame);
        REQUIRE(harness.reaches(MarketDataSourceState::Failed));
        REQUIRE(contains(harness.diagnostic(), violation.diagnostic));
        REQUIRE(harness.transport().connects() == 1U);
    }
    {
        LiveHarness harness;
        harness.go_live(harness.link());
        harness.link().push_binary();
        REQUIRE(harness.reaches(MarketDataSourceState::Failed));
        REQUIRE(contains(harness.diagnostic(), "binary frame"));
    }
    {
        // Data before the subscription is acknowledged.
        LiveHarness harness;
        harness.link().push(std::string(kConnected));
        REQUIRE(harness.link().await_sent(1));
        harness.link().push(quote_frame());
        REQUIRE(harness.reaches(MarketDataSourceState::Failed));
        REQUIRE(contains(harness.diagnostic(), "control messages must arrive alone and before data"));
    }
    {
        // A greeting other than "connected".
        LiveHarness harness;
        harness.link().push(std::string(kAuthenticated));
        REQUIRE(harness.reaches(MarketDataSourceState::Failed));
        REQUIRE(contains(harness.diagnostic(), "unexpected success \"authenticated\""));
    }
    {
        // An acknowledgement that does not match the request.
        LiveHarness harness;
        harness.link().push(std::string(kConnected));
        REQUIRE(harness.link().await_sent(1));
        harness.link().push(std::string(kAuthenticated));
        REQUIRE(harness.link().await_sent(2));
        harness.link().push(R"([{"T":"subscription","trades":["AAPL"],"quotes":["AAPL","SPY"]}])");
        REQUIRE(harness.reaches(MarketDataSourceState::Failed));
        REQUIRE(contains(harness.diagnostic(), "did not subscribe trades for SPY"));
    }
}

void test_transient_alpaca_errors_reconnect() {
    {
        LiveHarness harness;
        harness.link().push(std::string(kConnected));
        REQUIRE(harness.link().await_sent(1));
        harness.link().push(R"([{"T":"error","code":406,"msg":"connection limit exceeded"}])");
        REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
        REQUIRE(contains(harness.diagnostic(), "Alpaca error 406 (connection limit exceeded)"));
    }
    {
        LiveHarness harness;
        harness.go_live(harness.link());
        harness.link().push(R"([{"T":"error","code":407,"msg":"slow client"}])");
        REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
        REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);
    }
}

void test_shutdown_during_connection_is_prompt() {
    ManualClock clock(ManualClock::Mode::Hold);
    const MonotonicUtcClock receive_clock(clock, Timestamp::from_nanoseconds(kReceiptStart));
    MarketState market{MarketDataStreamId(std::string(kStream))};
    MarketDataNotifier notifier(clock, MarketDataNotifier::Options{});
    MarketStateFeed feed(market, notifier);
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport& fake = *transport;
    AlpacaLiveSource source(
        std::move(transport),
        fake_credentials(),
        aapl_and_spy(),
        feed,
        clock,
        receive_clock,
        [](MonotonicClock::duration bound) { return bound; }
    );
    // connect() is blocked: nothing is scripted.
    REQUIRE(fake.await_connect_attempts(1));
    REQUIRE(source.status().state == MarketDataSourceState::Connecting);
    source.stop();
    REQUIRE(source.status().state == MarketDataSourceState::Stopped);
    source.stop();
    REQUIRE(fake.connects() == 0U);
}

void test_shutdown_during_backoff_and_while_live_is_prompt() {
    {
        LiveHarness harness;
        harness.link().close();
        REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
        REQUIRE(harness.clock().await_waits(1));
        // Stop without advancing the clock to the backoff deadline.
        harness.source().stop();
        REQUIRE(harness.source().status().state == MarketDataSourceState::Stopped);
        REQUIRE(harness.transport().connects() == 1U);
    }
    {
        LiveHarness harness;
        harness.go_live(harness.link());
        harness.link().push(quote_frame());
        REQUIRE(eventually([&] { return harness.source().status().events_applied == 1U; }));
        std::thread first([&] { harness.source().stop(); });
        std::thread second([&] { harness.source().stop(); });
        first.join();
        second.join();
        REQUIRE(harness.source().status().state == MarketDataSourceState::Stopped);
        REQUIRE(harness.unavailable("AAPL") == MarketStateUnavailableCode::SourceUnavailable);
        REQUIRE(harness.link().released());
    }
}

void test_logs_every_transition_without_secrets() {
    LiveHarness harness;
    harness.go_live(harness.link());
    harness.link().close();
    REQUIRE(harness.reaches(MarketDataSourceState::Reconnecting));
    harness.source().stop();
    const auto log = harness.log();
    const std::vector<std::string_view> expected{
        "Alpaca IEX connecting: connecting to Alpaca IEX",
        "Alpaca IEX authenticating: authenticating with Alpaca",
        "Alpaca IEX subscribing: subscribing to IEX trades and quotes for 2 symbols",
        "Alpaca IEX live",
        "Alpaca IEX reconnecting: the connection closed: closed by peer; reconnecting in 1.000 s",
        "Alpaca IEX stopped",
    };
    REQUIRE(log.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE(log[index] == expected[index]);
    }
    harness.require_no_secret_leaked();
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {"connects, authenticates, subscribes, and goes live",
         test_connects_authenticates_subscribes_and_goes_live},
        {"describes itself as IEX only and refuses replay commands",
         test_describes_itself_as_iex_only_and_refuses_replay_commands},
        {"refuses an exhausted ingest sequence before connecting",
         test_refuses_an_exhausted_ingest_sequence_before_connecting},
        {"rejected credentials fail without reconnecting",
         test_rejected_credentials_fail_without_reconnecting},
        {"insufficient subscription is fatal", test_insufficient_subscription_is_fatal},
        {"greeting timeout reconnects after backoff", test_greeting_timeout_reconnects_after_backoff},
        {"authentication and subscription timeouts are transient",
         test_authentication_and_subscription_timeouts_are_transient},
        {"connection failures back off exponentially to the bound",
         test_connection_failures_back_off_exponentially_to_the_bound},
        {"disconnect blocks stale quotes until fresh ones arrive",
         test_disconnect_blocks_stale_quotes_until_fresh_ones_arrive},
        {"silence advances market time so quotes age", test_silence_advances_market_time_so_quotes_age},
        {"frames apply in order with contiguous sequences",
         test_frames_apply_in_order_with_contiguous_sequences},
        {"malformed data fails closed and is never applied",
         test_malformed_data_fails_closed_and_is_never_applied},
        {"protocol violations fail closed", test_protocol_violations_fail_closed},
        {"transient Alpaca errors reconnect", test_transient_alpaca_errors_reconnect},
        {"shutdown during connection is prompt", test_shutdown_during_connection_is_prompt},
        {"shutdown during backoff and while live is prompt",
         test_shutdown_during_backoff_and_while_live_is_prompt},
        {"logs every transition without secrets", test_logs_every_transition_without_secrets},
    });
}
