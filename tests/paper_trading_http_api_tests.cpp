#include "chronos/paper_trading/paper_trading_http_api.hpp"
#include "chronos/paper_trading/paper_trading_http_server.hpp"
#include "json_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using chronos::MarketState;
using chronos::PostgresPaperOrderRepository;
using chronos::paper_trading::HttpHeader;
using chronos::paper_trading::HttpRequest;
using chronos::paper_trading::HttpResponse;
using chronos::paper_trading::HttpServerOptions;
using chronos::paper_trading::PaperTradingHttpApi;
using chronos::paper_trading::PaperTradingHttpServer;
using chronos::paper_trading::PaperTradingService;
using chronos::test::apply;
using chronos::test::kMaximumQuoteAgeNanoseconds;
using chronos::test::paper_service_config;
using chronos::test::PostgresTestConnection;
using chronos::test::quote;
using chronos::test::QuoteLevels;
using chronos::test::require_no_json_numbers;
using chronos::test::terminate_backends;
using chronos::test::unique_test_id;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;
using Json = nlohmann::json;

std::string connection_string;

// One API stack: server-owned market state, service, and router.
struct TestApi {
    explicit TestApi(const std::string& connection = connection_string)
        : market(chronos::test::market_state()),
          service(std::make_unique<PaperTradingService>(
              std::make_unique<PostgresPaperOrderRepository>(connection),
              *market,
              paper_service_config()
          )),
          api(std::make_unique<PaperTradingHttpApi>(
              *service,
              [this](std::string_view line) { diagnostics.emplace_back(line); }
          )) {}

    std::vector<std::string> diagnostics;
    std::unique_ptr<MarketState> market;
    std::unique_ptr<PaperTradingService> service;
    std::unique_ptr<PaperTradingHttpApi> api;
};

struct ApiResponse {
    int status;
    Json body;
    std::vector<HttpHeader> headers;
};

// Every response in this suite is checked for valid JSON without numbers.
ApiResponse call(
    TestApi& api,
    std::string method,
    std::string path,
    std::string body = {},
    std::string content_type = "application/json"
) {
    const HttpResponse response = api.api->handle(HttpRequest{
        .method = std::move(method),
        .path = std::move(path),
        .content_type = std::move(content_type),
        .body = std::move(body),
    });
    Json parsed = Json::parse(response.body);
    require_no_json_numbers(parsed);
    return ApiResponse{
        .status = response.status,
        .body = std::move(parsed),
        .headers = response.headers,
    };
}

// Returns a copy so a caller may pass a temporary response.
Json require_error(
    const ApiResponse& response,
    int status,
    std::string_view code
) {
    REQUIRE(response.status == status);
    REQUIRE(response.body.at("error").at("code") == code);
    return response.body.at("error");
}

std::string accounts_path(const std::string& account_id) {
    return "/api/v1/accounts/" + account_id;
}

std::string orders_path(const std::string& account_id) {
    return accounts_path(account_id) + "/orders";
}

ApiResponse create_account(
    TestApi& api,
    const std::string& account_id,
    std::string_view starting_cash = "1000",
    std::string_view instrument = "AAPL"
) {
    return call(
        api,
        "POST",
        "/api/v1/accounts",
        Json{
            {"account_id", account_id},
            {"instrument", instrument},
            {"starting_cash", starting_cash},
        }.dump()
    );
}

std::string new_account(TestApi& api, std::string_view prefix) {
    std::string account_id = unique_test_id(prefix);
    REQUIRE(create_account(api, account_id).status == 201);
    return account_id;
}

Json order(
    std::string client_order_id,
    std::string_view side,
    std::string_view quantity,
    std::optional<std::string_view> limit_price = std::nullopt,
    std::string_view source_kind = "manual"
) {
    Json body{
        {"client_order_id", std::move(client_order_id)},
        {"source", {{"kind", source_kind}, {"id", "local-trader"}}},
        {"instrument", "AAPL"},
        {"side", side},
        {"type", limit_price ? "limit" : "market"},
        {"quantity", quantity},
        {"time_in_force", "ioc"},
    };
    if (limit_price) {
        body["limit_price"] = *limit_price;
    }
    return body;
}

ApiResponse submit(TestApi& api, const std::string& account_id, const Json& body) {
    return call(api, "POST", orders_path(account_id), body.dump());
}

std::int64_t recorded_orders(const std::string& account_id) {
    PostgresPaperOrderRepository inspector(connection_string);
    return inspector.journal_summary(account_id).order_count;
}

void test_health_reports_paper_mode_persistence_and_market_state() {
    TestApi api;
    const ApiResponse empty = call(api, "GET", "/health");
    REQUIRE(empty.status == 200);
    REQUIRE(empty.body.at("status") == "ok");
    REQUIRE(empty.body.at("trading_mode") == "paper");
    REQUIRE(empty.body.at("persistence") == "available");
    const Json& empty_market = empty.body.at("market_data");
    REQUIRE(empty_market.at("stream_id") == chronos::test::kTestStream);
    REQUIRE(empty_market.at("state") == "accepting");
    REQUIRE(empty_market.at("last_ingest_sequence").is_null());
    REQUIRE(empty_market.at("market_time").is_null());

    apply(*api.market, quote(0, 0));
    const Json market = call(api, "GET", "/health").body.at("market_data");
    REQUIRE(market.at("last_ingest_sequence") == "0");
    REQUIRE(market.at("market_time") == "2026-09-25T13:30:00.000000000Z");
    REQUIRE(market.at("interruption").is_null());
}

void test_account_lifecycle_uses_distinct_status_codes() {
    TestApi api;
    const std::string account_id = unique_test_id("http-account");

    const ApiResponse created = create_account(api, account_id, "2500.75");
    REQUIRE(created.status == 201);
    const Json& account = created.body.at("account");
    REQUIRE(account.at("trading_mode") == "paper");
    REQUIRE(account.at("account_id") == account_id);
    REQUIRE(account.at("instrument") == "AAPL");
    REQUIRE(account.at("version") == "0");
    REQUIRE(account.at("status") == "active");
    REQUIRE(account.at("starting_cash") == "2500.75");
    REQUIRE(account.at("cash") == "2500.75");
    REQUIRE(account.at("position") == "0");
    REQUIRE(account.at("average_entry_price").is_null());
    REQUIRE(account.at("last_fill_at").is_null());
    REQUIRE(account.at("risk_limits") == Json({
        {"maximum_order_quantity", "100"},
        {"maximum_order_notional", "100000"},
        {"maximum_position_quantity", "100"},
        {"maximum_quote_age_ns", "1000000000"},
        {"fee_per_fill", "0.01"},
    }));

    const ApiResponse retrieved = call(api, "GET", accounts_path(account_id));
    REQUIRE(retrieved.status == 200);
    REQUIRE(retrieved.body.at("account") == account);

    require_error(
        create_account(api, account_id, "1"),
        409,
        "account_already_exists"
    );
    REQUIRE(
        call(api, "GET", accounts_path(account_id)).body.at("account") ==
        account
    );
    require_error(
        call(api, "GET", accounts_path(unique_test_id("absent"))),
        404,
        "account_not_found"
    );
    const Json invalid = require_error(
        call(api, "GET", accounts_path("bad id")),
        400,
        "malformed_request"
    );
    REQUIRE(invalid.at("reason") == "invalid_identifier");
    REQUIRE(invalid.at("field") == "account_id");
    REQUIRE(require_error(
        create_account(api, unique_test_id("msft"), "1000", "MSFT"),
        422,
        "unsupported_instrument"
    ).at("field") == "instrument");
    REQUIRE(require_error(
        create_account(api, unique_test_id("broke"), "0"),
        422,
        "invalid_starting_cash"
    ).at("field") == "starting_cash");
    REQUIRE(require_error(
        create_account(api, unique_test_id("precise"), "1.000000001"),
        400,
        "malformed_request"
    ).at("reason") == "too_many_fractional_digits");
}

void test_filled_order_returns_exact_decimal_strings() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-fill");

    const ApiResponse response = submit(
        api,
        account_id,
        order("fill-1", "buy", "2.5")
    );
    REQUIRE(response.status == 201);
    REQUIRE(!response.body.contains("error"));
    const Json& placed = response.body.at("order");
    REQUIRE(placed.at("trading_mode") == "paper");
    REQUIRE(placed.at("account_id") == account_id);
    REQUIRE(placed.at("status") == "filled");
    REQUIRE(placed.at("submitted_at") == "2026-09-25T13:30:00.000000000Z");
    REQUIRE(placed.at("filled_quantity") == "2.5");
    REQUIRE(placed.at("fill").at("fill_id") == "fill-1:fill:0");
    REQUIRE(placed.at("fill").at("price") == "101");
    REQUIRE(placed.at("fill").at("notional") == "252.5");
    REQUIRE(placed.at("fill").at("fee") == "0.01");
    REQUIRE(placed.at("market").at("ingest_sequence") == "0");
    REQUIRE(placed.at("market").at("bid") == Json({
        {"price", "99"},
        {"quantity", "5"},
    }));

    const Json& account = response.body.at("account");
    REQUIRE(account.at("version") == "1");
    REQUIRE(account.at("cash") == "747.49");
    REQUIRE(account.at("position") == "2.5");
    REQUIRE(account.at("average_entry_price") == "101");
    REQUIRE(account.at("fees_paid") == "0.01");
    REQUIRE(
        call(api, "GET", accounts_path(account_id)).body.at("account") ==
        account
    );
}

void test_partial_fill_and_non_marketable_limit_are_201_outcomes() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-partial");

    const ApiResponse partial = submit(
        api,
        account_id,
        order("partial", "buy", "8")
    );
    REQUIRE(partial.status == 201);
    REQUIRE(
        partial.body.at("order").at("status") ==
        "partially_filled_and_cancelled"
    );
    REQUIRE(partial.body.at("order").at("filled_quantity") == "5");
    REQUIRE(partial.body.at("order").at("unfilled_quantity") == "3");
    REQUIRE(
        partial.body.at("order").at("cancellation_reason") ==
        "displayed_liquidity_exhausted"
    );

    const ApiResponse passive = submit(
        api,
        account_id,
        order("passive", "sell", "1", "99.5")
    );
    REQUIRE(passive.status == 201);
    REQUIRE(passive.body.at("order").at("status") == "cancelled");
    REQUIRE(
        passive.body.at("order").at("cancellation_reason") ==
        "limit_not_marketable"
    );
    REQUIRE(passive.body.at("order").at("fill").is_null());
    REQUIRE(passive.body.at("account").at("position") == "5");
    REQUIRE(passive.body.at("account").at("version") == "2");
}

void test_risk_and_stale_rejections_are_recorded_422_responses() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-risk");

    const ApiResponse risk = submit(api, account_id, order("big", "buy", "50"));
    const Json risk_error = require_error(risk, 422, "risk_rejected");
    REQUIRE(risk_error.at("reason") == "insufficient_cash");
    REQUIRE(risk_error.at("field") == "quantity");
    REQUIRE(risk.body.at("order").at("status") == "rejected");
    REQUIRE(risk.body.at("account").at("version") == "1");

    const ApiResponse limit = submit(
        api,
        account_id,
        order("maximum", "buy", "92233720368.54775807")
    );
    REQUIRE(require_error(limit, 422, "risk_rejected").at("reason") ==
            "order_quantity_limit_exceeded");
    REQUIRE(
        limit.body.at("order").at("quantity") == "92233720368.54775807"
    );

    const ApiResponse zero = submit(api, account_id, order("zero", "buy", "0"));
    REQUIRE(require_error(zero, 422, "risk_rejected").at("reason") ==
            "nonpositive_quantity");

    apply(*api.market, quote(1, kMaximumQuoteAgeNanoseconds + 1, "SPY"));
    const ApiResponse stale = submit(api, account_id, order("stale", "buy", "1"));
    const Json stale_error = require_error(stale, 422, "market_data_stale");
    REQUIRE(stale_error.at("reason") == "stale_market_state");
    REQUIRE(stale_error.at("field").is_null());
    REQUIRE(
        stale.body.at("order").at("submitted_at") ==
        "2026-09-25T13:30:01.000000001Z"
    );
    REQUIRE(recorded_orders(account_id) == 4);
}

void test_missing_market_data_returns_503_without_recording() {
    TestApi api;
    const std::string account_id = new_account(api, "http-no-market");

    const ApiResponse empty = submit(api, account_id, order("early", "buy", "1"));
    REQUIRE(require_error(empty, 503, "market_data_unavailable").at("reason") ==
            "no_top_of_book");
    REQUIRE(!empty.body.contains("order"));
    REQUIRE(recorded_orders(account_id) == 0);

    apply(*api.market, quote(0, 0));
    auto gap = chronos::validate_market_event(quote(5, 10));
    static_cast<void>(api.market->apply(
        std::get<chronos::ValidatedMarketEvent>(gap)
    ));
    REQUIRE(require_error(
        submit(api, account_id, order("early", "buy", "1")),
        503,
        "market_data_unavailable"
    ).at("reason") == "stream_interrupted");
    REQUIRE(
        call(api, "GET", "/health").body.at("market_data").at("state") ==
        "interrupted"
    );
    REQUIRE(recorded_orders(account_id) == 0);
}

void test_malformed_orders_never_reach_the_journal() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-malformed");
    const auto rejected = [&](std::string body,
                              std::string_view reason,
                              std::string_view field) {
        const ApiResponse response = call(
            api,
            "POST",
            orders_path(account_id),
            std::move(body)
        );
        const Json error = require_error(response, 400, "malformed_request");
        REQUIRE(error.at("reason") == reason);
        if (field.empty()) {
            REQUIRE(error.at("field").is_null());
        } else {
            REQUIRE(error.at("field") == field);
        }
        REQUIRE(!response.body.contains("order"));
    };
    const auto with = [](std::string_view field, Json value) {
        Json body = order("malformed", "buy", "1");
        body[std::string(field)] = std::move(value);
        return body.dump();
    };

    rejected(with("quantity", "1e2"), "invalid_decimal", "quantity");
    rejected(with("quantity", "+1"), "invalid_decimal", "quantity");
    rejected(with("quantity", 1), "invalid_type", "quantity");
    rejected(with("quantity", 1.5), "invalid_type", "quantity");
    rejected(
        with("quantity", "92233720368.54775808"),
        "decimal_out_of_range",
        "quantity"
    );
    rejected(
        with("quantity", "0.000000001"),
        "too_many_fractional_digits",
        "quantity"
    );
    rejected(with("limit_price", "abc"), "invalid_decimal", "limit_price");
    rejected(with("side", "short"), "unknown_enum_value", "side");
    rejected(with("type", "stop_limit"), "unknown_enum_value", "type");
    rejected(with("time_in_force", "gtc"), "unknown_enum_value", "time_in_force");
    rejected(
        with("source", Json{{"kind", "broker"}, {"id", "x"}}),
        "unknown_enum_value",
        "source.kind"
    );
    rejected(
        with("submitted_at", "2026-09-25T13:30:00Z"),
        "unexpected_field",
        "submitted_at"
    );
    rejected(with("bid", "100"), "unexpected_field", "bid");
    rejected(
        R"({"quantity":"1","quantity":"100"})",
        "duplicate_field",
        "quantity"
    );
    rejected("{\"client_order_id\":", "invalid_json", "");
    rejected(
        with("client_order_id", std::string("a\0b", 3)),
        "invalid_identifier",
        "client_order_id"
    );
    rejected(
        with("client_order_id", std::string(65, 'x')),
        "invalid_identifier",
        "client_order_id"
    );
    rejected(
        with("source", Json{{"kind", "manual"}, {"id", "has space"}}),
        "invalid_identifier",
        "source.id"
    );
    REQUIRE(recorded_orders(account_id) == 0);
}

void test_duplicate_client_order_id_returns_409_without_recording() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-duplicate");

    REQUIRE(submit(api, account_id, order("once", "buy", "1")).status == 201);
    const ApiResponse duplicate = submit(
        api,
        account_id,
        order("once", "buy", "1")
    );
    REQUIRE(require_error(duplicate, 409, "duplicate_client_order_id")
                .at("field") == "client_order_id");
    REQUIRE(!duplicate.body.contains("order"));
    REQUIRE(recorded_orders(account_id) == 1);
    REQUIRE(
        call(api, "GET", accounts_path(account_id))
            .body.at("account").at("position") == "1"
    );
}

void test_version_conflict_returns_409_and_the_same_id_can_retry() {
    const std::string application = unique_test_id("http-conflict");
    TestApi api(with_application_name(connection_string, application));
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-conflict");

    PostgresTestConnection lock_holder(connection_string);
    PostgresTestConnection observer(connection_string);
    static_cast<void>(lock_holder.query("BEGIN"));
    static_cast<void>(lock_holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));
    std::optional<HttpResponse> contended;
    std::thread submitter([&] {
        contended = api.api->handle(HttpRequest{
            .method = "POST",
            .path = orders_path(account_id),
            .content_type = "application/json",
            .body = order("contended", "buy", "1").dump(),
        });
    });
    wait_until_blocked_on_lock(observer, application);
    static_cast<void>(lock_holder.query(
        "UPDATE paper_accounts SET version = version + 1 "
        "WHERE account_id = $1",
        {account_id}
    ));
    static_cast<void>(lock_holder.query("COMMIT"));
    submitter.join();

    REQUIRE(contended.has_value());
    REQUIRE(contended->status == 409);
    const Json conflict = Json::parse(contended->body);
    require_no_json_numbers(conflict);
    REQUIRE(conflict.at("error").at("code") == "account_version_conflict");
    REQUIRE(!conflict.contains("order"));
    REQUIRE(recorded_orders(account_id) == 0);

    const ApiResponse retried = submit(
        api,
        account_id,
        order("contended", "buy", "1")
    );
    REQUIRE(retried.status == 201);
    REQUIRE(retried.body.at("account").at("version") == "2");
}

void test_routing_and_media_type_errors_are_json() {
    TestApi api;
    const std::string account_id = new_account(api, "http-routing");
    const std::string create_body = Json{
        {"account_id", unique_test_id("http-media")},
        {"instrument", "AAPL"},
        {"starting_cash", "1"},
    }.dump();

    require_error(call(api, "GET", "/"), 404, "route_not_found");
    require_error(call(api, "GET", "/api/v2/accounts"), 404, "route_not_found");
    require_error(call(api, "GET", "/api/v1/accounts/"), 404, "route_not_found");
    require_error(
        call(api, "GET", "/api/v1//accounts"),
        404,
        "route_not_found"
    );
    require_error(
        call(api, "POST", orders_path(account_id) + "/extra", "{}"),
        404,
        "route_not_found"
    );

    const auto require_allow = [](const ApiResponse& response,
                                  std::string_view allowed) {
        require_error(response, 405, "method_not_allowed");
        REQUIRE(response.headers.size() == 1);
        REQUIRE(response.headers.front().name == "Allow");
        REQUIRE(response.headers.front().value == allowed);
    };
    require_allow(call(api, "POST", "/health", "{}"), "GET");
    require_allow(call(api, "GET", "/api/v1/accounts"), "POST");
    require_allow(call(api, "DELETE", accounts_path(account_id)), "GET");
    require_allow(call(api, "GET", orders_path(account_id)), "POST");

    require_error(
        call(api, "POST", "/api/v1/accounts", create_body, "text/plain"),
        415,
        "unsupported_media_type"
    );
    require_error(
        call(api, "POST", "/api/v1/accounts", create_body, ""),
        415,
        "unsupported_media_type"
    );
    require_error(
        call(
            api,
            "POST",
            "/api/v1/accounts",
            create_body,
            "application/json; charset=latin1"
        ),
        415,
        "unsupported_media_type"
    );
    REQUIRE(call(
        api,
        "POST",
        "/api/v1/accounts",
        create_body,
        "Application/JSON; Charset=UTF-8"
    ).status == 201);
}

void test_durable_state_is_visible_through_a_new_api_instance() {
    std::string account_id;
    Json account_before_restart;
    {
        TestApi first;
        apply(*first.market, quote(0, 0));
        account_id = new_account(first, "http-restart");
        REQUIRE(submit(first, account_id, order("a", "buy", "3")).status == 201);
        account_before_restart =
            call(first, "GET", accounts_path(account_id)).body.at("account");
    }

    TestApi restarted;
    const ApiResponse recovered = call(
        restarted,
        "GET",
        accounts_path(account_id)
    );
    REQUIRE(recovered.status == 200);
    REQUIRE(recovered.body.at("account") == account_before_restart);
    REQUIRE(recovered.body.at("account").at("position") == "3");

    apply(*restarted.market, quote(0, 0));
    const ApiResponse remaining = submit(
        restarted,
        account_id,
        order("b", "buy", "3")
    );
    REQUIRE(remaining.status == 201);
    REQUIRE(remaining.body.at("order").at("filled_quantity") == "2");
}

void test_one_account_cannot_consume_another_accounts_liquidity() {
    TestApi api;
    apply(*api.market, quote(0, 0, "AAPL", QuoteLevels{.ask_quantity = "5"}));
    const std::string first = new_account(api, "http-shadow-a");
    const std::string second = new_account(api, "http-shadow-b");

    REQUIRE(submit(api, first, order("all", "buy", "5")).body.at("order")
                .at("status") == "filled");
    const ApiResponse exhausted = submit(api, first, order("more", "buy", "1"));
    REQUIRE(exhausted.status == 201);
    REQUIRE(exhausted.body.at("order").at("status") == "cancelled");
    REQUIRE(
        exhausted.body.at("order").at("cancellation_reason") ==
        "displayed_liquidity_exhausted"
    );

    const ApiResponse independent = submit(
        api,
        second,
        order("all", "buy", "5")
    );
    REQUIRE(independent.body.at("order").at("status") == "filled");
    REQUIRE(independent.body.at("order").at("filled_quantity") == "5");
}

void test_model_orders_use_the_same_endpoint_and_execution() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string manual = new_account(api, "http-manual");
    const std::string model = new_account(api, "http-model");

    const ApiResponse manual_response = submit(
        api,
        manual,
        order("same", "buy", "7", "101")
    );
    const ApiResponse model_response = submit(
        api,
        model,
        order("same", "buy", "7", "101", "model")
    );
    REQUIRE(manual_response.status == model_response.status);
    REQUIRE(
        model_response.body.at("order").at("source").at("kind") == "model"
    );

    Json manual_order = manual_response.body.at("order");
    Json model_order = model_response.body.at("order");
    manual_order.erase("account_id");
    model_order.erase("account_id");
    manual_order.erase("source");
    model_order.erase("source");
    REQUIRE(manual_order == model_order);

    Json manual_account = manual_response.body.at("account");
    Json model_account = model_response.body.at("account");
    manual_account.erase("account_id");
    model_account.erase("account_id");
    REQUIRE(manual_account == model_account);
}

void test_persistence_outage_returns_generic_503_and_logs_detail() {
    const std::string application = unique_test_id("http-outage");
    TestApi api(with_application_name(connection_string, application));
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-outage");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    const ApiResponse outage = submit(
        api,
        account_id,
        order("outage", "buy", "1")
    );
    const Json error = require_error(outage, 503, "persistence_unavailable");
    const std::string message = error.at("message");
    REQUIRE(message.find("PostgreSQL") == std::string::npos);
    REQUIRE(message.find("same identifiers") != std::string::npos);
    REQUIRE(!outage.body.contains("order"));
    REQUIRE(api.diagnostics.size() == 1);
    REQUIRE(
        api.diagnostics.front().find("PostgreSQL connection is unavailable") !=
        std::string::npos
    );

    REQUIRE(call(api, "GET", "/health").status == 200);
    REQUIRE(submit(api, account_id, order("outage", "buy", "1")).status == 201);
}

// Serves on a background thread and always stops and joins it, so a failed
// assertion reports a test failure instead of terminating the process.
class ServingThread final {
public:
    explicit ServingThread(PaperTradingHttpServer& server)
        : server_(server),
          thread_([this] {
              try {
                  server_.listen();
              } catch (const std::exception& error) {
                  failure_ = error.what();
              }
          }) {}

    ~ServingThread() { stop(); }

    ServingThread(const ServingThread&) = delete;
    ServingThread& operator=(const ServingThread&) = delete;

    // Returns the listen failure, or an empty string after a clean stop.
    std::string stop() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
        return failure_;
    }

private:
    PaperTradingHttpServer& server_;
    std::string failure_;
    std::thread thread_;
};

void test_socket_server_serves_json_with_paper_headers_and_limits() {
    TestApi api;
    std::mutex access_mutex;
    std::vector<std::string> access_log;
    PaperTradingHttpServer server(
        *api.api,
        HttpServerOptions{
            .host = "127.0.0.1",
            .port = 0,
            .maximum_request_body_bytes = 1024,
        },
        [&](std::string_view line) {
            const std::lock_guard lock(access_mutex);
            access_log.emplace_back(line);
        }
    );
    const int port = server.bind();
    ServingThread serving(server);

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(std::chrono::seconds(5));
    client.set_read_timeout(std::chrono::seconds(5));
    const auto require_json = [](const httplib::Result& result, int status) {
        REQUIRE(static_cast<bool>(result));
        REQUIRE(result->status == status);
        REQUIRE(result->get_header_value("Content-Type") == "application/json");
        REQUIRE(result->get_header_value("X-Chronos-Trading-Mode") == "paper");
        REQUIRE(result->get_header_value("Cache-Control") == "no-store");
        const Json body = Json::parse(result->body);
        require_no_json_numbers(body);
        return body;
    };

    REQUIRE(require_json(client.Get("/health"), 200).at("trading_mode") ==
            "paper");
    REQUIRE(require_json(client.Get("/missing"), 404).at("error").at("code") ==
            "route_not_found");
    REQUIRE(require_json(
        client.Post(
            "/api/v1/accounts",
            std::string(2048, ' '),
            "application/json"
        ),
        413
    ).at("error").at("code") == "payload_too_large");
    const std::string account_id = unique_test_id("socket");
    const Json created = require_json(
        client.Post(
            "/api/v1/accounts",
            Json{
                {"account_id", account_id},
                {"instrument", "SPY"},
                {"starting_cash", "10"},
            }.dump(),
            "application/json"
        ),
        201
    );
    REQUIRE(created.at("account").at("instrument") == "SPY");

    // A second server must not share the port with a running one.
    PaperTradingHttpServer duplicate(
        *api.api,
        HttpServerOptions{.host = "127.0.0.1", .port = port},
        [](std::string_view) {}
    );
    REQUIRE_THROWS_AS(duplicate.bind(), std::runtime_error);

    REQUIRE(serving.stop().empty());
    const std::lock_guard lock(access_mutex);
    REQUIRE(access_log.size() == 4);
    REQUIRE(access_log.front() == "GET /health 200");
}

}  // namespace

int main() {
    const char* configured_connection =
        std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "paper-trading HTTP API test skipped: "
                     "CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    PostgresPaperOrderRepository(connection_string)
        .apply_migration(chronos::test::migration_sql());

    return chronos::test::run_tests({
        {
            "health reports paper mode and components",
            test_health_reports_paper_mode_persistence_and_market_state,
        },
        {
            "account lifecycle status codes",
            test_account_lifecycle_uses_distinct_status_codes,
        },
        {
            "filled order returns exact decimal strings",
            test_filled_order_returns_exact_decimal_strings,
        },
        {
            "partial fill and passive limit are 201 outcomes",
            test_partial_fill_and_non_marketable_limit_are_201_outcomes,
        },
        {
            "risk and stale rejections are recorded 422s",
            test_risk_and_stale_rejections_are_recorded_422_responses,
        },
        {
            "missing market data returns 503",
            test_missing_market_data_returns_503_without_recording,
        },
        {
            "malformed orders never reach the journal",
            test_malformed_orders_never_reach_the_journal,
        },
        {
            "duplicate client order ID returns 409",
            test_duplicate_client_order_id_returns_409_without_recording,
        },
        {
            "version conflict returns 409 and retry succeeds",
            test_version_conflict_returns_409_and_the_same_id_can_retry,
        },
        {
            "routing and media type errors are JSON",
            test_routing_and_media_type_errors_are_json,
        },
        {
            "durable state visible through a new API",
            test_durable_state_is_visible_through_a_new_api_instance,
        },
        {
            "accounts cannot consume each other's liquidity",
            test_one_account_cannot_consume_another_accounts_liquidity,
        },
        {
            "model orders share the endpoint and execution",
            test_model_orders_use_the_same_endpoint_and_execution,
        },
        {
            "persistence outage returns generic 503",
            test_persistence_outage_returns_generic_503_and_logs_detail,
        },
        {
            "socket server serves JSON with paper headers",
            test_socket_server_serves_json_with_paper_headers_and_limits,
        },
    });
}
