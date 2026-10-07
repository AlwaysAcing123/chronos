// A live stream continued across server restarts (ADR 0020): the server's
// wiring with the real Alpaca source over its fake transport, the real
// PostgreSQL repository, matcher, service, deployments, and durable
// sequences, rebuilt over the same database to restart. No network,
// credentials, or broker. Clocks move only when a test moves them.
//
// The first test is the 2026-10-05 acceptance's failure: an account that
// last executed on sequence 1087 must be valued and traded on the first
// fresh quote after a restart. On the starting tree that quote was
// sequence 0, valuation reported "the quote does not continue the
// instrument's market-data timeline", and the sell was refused.
#include "chronos/alpaca/alpaca_live_source.hpp"
#include "chronos/market_data/leased_ingest_sequences.hpp"
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/paper_trading/deployment_runtime.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "chronos/paper_trading/working_order_matcher.hpp"
#include "chronos/postgresql_live_stream_sequences.hpp"
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
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using chronos::BookLevel;
using chronos::DecisionState;
using chronos::DeploymentState;
using chronos::ExecutionQuote;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEventHeader;
using chronos::MarketState;
using chronos::MarketStateUnavailable;
using chronos::MarketStateUnavailableCode;
using chronos::Money;
using chronos::PortfolioValuationErrorCode;
using chronos::PostgresIngestSequenceLeaseStore;
using chronos::PostgresPaperOrderRepository;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedExecutionError;
using chronos::SimulatedExecutionErrorCode;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::VenueId;
using chronos::alpaca_adapter::AlpacaCredentials;
using chronos::alpaca_adapter::AlpacaLiveOptions;
using chronos::alpaca_adapter::AlpacaLiveSource;
using chronos::alpaca_adapter::AlpacaSubscription;
using chronos::market_data::IngestSequenceLeaseStore;
using chronos::market_data::LeasedIngestSequences;
using chronos::market_data::LeasedIngestSequencesOptions;
using chronos::market_data::MarketDataNotifier;
using chronos::market_data::MarketDataSourceState;
using chronos::market_data::MarketEventGate;
using chronos::market_data::MarketStateFeed;
using chronos::market_data::MonotonicClock;
using chronos::market_data::MonotonicUtcClock;
using chronos::market_data::StreamWriter;
using chronos::paper_trading::DeploymentCreated;
using chronos::paper_trading::DeploymentCreateRequest;
using chronos::paper_trading::DeploymentHub;
using chronos::paper_trading::DeploymentManager;
using chronos::paper_trading::DeploymentRuntimeOptions;
using chronos::paper_trading::DeploymentStatus;
using chronos::paper_trading::PaperAccountView;
using chronos::paper_trading::PaperPortfolioView;
using chronos::paper_trading::PaperTradingError;
using chronos::paper_trading::PaperTradingErrorCode;
using chronos::paper_trading::PaperTradingService;
using chronos::paper_trading::PaperTradingServiceConfig;
using chronos::paper_trading::RecordedPaperOrder;
using chronos::paper_trading::WorkingOrderMatcher;
using chronos::paper_trading::WorkingOrderMatcherOptions;
using chronos::strategy::PredictedClass;
using chronos::test::eventually;
using chronos::test::FakeLink;
using chronos::test::FakeModel;
using chronos::test::FakeProvider;
using chronos::test::FakeTransport;
using chronos::test::ManualClock;
using chronos::test::PostgresTestConnection;
using chronos::test::predicted;
using namespace std::chrono_literals;

std::string connection_string;

constexpr std::string_view kStream = "alpaca-iex-restart-test";
constexpr std::string_view kAccount = "live-paper-restart";
constexpr std::uint64_t kBlock = std::uint64_t{1} << 20U;
constexpr std::uint64_t kLast = std::numeric_limits<std::uint64_t>::max();
// The acceptance account's last execution quote: sequence 1087, received
// 2026-10-05T16:21:19.925765166Z.
constexpr std::uint64_t kEarlierSequence = 1087;
constexpr std::int64_t kEarlierReceipt = 1'791'217'279'925'765'166;
// The restarted process's clock starts at 2026-10-05T16:33:29.259857708Z,
// the receipt of its first quote in the acceptance evidence.
constexpr std::int64_t kRestartReceipt = 1'791'218'009'259'857'708;

constexpr std::string_view kConnected = R"([{"T":"success","msg":"connected"}])";
constexpr std::string_view kAuthenticated = R"([{"T":"success","msg":"authenticated"}])";
constexpr std::string_view kSubscribed =
    R"([{"T":"subscription","trades":["AAPL","SPY"],"quotes":["AAPL","SPY"],"bars":[],)"
    R"("updatedBars":[],"dailyBars":[],"statuses":[],"lulds":[],"corrections":["AAPL","SPY"],)"
    R"("cancelErrors":["AAPL","SPY"]}])";
// The first quote after the restart, as in the acceptance evidence.
constexpr std::string_view kFreshQuote =
    R"([{"T":"q","S":"AAPL","bx":"V","bp":334.24,"bs":80,"ax":"V","ap":334.28,"as":40,)"
    R"("c":["R"],"z":"C","t":"2026-10-05T16:33:29.247797471Z"}])";

std::string quote_frame(std::string_view symbol, std::string_view bid, std::string_view ask) {
    return R"([{"T":"q","S":")" + std::string(symbol) + R"(","bx":"V","bp":)" + std::string(bid) +
        R"(,"bs":100,"ax":"V","ap":)" + std::string(ask) + R"(,"as":100,"c":["R"],"z":"C",)"
        R"("t":"2026-10-05T16:00:00.000000000Z"}])";
}

// Two quotes in one frame, numbered together.
std::string both_frame(std::string_view aapl_bid, std::string_view spy_bid) {
    return R"([{"T":"q","S":"AAPL","bx":"V","bp":)" + std::string(aapl_bid) +
        R"(,"bs":100,"ax":"V","ap":335.00,"as":100,"c":["R"],"z":"C","t":"2026-10-05T16:00:00.000000000Z"},)"
        R"({"T":"q","S":"SPY","bx":"V","bp":)" + std::string(spy_bid) +
        R"(,"bs":100,"ax":"V","ap":670.00,"as":100,"c":["R"],"z":"C","t":"2026-10-05T16:00:00.000000000Z"}])";
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

PaperTradingServiceConfig service_config() {
    return PaperTradingServiceConfig{
        .tradeable_instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
        .account_risk_limits = chronos::test::paper_risk_limits(),
    };
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

class ScratchSchema final : public chronos::test::ScratchSchema {
public:
    explicit ScratchSchema(bool through_005_only = false) : chronos::test::ScratchSchema(connection_string) {
        PostgresPaperOrderRepository store(connection());
        if (!through_005_only) {
            store.apply_migration(chronos::test::all_migrations_sql());
            return;
        }
        for (const char* file : {"001_paper_trading.sql", "002_paper_order_read_indexes.sql",
                                 "003_multi_instrument_accounts.sql", "004_working_orders.sql",
                                 "005_model_deployments.sql"}) {
            store.apply_migration(chronos::test::migration_sql(file));
        }
    }

    void upgrade() const {
        PostgresPaperOrderRepository store(connection());
        store.apply_migration(chronos::test::migration_sql("006_live_stream_sequences.sql"));
        store.apply_migration(chronos::test::migration_sql("007_live_stream_writer_fence.sql"));
    }
};

// The durable store, except that a crashed process's release never
// arrives: the lease stays until it expires.
class CrashableStore final : public IngestSequenceLeaseStore {
public:
    explicit CrashableStore(IngestSequenceLeaseStore& store) : store_(store) {}

    void crash() { crashed_ = true; }

    chronos::market_data::LeaseAcquisition acquire(
        const MarketDataStreamId& stream,
        const StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size,
        bool adopt_history
    ) override {
        return store_.acquire(stream, writer, lease, block_size, adopt_history);
    }

    chronos::market_data::LeaseRenewal renew(
        const MarketDataStreamId& stream,
        const StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size
    ) override {
        return store_.renew(stream, writer, lease, block_size);
    }

    void release(const MarketDataStreamId& stream, const StreamWriter& writer) override {
        if (crashed_) {
            throw std::runtime_error("the process was killed");
        }
        store_.release(stream, writer);
    }

private:
    IngestSequenceLeaseStore& store_;
    bool crashed_{false};
};

struct ProcessOptions {
    std::string name{"process"};
    std::int64_t receipt_start{kRestartReceipt};
    std::string stream{std::string(kStream)};
    bool adopt_history{false};
    // Another database than the scratch schema, such as one that is down.
    std::optional<std::string> sequence_database{};
};

// One server process: the server's wiring, over the shared database.
class ServerProcess final {
public:
    ServerProcess(const ScratchSchema& schema, std::shared_ptr<FakeModel> model, ProcessOptions options = {})
        : options_(std::move(options)),
          model_(std::move(model)),
          receive_clock_(clock_, Timestamp::from_nanoseconds(options_.receipt_start)),
          market_(MarketDataStreamId(options_.stream)),
          notifier_(clock_, MarketDataNotifier::Options{
              .maximum_subscribers = 2,
              .minimum_interval = MonotonicClock::duration::zero(),
              .heartbeat_interval = 1h,
          }),
          matcher_(
              std::make_unique<PostgresPaperOrderRepository>(schema.connection(), std::chrono::milliseconds(1'000)),
              WorkingOrderMatcherOptions{}
          ),
          hub_(&matcher_),
          feed_(market_, notifier_, 0, &hub_, &gate_),
          service_(
              std::make_unique<PostgresPaperOrderRepository>(schema.connection()),
              market_,
              service_config(),
              {},
              &gate_
          ),
          manager_(
              service_,
              hub_,
              std::make_unique<FakeProvider>(model_),
              DeploymentRuntimeOptions{
                  .stream = MarketDataStreamId(options_.stream),
                  .tradeable_instruments = {InstrumentId("AAPL"), InstrumentId("SPY")},
                  .account_risk_limits = chronos::test::paper_risk_limits(),
                  .model_load_timeout = 1'000ms,
                  .inference_timeout = 200ms,
                  .idle_wake = 5ms,
                  .source_status = [this] { return source_->status(); },
              }
          ),
          database_store_(options_.sequence_database.value_or(schema.connection())),
          store_(database_store_),
          sequences_(
              MarketDataStreamId(options_.stream),
              store_,
              StreamWriter{.id = options_.name + "-" + chronos::test::unique_test_id("w"), .label = options_.name},
              sequence_clock_,
              LeasedIngestSequencesOptions{.adopt_history = options_.adopt_history},
              [this](std::string_view line) { log_.emplace_back(line); }
          ) {
        service_.set_trading_status_view(&hub_);
        manager_.recover();
        sequences_.start();
        auto transport = std::make_unique<FakeTransport>();
        transport_ = transport.get();
        link_ = transport_->accept();
        source_ = std::make_unique<AlpacaLiveSource>(
            std::move(transport),
            fake_credentials(),
            aapl_and_spy(),
            feed_,
            sequences_,
            clock_,
            receive_clock_,
            [](MonotonicClock::duration bound) { return bound; },
            AlpacaLiveOptions{.clock_notice_interval = 1s},
            [](std::string_view) {}
        );
    }

    // The server's shutdown order: deployments, matching, the source and
    // its thread, then the lease.
    ~ServerProcess() {
        manager_.shutdown();
        matcher_.stop();
        source_->stop();
        sequences_.close();
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // Ends like a killed process: nothing it reserved is released.
    void crash_on_exit() { store_.crash(); }

    void go_live() { go_live(*link_); }

    void go_live(FakeLink& link) {
        const std::size_t sent = link.sent().size();
        link.push(std::string(kConnected));
        REQUIRE(link.await_sent(sent + 1));
        link.push(std::string(kAuthenticated));
        REQUIRE(link.await_sent(sent + 2));
        link.push(std::string(kSubscribed));
        REQUIRE(eventually([&] { return source_->status().state == MarketDataSourceState::Live; }));
    }

    // One frame received `after` the previous one; waits until all its
    // events are applied.
    void receive(std::string_view frame, std::size_t events = 1, std::chrono::milliseconds after = 0ms) {
        receive(*link_, frame, events, after);
    }

    void receive(FakeLink& link, std::string_view frame, std::size_t events, std::chrono::milliseconds after) {
        clock_.advance_to(clock_.now() + after);
        const std::uint64_t applied = source_->status().events_applied;
        link.push(std::string(frame));
        REQUIRE(eventually([&] { return source_->status().events_applied == applied + events; }));
    }

    // Waits for the lease, then for the source's next attempt.
    void reconnect_once_ready() {
        REQUIRE(eventually([&] { return !sequences_.readiness().has_value(); }));
        REQUIRE(clock_.await_waits(1));
        clock_.advance_to(clock_.now() + 30s);
        go_live();
    }

    ExecutionQuote quote(std::string symbol = "AAPL") {
        const auto read = market_.execution_quote(InstrumentId(std::move(symbol)));
        if (const auto* unavailable = std::get_if<MarketStateUnavailable>(&read)) {
            throw chronos::test::TestFailure("no quote: " + unavailable->message);
        }
        return std::get<ExecutionQuote>(read);
    }

    std::optional<MarketStateUnavailableCode> unavailable(std::string symbol = "AAPL") {
        const auto read = market_.execution_quote(InstrumentId(std::move(symbol)));
        const auto* unavailable = std::get_if<MarketStateUnavailable>(&read);
        return unavailable ? std::optional(unavailable->code) : std::nullopt;
    }

    std::string diagnostic() { return source_->status().diagnostic.value_or(""); }

    DeploymentStatus deployment(std::string_view id) {
        auto found = manager_.get(id);
        REQUIRE(std::holds_alternative<DeploymentStatus>(found));
        return std::get<DeploymentStatus>(std::move(found));
    }

    std::vector<chronos::DeploymentDecisionRecord> decisions(std::string_view id) {
        auto page = manager_.decisions(id, 200, std::nullopt);
        return std::get<chronos::DecisionPage>(std::move(page)).decisions;
    }

    PaperTradingService& service() { return service_; }
    DeploymentManager& manager() { return manager_; }
    LeasedIngestSequences& sequences() { return sequences_; }
    WorkingOrderMatcher& matcher() { return matcher_; }
    FakeTransport& transport() { return *transport_; }
    FakeLink& link() { return *link_; }
    ManualClock& clock() { return clock_; }
    ManualClock& sequence_clock() { return sequence_clock_; }
    AlpacaLiveSource& source() { return *source_; }
    MarketState& market() { return market_; }
    MarketStateFeed& feed() { return feed_; }

private:
    ProcessOptions options_;
    std::shared_ptr<FakeModel> model_;
    ManualClock clock_{ManualClock::Mode::Hold};
    ManualClock sequence_clock_{ManualClock::Mode::Hold};
    MonotonicUtcClock receive_clock_;
    MarketState market_;
    MarketDataNotifier notifier_;
    MarketEventGate gate_;
    WorkingOrderMatcher matcher_;
    DeploymentHub hub_;
    MarketStateFeed feed_;
    PaperTradingService service_;
    DeploymentManager manager_;
    PostgresIngestSequenceLeaseStore database_store_;
    CrashableStore store_;
    std::vector<std::string> log_;
    LeasedIngestSequences sequences_;
    FakeTransport* transport_{nullptr};
    std::shared_ptr<FakeLink> link_;
    std::unique_ptr<AlpacaLiveSource> source_;
};

std::shared_ptr<FakeModel> model() {
    return std::make_shared<FakeModel>(Timestamp::from_nanoseconds(kEarlierReceipt - 3'600'000'000'000));
}

void create_account(ServerProcess& process, std::string_view id = kAccount) {
    REQUIRE(std::holds_alternative<PaperAccountView>(
        process.service().create_account({.account_id = std::string(id), .starting_cash = Money::parse("10000")})
    ));
}

RecordedPaperOrder submitted(ServerProcess& process, const chronos::paper_trading::PaperOrderRequest& request) {
    auto result = process.service().submit_order(request);
    if (const auto* error = std::get_if<PaperTradingError>(&result)) {
        throw chronos::test::TestFailure("order refused: " + error->message);
    }
    return std::get<RecordedPaperOrder>(std::move(result));
}

void require_filled(const RecordedPaperOrder& recorded) {
    if (const auto* failed = std::get_if<SimulatedExecutionError>(&recorded.result)) {
        throw chronos::test::TestFailure("order not executed: " + failed->message);
    }
    REQUIRE(std::holds_alternative<chronos::PaperOrderReceipt>(recorded.result));
}

std::string describe(const PaperTradingError& error) {
    std::string text = error.message;
    for (const auto& failure : error.instruments) {
        text += "; " + failure.instrument.value() + ": " + failure.message;
    }
    return text;
}

PaperPortfolioView portfolio(ServerProcess& process, std::string_view account = kAccount) {
    auto result = process.service().get_portfolio(account);
    if (const auto* error = std::get_if<PaperTradingError>(&result)) {
        throw chronos::test::TestFailure("portfolio unavailable: " + describe(*error));
    }
    return std::get<PaperPortfolioView>(std::move(result));
}

void seed_sequences(const ScratchSchema& schema, std::uint64_t next, std::string_view stream = kStream) {
    PostgresTestConnection(schema.connection()).run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) VALUES ('" + std::string(stream) +
        "', " + std::to_string(next) + ", 'new')"
    );
}

void expire_leases(const ScratchSchema& schema) {
    PostgresTestConnection(schema.connection()).run_script(
        "UPDATE live_stream_sequences SET lease_expires_at = clock_timestamp() - interval '1 millisecond' "
        "WHERE writer_id IS NOT NULL"
    );
}

// The earlier process: an account bought one AAPL share on sequence 1087.
void leave_an_open_position_on_sequence_1087(const ScratchSchema& schema, bool crash) {
    seed_sequences(schema, kEarlierSequence);
    ServerProcess earlier(schema, model(), {.name = "earlier process", .receipt_start = kEarlierReceipt});
    earlier.go_live();
    create_account(earlier);
    earlier.receive(quote_frame("AAPL", "334.00", "334.02"));
    REQUIRE(earlier.quote().top_of_book.header.ingest_sequence == kEarlierSequence);
    require_filled(submitted(earlier, chronos::test::order_request(std::string(kAccount), "buy-1", Side::Buy, "1")));
    if (crash) {
        earlier.crash_on_exit();
    }
}

// The 2026-10-05 failure, after a clean stop and after a crash: the first
// fresh quote values and trades the recovered position.
void test_a_restarted_live_stream_values_and_trades_a_recovered_position_on_its_first_fresh_quote() {
    for (const bool crash : {false, true}) {
        const ScratchSchema schema;
        leave_an_open_position_on_sequence_1087(schema, crash);
        ServerProcess restarted(schema, model(), {.name = "restarted process"});
        if (crash) {
            // The killed process's lease has not expired: nothing is
            // published, and no Alpaca connection competes for the slot.
            REQUIRE(eventually([&] { return restarted.source().status().state == MarketDataSourceState::Reconnecting; }));
            REQUIRE(contains(restarted.diagnostic(), "written by earlier process until"));
            REQUIRE(restarted.transport().connects() == 0U);
            expire_leases(schema);
            restarted.sequence_clock().advance_to(restarted.sequence_clock().now() + 1s);
            restarted.reconnect_once_ready();
        } else {
            restarted.go_live();
        }
        restarted.receive(kFreshQuote);
        const ExecutionQuote fresh = restarted.quote();
        // After the whole block the earlier process reserved, used or not.
        REQUIRE(fresh.top_of_book.header.ingest_sequence == kEarlierSequence + kBlock);

        const PaperPortfolioView valued = portfolio(restarted);
        REQUIRE(valued.valuation.marks.at(InstrumentId("AAPL")).bid->price == Price::parse("334.24"));
        const RecordedPaperOrder sold =
            submitted(restarted, chronos::test::order_request(std::string(kAccount), "sell-1", Side::Sell, "1"));
        require_filled(sold);
        REQUIRE(sold.account.state.quantity(InstrumentId("AAPL")) == Quantity::parse("0"));
        REQUIRE(sold.market.header.ingest_sequence == kEarlierSequence + kBlock);
    }
}

DeploymentCreateRequest deployment_request(std::string id, std::string account) {
    return DeploymentCreateRequest{
        .deployment_id = std::move(id),
        .account_id = std::move(account),
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
}

void create_deployment(ServerProcess& process, std::string id, std::string account) {
    REQUIRE(std::holds_alternative<DeploymentCreated>(process.manager().create(deployment_request(id, account))));
}

void start_deployment(ServerProcess& process, std::string_view id) {
    REQUIRE(std::holds_alternative<DeploymentStatus>(process.manager().start(id)));
}

// After a restart, a manual order, a resting GTC order and its
// reservation, and a deployment started again by hand all continue on the
// same stream, and nothing the earlier process recorded is lost.
void test_manual_working_and_restarted_model_orders_continue_the_stream() {
    const ScratchSchema schema;
    auto shared_model = model();
    std::uint64_t watermark = 0;
    {
        ServerProcess earlier(schema, shared_model, {.name = "earlier process", .receipt_start = kEarlierReceipt});
        earlier.go_live();
        create_account(earlier);
        create_deployment(earlier, "live-1", "live-model-account");
        earlier.receive(quote_frame("AAPL", "334.00", "334.02"));
        require_filled(submitted(earlier, chronos::test::order_request(std::string(kAccount), "buy-1", Side::Buy, "1")));
        auto resting = chronos::test::order_request(
            std::string(kAccount), "sell-at-336", Side::Sell, "1", chronos::OrderType::Limit, "336"
        );
        resting.time_in_force = chronos::TimeInForce::GoodTilCancelled;
        const RecordedPaperOrder rested = submitted(earlier, resting);
        REQUIRE(rested.account.working_orders == 1U);

        // The model buys: a decision on one quote, the order on a later one.
        start_deployment(earlier, "live-1");
        earlier.receive(quote_frame("AAPL", "334.01", "334.03"), 1, 10ms);
        REQUIRE(eventually([&] { return earlier.deployment("live-1").run->order_decisions == 1; }));
        earlier.receive(quote_frame("AAPL", "334.02", "334.04"), 1, 60ms);
        REQUIRE(eventually([&] { return earlier.deployment("live-1").run->orders_recorded == 1; }));
        REQUIRE(std::holds_alternative<DeploymentStatus>(earlier.manager().stop("live-1")));
        watermark = *earlier.deployment("live-1").deployment.watermark_ingest_sequence;
    }

    ServerProcess restarted(schema, shared_model, {.name = "restarted process"});
    // Stopped by hand before the restart, and so it stays.
    REQUIRE(restarted.deployment("live-1").deployment.state == DeploymentState::Stopped);
    REQUIRE(restarted.deployment("live-1").deployment.state_reason == "stop_requested");
    restarted.go_live();
    const int requests = shared_model->requests.load();
    // The first fresh quote crosses the resting sell: it fills on it, and
    // its reservation ends.
    restarted.receive(quote_frame("AAPL", "336.10", "336.20"));
    REQUIRE(eventually([&] { return restarted.matcher().status().fills == 1U; }));
    const auto account = std::get<PaperAccountView>(restarted.service().get_account(kAccount));
    REQUIRE(account.state.quantity(InstrumentId("AAPL")) == Quantity::parse("0"));
    REQUIRE(account.working_orders == 0U);
    REQUIRE(shared_model->requests.load() == requests);

    // A manual order on the next quote.
    restarted.receive(quote_frame("AAPL", "336.00", "336.05"), 1, 5ms);
    require_filled(submitted(restarted, chronos::test::order_request(std::string(kAccount), "buy-2", Side::Buy, "1")));

    // Started by hand, the model decides on fresh quotes only, after its
    // durable watermark, and sells after its latency.
    shared_model->answer_with([](const chronos::paper_trading::InferenceRequest&) {
        return predicted(PredictedClass::Down);
    });
    start_deployment(restarted, "live-1");
    restarted.receive(quote_frame("AAPL", "336.01", "336.06"), 1, 10ms);
    REQUIRE(eventually([&] { return restarted.deployment("live-1").run->order_decisions == 1; }));
    restarted.receive(quote_frame("AAPL", "336.02", "336.07"), 1, 60ms);
    REQUIRE(eventually([&] { return restarted.deployment("live-1").run->orders_recorded == 1; }));
    const auto decided = restarted.decisions("live-1");
    REQUIRE(decided.front().decision.observation_ingest_sequence > watermark);
    REQUIRE(decided.front().decision.run_generation == 2);
    REQUIRE(decided.front().state == DecisionState::Recorded);
    REQUIRE(std::get<PaperAccountView>(restarted.service().get_account("live-model-account"))
                .state.quantity(InstrumentId("AAPL")) == Quantity::parse("0"));
}

// A crash with a decision pending: after the restart the deployment is
// stopped, the decision abandoned, a stopped deployment stays stopped, and
// no quote reaches a model until someone starts it.
void test_after_a_crash_deployments_stay_inactive_and_a_pending_decision_is_abandoned() {
    const ScratchSchema schema;
    auto shared_model = model();
    std::int64_t interrupted = 0;
    {
        ServerProcess earlier(schema, shared_model, {.name = "earlier process", .receipt_start = kEarlierReceipt});
        earlier.go_live();
        create_deployment(earlier, "live-1", "live-model-account");
        create_deployment(earlier, "idle-1", "idle-model-account");
        start_deployment(earlier, "live-1");
        earlier.receive(quote_frame("AAPL", "334.00", "334.02"));
        REQUIRE(eventually([&] { return earlier.deployment("live-1").run->order_decisions == 1; }));
        interrupted = earlier.decisions("live-1").front().decision_sequence;
        earlier.crash_on_exit();
    }
    // The shutdown above stopped it cleanly; a crash leaves it running with
    // its decision pending, as here.
    PostgresTestConnection(schema.connection()).run_script(
        "UPDATE model_deployments SET state = 'running', state_reason = 'started' WHERE deployment_id = 'live-1';"
        "UPDATE model_deployment_decisions SET state = 'pending', resolution = NULL, resolved_at_market_ns = NULL,"
        " resolved_at = NULL WHERE decision_sequence = " + std::to_string(interrupted) + ";"
    );
    expire_leases(schema);

    ServerProcess restarted(schema, shared_model, {.name = "restarted process"});
    REQUIRE(restarted.deployment("live-1").deployment.state == DeploymentState::Stopped);
    REQUIRE(restarted.deployment("live-1").deployment.state_reason == "server_restarted");
    REQUIRE(!restarted.deployment("live-1").active);
    REQUIRE(restarted.decisions("live-1").front().resolution == std::optional<std::string>("server_restarted"));
    REQUIRE(restarted.deployment("idle-1").deployment.state == DeploymentState::Stopped);
    restarted.go_live();
    const int requests = shared_model->requests.load();
    for (int quote = 0; quote < 3; ++quote) {
        restarted.receive(quote_frame("AAPL", "334.10", "334.12"), 1, 100ms);
    }
    REQUIRE(shared_model->requests.load() == requests);
    REQUIRE(!restarted.deployment("live-1").active);
    REQUIRE(!restarted.deployment("idle-1").active);
    REQUIRE(PostgresTestConnection(schema.connection()).scalar("SELECT count(*) FROM paper_order_records") == "0");
}

// AAPL and SPY share one sequence on the stream, before and after a
// restart; another stream ID has sequences of its own and cannot value or
// trade an account bound to this one.
void test_instruments_share_one_sequence_and_streams_stay_isolated() {
    const ScratchSchema schema;
    std::set<std::uint64_t> published;
    std::uint64_t latest = 0;
    const auto record = [&](ServerProcess& process) {
        for (const char* symbol : {"AAPL", "SPY"}) {
            const std::uint64_t sequence = process.quote(symbol).top_of_book.header.ingest_sequence;
            REQUIRE(published.insert(sequence).second);
            latest = std::max(latest, sequence);
        }
    };
    {
        ServerProcess earlier(schema, model(), {.name = "earlier process", .receipt_start = kEarlierReceipt});
        earlier.go_live();
        create_account(earlier);
        earlier.receive(both_frame("334.00", "669.00"), 2);
        REQUIRE(earlier.quote("AAPL").top_of_book.header.ingest_sequence == 0U);
        REQUIRE(earlier.quote("SPY").top_of_book.header.ingest_sequence == 1U);
        record(earlier);
        require_filled(submitted(earlier, chronos::test::order_request(std::string(kAccount), "buy-aapl", Side::Buy, "1")));
        auto spy = chronos::test::order_request(std::string(kAccount), "buy-spy", Side::Buy, "1");
        spy.instrument = "SPY";
        require_filled(submitted(earlier, spy));
        earlier.receive(both_frame("334.01", "669.01"), 2, 5ms);
        record(earlier);
    }
    {
        ServerProcess restarted(schema, model(), {.name = "restarted process"});
        restarted.go_live();
        restarted.receive(both_frame("334.20", "669.20"), 2);
        const std::uint64_t first = restarted.quote("AAPL").top_of_book.header.ingest_sequence;
        REQUIRE(first > latest);
        REQUIRE(restarted.quote("SPY").top_of_book.header.ingest_sequence == first + 1);
        record(restarted);
        const PaperPortfolioView valued = portfolio(restarted);
        REQUIRE(valued.valuation.marks.size() == 2U);
        REQUIRE(valued.valuation.marks.at(InstrumentId("SPY")).bid->price == Price::parse("669.20"));
    }
    // Another stream: its own sequences from zero, and no way to value or
    // trade an account bound to the first.
    ServerProcess other(schema, model(), {.name = "other stream", .stream = "alpaca-iex-other"});
    other.go_live();
    other.receive(quote_frame("AAPL", "334.30", "334.32"));
    REQUIRE(other.quote().top_of_book.header.ingest_sequence == 0U);
    const auto valued = other.service().get_portfolio(kAccount);
    REQUIRE(std::holds_alternative<PaperTradingError>(valued));
    REQUIRE(std::get<PaperTradingError>(valued).valuation ==
        std::optional(PortfolioValuationErrorCode::MarketTimelineMismatch));
    const auto refused = other.service().submit_order(
        chronos::test::order_request(std::string(kAccount), "cross-stream", Side::Sell, "1")
    );
    if (const auto* recorded = std::get_if<RecordedPaperOrder>(&refused)) {
        REQUIRE(std::holds_alternative<SimulatedExecutionError>(recorded->result));
        REQUIRE(std::get<SimulatedExecutionError>(recorded->result).code ==
            SimulatedExecutionErrorCode::MarketStreamMismatch);
    } else {
        REQUIRE(std::holds_alternative<PaperTradingError>(refused));
    }
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar(
        "SELECT string_agg(stream_id, ',' ORDER BY stream_id) FROM live_stream_sequences"
    ) == "alpaca-iex-other,alpaca-iex-restart-test");
}

// An IEX quote of the stream built by hand, to show what the live source
// can no longer produce is still refused by the unchanged guards.
TopOfBookEvent iex_quote(
    std::uint64_t sequence,
    std::int64_t received,
    std::string_view bid,
    std::string_view ask = "334.40"
) {
    return TopOfBookEvent{
        .header = MarketEventHeader{
            .instrument = InstrumentId("AAPL"),
            .stream = MarketDataStreamId(std::string(kStream)),
            .venue = VenueId("IEX"),
            .event_timestamp = Timestamp::from_nanoseconds(received - 1),
            .receive_timestamp = Timestamp::from_nanoseconds(received),
            .ingest_sequence = sequence,
            .venue_sequence = std::nullopt,
            .quality = MarketDataQuality{}.with(MarketDataQualityFlag::SourceSpecific),
        },
        .bid = BookLevel{.price = Price::parse(bid), .quantity = Quantity::parse("100")},
        .ask = BookLevel{.price = Price::parse(ask), .quantity = Quantity::parse("100")},
    };
}

// Reconnecting in the same process continues the run with no jump and
// needs a fresh quote; silence makes the quote stale; and an older quote,
// or a different quote under the account's own sequence, stays refused.
void test_reconnects_silence_and_old_or_altered_quotes_stay_refused() {
    const ScratchSchema schema;
    leave_an_open_position_on_sequence_1087(schema, false);
    ServerProcess restarted(schema, model(), {.name = "restarted process"});
    restarted.go_live();
    restarted.receive(kFreshQuote);
    const std::uint64_t first = restarted.quote().top_of_book.header.ingest_sequence;
    static_cast<void>(portfolio(restarted));

    // Same process, new connection: nothing is executable until a fresh
    // quote, which continues the same sequence.
    restarted.link().close();
    REQUIRE(eventually([&] { return restarted.source().status().state == MarketDataSourceState::Reconnecting; }));
    REQUIRE(restarted.unavailable() == MarketStateUnavailableCode::SourceUnavailable);
    REQUIRE(std::holds_alternative<PaperTradingError>(restarted.service().get_portfolio(kAccount)));
    auto second = restarted.transport().accept();
    REQUIRE(restarted.clock().await_waits(1));
    restarted.clock().advance_to(restarted.clock().now() + 1s);
    restarted.go_live(*second);
    REQUIRE(restarted.unavailable() == MarketStateUnavailableCode::AwaitingFreshQuote);
    const auto awaiting = restarted.service().get_portfolio(kAccount);
    REQUIRE(std::holds_alternative<PaperTradingError>(awaiting));
    restarted.receive(*second, quote_frame("AAPL", "334.30", "334.34"), 1, 0ms);
    REQUIRE(restarted.quote().top_of_book.header.ingest_sequence == first + 1);
    static_cast<void>(portfolio(restarted));

    // Silence: the quote ages past the account's limit.
    REQUIRE(second->await_drained());
    restarted.clock().advance_to(restarted.clock().now() + 2s);
    second->push_timeout();
    REQUIRE(second->await_drained());
    const auto stale = restarted.service().get_portfolio(kAccount);
    REQUIRE(std::holds_alternative<PaperTradingError>(stale));
    REQUIRE(contains(describe(std::get<PaperTradingError>(stale)), "older than the account's configured risk limit"));

    // The guards themselves: a different quote under the account's own
    // execution sequence (1087, bought at 334.00 / 334.02), or an older
    // one, refused for valuation and orders alike. Both belong to the
    // earlier process's generation, so an order on them is refused by the
    // writer fence before any rejection could be recorded (ADR 0020); the
    // execution guards behind it are covered in paper_execution_tests.
    PostgresTestConnection inspector(schema.connection());
    const std::string records_before = inspector.scalar("SELECT count(*) FROM paper_order_records");
    for (const auto& [sequence, bid] : {std::pair{kEarlierSequence, "334.10"}, std::pair{kEarlierSequence - 1, "334.30"}}) {
        MarketState forged(MarketDataStreamId(std::string(kStream)), sequence);
        chronos::test::apply(forged, iex_quote(sequence, kRestartReceipt + 10'000'000'000, bid));
        PaperTradingService service(
            std::make_unique<PostgresPaperOrderRepository>(schema.connection()),
            forged,
            service_config()
        );
        const auto valued = service.get_portfolio(kAccount);
        REQUIRE(std::holds_alternative<PaperTradingError>(valued));
        REQUIRE(contains(describe(std::get<PaperTradingError>(valued)), "does not continue the instrument's"));
        const auto sold = service.submit_order(
            chronos::test::order_request(std::string(kAccount), "forged-" + std::to_string(sequence), Side::Sell, "1")
        );
        REQUIRE(std::holds_alternative<PaperTradingError>(sold));
        REQUIRE(std::get<PaperTradingError>(sold).code == chronos::paper_trading::PaperTradingErrorCode::MarketDataUnavailable);
        REQUIRE(contains(describe(std::get<PaperTradingError>(sold)), "belongs to an earlier writer"));
    }
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_order_records") == records_before);
}

// Two servers on one stream and database: the second waits, says who
// holds the stream, opens no upstream connection, and publishes nothing;
// after the first stops, it continues after everything the first reserved.
void test_a_second_server_on_the_stream_waits_without_connecting() {
    const ScratchSchema schema;
    auto first = std::make_unique<ServerProcess>(
        schema, model(), ProcessOptions{.name = "first server", .receipt_start = kEarlierReceipt}
    );
    first->go_live();
    first->receive(quote_frame("AAPL", "334.00", "334.02"));
    REQUIRE(first->quote().top_of_book.header.ingest_sequence == 0U);

    ServerProcess second(schema, model(), {.name = "second server"});
    REQUIRE(eventually([&] { return second.source().status().state == MarketDataSourceState::Reconnecting; }));
    REQUIRE(contains(second.diagnostic(), "written by first server until"));
    REQUIRE(second.transport().connects() == 0U);
    // The first keeps publishing meanwhile.
    first->receive(quote_frame("AAPL", "334.01", "334.03"), 1, 5ms);
    REQUIRE(first->quote().top_of_book.header.ingest_sequence == 1U);
    REQUIRE(second.source().status().events_applied == 0U);

    first.reset();  // a clean stop releases the stream
    second.sequence_clock().advance_to(second.sequence_clock().now() + 1s);
    second.reconnect_once_ready();
    second.receive(kFreshQuote);
    REQUIRE(second.quote().top_of_book.header.ingest_sequence == kBlock);
}

// Without its database the source cannot hold the stream: it opens no
// connection, and says why.
void test_an_unreachable_sequence_database_keeps_the_feed_closed() {
    const ScratchSchema schema;
    ServerProcess unreachable(
        schema, model(), {.name = "cut off", .sequence_database = "postgresql://chronos@127.0.0.1:1/none"}
    );
    REQUIRE(eventually([&] { return unreachable.source().status().state == MarketDataSourceState::Reconnecting; }));
    REQUIRE(contains(unreachable.diagnostic(), "could not connect to PostgreSQL"));
    REQUIRE(unreachable.transport().connects() == 0U);
    REQUIRE(unreachable.unavailable() == MarketStateUnavailableCode::SourceUnavailable);
}

// At the last sequences of a stream: each is published once, nothing
// wraps, and the source then fails, asking for a new stream ID.
void test_a_stream_ends_at_its_last_sequence_without_wrapping() {
    const ScratchSchema schema;
    seed_sequences(schema, kLast - 1);
    ServerProcess process(schema, model(), {.name = "last"});
    process.go_live();
    process.receive(both_frame("334.00", "669.00"), 2);
    REQUIRE(process.quote("AAPL").top_of_book.header.ingest_sequence == kLast - 1);
    REQUIRE(process.quote("SPY").top_of_book.header.ingest_sequence == kLast);
    REQUIRE(process.market().status().last_ingest_sequence == std::optional<std::uint64_t>(kLast));
    process.link().push(quote_frame("AAPL", "334.10", "334.12"));
    REQUIRE(eventually([&] { return process.source().status().state == MarketDataSourceState::Failed; }));
    REQUIRE(contains(process.diagnostic(), "new stream ID"));
    REQUIRE(process.source().status().events_applied == 2U);
    REQUIRE(PostgresTestConnection(schema.connection()).scalar(
        "SELECT next_sequence::text FROM live_stream_sequences"
    ) == "18446744073709551616");
}

// A database the acceptance would have left: history on the stream from
// before migration 006. Without the explicit flag the source refuses and
// opens no connection; with it, the stream continues after that history.
void test_a_legacy_database_continues_only_after_an_explicit_adoption() {
    const ScratchSchema schema(true);
    {
        MarketState market(MarketDataStreamId(std::string(kStream)), kEarlierSequence);
        PaperTradingService service(
            std::make_unique<PostgresPaperOrderRepository>(schema.connection()), market, service_config()
        );
        REQUIRE(std::holds_alternative<PaperAccountView>(
            service.create_account({.account_id = std::string(kAccount), .starting_cash = Money::parse("10000")})
        ));
        chronos::test::apply(market, iex_quote(kEarlierSequence, kEarlierReceipt, "334.00"));
        const auto bought = service.submit_order(
            chronos::test::order_request(std::string(kAccount), "buy-1", Side::Buy, "1")
        );
        REQUIRE(std::holds_alternative<RecordedPaperOrder>(bought));
    }
    schema.upgrade();
    {
        ServerProcess refusing(schema, model(), {.name = "without adoption"});
        REQUIRE(eventually([&] { return refusing.source().status().state == MarketDataSourceState::Failed; }));
        REQUIRE(contains(refusing.diagnostic(), "up to ingest sequence 1087"));
        REQUIRE(contains(refusing.diagnostic(), "--adopt-live-stream-history"));
        REQUIRE(refusing.transport().connects() == 0U);
    }
    REQUIRE(PostgresTestConnection(schema.connection()).scalar("SELECT count(*) FROM live_stream_sequences") == "0");
    ServerProcess adopting(schema, model(), {.name = "with adoption", .adopt_history = true});
    adopting.go_live();
    adopting.receive(kFreshQuote);
    REQUIRE(adopting.quote().top_of_book.header.ingest_sequence == kEarlierSequence + 1);
    static_cast<void>(portfolio(adopting));
    require_filled(submitted(adopting, chronos::test::order_request(std::string(kAccount), "sell-1", Side::Sell, "1")));
    REQUIRE(PostgresTestConnection(schema.connection()).scalar(
        "SELECT origin || '/' || adopted_maximum::text FROM live_stream_sequences"
    ) == "adopted/1087");
}

// Codex's review, finding 3. Process A believes it still owns the stream:
// its clocks have not moved, exactly as if it paused between its local
// check and applying an event. Its lease has expired in the database, and
// a successor has taken the stream. A now applies a quote it numbered
// before the pause. Nothing authoritative may follow from it.
void take_over_from(const ScratchSchema& schema) {
    expire_leases(schema);
    PostgresIngestSequenceLeaseStore successor(schema.connection());
    REQUIRE(std::holds_alternative<chronos::market_data::LeaseGranted>(successor.acquire(
        MarketDataStreamId(std::string(kStream)),
        StreamWriter{.id = "successor", .label = "successor process"},
        15'000ms,
        kBlock,
        false
    )));
}

// The next sequence of the run the process's source is publishing in, as
// its session would number its next frame.
struct Numbered {
    std::uint64_t sequence;
    std::uint64_t run;
};

Numbered next_issued(ServerProcess& process) {
    const auto run = process.sequences().current_run();
    if (!run) {
        throw chronos::test::TestFailure("the process has no publication run under way");
    }
    const auto issued = process.sequences().issue(*run, 1);
    if (const auto* unavailable = std::get_if<chronos::market_data::IngestSequencesUnavailable>(&issued)) {
        throw chronos::test::TestFailure("not issued: " + unavailable->diagnostic);
    }
    return Numbered{.sequence = std::get<std::uint64_t>(issued), .run = *run};
}

chronos::ValidatedMarketEvent validated(TopOfBookEvent event) {
    auto result = chronos::validate_market_event(chronos::MarketEvent(std::move(event)));
    REQUIRE(std::holds_alternative<chronos::ValidatedMarketEvent>(result));
    return std::get<chronos::ValidatedMarketEvent>(std::move(result));
}

void test_a_deposed_writers_quote_can_neither_trade_be_valued_nor_be_decided() {
    const ScratchSchema schema;
    auto shared_model = model();
    ServerProcess deposed(schema, shared_model, {.name = "deposed writer", .receipt_start = kEarlierReceipt});
    deposed.go_live();
    create_account(deposed);
    create_deployment(deposed, "live-1", "live-model-account");
    deposed.receive(quote_frame("AAPL", "334.00", "334.02"));
    require_filled(submitted(deposed, chronos::test::order_request(std::string(kAccount), "buy-1", Side::Buy, "1")));
    start_deployment(deposed, "live-1");
    PostgresTestConnection inspector(schema.connection());
    const std::string records_before = inspector.scalar("SELECT count(*) FROM paper_order_records");

    take_over_from(schema);
    const auto [sequence, run_id] = next_issued(deposed);
    const auto published = deposed.feed().publish(
        validated(iex_quote(sequence, kEarlierReceipt + 1'000'000'000, "334.10")), deposed.sequences(), run_id
    );
    std::cout << "deposed quote " << sequence << ": refused=" << published.refused.has_value()
              << " failed=" << published.failed.has_value() << '\n';
    const auto valued = deposed.service().get_portfolio(kAccount);
    std::cout << "valuation on it: " << (std::holds_alternative<PaperPortfolioView>(valued) ? "valued" : "refused") << '\n';
    const auto sold = deposed.service().submit_order(
        chronos::test::order_request(std::string(kAccount), "sell-after-takeover", Side::Sell, "1")
    );
    const bool executed = std::holds_alternative<RecordedPaperOrder>(sold) &&
        std::holds_alternative<chronos::PaperOrderReceipt>(std::get<RecordedPaperOrder>(sold).result);
    std::cout << "manual sell on it: " << (executed ? "executed" : "refused") << '\n';
    REQUIRE(eventually([&] { return deposed.deployment("live-1").run->candidates >= 1U; }));
    REQUIRE(eventually([&] {
        const auto run = *deposed.deployment("live-1").run;
        return run.order_decisions + run.holds > 0U || !run.skipped.empty();
    }));
    const auto run = *deposed.deployment("live-1").run;
    std::cout << "model decisions on it: " << run.order_decisions + run.holds << '\n';

    REQUIRE(!std::holds_alternative<PaperPortfolioView>(valued));
    REQUIRE(!executed);
    REQUIRE(run.order_decisions + run.holds == 0U);
    REQUIRE(deposed.decisions("live-1").empty());
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_order_records") == records_before);
}

void test_a_deposed_writers_quote_fills_no_working_order() {
    const ScratchSchema schema;
    ServerProcess deposed(schema, model(), {.name = "deposed writer", .receipt_start = kEarlierReceipt});
    deposed.go_live();
    create_account(deposed);
    deposed.receive(quote_frame("AAPL", "334.00", "334.02"));
    require_filled(submitted(deposed, chronos::test::order_request(std::string(kAccount), "buy-1", Side::Buy, "1")));
    auto resting = chronos::test::order_request(
        std::string(kAccount), "sell-at-334.50", Side::Sell, "1", chronos::OrderType::Limit, "334.50"
    );
    resting.time_in_force = chronos::TimeInForce::GoodTilCancelled;
    REQUIRE(submitted(deposed, resting).account.working_orders == 1U);
    PostgresTestConnection inspector(schema.connection());
    const std::string fills_before = inspector.scalar("SELECT count(*) FROM paper_fills");

    take_over_from(schema);
    // Its bid crosses the resting sell; the matcher observes it first.
    const auto [sequence, run_id] = next_issued(deposed);
    const auto published = deposed.feed().publish(
        validated(iex_quote(sequence, kEarlierReceipt + 1'000'000'000, "334.60", "334.65")),
        deposed.sequences(),
        run_id
    );
    std::cout << "deposed crossing quote " << sequence << ": refused=" << published.refused.has_value()
              << " failed=" << published.failed.value_or("no") << '\n';
    std::cout << "fills: before " << fills_before << ", after " << inspector.scalar("SELECT count(*) FROM paper_fills") << '\n';
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_fills") == fills_before);
    REQUIRE(std::get<PaperAccountView>(deposed.service().get_account(kAccount)).working_orders == 1U);
    // The matcher's refusal is an observer failure: the market state was
    // suspended before the gate was released.
    REQUIRE(published.failed.has_value());
    REQUIRE(contains(*published.failed, "belongs to an earlier writer"));
    REQUIRE(deposed.unavailable() == MarketStateUnavailableCode::SourceUnavailable);
}

// Codex's review, finding 2, through every consumer. The writer lease
// lapses locally while the feed is idle: no frame, no read timeout. The
// cached quote stops serving valuation, manual orders, the model's pending
// order, and the deployment's view at that instant; a crossing frame after
// it publishes and fills nothing; a renewal that succeeds late does not
// revive the quote. Only a new session and a fresh quote do.
void test_a_lapsed_writers_cached_quote_serves_nothing_until_a_fresh_one() {
    const ScratchSchema schema;
    auto shared_model = model();
    ServerProcess process(schema, shared_model, {.name = "lapsing writer", .receipt_start = kEarlierReceipt});
    process.go_live();
    create_account(process);
    create_deployment(process, "live-1", "live-model-account");
    process.receive(quote_frame("AAPL", "334.00", "334.02"));
    require_filled(submitted(process, chronos::test::order_request(std::string(kAccount), "buy-1", Side::Buy, "1")));
    auto resting = chronos::test::order_request(
        std::string(kAccount), "sell-at-334.50", Side::Sell, "1", chronos::OrderType::Limit, "334.50"
    );
    resting.time_in_force = chronos::TimeInForce::GoodTilCancelled;
    REQUIRE(submitted(process, resting).account.working_orders == 1U);
    // The model decides to buy; its order waits for a later quote.
    start_deployment(process, "live-1");
    process.receive(quote_frame("AAPL", "334.01", "334.03"), 1, 10ms);
    REQUIRE(eventually([&] { return process.deployment("live-1").run->order_decisions == 1U; }));
    REQUIRE(process.link().await_drained());
    PostgresTestConnection inspector(schema.connection());
    const std::string fills_before = inspector.scalar("SELECT count(*) FROM paper_fills");
    const std::string records_before = inspector.scalar("SELECT count(*) FROM paper_order_records");
    const std::uint64_t applied = process.source().status().events_applied;

    // Exactly the local deadline: acquisition + 15 s - 3 s.
    process.sequence_clock().advance_to(process.sequence_clock().now() + 12s);
    REQUIRE(process.unavailable() == MarketStateUnavailableCode::SourceUnavailable);
    const auto valued = process.service().get_portfolio(kAccount);
    REQUIRE(std::holds_alternative<PaperTradingError>(valued));
    const auto sold = process.service().submit_order(
        chronos::test::order_request(std::string(kAccount), "sell-after-lapse", Side::Sell, "1")
    );
    REQUIRE(!(std::holds_alternative<RecordedPaperOrder>(sold) &&
              std::holds_alternative<chronos::PaperOrderReceipt>(std::get<RecordedPaperOrder>(sold).result)));
    REQUIRE(eventually([&] {
        const auto run = *process.deployment("live-1").run;
        const auto waited = run.waits.find("market_data_unavailable");
        return waited != run.waits.end() && waited->second > 0U;
    }));
    const auto status = process.deployment("live-1");
    REQUIRE(status.run->orders_recorded == 0U);
    REQUIRE(!status.market->quote.available);
    REQUIRE(status.market->quote.unavailable_reason == std::optional<std::string>("source_unavailable"));

    // A crossing frame now ends the session; none of it is published.
    process.link().push(quote_frame("AAPL", "334.60", "334.65"));
    REQUIRE(eventually([&] { return process.source().status().state == MarketDataSourceState::Reconnecting; }));
    REQUIRE(process.source().status().events_applied == applied);
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_fills") == fills_before);
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_order_records") == records_before);

    // The keeper renews late: the lease is this process's again, the quote
    // is still not.
    REQUIRE(eventually([&] { return !process.sequences().readiness().has_value(); }));
    REQUIRE(process.unavailable().has_value());
    REQUIRE(std::holds_alternative<PaperTradingError>(process.service().get_portfolio(kAccount)));

    // A new session: still nothing until a fresh quote, which then values
    // the position and fills the resting sell.
    auto second = process.transport().accept();
    REQUIRE(process.clock().await_waits(1));
    process.clock().advance_to(process.clock().now() + 1s);
    process.go_live(*second);
    REQUIRE(process.unavailable() == MarketStateUnavailableCode::AwaitingFreshQuote);
    REQUIRE(std::holds_alternative<PaperTradingError>(process.service().get_portfolio(kAccount)));
    process.receive(*second, quote_frame("AAPL", "334.60", "334.65"), 1, 0ms);
    REQUIRE(eventually([&] { return process.matcher().status().fills == 1U; }));
    REQUIRE(std::get<PaperAccountView>(process.service().get_account(kAccount)).working_orders == 0U);
    REQUIRE(process.quote().top_of_book.header.ingest_sequence > process.market().status().first_ingest_sequence.value());
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr || std::string_view(configured_connection).empty()) {
        std::cout << "live stream restart test skipped: CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {
            "a restarted live stream values and trades a recovered position on its first fresh quote",
            test_a_restarted_live_stream_values_and_trades_a_recovered_position_on_its_first_fresh_quote,
        },
        {
            "manual, working, and restarted model orders continue the stream",
            test_manual_working_and_restarted_model_orders_continue_the_stream,
        },
        {
            "after a crash, deployments stay inactive and a pending decision is abandoned",
            test_after_a_crash_deployments_stay_inactive_and_a_pending_decision_is_abandoned,
        },
        {
            "instruments share one sequence and streams stay isolated",
            test_instruments_share_one_sequence_and_streams_stay_isolated,
        },
        {
            "reconnects, silence, and old or altered quotes stay refused",
            test_reconnects_silence_and_old_or_altered_quotes_stay_refused,
        },
        {
            "a second server on the stream waits without connecting",
            test_a_second_server_on_the_stream_waits_without_connecting,
        },
        {
            "an unreachable sequence database keeps the feed closed",
            test_an_unreachable_sequence_database_keeps_the_feed_closed,
        },
        {
            "a stream ends at its last sequence without wrapping",
            test_a_stream_ends_at_its_last_sequence_without_wrapping,
        },
        {
            "a legacy database continues only after an explicit adoption",
            test_a_legacy_database_continues_only_after_an_explicit_adoption,
        },
        {
            "a deposed writer's quote can neither trade, be valued, nor be decided",
            test_a_deposed_writers_quote_can_neither_trade_be_valued_nor_be_decided,
        },
        {"a deposed writer's quote fills no working order", test_a_deposed_writers_quote_fills_no_working_order},
        {
            "a lapsed writer's cached quote serves nothing until a fresh one",
            test_a_lapsed_writers_cached_quote_serves_nothing_until_a_fresh_one,
        },
    });
}
