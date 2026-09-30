// The HTTP contract of good-til-cancelled orders: strict submission and
// cancel parsing, order lookup as the order changes, working-order and
// per-order pages with bound cursors, stable errors, uncertain outcomes, the
// matcher in health, and responsiveness while a replay is matched.
#include "chronos/market_data/market_data_notifier.hpp"
#include "chronos/market_data/market_data_source.hpp"
#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_data/market_state_feed.hpp"
#include "chronos/market_data/monotonic_clock.hpp"
#include "chronos/market_data/paced_replay_source.hpp"
#include "chronos/paper_trading/paper_trading_http_api.hpp"
#include "chronos/paper_trading/working_order_matcher.hpp"
#include "json_test_support.hpp"
#include "market_data_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace market_data = chronos::market_data;
using chronos::MarketEvent;
using chronos::MarketEventRead;
using chronos::PostgresPaperOrderRepository;
using chronos::TopOfBookEvent;
using chronos::paper_trading::HttpRequest;
using chronos::paper_trading::HttpResponse;
using chronos::paper_trading::PaperTradingHttpApi;
using chronos::paper_trading::PaperTradingService;
using chronos::paper_trading::WorkingOrderMatcher;
using chronos::paper_trading::WorkingOrderMatcherOptions;
using chronos::test::all_migrations_sql;
using chronos::test::PostgresTestConnection;
using chronos::test::QuoteLevels;
using chronos::test::quote;
using chronos::test::require_no_json_numbers;
using chronos::test::terminate_backends;
using chronos::test::unique_test_id;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;
using Json = nlohmann::json;

std::string connection_string;

// The server's wiring on a stream of its own (see the service tests).
struct GtcApi {
    explicit GtcApi(std::string service_connection = connection_string)
        : stream(unique_test_id("gtc-http-stream")),
          market(std::make_unique<chronos::MarketState>(chronos::MarketDataStreamId(stream))),
          notifier(clock, market_data::MarketDataNotifier::Options{}),
          matcher(
              std::make_unique<PostgresPaperOrderRepository>(connection_string),
              WorkingOrderMatcherOptions{
                  .first_retry_delay = std::chrono::milliseconds(5),
                  .maximum_retry_delay = std::chrono::milliseconds(20),
              },
              [this] { notifier.publish_activity(); }
          ),
          feed(*market, notifier, 0, &matcher, &gate),
          source(std::make_unique<market_data::NoMarketDataSource>(
              chronos::test::paper_service_config().tradeable_instruments
          )),
          service(
              std::make_unique<PostgresPaperOrderRepository>(std::move(service_connection)),
              *market,
              chronos::test::paper_service_config(),
              [this] { notifier.publish_activity(); },
              &gate
          ),
          api(service, *source, notifier, [](std::string_view) {}, &matcher) {}

    void publish_ask(std::string_view ask, std::string_view size, std::string_view instrument = "AAPL") {
        TopOfBookEvent event = quote(
            next_sequence,
            static_cast<std::int64_t>(next_sequence) * 1'000,
            std::string(instrument),
            QuoteLevels{.bid_price = "1", .bid_quantity = "5", .ask_price = ask, .ask_quantity = size}
        );
        event.header.stream = chronos::MarketDataStreamId(stream);
        const auto rejected = feed.apply(chronos::test::validated(MarketEvent(event)));
        if (rejected) {
            throw chronos::test::TestFailure(*rejected);
        }
        ++next_sequence;
    }

    std::string stream;
    market_data::SteadyClock clock;
    std::unique_ptr<chronos::MarketState> market;
    market_data::MarketDataNotifier notifier;
    market_data::MarketEventGate gate;
    WorkingOrderMatcher matcher;
    market_data::MarketStateFeed feed;
    std::unique_ptr<market_data::MarketDataSource> source;
    PaperTradingService service;
    PaperTradingHttpApi api;
    std::uint64_t next_sequence{0};
};

struct ApiResponse {
    int status;
    Json body;
};

ApiResponse call(
    GtcApi& server,
    std::string method,
    const std::string& target,
    std::string body = {},
    std::string content_type = "application/json"
) {
    const std::size_t question = target.find('?');
    const HttpResponse response = server.api.handle(HttpRequest{
        .method = std::move(method),
        .path = target.substr(0, question),
        .query = question == std::string::npos ? std::string() : target.substr(question + 1),
        .content_type = std::move(content_type),
        .body = std::move(body),
    });
    Json parsed = Json::parse(response.body);
    require_no_json_numbers(parsed);
    return ApiResponse{.status = response.status, .body = std::move(parsed)};
}

Json require_error(const ApiResponse& response, int status, std::string_view code) {
    if (response.status != status || response.body.at("error").at("code") != code) {
        throw chronos::test::TestFailure(
            "expected " + std::to_string(status) + " " + std::string(code) + ", got " +
            std::to_string(response.status) + " " + response.body.dump()
        );
    }
    return response.body.at("error");
}

std::string account_path(const std::string& account_id) {
    return "/api/v1/accounts/" + account_id;
}

std::string order_path(const std::string& account_id, std::string_view client_order_id) {
    return account_path(account_id) + "/orders/" + std::string(client_order_id);
}

std::string new_account(GtcApi& server, std::string_view prefix) {
    const std::string account_id = unique_test_id(prefix);
    const ApiResponse created = call(
        server,
        "POST",
        "/api/v1/accounts",
        Json{{"account_id", account_id}, {"starting_cash", "10000"}}.dump()
    );
    REQUIRE(created.status == 201);
    return account_id;
}

Json order_body(
    std::string client_order_id,
    std::string_view side,
    std::string_view quantity,
    std::optional<std::string_view> limit,
    std::string_view time_in_force
) {
    Json body{
        {"client_order_id", std::move(client_order_id)},
        {"source", {{"kind", "manual"}, {"id", "local-trader"}}},
        {"instrument", "AAPL"},
        {"side", side},
        {"type", limit ? "limit" : "market"},
        {"quantity", quantity},
        {"time_in_force", time_in_force},
    };
    if (limit) {
        body["limit_price"] = *limit;
    }
    return body;
}

ApiResponse submit_gtc(
    GtcApi& server,
    const std::string& account_id,
    std::string client_order_id,
    std::string_view quantity = "10",
    std::string_view limit = "100"
) {
    return call(
        server,
        "POST",
        account_path(account_id) + "/orders",
        order_body(std::move(client_order_id), "buy", quantity, limit, "gtc").dump()
    );
}

ApiResponse cancel(
    GtcApi& server,
    const std::string& account_id,
    std::string_view client_order_id,
    std::string body
) {
    return call(server, "POST", order_path(account_id, client_order_id) + "/cancel", std::move(body));
}

std::string cancel_body(std::string_view cancel_request_id) {
    return Json{{"cancel_request_id", cancel_request_id}}.dump();
}

void test_gtc_orders_are_accepted_and_change_as_they_fill() {
    GtcApi server;
    const std::string account_id = new_account(server, "http-gtc");
    server.publish_ask("101", "5");

    const ApiResponse accepted = submit_gtc(server, account_id, "b1");
    REQUIRE(accepted.status == 201);
    REQUIRE(!accepted.body.contains("error"));
    const Json& order = accepted.body.at("order");
    REQUIRE(order.at("time_in_force") == "gtc");
    REQUIRE(order.at("status") == "working");
    REQUIRE(order.at("quantity") == "10");
    REQUIRE(order.at("filled_quantity") == "0");
    REQUIRE(order.at("remaining_quantity") == "10");
    REQUIRE(order.at("fill_count") == "0");
    REQUIRE(order.at("fee_charged") == false);
    REQUIRE(order.at("cumulative_fee") == "0");
    REQUIRE(order.at("reserved_cash") == "1000.01");
    REQUIRE(order.at("reserved_quantity") == "0");
    REQUIRE(order.at("priority").is_string());
    REQUIRE(order.at("fill").is_null());
    REQUIRE(order.at("cancellation_reason").is_null());
    REQUIRE(accepted.body.at("account").at("reserved_cash") == "1000.01");
    REQUIRE(accepted.body.at("account").at("available_cash") == "8999.99");
    REQUIRE(accepted.body.at("account").at("working_order_count") == "1");

    server.publish_ask("100", "4");
    const ApiResponse partial = call(server, "GET", order_path(account_id, "b1"));
    REQUIRE(partial.status == 200);
    REQUIRE(partial.body.at("order").at("status") == "partially_filled");
    REQUIRE(partial.body.at("order").at("filled_quantity") == "4");
    REQUIRE(partial.body.at("order").at("remaining_quantity") == "6");
    REQUIRE(partial.body.at("order").at("filled_notional") == "400");
    REQUIRE(partial.body.at("order").at("fee_charged") == true);
    REQUIRE(partial.body.at("order").at("cumulative_fee") == "0.01");
    REQUIRE(partial.body.at("order").at("reserved_cash") == "600");
    // The submission rested, so the resource has no submission-time fill.
    REQUIRE(partial.body.at("order").at("fill").is_null());

    server.publish_ask("99.5", "10");
    const Json filled = call(server, "GET", order_path(account_id, "b1")).body.at("order");
    REQUIRE(filled.at("status") == "filled");
    REQUIRE(filled.at("fill_count") == "2");
    REQUIRE(filled.at("reserved_cash") == "0");

    const ApiResponse history = call(server, "GET", account_path(account_id) + "/orders");
    REQUIRE(history.body.at("orders")[0].at("status") == "filled");
    const ApiResponse fills = call(server, "GET", account_path(account_id) + "/fills");
    REQUIRE(fills.body.at("fills").size() == 2);
    REQUIRE(fills.body.at("fills")[0].at("fill_id") == "b1:fill:1");
    REQUIRE(fills.body.at("fills")[0].at("fee") == "0");
    REQUIRE(fills.body.at("fills")[1].at("fee") == "0.01");

    const ApiResponse account = call(server, "GET", account_path(account_id));
    REQUIRE(account.body.at("account").at("cash") == "9002.99");
    REQUIRE(account.body.at("account").at("positions")[0].at("available_quantity") == "10");

    const Json health = call(server, "GET", "/health").body;
    REQUIRE(health.at("order_matching").at("state") == "running");
    REQUIRE(health.at("order_matching").at("fills") == "2");
    REQUIRE(health.at("order_matching").at("observations") == "3");
}

void test_submission_parsing_is_strict_about_time_in_force() {
    GtcApi server;
    const std::string account_id = new_account(server, "http-gtc-parse");
    server.publish_ask("101", "5");
    for (const std::string_view invalid : {"GTC", "fok", "day", ""}) {
        const Json error = require_error(
            call(server, "POST", account_path(account_id) + "/orders",
                 order_body("x", "buy", "1", "100", invalid).dump()),
            400,
            "malformed_request"
        );
        REQUIRE(error.at("reason") == "unknown_enum_value");
        REQUIRE(error.at("field") == "time_in_force");
    }
    // A market order cannot rest: a recorded risk rejection.
    const ApiResponse market = call(
        server,
        "POST",
        account_path(account_id) + "/orders",
        order_body("m1", "buy", "1", std::nullopt, "gtc").dump()
    );
    const Json error = require_error(market, 422, "risk_rejected");
    REQUIRE(error.at("reason") == "market_order_requires_ioc");
    REQUIRE(error.at("field") == "time_in_force");
    REQUIRE(market.body.at("order").at("status") == "rejected");
    REQUIRE(market.body.at("order").at("priority").is_null());
    require_error(
        call(server, "POST", account_path(account_id) + "/orders",
             order_body("m1", "buy", "1", "100", "gtc").dump()),
        409,
        "duplicate_client_order_id"
    );
}

void test_cancel_requests_are_strict_idempotent_and_stable() {
    GtcApi server;
    const std::string account_id = new_account(server, "http-cancel");
    server.publish_ask("101", "5");
    REQUIRE(submit_gtc(server, account_id, "b1").status == 201);
    REQUIRE(submit_gtc(server, account_id, "b2").status == 201);

    const auto malformed = [&](std::string body, std::string_view reason) {
        const Json error = require_error(cancel(server, account_id, "b1", std::move(body)), 400, "malformed_request");
        REQUIRE(error.at("reason") == reason);
    };
    malformed("{}", "missing_field");
    malformed(R"({"cancel_request_id":1})", "invalid_type");
    malformed(R"({"cancel_request_id":"c1","reason":"x"})", "unexpected_field");
    malformed(R"({"cancel_request_id":"c1","cancel_request_id":"c2"})", "duplicate_field");
    malformed(R"({"cancel_request_id":"has space"})", "invalid_identifier");
    malformed(R"({"cancel_request_id":"-starts-with-dash"})", "invalid_identifier");
    malformed("[]", "invalid_type");
    malformed("{", "invalid_json");
    const HttpResponse wrong_type = server.api.handle(HttpRequest{
        .method = "POST",
        .path = order_path(account_id, "b1") + "/cancel",
        .query = {},
        .content_type = "text/plain",
        .body = cancel_body("c1"),
    });
    REQUIRE(wrong_type.status == 415);
    const ApiResponse wrong_method = call(server, "GET", order_path(account_id, "b1") + "/cancel");
    require_error(wrong_method, 405, "method_not_allowed");
    require_error(
        call(server, "POST", order_path(account_id, "b1") + "/cancel?force=1", cancel_body("c1")),
        400,
        "malformed_request"
    );
    // Nothing above cancelled anything.
    REQUIRE(call(server, "GET", order_path(account_id, "b1")).body.at("order").at("status") == "working");

    const ApiResponse first = cancel(server, account_id, "b1", cancel_body("cancel-1"));
    REQUIRE(first.status == 200);
    REQUIRE(first.body.at("already_cancelled") == false);
    REQUIRE(first.body.at("order").at("status") == "cancelled");
    REQUIRE(first.body.at("order").at("cancellation_reason") == "user_requested");
    REQUIRE(first.body.at("order").at("cancel_request_id") == "cancel-1");
    REQUIRE(first.body.at("order").at("reserved_cash") == "0");
    REQUIRE(first.body.at("account").at("reserved_cash") == "1000.01");
    const ApiResponse retry = cancel(server, account_id, "b1", cancel_body("cancel-1"));
    REQUIRE(retry.status == 200);
    REQUIRE(retry.body.at("already_cancelled") == true);
    REQUIRE(retry.body.at("order") == first.body.at("order"));

    const ApiResponse terminal = cancel(server, account_id, "b1", cancel_body("cancel-2"));
    require_error(terminal, 409, "order_not_cancellable");
    REQUIRE(terminal.body.at("order").at("status") == "cancelled");
    require_error(
        cancel(server, account_id, "b2", cancel_body("cancel-1")),
        409,
        "duplicate_cancel_request_id"
    );
    require_error(cancel(server, account_id, "missing", cancel_body("cancel-3")), 404, "order_not_found");
    require_error(cancel(server, "missing-account", "b2", cancel_body("cancel-3")), 404, "account_not_found");
}

void test_pages_and_cursors_over_http() {
    GtcApi server;
    const std::string account_id = new_account(server, "http-pages");
    server.publish_ask("101", "5");
    for (const std::string& id : {std::string("a0"), std::string("a1"), std::string("a2")}) {
        REQUIRE(submit_gtc(server, account_id, id).status == 201);
    }
    const ApiResponse first = call(server, "GET", account_path(account_id) + "/working-orders?limit=2");
    REQUIRE(first.status == 200);
    REQUIRE(first.body.at("orders").size() == 2);
    REQUIRE(first.body.at("orders")[0].at("client_order_id") == "a0");
    const std::string cursor = first.body.at("next_cursor").get<std::string>();
    const ApiResponse second = call(
        server,
        "GET",
        account_path(account_id) + "/working-orders?limit=2&cursor=" + cursor
    );
    REQUIRE(second.body.at("orders").size() == 1);
    REQUIRE(second.body.at("next_cursor").is_null());
    const Json invalid = require_error(
        call(server, "GET", account_path(account_id) + "/orders?cursor=" + cursor),
        400,
        "malformed_request"
    );
    REQUIRE(invalid.at("reason") == "invalid_cursor");
    require_error(
        call(server, "GET", account_path(account_id) + "/working-orders?limit=201"),
        400,
        "malformed_request"
    );
    require_error(
        call(server, "GET", account_path(account_id) + "/working-orders?sort=asc"),
        400,
        "malformed_request"
    );

    for (const std::string_view size : {"4", "4", "2"}) {
        server.publish_ask("100", size);
    }
    const ApiResponse fills = call(server, "GET", order_path(account_id, "a0") + "/fills?limit=2");
    REQUIRE(fills.status == 200);
    REQUIRE(fills.body.at("fills").size() == 2);
    REQUIRE(fills.body.at("fills")[0].at("fill_index") == "0");
    REQUIRE(fills.body.at("fills")[1].at("fill_index") == "1");
    const std::string fill_cursor = fills.body.at("next_cursor").get<std::string>();
    const ApiResponse rest = call(
        server,
        "GET",
        order_path(account_id, "a0") + "/fills?limit=2&cursor=" + fill_cursor
    );
    REQUIRE(rest.body.at("fills").size() == 1);
    REQUIRE(rest.body.at("fills")[0].at("fill_id") == "a0:fill:2");
    for (const std::string& misuse : {
             order_path(account_id, "a1") + "/fills?cursor=" + fill_cursor,
             order_path(account_id, "a0") + "/events?cursor=" + fill_cursor,
             account_path(account_id) + "/fills?cursor=" + fill_cursor,
         }) {
        REQUIRE(require_error(call(server, "GET", misuse), 400, "malformed_request").at("reason") ==
            "invalid_cursor");
    }
    // An order's pages take no symbol filter.
    require_error(
        call(server, "GET", order_path(account_id, "a0") + "/fills?instrument=AAPL"),
        400,
        "malformed_request"
    );
    const ApiResponse events = call(server, "GET", order_path(account_id, "a0") + "/events");
    REQUIRE(events.status == 200);
    const Json& lifecycle = events.body.at("events");
    REQUIRE(lifecycle.size() == 10);
    REQUIRE(lifecycle[0].at("type") == "order_submitted");
    REQUIRE(lifecycle[0].at("sequence") == "1");
    REQUIRE(lifecycle[1].at("type") == "order_accepted");
    REQUIRE(lifecycle[3].at("type") == "fill_applied");
    REQUIRE(lifecycle[3].at("fill").at("fill_id") == "a0:fill:0");
    REQUIRE(lifecycle[3].at("fill").at("notional") == "400");
    REQUIRE(lifecycle[8].at("type") == "order_filled");
    REQUIRE(lifecycle[8].at("fill").is_null());
    require_error(call(server, "GET", order_path(account_id, "missing") + "/events"), 404, "order_not_found");
    require_error(call(server, "POST", order_path(account_id, "a0") + "/events", "{}"), 405, "method_not_allowed");
    require_error(call(server, "GET", order_path(account_id, "a0") + "/extra"), 404, "route_not_found");
}

void test_uncertain_results_are_reported_without_recording_twice() {
    const std::string application = unique_test_id("http-gtc-uncertain");
    GtcApi server(with_application_name(connection_string, application));
    const std::string account_id = new_account(server, "http-gtc-uncertain");
    server.publish_ask("101", "5");

    const auto interrupted = [&](std::function<ApiResponse()> request) {
        PostgresTestConnection holder(connection_string);
        PostgresTestConnection observer(connection_string);
        static_cast<void>(holder.query("BEGIN"));
        static_cast<void>(holder.query(
            "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
            {account_id}
        ));
        std::optional<ApiResponse> response;
        std::thread client([&] { response = request(); });
        wait_until_blocked_on_lock(observer, application);
        terminate_backends(observer, application);
        client.join();
        static_cast<void>(holder.query("ROLLBACK"));
        return std::move(*response);
    };

    const ApiResponse lost_submit = interrupted([&] { return submit_gtc(server, account_id, "b1"); });
    const Json submit_error = require_error(lost_submit, 503, "persistence_unavailable");
    REQUIRE(!lost_submit.body.contains("order"));
    // Reconcile: not recorded, so the same ID is submitted again.
    require_error(call(server, "GET", order_path(account_id, "b1")), 404, "order_not_found");
    REQUIRE(submit_gtc(server, account_id, "b1").status == 201);

    const ApiResponse lost_cancel = interrupted([&] {
        return cancel(server, account_id, "b1", cancel_body("cancel-1"));
    });
    require_error(lost_cancel, 503, "persistence_unavailable");
    REQUIRE(call(server, "GET", order_path(account_id, "b1")).body.at("order").at("status") == "working");
    const ApiResponse retried = cancel(server, account_id, "b1", cancel_body("cancel-1"));
    REQUIRE(retried.status == 200);
    REQUIRE(retried.body.at("already_cancelled") == false);
    static_cast<void>(submit_error);
}

// Reads and cancellations stay responsive while a replay is matched
// against many working orders: they never wait for the market-event gate,
// and matching holds each account only for its own transaction.
void test_the_api_responds_while_a_replay_is_matched() {
    GtcApi server;
    const std::string account_id = new_account(server, "http-gtc-busy");
    server.publish_ask("101", "5");
    for (int index = 0; index < 10; ++index) {
        REQUIRE(submit_gtc(server, account_id, "w" + std::to_string(index), "1", "50").status == 201);
    }
    std::deque<MarketEventRead> reads;
    for (std::uint64_t sequence = server.next_sequence; sequence < server.next_sequence + 400; ++sequence) {
        TopOfBookEvent event = quote(
            sequence,
            static_cast<std::int64_t>(sequence) * 1'000,
            "AAPL",
            QuoteLevels{.bid_price = "1", .bid_quantity = "5", .ask_price = "101", .ask_quantity = "5"}
        );
        event.header.stream = chronos::MarketDataStreamId(server.stream);
        reads.emplace_back(chronos::test::validated(MarketEvent(event)));
    }
    market_data::PacedReplaySource replay(
        std::make_unique<chronos::test::ScriptedReader>(std::move(reads)),
        server.feed,
        server.clock,
        market_data::MarketDataSourceDescription{},
        market_data::PacedReplayOptions{.speed = chronos::ReplaySpeed::Maximum, .start_immediately = true}
    );
    std::chrono::nanoseconds slowest{0};
    int requests = 0;
    while (replay.status().state != market_data::MarketDataSourceState::Completed && requests < 200) {
        const auto started = std::chrono::steady_clock::now();
        REQUIRE(call(server, "GET", order_path(account_id, "w0")).status == 200);
        REQUIRE(call(server, "GET", account_path(account_id) + "/working-orders").status == 200);
        slowest = std::max(slowest, std::chrono::steady_clock::now() - started);
        ++requests;
    }
    const ApiResponse cancelled = cancel(server, account_id, "w9", cancel_body("cancel-busy"));
    REQUIRE(cancelled.status == 200);
    REQUIRE(chronos::test::eventually([&] {
        return replay.status().state == market_data::MarketDataSourceState::Completed;
    }));
    std::cout << "API reads during replay matching: " << requests << " pairs, slowest "
              << std::chrono::duration<double, std::milli>(slowest).count() << " ms\n";
    REQUIRE(requests > 0);
    REQUIRE(slowest < std::chrono::seconds(2));
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "GTC HTTP API test skipped: CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    {
        PostgresPaperOrderRepository migrations(connection_string);
        migrations.apply_migration(all_migrations_sql());
    }
    return chronos::test::run_tests({
        {"GTC orders change as they fill", test_gtc_orders_are_accepted_and_change_as_they_fill},
        {"time in force parsing is strict", test_submission_parsing_is_strict_about_time_in_force},
        {"cancel requests are strict and idempotent", test_cancel_requests_are_strict_idempotent_and_stable},
        {"pages and cursors over HTTP", test_pages_and_cursors_over_http},
        {"uncertain results reconcile", test_uncertain_results_are_reported_without_recording_twice},
        {"the API responds during matching", test_the_api_responds_while_a_replay_is_matched},
    });
}
