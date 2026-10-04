// Model deployments on the live Alpaca IEX source (ADR 0018), through the
// adapter's fake transport: no network, no credentials, no broker. The
// deployment decides on live quotes, orders only after its latency, never
// trades a quote from before a reconnect, and expires a signal the market
// clock leaves behind during silence.
#include "chronos/alpaca/alpaca_live_source.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/paper_trading/deployment_runtime.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "alpaca_test_support.hpp"
#include "deployment_test_support.hpp"
#include "market_data_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

using chronos::DecisionState;
using chronos::DeploymentState;
using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::Money;
using chronos::PostgresPaperOrderRepository;
using chronos::Quantity;
using chronos::Timestamp;
using chronos::alpaca_adapter::AlpacaCredentials;
using chronos::alpaca_adapter::AlpacaLiveOptions;
using chronos::alpaca_adapter::AlpacaLiveSource;
using chronos::alpaca_adapter::AlpacaSubscription;
using chronos::market_data::MarketDataNotifier;
using chronos::market_data::MarketDataSourceState;
using chronos::market_data::MarketEventGate;
using chronos::market_data::MarketStateFeed;
using chronos::market_data::MonotonicClock;
using chronos::market_data::MonotonicUtcClock;
using chronos::paper_trading::DeploymentCreated;
using chronos::paper_trading::DeploymentCreateRequest;
using chronos::paper_trading::DeploymentError;
using chronos::paper_trading::DeploymentHub;
using chronos::paper_trading::DeploymentManager;
using chronos::paper_trading::DeploymentRunCounters;
using chronos::paper_trading::DeploymentRuntimeOptions;
using chronos::paper_trading::DeploymentStatus;
using chronos::paper_trading::PaperTradingService;
using chronos::paper_trading::PaperTradingServiceConfig;
using chronos::test::eventually;
using chronos::test::FakeLink;
using chronos::test::FakeModel;
using chronos::test::FakeProvider;
using chronos::test::FakeTransport;
using chronos::test::ManualClock;
using chronos::test::predicted;
using chronos::strategy::PredictedClass;
using namespace std::chrono_literals;

std::string connection_string;

constexpr std::string_view kStream = "alpaca-iex-deployment-test";
// Local receipt starts at 2026-09-28T13:30:01Z.
constexpr std::int64_t kReceiptStart = 1'790'602'201'000'000'000;
constexpr std::string_view kConnected = R"([{"T":"success","msg":"connected"}])";
constexpr std::string_view kAuthenticated = R"([{"T":"success","msg":"authenticated"}])";
constexpr std::string_view kSubscribed =
    R"([{"T":"subscription","trades":["AAPL","SPY"],"quotes":["AAPL","SPY"],"bars":[],)"
    R"("updatedBars":[],"dailyBars":[],"statuses":[],"lulds":[],"corrections":["AAPL","SPY"],)"
    R"("cancelErrors":["AAPL","SPY"]}])";

std::string quote_frame(std::string_view ask) {
    return R"([{"T":"q","S":"AAPL","bx":"V","bp":227.30,"bs":300,"ax":"V","ap":)" + std::string(ask) +
        R"(,"as":100,"c":["R"],"z":"C","t":"2026-09-28T13:30:00.100000000Z"}])";
}

AlpacaCredentials fake_credentials() {
    // Obviously fake; no real value appears in any test.
    auto loaded = AlpacaCredentials::from_environment([](std::string_view name) {
        return std::optional<std::string>(
            name == "APCA_API_KEY_ID" ? "FAKEKEYID000000TEST" : "fake-secret-value-for-tests-only"
        );
    });
    REQUIRE(std::holds_alternative<AlpacaCredentials>(loaded));
    return std::get<AlpacaCredentials>(std::move(loaded));
}

AlpacaSubscription aapl_and_spy() {
    auto created = AlpacaSubscription::create({InstrumentId("AAPL"), InstrumentId("SPY")});
    REQUIRE(std::holds_alternative<AlpacaSubscription>(created));
    return std::get<AlpacaSubscription>(std::move(created));
}

class ScratchSchema final : public chronos::test::ScratchSchema {
public:
    ScratchSchema() : chronos::test::ScratchSchema(connection_string) {
        PostgresPaperOrderRepository(connection()).apply_migration(chronos::test::all_migrations_sql());
    }
};

// The server's wiring with the live source over a fake transport and a held
// manual clock: the gate, the hub, the service, and deployments.
class LiveDeployment final {
public:
    explicit LiveDeployment(const ScratchSchema& schema)
        : model_(std::make_shared<FakeModel>(Timestamp::from_nanoseconds(kReceiptStart - 1'000'000'000))),
          receive_clock_(clock_, Timestamp::from_nanoseconds(kReceiptStart)),
          market_(MarketDataStreamId(std::string(kStream))),
          notifier_(clock_, MarketDataNotifier::Options{
              .maximum_subscribers = 2,
              .minimum_interval = MonotonicClock::duration::zero(),
              .heartbeat_interval = 1h,
          }),
          hub_(nullptr),
          feed_(market_, notifier_, 0, &hub_, &gate_),
          service_(
              std::make_unique<PostgresPaperOrderRepository>(schema.connection()),
              market_,
              PaperTradingServiceConfig{
                  .tradeable_instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
                  .account_risk_limits = chronos::test::paper_risk_limits(),
              },
              {},
              &gate_
          ),
          manager_(
              service_,
              hub_,
              std::make_unique<FakeProvider>(model_),
              DeploymentRuntimeOptions{
                  .stream = MarketDataStreamId(std::string(kStream)),
                  .tradeable_instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
                  .account_risk_limits = chronos::test::paper_risk_limits(),
                  .model_load_timeout = 1'000ms,
                  .inference_timeout = 200ms,
                  .idle_wake = 5ms,
              }
          ) {
        service_.set_trading_status_view(&hub_);
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
            AlpacaLiveOptions{.clock_notice_interval = 1s},
            [](std::string_view) {}
        );
    }

    ~LiveDeployment() {
        manager_.shutdown();
        source_->stop();
    }

    LiveDeployment(const LiveDeployment&) = delete;
    LiveDeployment& operator=(const LiveDeployment&) = delete;

    void go_live(FakeLink& link) {
        const std::size_t sent = link.sent().size();
        link.push(std::string(kConnected));
        REQUIRE(link.await_sent(sent + 1));
        link.push(std::string(kAuthenticated));
        REQUIRE(link.await_sent(sent + 2));
        link.push(std::string(kSubscribed));
        REQUIRE(eventually([&] { return source_->status().state == MarketDataSourceState::Live; }));
    }

    // A quote received `after` the previous one.
    void quote(FakeLink& link, std::string_view ask, std::chrono::milliseconds after = 0ms) {
        clock_.advance_to(clock_.now() + after);
        const std::uint64_t applied = source_->status().events_applied;
        link.push(quote_frame(ask));
        REQUIRE(eventually([&] { return source_->status().events_applied == applied + 1; }));
    }

    void start() {
        DeploymentCreateRequest request{
            .deployment_id = "live-1",
            .account_id = "live-account",
            .starting_cash = Money::parse("10000"),
            .experiment_id = std::string(64, 'e'),
            .model = "logistic",
            .instrument = "AAPL",
            .threshold_ppb = 600'000'000,
            .target_quantity = Quantity::parse("1"),
            .decision_interval_ns = 1'000'000,
            .latency_ns = 50'000'000,
            .max_signal_age_ns = 3'000'000'000,
            .max_orders_per_run = 10,
            .max_consecutive_rejections = 3,
        };
        REQUIRE(std::holds_alternative<DeploymentCreated>(manager_.create(request)));
        REQUIRE(std::holds_alternative<DeploymentStatus>(manager_.start("live-1")));
    }

    DeploymentRunCounters counters() {
        auto found = manager_.get("live-1");
        REQUIRE(std::holds_alternative<DeploymentStatus>(found));
        return *std::get<DeploymentStatus>(found).run;
    }

    std::vector<chronos::DeploymentDecisionRecord> decisions() {
        auto page = manager_.decisions("live-1", 200, std::nullopt);
        return std::get<chronos::DecisionPage>(std::move(page)).decisions;
    }

    std::uint64_t waited(const std::string& reason) {
        const auto counted = counters();
        const auto found = counted.waits.find(reason);
        return found == counted.waits.end() ? 0 : found->second;
    }

    Quantity position() {
        return std::get<chronos::paper_trading::PaperAccountView>(service_.get_account("live-account"))
            .state.quantity(InstrumentId("AAPL"));
    }

    FakeLink& link() { return *first_link_; }
    FakeTransport& transport() { return *transport_; }
    ManualClock& clock() { return clock_; }
    AlpacaLiveSource& source() { return *source_; }
    FakeModel& model() { return *model_; }

private:
    std::shared_ptr<FakeModel> model_;
    ManualClock clock_;
    MonotonicUtcClock receive_clock_;
    chronos::MarketState market_;
    MarketDataNotifier notifier_;
    MarketEventGate gate_;
    DeploymentHub hub_;
    MarketStateFeed feed_;
    PaperTradingService service_;
    DeploymentManager manager_;
    FakeTransport* transport_{nullptr};
    std::shared_ptr<FakeLink> first_link_;
    std::unique_ptr<AlpacaLiveSource> source_;
};

void test_a_live_signal_orders_on_a_later_quote_after_its_latency() {
    const ScratchSchema schema;
    LiveDeployment live(schema);
    live.start();
    live.go_live(live.link());
    live.quote(live.link(), "227.36");
    REQUIRE(eventually([&] { return live.counters().order_decisions == 1; }));
    // 20 ms later: a later quote, but sooner than the 50 ms latency.
    live.quote(live.link(), "227.37", 20ms);
    REQUIRE(eventually([&] { return live.waited("awaiting_latency") > 0; }));
    REQUIRE(live.counters().orders_recorded == 0);
    live.quote(live.link(), "227.38", 40ms);
    REQUIRE(eventually([&] { return live.counters().orders_recorded == 1; }));
    const auto decision = live.decisions().front();
    REQUIRE(decision.state == DecisionState::Recorded);
    REQUIRE(decision.execution_ingest_sequence == std::optional<std::uint64_t>(2));
    REQUIRE(decision.execution_receive->nanoseconds() - decision.decision.observation_receive.nanoseconds() == 60'000'000);
    REQUIRE(live.position() == Quantity::parse("1"));
}

void test_a_reconnect_never_fills_a_quote_from_before_the_gap() {
    const ScratchSchema schema;
    LiveDeployment live(schema);
    live.start();
    live.go_live(live.link());
    live.quote(live.link(), "227.36");
    REQUIRE(eventually([&] { return live.counters().order_decisions == 1; }));
    // The connection drops before any eligible quote.
    live.link().close();
    REQUIRE(eventually([&] { return live.source().status().state == MarketDataSourceState::Reconnecting; }));
    REQUIRE(live.clock().await_waits(1));
    auto second = live.transport().accept();
    live.clock().advance_to(live.clock().now() + 1s);
    live.go_live(*second);
    // Connected again, the old quote is not executable, so the order waits.
    REQUIRE(eventually([&] { return live.waited("market_data_unavailable") > 0; }));
    REQUIRE(live.counters().orders_recorded == 0);
    live.quote(*second, "227.50", 10ms);
    REQUIRE(eventually([&] { return live.counters().orders_recorded == 1; }));
    const auto decision = live.decisions().front();
    // Executed on the fresh quote, received after the reconnect.
    REQUIRE(decision.execution_ingest_sequence == std::optional<std::uint64_t>(1));
    REQUIRE(decision.execution_receive->nanoseconds() > kReceiptStart + 1'000'000'000);
    REQUIRE(live.position() == Quantity::parse("1"));
}

void test_silence_expires_a_pending_signal() {
    const ScratchSchema schema;
    LiveDeployment live(schema);
    live.start();
    live.go_live(live.link());
    live.quote(live.link(), "227.36");
    REQUIRE(eventually([&] { return live.counters().order_decisions == 1; }));
    REQUIRE(live.link().await_drained());
    // No quote for four seconds: the clock advances past the signal's
    // three-second age, and the decision is abandoned, never filled late.
    live.clock().advance_to(live.clock().now() + 4s);
    live.link().push_timeout();
    REQUIRE(live.link().await_drained());
    REQUIRE(eventually([&] { return live.counters().abandoned == 1; }));
    REQUIRE(live.decisions().front().resolution == std::optional<std::string>("signal_expired"));
    REQUIRE(live.position() == Quantity::parse("0"));
}

}  // namespace

int main() {
    const char* configured = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured == nullptr || std::string_view(configured).empty()) {
        std::cout << "Alpaca deployment test skipped: CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured;
    return chronos::test::run_tests({
        {"a live signal orders on a later quote after its latency", test_a_live_signal_orders_on_a_later_quote_after_its_latency},
        {"a reconnect never fills a quote from before the gap", test_a_reconnect_never_fills_a_quote_from_before_the_gap},
        {"silence expires a pending signal", test_silence_expires_a_pending_signal},
    });
}
