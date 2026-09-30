// Good-til-cancelled orders through the transport-independent service, with
// the working-order matcher attached to the market-event path as the server
// wires it: submissions, matching of later observations, cancellation,
// pages and cursors, the market-event gate, outages, restarts, replay
// pause and resume, and matching throughput.
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data/paced_replay_source.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "chronos/paper_trading/working_order_matcher.hpp"
#include "market_data_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
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

namespace market_data = chronos::market_data;
using chronos::MarketEvent;
using chronos::MarketEventRead;
using chronos::Money;
using chronos::OrderType;
using chronos::PaperOrderRecord;
using chronos::PaperOrderStatus;
using chronos::PostgresPaperOrderRepository;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedCancellationReason;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::paper_trading::CancelledPaperOrder;
using chronos::paper_trading::CancelPaperOrderRequest;
using chronos::paper_trading::CreatePaperAccountRequest;
using chronos::paper_trading::ForwardPageRequest;
using chronos::paper_trading::HistoryPageRequest;
using chronos::paper_trading::OrderEventPage;
using chronos::paper_trading::OrderFillPage;
using chronos::paper_trading::PaperAccountView;
using chronos::paper_trading::PaperOrderRequest;
using chronos::paper_trading::PaperTradingError;
using chronos::paper_trading::PaperTradingErrorCode;
using chronos::paper_trading::PaperTradingService;
using chronos::paper_trading::PaperTradingServiceConfig;
using chronos::paper_trading::RecordedPaperOrder;
using chronos::paper_trading::WorkingOrderMatcher;
using chronos::paper_trading::WorkingOrderMatcherOptions;
using chronos::paper_trading::WorkingOrderMatcherState;
using chronos::paper_trading::WorkingOrderPage;
using chronos::test::all_migrations_sql;
using chronos::test::PostgresTestConnection;
using chronos::test::QuoteLevels;
using chronos::test::quote;
using chronos::test::terminate_backends;
using chronos::test::unique_test_id;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;

std::string connection_string;

Money money(std::string_view value) { return Money::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

WorkingOrderMatcherOptions fast_retries() {
    return WorkingOrderMatcherOptions{
        .first_retry_delay = std::chrono::milliseconds(5),
        .maximum_retry_delay = std::chrono::milliseconds(20),
    };
}

// The server's wiring: one market state fed through a gate, a matcher with
// its own connection, and a service with another. Each server has a stream
// of its own unless told to continue one: every account on a stream is
// matched, so orders other tests left working in the shared database must
// never see these observations.
struct Server {
    explicit Server(
        std::string service_connection = connection_string,
        std::string matcher_connection = connection_string,
        PaperTradingServiceConfig config = chronos::test::paper_service_config(),
        std::optional<std::chrono::milliseconds> matcher_lock_timeout = std::nullopt,
        WorkingOrderMatcherOptions matcher_options = fast_retries(),
        std::string stream_id = unique_test_id("gtc-stream")
    )
        : stream(std::move(stream_id)),
          market(std::make_unique<chronos::MarketState>(chronos::MarketDataStreamId(stream))),
          notifier(clock, market_data::MarketDataNotifier::Options{}),
          matcher(
              std::make_unique<PostgresPaperOrderRepository>(
                  std::move(matcher_connection),
                  matcher_lock_timeout
              ),
              matcher_options,
              [this] {
                  activity.fetch_add(1);
                  notifier.publish_activity();
              },
              [this](std::string_view line) {
                  const std::lock_guard lock(log_mutex);
                  log.emplace_back(line);
              }
          ),
          feed(*market, notifier, 0, &matcher, &gate),
          service(
              std::make_unique<PostgresPaperOrderRepository>(
                  std::move(service_connection)
              ),
              *market,
              std::move(config),
              [this] { notifier.publish_activity(); },
              &gate
          ) {}

    // Applies and matches one event, as a source does. A stream's ingest
    // sequences are contiguous from 0 across instruments.
    void publish(const TopOfBookEvent& event) {
        const auto rejected = feed.apply(chronos::test::validated(MarketEvent(event)));
        if (rejected) {
            throw chronos::test::TestFailure("feed rejected an event: " + *rejected);
        }
        next_sequence = event.header.ingest_sequence + 1;
    }

    // The next observation, received 1 microsecond per sequence after the
    // open.
    TopOfBookEvent next_ask(
        std::string_view ask,
        std::string_view size,
        std::string_view instrument = "AAPL"
    );

    void publish_ask(
        std::string_view ask,
        std::string_view size,
        std::string_view instrument = "AAPL"
    ) {
        publish(next_ask(ask, size, instrument));
    }

    std::string create_account(std::string_view prefix, std::string_view cash = "10000") {
        const std::string account_id = unique_test_id(prefix);
        auto created = service.create_account(CreatePaperAccountRequest{
            .account_id = account_id,
            .starting_cash = money(cash),
        });
        REQUIRE(std::holds_alternative<PaperAccountView>(created));
        return account_id;
    }

    std::string stream;
    market_data::SteadyClock clock;
    std::unique_ptr<chronos::MarketState> market;
    market_data::MarketDataNotifier notifier;
    market_data::MarketEventGate gate;
    std::atomic<int> activity{0};
    std::mutex log_mutex;
    std::vector<std::string> log;
    WorkingOrderMatcher matcher;
    market_data::MarketStateFeed feed;
    PaperTradingService service;
    std::uint64_t next_sequence{0};
};

PaperOrderRequest gtc(
    const std::string& account_id,
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::string_view limit
) {
    PaperOrderRequest request = chronos::test::order_request(
        account_id,
        std::move(client_order_id),
        side,
        order_quantity,
        OrderType::Limit,
        limit
    );
    request.time_in_force = TimeInForce::GoodTilCancelled;
    return request;
}

TopOfBookEvent ask_at(
    std::uint64_t sequence,
    std::string_view ask,
    std::string_view size,
    std::string_view instrument = "AAPL"
) {
    return quote(
        sequence,
        static_cast<std::int64_t>(sequence) * 1'000,
        std::string(instrument),
        QuoteLevels{
            .bid_price = "1",
            .bid_quantity = "5",
            .ask_price = ask,
            .ask_quantity = size,
        }
    );
}

TopOfBookEvent Server::next_ask(
    std::string_view ask,
    std::string_view size,
    std::string_view instrument
) {
    TopOfBookEvent event = ask_at(next_sequence, ask, size, instrument);
    event.header.stream = chronos::MarketDataStreamId(stream);
    return event;
}

Timestamp time_of(std::uint64_t sequence) {
    return Timestamp::from_nanoseconds(
        chronos::test::kMarketOpenNanoseconds +
        static_cast<std::int64_t>(sequence) * 1'000
    );
}

RecordedPaperOrder recorded(chronos::paper_trading::PaperOrderSubmissionResult result) {
    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
        throw chronos::test::TestFailure("submission failed: " + failure->message);
    }
    return std::get<RecordedPaperOrder>(std::move(result));
}

template <typename Result>
PaperTradingError failure(Result result, PaperTradingErrorCode code) {
    REQUIRE(std::holds_alternative<PaperTradingError>(result));
    PaperTradingError error = std::get<PaperTradingError>(std::move(result));
    REQUIRE(error.code == code);
    return error;
}

PaperOrderRecord order_of(Server& server, const std::string& account_id, std::string_view id) {
    auto found = server.service.get_order(account_id, id);
    REQUIRE(std::holds_alternative<PaperOrderRecord>(found));
    return std::get<PaperOrderRecord>(std::move(found));
}

CancelledPaperOrder cancelled(chronos::paper_trading::CancelPaperOrderResult result) {
    if (const auto* error = std::get_if<PaperTradingError>(&result)) {
        throw chronos::test::TestFailure("cancel failed: " + error->message);
    }
    return std::get<CancelledPaperOrder>(std::move(result));
}

CancelPaperOrderRequest cancel_request(
    const std::string& account_id,
    std::string client_order_id,
    std::string cancel_request_id
) {
    return CancelPaperOrderRequest{
        .account_id = account_id,
        .client_order_id = std::move(client_order_id),
        .cancel_request_id = std::move(cancel_request_id),
    };
}

void test_a_working_order_fills_from_later_observations() {
    Server server;
    const std::string account_id = server.create_account("svc-gtc");
    server.publish_ask("101", "5");

    const RecordedPaperOrder accepted =
        recorded(server.service.submit_order(gtc(account_id, "b1", Side::Buy, "10", "100")));
    REQUIRE(accepted.order.state->status == PaperOrderStatus::Working);
    REQUIRE(accepted.order.state->reservation.cash == money("1000.01"));
    REQUIRE(accepted.account.reservations.reserved_cash == money("1000.01"));
    REQUIRE(accepted.account.working_orders == 1);

    const int activity_before = server.activity.load();
    server.publish_ask("100", "4");
    REQUIRE(server.activity.load() == activity_before + 1);
    PaperOrderRecord partial = order_of(server, account_id, "b1");
    REQUIRE(partial.state->status == PaperOrderStatus::PartiallyFilled);
    REQUIRE(partial.state->filled_quantity == quantity("4"));
    REQUIRE(partial.state->reservation.cash == money("600"));

    // An observation of another symbol does not touch it.
    server.publish_ask("95", "100", "SPY");
    REQUIRE(order_of(server, account_id, "b1").state->filled_quantity == quantity("4"));

    server.publish_ask("99.5", "10");
    const PaperOrderRecord filled = order_of(server, account_id, "b1");
    REQUIRE(filled.state->status == PaperOrderStatus::Filled);
    REQUIRE(filled.state->fill_count == 2);
    REQUIRE(filled.state->filled_notional == money("997"));
    REQUIRE(filled.state->cumulative_fee == money("0.01"));

    auto account = server.service.get_account(account_id);
    const auto& view = std::get<PaperAccountView>(account);
    REQUIRE(view.state.cash == money("9002.99"));
    REQUIRE(view.reservations.reserved_cash == Money{});
    REQUIRE(view.working_orders == 0);
    const auto status = server.matcher.status();
    REQUIRE(status.state == WorkingOrderMatcherState::Running);
    REQUIRE(status.observations == 4);
    REQUIRE(status.fills == 2);
    REQUIRE(status.retries == 0);
}

void test_market_orders_cannot_rest_and_consume_their_id() {
    Server server;
    const std::string account_id = server.create_account("svc-market-gtc");
    server.publish_ask("101", "5");
    PaperOrderRequest market = chronos::test::order_request(account_id, "m1", Side::Buy, "1");
    market.time_in_force = TimeInForce::GoodTilCancelled;
    const RecordedPaperOrder rejected = recorded(server.service.submit_order(market));
    const auto& risk = std::get<chronos::PreTradeRiskRejection>(rejected.result);
    REQUIRE(risk.code == chronos::PreTradeRiskErrorCode::MarketOrderRequiresImmediateOrCancel);
    REQUIRE(!rejected.order.state.has_value());
    // The client order ID is consumed, as for every recorded rejection.
    failure(
        server.service.submit_order(gtc(account_id, "m1", Side::Buy, "1", "100")),
        PaperTradingErrorCode::DuplicateClientOrderId
    );
}

void test_cancellation_rules() {
    Server server;
    const std::string account_id = server.create_account("svc-cancel");
    server.publish_ask("101", "5");
    for (const std::string& id : {std::string("b1"), std::string("b2"), std::string("b3")}) {
        static_cast<void>(recorded(server.service.submit_order(gtc(account_id, id, Side::Buy, "10", "100"))));
    }
    static_cast<void>(recorded(server.service.submit_order(
        chronos::test::order_request(account_id, "ioc-1", Side::Buy, "1")
    )));

    const CancelledPaperOrder first = cancelled(
        server.service.cancel_order(cancel_request(account_id, "b1", "cancel-1"))
    );
    REQUIRE(!first.already_cancelled);
    REQUIRE(first.order.state->status == PaperOrderStatus::Cancelled);
    REQUIRE(first.order.state->cancellation_reason == SimulatedCancellationReason::UserRequested);
    REQUIRE(first.order.state->cancel_request_id == "cancel-1");
    REQUIRE(first.account.reservations.reserved_cash == money("2000.02"));

    // The same request again changes nothing and returns the order.
    const CancelledPaperOrder retried = cancelled(
        server.service.cancel_order(cancel_request(account_id, "b1", "cancel-1"))
    );
    REQUIRE(retried.already_cancelled);
    REQUIRE(retried.order.state == first.order.state);
    REQUIRE(retried.account.version == first.account.version);

    // Another request against a terminal order is refused with the order.
    const PaperTradingError terminal = failure(
        server.service.cancel_order(cancel_request(account_id, "b1", "cancel-2")),
        PaperTradingErrorCode::OrderNotCancellable
    );
    REQUIRE(terminal.order->state->status == PaperOrderStatus::Cancelled);
    // So is an immediate-or-cancel order, which never works.
    const PaperTradingError immediate = failure(
        server.service.cancel_order(cancel_request(account_id, "ioc-1", "cancel-3")),
        PaperTradingErrorCode::OrderNotCancellable
    );
    REQUIRE(immediate.order->intent.time_in_force == TimeInForce::ImmediateOrCancel);
    // One request ID cancels one order.
    failure(
        server.service.cancel_order(cancel_request(account_id, "b2", "cancel-1")),
        PaperTradingErrorCode::DuplicateCancelRequestId
    );
    REQUIRE(order_of(server, account_id, "b2").state->status == PaperOrderStatus::Working);
    // Unknown orders and accounts stay distinguishable.
    failure(
        server.service.cancel_order(cancel_request(account_id, "missing", "cancel-4")),
        PaperTradingErrorCode::OrderNotFound
    );
    failure(
        server.service.cancel_order(cancel_request("missing-account", "b2", "cancel-4")),
        PaperTradingErrorCode::AccountNotFound
    );
    failure(
        server.service.cancel_order(cancel_request(account_id, "b2", "has space")),
        PaperTradingErrorCode::InvalidIdentifier
    );

    // A partial fill keeps its fill and fee when the remainder is cancelled.
    server.publish_ask("100", "4");
    const CancelledPaperOrder partial = cancelled(
        server.service.cancel_order(cancel_request(account_id, "b2", "cancel-5"))
    );
    REQUIRE(partial.order.state->status == PaperOrderStatus::PartiallyFilledAndCancelled);
    REQUIRE(partial.order.state->filled_quantity == quantity("4"));
    REQUIRE(partial.order.state->cumulative_fee == money("0.01"));
    // Cancelled at the market clock, after its fill.
    REQUIRE(partial.order.state->updated_at == time_of(1));

    // Cancellation needs no market data and works on a halted account.
    PostgresTestConnection editor(connection_string);
    static_cast<void>(editor.query(
        "UPDATE paper_accounts SET is_healthy = false WHERE account_id = $1",
        {account_id}
    ));
    server.market->suspend();
    const CancelledPaperOrder halted = cancelled(
        server.service.cancel_order(cancel_request(account_id, "b3", "cancel-6"))
    );
    REQUIRE(halted.order.state->status == PaperOrderStatus::Cancelled);
    REQUIRE(!halted.account.is_healthy);
    REQUIRE(halted.account.reservations.reserved_cash == Money{});
}

void test_working_order_and_per_order_pages_bind_their_cursors() {
    Server server;
    const std::string account_id = server.create_account("svc-pages");
    const std::string other_account = server.create_account("svc-pages-other");
    server.publish_ask("101", "5");
    server.publish_ask("501", "5", "SPY");
    for (int index = 0; index < 3; ++index) {
        static_cast<void>(recorded(server.service.submit_order(
            gtc(account_id, "a" + std::to_string(index), Side::Buy, "10", "100")
        )));
    }
    PaperOrderRequest spy = gtc(account_id, "s0", Side::Buy, "1", "500");
    spy.instrument = "SPY";
    static_cast<void>(recorded(server.service.submit_order(spy)));

    const auto first = std::get<WorkingOrderPage>(server.service.list_working_orders(
        account_id,
        ForwardPageRequest{.limit = 2, .cursor = std::nullopt, .instrument = std::nullopt}
    ));
    REQUIRE(first.orders.size() == 2);
    REQUIRE(first.orders[0].intent.client_order_id == "a0");
    REQUIRE(first.next_cursor.has_value());
    const auto second = std::get<WorkingOrderPage>(server.service.list_working_orders(
        account_id,
        ForwardPageRequest{.limit = 2, .cursor = first.next_cursor, .instrument = std::nullopt}
    ));
    REQUIRE(second.orders.size() == 2);
    REQUIRE(second.orders[0].intent.client_order_id == "a2");
    REQUIRE(second.orders[1].intent.client_order_id == "s0");
    REQUIRE(!second.next_cursor.has_value());
    const auto spy_only = std::get<WorkingOrderPage>(server.service.list_working_orders(
        account_id,
        ForwardPageRequest{.limit = 10, .cursor = std::nullopt, .instrument = "SPY"}
    ));
    REQUIRE(spy_only.orders.size() == 1);

    // A cursor is bound to its account, its filter, and its kind.
    failure(
        server.service.list_working_orders(
            other_account,
            ForwardPageRequest{.limit = 2, .cursor = first.next_cursor, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::InvalidCursor
    );
    failure(
        server.service.list_working_orders(
            account_id,
            ForwardPageRequest{.limit = 2, .cursor = first.next_cursor, .instrument = "AAPL"}
        ),
        PaperTradingErrorCode::InvalidCursor
    );
    failure(
        server.service.list_orders(
            account_id,
            HistoryPageRequest{.limit = 2, .cursor = first.next_cursor, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::InvalidCursor
    );
    failure(
        server.service.list_working_orders(
            account_id,
            ForwardPageRequest{.limit = 0, .cursor = std::nullopt, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::InvalidPageLimit
    );
    failure(
        server.service.list_working_orders(
            "missing-account",
            ForwardPageRequest{.limit = 2, .cursor = std::nullopt, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::AccountNotFound
    );

    // Three observations fill a0 in three fills.
    for (const std::string_view size : {"4", "4", "2"}) {
        server.publish_ask("100", size);
    }
    const auto fills = std::get<OrderFillPage>(server.service.list_order_fills(
        account_id,
        "a0",
        ForwardPageRequest{.limit = 2, .cursor = std::nullopt, .instrument = std::nullopt}
    ));
    REQUIRE(fills.fills.size() == 2);
    REQUIRE(fills.fills[0].fill.fill_id == "a0:fill:0");
    REQUIRE(fills.next_cursor.has_value());
    const auto rest = std::get<OrderFillPage>(server.service.list_order_fills(
        account_id,
        "a0",
        ForwardPageRequest{.limit = 2, .cursor = fills.next_cursor, .instrument = std::nullopt}
    ));
    REQUIRE(rest.fills.size() == 1);
    REQUIRE(rest.fills[0].fill.fill_id == "a0:fill:2");
    // A fills cursor is bound to its order and its kind.
    failure(
        server.service.list_order_fills(
            account_id,
            "a1",
            ForwardPageRequest{.limit = 2, .cursor = fills.next_cursor, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::InvalidCursor
    );
    failure(
        server.service.list_order_events(
            account_id,
            "a0",
            ForwardPageRequest{.limit = 2, .cursor = fills.next_cursor, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::InvalidCursor
    );
    const auto events = std::get<OrderEventPage>(server.service.list_order_events(
        account_id,
        "a0",
        ForwardPageRequest{.limit = 50, .cursor = std::nullopt, .instrument = std::nullopt}
    ));
    REQUIRE(events.events.size() == 10);
    REQUIRE(!events.next_cursor.has_value());
    failure(
        server.service.list_order_events(
            account_id,
            "missing",
            ForwardPageRequest{.limit = 2, .cursor = std::nullopt, .instrument = std::nullopt}
        ),
        PaperTradingErrorCode::OrderNotFound
    );
}

// While an event is applied and matched, a submission waits; past its wait
// it reports matching as busy and records nothing.
void test_submissions_wait_for_the_market_event_gate() {
    PaperTradingServiceConfig config = chronos::test::paper_service_config();
    config.order_matching_wait = std::chrono::milliseconds(50);
    Server server(connection_string, connection_string, config);
    const std::string account_id = server.create_account("svc-gate");
    server.publish_ask("101", "5");
    {
        auto applying = server.gate.apply_event();
        failure(
            server.service.submit_order(gtc(account_id, "b1", Side::Buy, "1", "100")),
            PaperTradingErrorCode::OrderMatchingBusy
        );
        // Reads and cancellations do not wait for the gate.
        REQUIRE(std::holds_alternative<PaperAccountView>(server.service.get_account(account_id)));
    }
    failure(server.service.get_order(account_id, "b1"), PaperTradingErrorCode::OrderNotFound);
    static_cast<void>(recorded(server.service.submit_order(gtc(account_id, "b1", Side::Buy, "1", "100"))));
}

// Submissions waiting for the gate hold no lock that other requests need:
// reads, cancellations, and health answer at once, and queued submissions
// share one deadline instead of waiting one after another.
void test_waiting_submissions_block_no_other_request() {
    using Clock = std::chrono::steady_clock;
    constexpr auto wait = std::chrono::milliseconds(3'000);
    PaperTradingServiceConfig config = chronos::test::paper_service_config();
    config.order_matching_wait = wait;
    Server server(connection_string, connection_string, config);
    const std::string account_id = server.create_account("svc-gate-wait");
    server.publish_ask("101", "5");
    auto applying = server.gate.apply_event();
    const Clock::time_point started = Clock::now();
    std::vector<std::thread> submissions;
    std::vector<std::optional<chronos::paper_trading::PaperOrderSubmissionResult>> results(2);
    for (std::size_t index = 0; index < results.size(); ++index) {
        submissions.emplace_back([&server, &results, &account_id, index] {
            results[index] = server.service.submit_order(
                gtc(account_id, "wait-" + std::to_string(index), Side::Buy, "1", "100")
            );
        });
    }
    // Both submissions are waiting, one for the gate and one to be next.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Measured while they wait; checked once they have finished.
    Clock::duration slowest{};
    const auto timed = [&slowest](auto&& request) {
        const Clock::time_point begun = Clock::now();
        auto result = request();
        slowest = std::max(slowest, Clock::now() - begun);
        return result;
    };
    const auto account = timed([&] { return server.service.get_account(account_id); });
    const auto health = timed([&] { return server.service.health(); });
    auto cancel = timed([&] {
        return server.service.cancel_order(CancelPaperOrderRequest{
            .account_id = account_id,
            .client_order_id = "missing",
            .cancel_request_id = "cancel-missing",
        });
    });

    for (std::thread& submission : submissions) {
        submission.join();
    }
    const auto elapsed = Clock::now() - started;
    REQUIRE(slowest < std::chrono::milliseconds(1'500));
    REQUIRE(std::holds_alternative<PaperAccountView>(account));
    REQUIRE(health.persistence_available);
    failure(std::move(cancel), PaperTradingErrorCode::OrderNotFound);
    REQUIRE(elapsed >= wait);
    REQUIRE(elapsed < wait + std::chrono::milliseconds(1'500));
    for (auto& result : results) {
        REQUIRE(result.has_value());
        failure(std::move(*result), PaperTradingErrorCode::OrderMatchingBusy);
    }
}

// An order accepted before an observation is offered it even when its
// submission raced the observation's arrival: the gate orders them. Each
// ask at 100 has one share, which goes to the oldest order working when it
// arrives. So once the oldest order still working was accepted, every later
// ask at 100 must have filled some order.
void test_every_later_observation_is_offered_to_a_new_order() {
    Server server;
    const std::string account_id = server.create_account("svc-race");
    server.publish_ask("101", "5");
    std::atomic<bool> done{false};
    constexpr std::uint64_t kLast = 200;
    std::thread source([&] {
        for (std::uint64_t sequence = 1; sequence < kLast; ++sequence) {
            server.publish_ask(sequence % 2 == 0 ? "100" : "101", "1");
        }
        done.store(true);
    });
    int orders = 0;
    while (!done.load() && orders < 40) {
        static_cast<void>(recorded(server.service.submit_order(
            gtc(account_id, "r" + std::to_string(orders), Side::Buy, "1", "100")
        )));
        ++orders;
    }
    source.join();
    REQUIRE(orders > 0);

    PostgresTestConnection inspector(connection_string);
    std::vector<std::uint64_t> filled_at;
    for (const auto& row : inspector.query(
             "SELECT market_ingest_sequence FROM paper_fills WHERE account_id = $1 "
             "ORDER BY market_ingest_sequence",
             {account_id}
         )) {
        filled_at.push_back(std::stoull(row.front()));
    }
    const auto working = std::get<WorkingOrderPage>(server.service.list_working_orders(
        account_id,
        ForwardPageRequest{.limit = 50, .cursor = std::nullopt, .instrument = std::nullopt}
    ));
    const std::uint64_t oldest_working = working.orders.empty()
        ? kLast
        : working.orders.front().market.header.ingest_sequence;
    for (std::uint64_t sequence = oldest_working + 1; sequence < kLast; ++sequence) {
        if (sequence % 2 == 0) {
            REQUIRE(std::find(filled_at.begin(), filled_at.end(), sequence) != filled_at.end());
        }
    }
    // No observation filled twice.
    REQUIRE(std::adjacent_find(filled_at.begin(), filled_at.end()) == filled_at.end());
    REQUIRE(server.matcher.status().retries == 0);
}

void test_an_uncertain_cancellation_is_reconciled_and_retried() {
    const std::string application = unique_test_id("svc-cancel-loss");
    Server server(with_application_name(connection_string, application));
    const std::string account_id = server.create_account("svc-cancel-loss");
    server.publish_ask("101", "5");
    static_cast<void>(recorded(server.service.submit_order(gtc(account_id, "b1", Side::Buy, "1", "100"))));

    PostgresTestConnection holder(connection_string);
    PostgresTestConnection observer(connection_string);
    static_cast<void>(holder.query("BEGIN"));
    static_cast<void>(holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));
    std::optional<chronos::paper_trading::CancelPaperOrderResult> outcome;
    std::thread canceller([&] {
        outcome = server.service.cancel_order(cancel_request(account_id, "b1", "cancel-1"));
    });
    wait_until_blocked_on_lock(observer, application);
    terminate_backends(observer, application);
    canceller.join();
    static_cast<void>(holder.query("ROLLBACK"));
    failure(std::move(*outcome), PaperTradingErrorCode::PersistenceUnavailable);

    // The client reads the order, finds it working, and retries the same ID.
    REQUIRE(order_of(server, account_id, "b1").state->status == PaperOrderStatus::Working);
    const CancelledPaperOrder retried = cancelled(
        server.service.cancel_order(cancel_request(account_id, "b1", "cancel-1"))
    );
    REQUIRE(!retried.already_cancelled);
    REQUIRE(retried.order.state->status == PaperOrderStatus::Cancelled);
    // A retry after an uncertain success finds it cancelled by that ID.
    REQUIRE(cancelled(
        server.service.cancel_order(cancel_request(account_id, "b1", "cancel-1"))
    ).already_cancelled);
}

void test_the_matcher_retries_through_a_lost_connection() {
    const std::string application = unique_test_id("svc-matcher-loss");
    // The first retry waits long enough for the terminated backend to be
    // seen gone before the matcher reconnects.
    Server server(
        connection_string,
        with_application_name(connection_string, application),
        chronos::test::paper_service_config(),
        std::nullopt,
        WorkingOrderMatcherOptions{
            .first_retry_delay = std::chrono::milliseconds(300),
            .maximum_retry_delay = std::chrono::milliseconds(300),
        }
    );
    const std::string account_id = server.create_account("svc-matcher-loss");
    server.publish_ask("101", "5");
    static_cast<void>(recorded(server.service.submit_order(gtc(account_id, "b1", Side::Buy, "4", "100"))));

    // The matcher waits for the account while another session holds it; its
    // connection is then lost, so it reconnects and books the fill once.
    PostgresTestConnection holder(connection_string);
    PostgresTestConnection observer(connection_string);
    static_cast<void>(holder.query("BEGIN"));
    static_cast<void>(holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));
    std::thread source([&] { server.publish_ask("100", "4"); });
    wait_until_blocked_on_lock(observer, application);
    terminate_backends(observer, application);
    static_cast<void>(holder.query("ROLLBACK"));
    source.join();

    const PaperOrderRecord filled = order_of(server, account_id, "b1");
    REQUIRE(filled.state->status == PaperOrderStatus::Filled);
    REQUIRE(filled.state->fill_count == 1);
    const auto status = server.matcher.status();
    REQUIRE(status.retries >= 1);
    REQUIRE(status.fills == 1);
    REQUIRE(status.state == WorkingOrderMatcherState::Running);
}

// A lock that is never released cannot hold shutdown: the matcher's lock
// wait times out, it retries, and stop() ends the retries.
void test_stopping_ends_a_matcher_waiting_for_a_held_account() {
    Server server(
        connection_string,
        connection_string,
        chronos::test::paper_service_config(),
        std::chrono::milliseconds(50)
    );
    const std::string account_id = server.create_account("svc-matcher-stop");
    server.publish_ask("101", "5");
    static_cast<void>(recorded(server.service.submit_order(gtc(account_id, "b1", Side::Buy, "4", "100"))));

    PostgresTestConnection holder(connection_string);
    static_cast<void>(holder.query("BEGIN"));
    static_cast<void>(holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));
    std::thread source([&] { server.publish_ask("100", "4"); });
    REQUIRE(chronos::test::eventually([&] {
        return server.matcher.status().state == WorkingOrderMatcherState::Retrying;
    }));
    server.matcher.stop();
    source.join();
    static_cast<void>(holder.query("ROLLBACK"));
    REQUIRE(server.matcher.status().state == WorkingOrderMatcherState::Stopped);
    REQUIRE(order_of(server, account_id, "b1").state->status == PaperOrderStatus::Working);
    // A stopped matcher offers later observations to nobody.
    server.publish_ask("100", "4");
    REQUIRE(order_of(server, account_id, "b1").state->status == PaperOrderStatus::Working);
}

void test_a_restarted_server_recovers_and_continues_working_orders() {
    std::string account_id;
    const std::string stream = unique_test_id("gtc-restart-stream");
    const auto server_on_stream = [&] {
        return std::make_unique<Server>(
            connection_string,
            connection_string,
            chronos::test::paper_service_config(),
            std::nullopt,
            fast_retries(),
            stream
        );
    };
    {
        const auto first_server = server_on_stream();
        Server& first = *first_server;
        account_id = first.create_account("svc-restart");
        first.publish_ask("101", "5");
        static_cast<void>(recorded(first.service.submit_order(gtc(account_id, "b1", Side::Buy, "10", "100"))));
        first.publish_ask("100", "3");
    }
    const auto restarted_server = server_on_stream();
    Server& restarted = *restarted_server;
    auto working = std::get<WorkingOrderPage>(restarted.service.list_working_orders(
        account_id,
        ForwardPageRequest{.limit = 10, .cursor = std::nullopt, .instrument = std::nullopt}
    ));
    REQUIRE(working.orders.size() == 1);
    REQUIRE(working.orders[0].state->filled_quantity == quantity("3"));
    // A replay from the start offers nothing the account already used...
    restarted.publish_ask("101", "5");
    restarted.publish_ask("100", "3");
    REQUIRE(order_of(restarted, account_id, "b1").state->filled_quantity == quantity("3"));
    // ...and continues with every later observation.
    restarted.publish_ask("100", "10");
    REQUIRE(order_of(restarted, account_id, "b1").state->status == PaperOrderStatus::Filled);
}

// A replay paused and resumed repeatedly books exactly the fills of one
// played straight through, and starting and stopping sources and matchers
// repeatedly leaves nothing running.
void test_pause_resume_and_repeated_start_stop_are_exact() {
    const auto events = [](const Server& server, std::uint64_t first) {
        std::deque<MarketEventRead> reads;
        for (std::uint64_t sequence = first; sequence < first + 60; ++sequence) {
            TopOfBookEvent event = ask_at(sequence, sequence % 3 == 0 ? "100" : "101", "1");
            event.header.stream = chronos::MarketDataStreamId(server.stream);
            reads.emplace_back(chronos::test::validated(MarketEvent(event)));
        }
        return reads;
    };
    const auto fills_of = [](const std::string& account_id) {
        PostgresTestConnection inspector(connection_string);
        return inspector.scalar(
            "SELECT coalesce(string_agg(fill_id || '@' || market_ingest_sequence, ',' "
            "ORDER BY fill_record_id), '') FROM paper_fills WHERE account_id = $1",
            {account_id}
        );
    };
    const auto run = [&](bool pause) {
        Server server;
        const std::string account_id = server.create_account(pause ? "svc-paused" : "svc-straight");
        server.publish_ask("101", "5");
        static_cast<void>(recorded(server.service.submit_order(gtc(account_id, "b1", Side::Buy, "30", "100"))));
        {
            market_data::PacedReplaySource replay(
                std::make_unique<chronos::test::ScriptedReader>(events(server, 1)),
                server.feed,
                server.clock,
                market_data::MarketDataSourceDescription{},
                market_data::PacedReplayOptions{
                    .speed = chronos::ReplaySpeed::Maximum,
                    .start_immediately = true,
                }
            );
            if (pause) {
                for (int round = 0; round < 5; ++round) {
                    static_cast<void>(replay.control_replay(market_data::ReplayCommand::Pause));
                    static_cast<void>(replay.control_replay(market_data::ReplayCommand::Resume));
                }
            }
            REQUIRE(chronos::test::eventually([&] {
                return replay.status().state == market_data::MarketDataSourceState::Completed;
            }));
        }
        return fills_of(account_id);
    };
    const std::string straight = run(false);
    REQUIRE(!straight.empty());
    // The client order ID is part of the fill ID, and both runs used "b1".
    REQUIRE(run(true) == straight);

    for (int cycle = 0; cycle < 5; ++cycle) {
        Server server;
        market_data::PacedReplaySource replay(
            std::make_unique<chronos::test::ScriptedReader>(events(server, 0)),
            server.feed,
            server.clock,
            market_data::MarketDataSourceDescription{},
            market_data::PacedReplayOptions{
                .speed = chronos::ReplaySpeed::Maximum,
                .start_immediately = cycle % 2 == 0,
            }
        );
        server.matcher.stop();
        replay.stop();
    }
}

// Throughput of matching as the server runs it: one indexed candidate query
// per top of book, plus one account transaction per fill. Printed for the
// record; the bound only catches a pathological regression.
void test_matching_throughput_is_measured() {
    Server server;
    const std::string account_id = server.create_account("svc-throughput", "100000");
    server.publish_ask("101", "5");
    for (int index = 0; index < 20; ++index) {
        static_cast<void>(recorded(server.service.submit_order(
            gtc(account_id, "t" + std::to_string(index), Side::Buy, "1", "50")
        )));
    }
    constexpr std::uint64_t kObservations = 1'000;
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t index = 0; index < kObservations; ++index) {
        server.publish_ask("101", "5");
    }
    const auto unmarketable = std::chrono::steady_clock::now() - started;
    const auto filling_started = std::chrono::steady_clock::now();
    for (int index = 0; index < 20; ++index) {
        server.publish_ask("50", "1");
    }
    const auto filling = std::chrono::steady_clock::now() - filling_started;
    const double per_observation_us =
        std::chrono::duration<double, std::micro>(unmarketable).count() / kObservations;
    const double per_fill_us = std::chrono::duration<double, std::micro>(filling).count() / 20;
    std::cout << "matching throughput: " << per_observation_us
              << " us per observation with 20 unmarketable working orders ("
              << 1e6 / per_observation_us << " observations/s); " << per_fill_us
              << " us per observation that books a fill\n";
    REQUIRE(server.matcher.status().fills == 20);
    REQUIRE(per_observation_us < 50'000);
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "Working-order service test skipped: "
                     "CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    {
        PostgresPaperOrderRepository migrations(connection_string);
        migrations.apply_migration(all_migrations_sql());
    }
    return chronos::test::run_tests({
        {"a working order fills from later observations", test_a_working_order_fills_from_later_observations},
        {"market orders cannot rest", test_market_orders_cannot_rest_and_consume_their_id},
        {"cancellation rules", test_cancellation_rules},
        {"pages bind their cursors", test_working_order_and_per_order_pages_bind_their_cursors},
        {"submissions wait for the gate", test_submissions_wait_for_the_market_event_gate},
        {"waiting submissions block no other request", test_waiting_submissions_block_no_other_request},
        {"every later observation reaches a new order", test_every_later_observation_is_offered_to_a_new_order},
        {"uncertain cancellation is reconciled", test_an_uncertain_cancellation_is_reconciled_and_retried},
        {"matcher retries through a lost connection", test_the_matcher_retries_through_a_lost_connection},
        {"stop ends a waiting matcher", test_stopping_ends_a_matcher_waiting_for_a_held_account},
        {"restart recovers working orders", test_a_restarted_server_recovers_and_continues_working_orders},
        {"pause, resume, start, and stop are exact", test_pause_resume_and_repeated_start_stop_are_exact},
        {"matching throughput is measured", test_matching_throughput_is_measured},
    });
}
