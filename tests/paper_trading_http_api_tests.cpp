#include "chronos/paper_trading/history_cursor.hpp"
#include "chronos/paper_trading/paper_trading_http_api.hpp"
#include "chronos/paper_trading/paper_trading_http_server.hpp"
#include "json_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstddef>
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

using chronos::InstrumentId;
using chronos::MarketState;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PersistentPaperOrderSession;
using chronos::Quantity;
using chronos::Side;
using chronos::TimeInForce;
using chronos::TopOfBookEvent;
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
// A "?query" suffix on the target is split off as the transport does.
ApiResponse call(
    TestApi& api,
    std::string method,
    const std::string& target,
    std::string body = {},
    std::string content_type = "application/json"
) {
    const std::size_t question = target.find('?');
    const HttpResponse response = api.api->handle(HttpRequest{
        .method = std::move(method),
        .path = target.substr(0, question),
        .query = question == std::string::npos
            ? std::string()
            : target.substr(question + 1),
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
            .query = {},
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
        call(api, "GET", orders_path(account_id) + "/order-1/extra"),
        404,
        "route_not_found"
    );
    require_error(
        call(api, "GET", accounts_path(account_id) + "/positions"),
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
    require_allow(call(api, "DELETE", orders_path(account_id)), "GET, POST");
    require_allow(call(api, "POST", orders_path(account_id) + "/order-1", "{}"), "GET");
    require_allow(call(api, "POST", accounts_path(account_id) + "/fills", "{}"), "GET");

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

// ---------------------------------------------------------------------------
// Journal read endpoints

std::string fills_path(const std::string& account_id) {
    return accounts_path(account_id) + "/fills";
}

std::vector<std::string> client_order_ids(const Json& resources) {
    std::vector<std::string> ids;
    for (const Json& resource : resources) {
        ids.push_back(resource.at("client_order_id"));
    }
    return ids;
}

// Submits a varied history: three booked fills, a passive cancellation, and
// a risk rejection, oldest to newest.
void submit_varied_history(TestApi& api, const std::string& account_id) {
    REQUIRE(submit(api, account_id, order("fill-a", "buy", "1")).status == 201);
    REQUIRE(submit(api, account_id, order("fill-b", "buy", "1.5")).status == 201);
    REQUIRE(submit(api, account_id, order("passive", "buy", "1", "100")).status ==
            201);
    REQUIRE(submit(api, account_id, order("too-big", "buy", "50")).status == 422);
    REQUIRE(submit(
        api,
        account_id,
        order("fill-c", "sell", "0.5", std::nullopt, "model")
    ).status == 201);
}

void test_order_history_pages_newest_first_with_cursors() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-history");
    const ApiResponse empty = call(api, "GET", orders_path(account_id));
    REQUIRE(empty.status == 200);
    REQUIRE(empty.body.at("orders") == Json::array());
    REQUIRE(empty.body.at("next_cursor").is_null());

    submit_varied_history(api, account_id);
    const ApiResponse first = call(api, "GET", orders_path(account_id) + "?limit=2");
    REQUIRE(first.status == 200);
    REQUIRE((client_order_ids(first.body.at("orders")) ==
             std::vector<std::string>{"fill-c", "too-big"}));
    const std::string cursor = first.body.at("next_cursor");
    const Json rejected = first.body.at("orders")[1];
    REQUIRE(rejected.at("status") == "rejected");
    REQUIRE(rejected.at("rejection").at("code") == "risk_rejected");
    REQUIRE(rejected.at("rejection").at("reason") == "insufficient_cash");
    REQUIRE(first.body.at("orders")[0].at("source").at("kind") == "model");

    const ApiResponse second = call(
        api,
        "GET",
        orders_path(account_id) + "?cursor=" + cursor + "&limit=2"
    );
    REQUIRE((client_order_ids(second.body.at("orders")) ==
             std::vector<std::string>{"passive", "fill-b"}));
    const ApiResponse last = call(
        api,
        "GET",
        orders_path(account_id) + "?limit=2&cursor=" +
            second.body.at("next_cursor").get<std::string>()
    );
    REQUIRE((client_order_ids(last.body.at("orders")) ==
             std::vector<std::string>{"fill-a"}));
    REQUIRE(last.body.at("next_cursor").is_null());
    REQUIRE(call(api, "GET", orders_path(account_id)).body.at("orders").size() ==
            5);
}

void test_fill_history_returns_only_booked_fills() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-fills");
    REQUIRE(call(api, "GET", fills_path(account_id)).body.at("fills") ==
            Json::array());
    submit_varied_history(api, account_id);

    const ApiResponse fills = call(api, "GET", fills_path(account_id) + "?limit=2");
    REQUIRE(fills.status == 200);
    REQUIRE((client_order_ids(fills.body.at("fills")) ==
             std::vector<std::string>{"fill-c", "fill-b"}));
    REQUIRE(fills.body.at("fills")[1] == Json({
        {"trading_mode", "paper"},
        {"account_id", account_id},
        {"client_order_id", "fill-b"},
        {"source", {{"kind", "manual"}, {"id", "local-trader"}}},
        {"instrument", "AAPL"},
        {"fill_id", "fill-b:fill:0"},
        {"side", "buy"},
        {"price", "101"},
        {"quantity", "1.5"},
        {"notional", "151.5"},
        {"fee", "0.01"},
        {"filled_at", "2026-09-25T13:30:00.000000000Z"},
    }));
    const ApiResponse rest = call(
        api,
        "GET",
        fills_path(account_id) + "?cursor=" +
            fills.body.at("next_cursor").get<std::string>()
    );
    REQUIRE((client_order_ids(rest.body.at("fills")) ==
             std::vector<std::string>{"fill-a"}));
    REQUIRE(rest.body.at("next_cursor").is_null());
}

void test_order_lookup_returns_the_submitted_resource() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-lookup");
    const ApiResponse executed = submit(api, account_id, order("partial", "buy", "8"));
    const ApiResponse rejected = submit(api, account_id, order("rejected", "buy", "50"));
    REQUIRE(rejected.body.at("order").at("rejection") == rejected.body.at("error"));

    for (const auto& [id, submission] : {
             std::pair{std::string("partial"), executed},
             std::pair{std::string("rejected"), rejected},
         }) {
        const ApiResponse lookup = call(
            api,
            "GET",
            orders_path(account_id) + "/" + id
        );
        REQUIRE(lookup.status == 200);
        REQUIRE(lookup.body.at("order") == submission.body.at("order"));
    }
    REQUIRE(require_error(
        call(api, "GET", orders_path(account_id) + "/never-sent"),
        404,
        "order_not_found"
    ).at("field") == "client_order_id");
    require_error(
        call(api, "GET", orders_path(unique_test_id("absent")) + "/partial"),
        404,
        "account_not_found"
    );
    REQUIRE(require_error(
        call(api, "GET", orders_path(account_id) + "/bad id"),
        400,
        "malformed_request"
    ).at("reason") == "invalid_identifier");
}

void test_history_queries_are_strict_on_every_route() {
    TestApi api;
    const std::string account_id = new_account(api, "http-queries");
    const std::string other_account = new_account(api, "http-queries-other");
    const auto rejected = [&](const std::string& target,
                              std::string_view reason,
                              std::string_view method = "GET") {
        const Json error = require_error(
            call(api, std::string(method), target, "{}"),
            400,
            "malformed_request"
        );
        REQUIRE(error.at("reason") == reason);
        return error;
    };
    for (const std::string& path : {orders_path(account_id), fills_path(account_id)}) {
        rejected(path + "?limit=0", "invalid_limit");
        rejected(path + "?limit=201", "invalid_limit");
        rejected(path + "?limit=abc", "invalid_limit");
        rejected(path + "?limit=5&limit=5", "duplicate_field");
        REQUIRE(rejected(path + "?offset=5", "unexpected_field").at("field") ==
                "offset");
        rejected(path + "?limit", "invalid_query");
        REQUIRE(rejected(path + "?cursor=garbage", "invalid_cursor").at("field") ==
                "cursor");
        REQUIRE(call(api, "GET", path + "?limit=200").status == 200);
    }

    const std::string fills_cursor = chronos::paper_trading::encode_history_cursor(
        chronos::paper_trading::HistoryCursorKind::Fills,
        account_id,
        1
    );
    rejected(orders_path(account_id) + "?cursor=" + fills_cursor, "invalid_cursor");
    const std::string other_cursor = chronos::paper_trading::encode_history_cursor(
        chronos::paper_trading::HistoryCursorKind::Fills,
        other_account,
        1
    );
    rejected(fills_path(account_id) + "?cursor=" + other_cursor, "invalid_cursor");

    REQUIRE(rejected("/health?verbose=1", "unexpected_field").at("field") ==
            "verbose");
    rejected(accounts_path(account_id) + "?limit=5", "unexpected_field");
    rejected(orders_path(account_id) + "/some-order?limit=5", "unexpected_field");
    rejected(orders_path(account_id) + "?dry_run=true", "unexpected_field", "POST");
    rejected("/api/v1/accounts?x=1", "unexpected_field", "POST");
}

void test_uncertain_submission_is_reconciled_over_http() {
    const std::string application = unique_test_id("http-reconcile");
    TestApi api(with_application_name(connection_string, application));
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-reconcile");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    require_error(
        submit(api, account_id, order("uncertain", "buy", "1")),
        503,
        "persistence_unavailable"
    );
    // Not recorded: resubmitting the same client_order_id is safe.
    require_error(
        call(api, "GET", orders_path(account_id) + "/uncertain"),
        404,
        "order_not_found"
    );
    const ApiResponse recorded = submit(api, account_id, order("uncertain", "buy", "1"));
    REQUIRE(recorded.status == 201);
    const ApiResponse reconciled = call(
        api,
        "GET",
        orders_path(account_id) + "/uncertain"
    );
    REQUIRE(reconciled.body.at("order") == recorded.body.at("order"));
    require_error(
        submit(api, account_id, order("uncertain", "buy", "1")),
        409,
        "duplicate_client_order_id"
    );
}

void test_read_outage_returns_generic_503_and_logs_detail() {
    const std::string application = unique_test_id("http-read-outage");
    TestApi api(with_application_name(connection_string, application));
    const std::string account_id = new_account(api, "http-read-outage");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    const Json error = require_error(
        call(api, "GET", orders_path(account_id)),
        503,
        "persistence_unavailable"
    );
    const std::string message = error.at("message");
    REQUIRE(message.find("PostgreSQL") == std::string::npos);
    REQUIRE(message.find("host") == std::string::npos);
    REQUIRE(api.diagnostics.size() == 1);
    REQUIRE(api.diagnostics.front().starts_with(
        "list orders: PostgreSQL connection is unavailable"
    ));
    REQUIRE(call(api, "GET", orders_path(account_id)).status == 200);
}

// A malformed intent can be journaled by an in-process caller. The API has
// no representation for it, so the history fails closed with a generic 500
// instead of misreporting the order.
void test_unrepresentable_journal_record_fails_closed() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-unrepresentable");
    {
        PostgresPaperOrderRepository direct(connection_string);
        auto session = PersistentPaperOrderSession::load(direct, account_id);
        REQUIRE(session.has_value());
        OrderIntent malformed{
            .client_order_id = "malformed",
            .account_id = account_id,
            .source = OrderSource{
                .kind = OrderSourceKind::Manual,
                .source_id = "in-process",
            },
            .instrument = InstrumentId("AAPL"),
            .side = static_cast<Side>(99),
            .type = OrderType::Market,
            .quantity = Quantity::parse("1"),
            .limit_price = std::nullopt,
            .time_in_force = TimeInForce::ImmediateOrCancel,
            .submitted_at = quote(0, 0).header.receive_timestamp,
        };
        static_cast<void>(session->submit(malformed, quote(0, 0)));
    }

    require_error(
        call(api, "GET", orders_path(account_id)),
        500,
        "internal_error"
    );
    require_error(
        call(api, "GET", orders_path(account_id) + "/malformed"),
        500,
        "internal_error"
    );
    REQUIRE(api.diagnostics.size() == 2);
    REQUIRE(
        api.diagnostics.front().find("undeclared enum value") !=
        std::string::npos
    );
    REQUIRE(call(api, "GET", fills_path(account_id)).status == 200);
}

void test_socket_server_serves_the_read_side() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "socket-history");
    submit_varied_history(api, account_id);
    PaperTradingHttpServer server(
        *api.api,
        HttpServerOptions{.host = "127.0.0.1", .port = 0},
        [](std::string_view) {}
    );
    const int port = server.bind();
    ServingThread serving(server);

    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(std::chrono::seconds(5));
    const auto get_json = [&client](const std::string& target, int status) {
        const auto result = client.Get(target);
        REQUIRE(static_cast<bool>(result));
        REQUIRE(result->status == status);
        REQUIRE(result->get_header_value("Content-Type") == "application/json");
        REQUIRE(result->get_header_value("X-Chronos-Trading-Mode") == "paper");
        const Json body = Json::parse(result->body);
        require_no_json_numbers(body);
        return body;
    };

    std::vector<std::string> paged;
    std::string target = orders_path(account_id) + "?limit=2";
    while (true) {
        const Json page = get_json(target, 200);
        for (const Json& resource : page.at("orders")) {
            paged.push_back(resource.at("client_order_id"));
        }
        if (page.at("next_cursor").is_null()) {
            break;
        }
        target = orders_path(account_id) + "?limit=2&cursor=" +
            page.at("next_cursor").get<std::string>();
    }
    REQUIRE((paged == std::vector<std::string>{
        "fill-c", "too-big", "passive", "fill-b", "fill-a",
    }));
    REQUIRE(get_json(orders_path(account_id) + "/fill-b", 200)
                .at("order").at("fill").at("notional") == "151.5");
    REQUIRE(get_json(fills_path(account_id), 200).at("fills").size() == 3);

    // The raw query reaches the strict parser, which the library's own
    // parameter map would have merged.
    REQUIRE(get_json(orders_path(account_id) + "?limit=1&limit=2", 400)
                .at("error").at("reason") == "duplicate_field");
    REQUIRE(get_json(orders_path(account_id) + "?%6Cimit=1", 200)
                .at("orders").size() == 1);
    REQUIRE(serving.stop().empty());
}

// ---------------------------------------------------------------------------
// Portfolio

std::string portfolio_path(const std::string& account_id) {
    return accounts_path(account_id) + "/portfolio";
}

// Buy 2 at the ask of 101 and sell 0.5 at the bid of 99 through a model
// source, both through the real submission route.
void submit_portfolio_orders(TestApi& api, const std::string& account_id) {
    REQUIRE(submit(api, account_id, order("buy", "buy", "2")).status == 201);
    REQUIRE(submit(
        api,
        account_id,
        order("model-sell", "sell", "0.5", std::nullopt, "model")
    ).status == 201);
}

// Written independently of the implementation: cost basis 202 - 50.5,
// realized PnL 49.5 - 50.5, cash 1000 - 202.01 + 49.49, fees 2 × 0.01, and
// at the bid of 99, market value 148.5, unrealized PnL -3, total PnL -4.02.
void require_expected_portfolio(const Json& portfolio, const std::string& account_id) {
    REQUIRE(portfolio.at("trading_mode") == "paper");
    REQUIRE(portfolio.at("account_id") == account_id);
    REQUIRE(portfolio.at("instrument") == "AAPL");
    REQUIRE(portfolio.at("account_version") == "2");
    REQUIRE(portfolio.at("status") == "active");
    REQUIRE(portfolio.at("valued_at") == "2026-09-25T13:30:00.000000000Z");
    REQUIRE(portfolio.at("mark_policy") == "best_bid");
    REQUIRE(portfolio.at("mark_price") == "99");
    REQUIRE(portfolio.at("market").at("ingest_sequence") == "0");
    REQUIRE(portfolio.at("market").at("quality_flags") == Json::array({"replay"}));
    REQUIRE(portfolio.at("starting_cash") == "1000");
    REQUIRE(portfolio.at("cash") == "847.48");
    REQUIRE(portfolio.at("position") == "1.5");
    REQUIRE(portfolio.at("open_cost_basis") == "151.5");
    REQUIRE(portfolio.at("average_entry_price") == "101");
    REQUIRE(portfolio.at("position_market_value") == "148.5");
    REQUIRE(portfolio.at("unrealized_pnl") == "-3");
    REQUIRE(portfolio.at("gross_realized_pnl") == "-1");
    REQUIRE(portfolio.at("fees_paid") == "0.02");
    REQUIRE(portfolio.at("total_pnl") == "-4.02");
    REQUIRE(portfolio.at("equity") == "995.98");
    REQUIRE(portfolio.at("return_percent") == "-0.402");
}

void test_portfolio_reflects_orders_submitted_through_the_api() {
    std::string account_id;
    Json before_restart;
    {
        TestApi api;
        apply(*api.market, quote(0, 0));
        account_id = new_account(api, "http-portfolio");
        const ApiResponse flat = call(api, "GET", portfolio_path(account_id));
        REQUIRE(flat.status == 200);
        REQUIRE(flat.body.at("portfolio").at("average_entry_price").is_null());
        REQUIRE(flat.body.at("portfolio").at("equity") == "1000");

        submit_portfolio_orders(api, account_id);
        const ApiResponse valued = call(api, "GET", portfolio_path(account_id));
        REQUIRE(valued.status == 200);
        require_expected_portfolio(valued.body.at("portfolio"), account_id);
        before_restart = valued.body;
        REQUIRE(call(api, "GET", accounts_path(account_id)).body.at("account")
                    .at("version") == "2");
    }

    TestApi restarted;
    apply(*restarted.market, quote(0, 0));
    const ApiResponse after_restart = call(
        restarted,
        "GET",
        portfolio_path(account_id)
    );
    REQUIRE(after_restart.status == 200);
    REQUIRE(after_restart.body == before_restart);
}

void test_portfolio_route_is_get_only_and_accepts_no_query() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-portfolio-route");
    for (const std::string_view method : {"POST", "PUT", "DELETE"}) {
        const ApiResponse rejected = call(
            api,
            std::string(method),
            portfolio_path(account_id),
            "{}"
        );
        require_error(rejected, 405, "method_not_allowed");
        REQUIRE(rejected.headers.front().value == "GET");
    }
    REQUIRE(require_error(
        call(api, "GET", portfolio_path(account_id) + "?mark=ask"),
        400,
        "malformed_request"
    ).at("reason") == "unexpected_field");
    REQUIRE(require_error(
        call(api, "GET", portfolio_path(account_id) + "?limit=1&limit=2"),
        400,
        "malformed_request"
    ).at("reason") == "unexpected_field");
    REQUIRE(require_error(
        call(api, "GET", portfolio_path(account_id) + "?mark"),
        400,
        "malformed_request"
    ).at("reason") == "invalid_query");
    require_error(
        call(api, "GET", portfolio_path(account_id) + "/history"),
        404,
        "route_not_found"
    );
}

void test_portfolio_failures_have_stable_codes() {
    TestApi api;
    const std::string account_id = new_account(api, "http-portfolio-errors");
    require_error(
        call(api, "GET", portfolio_path(unique_test_id("absent"))),
        404,
        "account_not_found"
    );
    REQUIRE(require_error(
        call(api, "GET", portfolio_path("bad id")),
        400,
        "malformed_request"
    ).at("reason") == "invalid_identifier");
    const auto unavailable_reason = [&](TestApi& target) {
        const ApiResponse response = call(target, "GET", portfolio_path(account_id));
        REQUIRE(!response.body.contains("portfolio"));
        return require_error(response, 503, "market_data_unavailable").at("reason");
    };
    REQUIRE(unavailable_reason(api) == "no_top_of_book");

    TestApi ask_only;
    TopOfBookEvent no_bid = quote(0, 0);
    no_bid.bid = std::nullopt;
    apply(*ask_only.market, no_bid);
    REQUIRE(unavailable_reason(ask_only) == "missing_best_bid");

    TestApi interrupted;
    apply(*interrupted.market, quote(0, 0));
    auto gap = chronos::validate_market_event(quote(2, 10));
    static_cast<void>(interrupted.market->apply(
        std::get<chronos::ValidatedMarketEvent>(gap)
    ));
    REQUIRE(unavailable_reason(interrupted) == "stream_interrupted");

    TestApi stale;
    apply(*stale.market, quote(0, 0));
    apply(*stale.market, quote(
        1,
        chronos::test::kMaximumQuoteAgeNanoseconds + 1,
        "SPY"
    ));
    REQUIRE(unavailable_reason(stale) == "stale_market_state");
}

void test_portfolio_rejects_a_replay_behind_the_account() {
    std::string account_id;
    {
        TestApi original;
        for (std::uint64_t sequence = 0; sequence <= 3; ++sequence) {
            apply(*original.market, quote(sequence, static_cast<std::int64_t>(sequence)));
        }
        account_id = new_account(original, "http-portfolio-timeline");
        REQUIRE(submit(original, account_id, order("at-three", "buy", "1")).status ==
                201);
    }
    TestApi earlier;
    apply(*earlier.market, quote(0, 0));
    apply(*earlier.market, quote(1, 1));
    REQUIRE(require_error(
        call(earlier, "GET", portfolio_path(account_id)),
        503,
        "market_data_unavailable"
    ).at("reason") == "incompatible_market_timeline");
}

void test_portfolio_outage_returns_a_generic_error() {
    const std::string application = unique_test_id("http-portfolio-outage");
    TestApi api(with_application_name(connection_string, application));
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "http-portfolio-outage");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    const Json error = require_error(
        call(api, "GET", portfolio_path(account_id)),
        503,
        "persistence_unavailable"
    );
    REQUIRE(error.at("message").get<std::string>().find("PostgreSQL") ==
            std::string::npos);
    REQUIRE(api.diagnostics.size() == 1);
    REQUIRE(api.diagnostics.front().starts_with(
        "get portfolio: PostgreSQL connection is unavailable"
    ));
    REQUIRE(call(api, "GET", portfolio_path(account_id)).status == 200);
}

void test_socket_server_serves_the_portfolio() {
    TestApi api;
    apply(*api.market, quote(0, 0));
    const std::string account_id = new_account(api, "socket-portfolio");
    submit_portfolio_orders(api, account_id);
    PaperTradingHttpServer server(
        *api.api,
        HttpServerOptions{.host = "127.0.0.1", .port = 0},
        [](std::string_view) {}
    );
    const int port = server.bind();
    ServingThread serving(server);

    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(std::chrono::seconds(5));
    const auto valued = client.Get(portfolio_path(account_id));
    REQUIRE(static_cast<bool>(valued));
    REQUIRE(valued->status == 200);
    REQUIRE(valued->get_header_value("Content-Type") == "application/json");
    REQUIRE(valued->get_header_value("X-Chronos-Trading-Mode") == "paper");
    REQUIRE(valued->get_header_value("Cache-Control") == "no-store");
    REQUIRE(valued->get_header_value("X-Content-Type-Options") == "nosniff");
    const Json body = Json::parse(valued->body);
    require_no_json_numbers(body);
    require_expected_portfolio(body.at("portfolio"), account_id);

    const auto with_query = client.Get(portfolio_path(account_id) + "?mark=mid");
    REQUIRE(static_cast<bool>(with_query));
    REQUIRE(with_query->status == 400);
    REQUIRE(with_query->get_header_value("X-Chronos-Trading-Mode") == "paper");
    REQUIRE(serving.stop().empty());
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
        .apply_migration(chronos::test::all_migrations_sql());

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
        {
            "order history pages newest first with cursors",
            test_order_history_pages_newest_first_with_cursors,
        },
        {
            "fill history returns only booked fills",
            test_fill_history_returns_only_booked_fills,
        },
        {
            "order lookup returns the submitted resource",
            test_order_lookup_returns_the_submitted_resource,
        },
        {
            "history queries are strict on every route",
            test_history_queries_are_strict_on_every_route,
        },
        {
            "uncertain submission is reconciled over HTTP",
            test_uncertain_submission_is_reconciled_over_http,
        },
        {
            "read outage returns generic 503",
            test_read_outage_returns_generic_503_and_logs_detail,
        },
        {
            "unrepresentable journal record fails closed",
            test_unrepresentable_journal_record_fails_closed,
        },
        {
            "socket server serves the read side",
            test_socket_server_serves_the_read_side,
        },
        {
            "portfolio reflects orders submitted through the API",
            test_portfolio_reflects_orders_submitted_through_the_api,
        },
        {
            "portfolio route is GET only and accepts no query",
            test_portfolio_route_is_get_only_and_accepts_no_query,
        },
        {
            "portfolio failures have stable codes",
            test_portfolio_failures_have_stable_codes,
        },
        {
            "portfolio rejects a replay behind the account",
            test_portfolio_rejects_a_replay_behind_the_account,
        },
        {
            "portfolio outage returns a generic error",
            test_portfolio_outage_returns_a_generic_error,
        },
        {
            "socket server serves the portfolio",
            test_socket_server_serves_the_portfolio,
        },
    });
}
