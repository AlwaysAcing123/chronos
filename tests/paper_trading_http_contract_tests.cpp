#include "chronos/paper_trading/paper_trading_http_contract.hpp"
#include "json_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::MarketReplayError;
using chronos::MarketReplayErrorCode;
using chronos::MarketStateStatus;
using chronos::MarketStateUnavailableCode;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperOrderProcessingError;
using chronos::PaperOrderProcessingErrorCode;
using chronos::PaperOrderProcessor;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::paper_trading::CreatePaperAccountRequest;
using chronos::paper_trading::HttpResponse;
using chronos::paper_trading::MalformedRequest;
using chronos::paper_trading::MalformedRequestReason;
using chronos::paper_trading::PaperAccountView;
using chronos::paper_trading::PaperOrderRequest;
using chronos::paper_trading::PaperTradingError;
using chronos::paper_trading::PaperTradingErrorCode;
using chronos::paper_trading::PaperTradingHealth;
using chronos::paper_trading::RecordedPaperOrder;
using chronos::test::kMarketOpenNanoseconds;
using chronos::test::paper_risk_limits;
using chronos::test::quote;
using chronos::test::require_no_json_numbers;
using Json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers

Json response_json(const HttpResponse& response) {
    Json body = Json::parse(response.body);
    require_no_json_numbers(body);
    return body;
}

MalformedRequest require_malformed_order(
    std::string_view body,
    MalformedRequestReason reason,
    std::string_view field
) {
    auto parsed = chronos::paper_trading::parse_order_request("account", body);
    REQUIRE(std::holds_alternative<MalformedRequest>(parsed));
    MalformedRequest malformed = std::get<MalformedRequest>(std::move(parsed));
    REQUIRE(malformed.reason == reason);
    REQUIRE(malformed.field == field);
    REQUIRE(!malformed.message.empty());
    return malformed;
}

PaperOrderRequest require_order(std::string_view body) {
    auto parsed = chronos::paper_trading::parse_order_request("account", body);
    REQUIRE(std::holds_alternative<PaperOrderRequest>(parsed));
    return std::get<PaperOrderRequest>(std::move(parsed));
}

Json valid_order_json() {
    return Json{
        {"client_order_id", "order-1"},
        {"source", {{"kind", "manual"}, {"id", "local-trader"}}},
        {"instrument", "AAPL"},
        {"side", "buy"},
        {"type", "limit"},
        {"quantity", "2.5"},
        {"limit_price", "101.25"},
        {"time_in_force", "ioc"},
    };
}

std::string order_with(std::string_view field, Json value) {
    Json order = valid_order_json();
    order[std::string(field)] = std::move(value);
    return order.dump();
}

std::string order_without(std::string_view field) {
    Json order = valid_order_json();
    order.erase(std::string(field));
    return order.dump();
}

OrderIntent intent(
    std::string client_order_id,
    Side side,
    std::string_view quantity,
    std::int64_t submitted_offset = 0,
    std::optional<std::string_view> limit_price = std::nullopt
) {
    return OrderIntent{
        .client_order_id = std::move(client_order_id),
        .account_id = "account-1",
        .source = OrderSource{
            .kind = OrderSourceKind::Model,
            .source_id = "momentum-v1",
        },
        .instrument = InstrumentId("AAPL"),
        .side = side,
        .type = limit_price ? OrderType::Limit : OrderType::Market,
        .quantity = Quantity::parse(quantity),
        .limit_price = limit_price
            ? std::optional<Price>(Price::parse(*limit_price))
            : std::nullopt,
        .time_in_force = TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(
            kMarketOpenNanoseconds + submitted_offset
        ),
    };
}

PaperOrderProcessor processor() {
    return PaperOrderProcessor(
        "account-1",
        InstrumentId("AAPL"),
        Money::parse("1000"),
        paper_risk_limits()
    );
}

PaperAccountView view_of(const PaperOrderProcessor& source, std::int64_t version) {
    return PaperAccountView{
        .account_id = source.account().account_id(),
        .instrument = source.account().instrument(),
        .state = source.account().state(),
        .risk_limits = source.risk_limits(),
        .is_healthy = source.is_healthy(),
        .version = version,
    };
}

RecordedPaperOrder record(
    PaperOrderProcessor& source,
    OrderIntent order,
    TopOfBookEvent market,
    std::int64_t version
) {
    auto result = source.submit(order, market);
    return RecordedPaperOrder{
        .intent = std::move(order),
        .market = std::move(market),
        .result = std::move(result),
        .account = view_of(source, version),
    };
}

Json require_error_body(
    const HttpResponse& response,
    int status,
    std::string_view code
) {
    REQUIRE(response.status == status);
    Json body = response_json(response);
    REQUIRE(body.at("error").at("code") == code);
    REQUIRE(body.at("error").at("message").is_string());
    REQUIRE(!body.at("error").at("message").get<std::string>().empty());
    return body;
}

// ---------------------------------------------------------------------------
// Request parsing

void test_create_account_request_parses_exact_values() {
    auto parsed = chronos::paper_trading::parse_create_account_request(
        R"({"account_id":"acct-1","instrument":"AAPL",)"
        R"("starting_cash":"25000.12345678"})"
    );
    REQUIRE(std::holds_alternative<CreatePaperAccountRequest>(parsed));
    const auto& request = std::get<CreatePaperAccountRequest>(parsed);
    REQUIRE(request.account_id == "acct-1");
    REQUIRE(request.instrument == "AAPL");
    REQUIRE(request.starting_cash.raw() == 2'500'012'345'678);

    auto extra = chronos::paper_trading::parse_create_account_request(
        R"({"account_id":"a","instrument":"AAPL","starting_cash":"1",)"
        R"("risk_limits":{}})"
    );
    REQUIRE(std::holds_alternative<MalformedRequest>(extra));
    REQUIRE(
        std::get<MalformedRequest>(extra).reason ==
        MalformedRequestReason::UnexpectedField
    );
    REQUIRE(std::get<MalformedRequest>(extra).field == "risk_limits");
}

void test_order_request_parses_every_field_and_optional_limit() {
    const PaperOrderRequest limit = require_order(valid_order_json().dump());
    REQUIRE(limit.account_id == "account");
    REQUIRE(limit.client_order_id == "order-1");
    REQUIRE(limit.source.kind == OrderSourceKind::Manual);
    REQUIRE(limit.source.source_id == "local-trader");
    REQUIRE(limit.instrument == "AAPL");
    REQUIRE(limit.side == Side::Buy);
    REQUIRE(limit.type == OrderType::Limit);
    REQUIRE(limit.quantity == Quantity::parse("2.5"));
    REQUIRE(limit.limit_price == Price::parse("101.25"));
    REQUIRE(limit.time_in_force == TimeInForce::ImmediateOrCancel);

    Json market_order = valid_order_json();
    market_order.erase("limit_price");
    market_order["type"] = "market";
    market_order["side"] = "sell";
    market_order["source"] = {{"kind", "model"}, {"id", "momentum-v1.2"}};
    const PaperOrderRequest market = require_order(market_order.dump());
    REQUIRE(market.type == OrderType::Market);
    REQUIRE(market.side == Side::Sell);
    REQUIRE(market.source.kind == OrderSourceKind::Model);
    REQUIRE(!market.limit_price.has_value());

    // Domain rules are not duplicated here: the risk engine rejects these.
    REQUIRE(require_order(order_with("quantity", "0")).quantity == Quantity{});
    REQUIRE(
        require_order(order_with("quantity", "-3")).quantity ==
        Quantity::parse("-3")
    );
    REQUIRE(
        require_order(order_with("quantity", "1.50")).quantity ==
        Quantity::parse("1.5")
    );
}

void test_decimal_text_grammar_is_strict() {
    for (const std::string_view invalid : {
             "",
             "-",
             "+1",
             ".5",
             "5.",
             "1e3",
             "1E3",
             "01",
             "-01",
             "00.5",
             "1.2.3",
             " 1",
             "1 ",
             "0x10",
             "NaN",
             "Infinity",
             "1,000",
             "1_000",
             "--1",
             "\xd9\xa1",
         }) {
        require_malformed_order(
            order_with("quantity", std::string(invalid)),
            MalformedRequestReason::InvalidDecimal,
            "quantity"
        );
    }
    require_malformed_order(
        order_with("limit_price", "101.x"),
        MalformedRequestReason::InvalidDecimal,
        "limit_price"
    );
    REQUIRE(require_order(order_with("quantity", "0.5")).quantity ==
            Quantity::parse("0.5"));
    REQUIRE(require_order(order_with("quantity", "-0")).quantity == Quantity{});
}

void test_decimal_precision_and_range_are_enforced_per_type() {
    REQUIRE(require_order(order_with("quantity", "1.00000001")).quantity ==
            Quantity::from_raw(100'000'001));
    require_malformed_order(
        order_with("quantity", "1.000000001"),
        MalformedRequestReason::TooManyFractionalDigits,
        "quantity"
    );
    REQUIRE(require_order(order_with("limit_price", "101.000000001"))
                .limit_price == Price::from_raw(101'000'000'001));
    require_malformed_order(
        order_with("limit_price", "101.0000000001"),
        MalformedRequestReason::TooManyFractionalDigits,
        "limit_price"
    );

    constexpr auto kMaximumRaw = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMinimumRaw = std::numeric_limits<std::int64_t>::min();
    REQUIRE(require_order(order_with("quantity", "92233720368.54775807"))
                .quantity == Quantity::from_raw(kMaximumRaw));
    REQUIRE(require_order(order_with("quantity", "-92233720368.54775808"))
                .quantity == Quantity::from_raw(kMinimumRaw));
    require_malformed_order(
        order_with("quantity", "92233720368.54775808"),
        MalformedRequestReason::DecimalOutOfRange,
        "quantity"
    );
    require_malformed_order(
        order_with("limit_price", "9223372037"),
        MalformedRequestReason::DecimalOutOfRange,
        "limit_price"
    );
    require_malformed_order(
        order_with("quantity", std::string(400, '9')),
        MalformedRequestReason::DecimalOutOfRange,
        "quantity"
    );

    auto rich = chronos::paper_trading::parse_create_account_request(
        R"({"account_id":"a","instrument":"AAPL",)"
        R"("starting_cash":"92233720368.54775808"})"
    );
    REQUIRE(std::holds_alternative<MalformedRequest>(rich));
    REQUIRE(
        std::get<MalformedRequest>(rich).reason ==
        MalformedRequestReason::DecimalOutOfRange
    );
    REQUIRE(std::get<MalformedRequest>(rich).field == "starting_cash");
}

void test_non_string_decimals_are_rejected() {
    for (const Json& value : {
             Json(10),
             Json(1.5),
             Json(-2),
             Json(true),
             Json(nullptr),
             Json::object(),
             Json::array(),
         }) {
        require_malformed_order(
            order_with("quantity", value),
            MalformedRequestReason::InvalidType,
            "quantity"
        );
    }
    require_malformed_order(
        order_with("limit_price", nullptr),
        MalformedRequestReason::InvalidType,
        "limit_price"
    );
    require_malformed_order(
        R"({"client_order_id":"o","source":{"kind":"manual","id":"u"},)"
        R"("instrument":"AAPL","side":"buy","type":"market",)"
        R"("quantity":100000000000000000000000,"time_in_force":"ioc"})",
        MalformedRequestReason::InvalidType,
        "quantity"
    );
}

void test_enum_values_are_closed_and_case_sensitive() {
    const MalformedRequest side = require_malformed_order(
        order_with("side", "short"),
        MalformedRequestReason::UnknownEnumValue,
        "side"
    );
    REQUIRE(side.message == "side must be one of: buy, sell");
    require_malformed_order(
        order_with("side", "BUY"),
        MalformedRequestReason::UnknownEnumValue,
        "side"
    );
    require_malformed_order(
        order_with("type", "stop"),
        MalformedRequestReason::UnknownEnumValue,
        "type"
    );
    require_malformed_order(
        order_with("time_in_force", "day"),
        MalformedRequestReason::UnknownEnumValue,
        "time_in_force"
    );
    require_malformed_order(
        order_with("source", Json{{"kind", "robot"}, {"id", "r"}}),
        MalformedRequestReason::UnknownEnumValue,
        "source.kind"
    );
    require_malformed_order(
        order_with("side", 1),
        MalformedRequestReason::InvalidType,
        "side"
    );
}

void test_request_structure_is_strict() {
    require_malformed_order("", MalformedRequestReason::InvalidJson, "");
    require_malformed_order("{", MalformedRequestReason::InvalidJson, "");
    require_malformed_order(
        "{\"client_order_id\":\"\xff\"}",
        MalformedRequestReason::InvalidJson,
        ""
    );
    require_malformed_order(
        "{/* comment */}",
        MalformedRequestReason::InvalidJson,
        ""
    );
    require_malformed_order("[]", MalformedRequestReason::InvalidType, "");
    require_malformed_order("\"order\"", MalformedRequestReason::InvalidType, "");

    require_malformed_order(
        R"({"quantity":"1","quantity":"1000"})",
        MalformedRequestReason::DuplicateField,
        "quantity"
    );
    require_malformed_order(
        R"({"source":{"kind":"manual","kind":"model","id":"u"}})",
        MalformedRequestReason::DuplicateField,
        "source.kind"
    );
    require_malformed_order(
        order_with("submitted_at", "2026-09-25T13:30:00Z"),
        MalformedRequestReason::UnexpectedField,
        "submitted_at"
    );
    require_malformed_order(
        order_with("price", "1"),
        MalformedRequestReason::UnexpectedField,
        "price"
    );
    require_malformed_order(
        order_with("account_id", "someone-else"),
        MalformedRequestReason::UnexpectedField,
        "account_id"
    );
    require_malformed_order(
        order_with(
            "source",
            Json{{"kind", "manual"}, {"id", "u"}, {"account", "a"}}
        ),
        MalformedRequestReason::UnexpectedField,
        "source.account"
    );
    require_malformed_order(
        order_with("source", "manual"),
        MalformedRequestReason::InvalidType,
        "source"
    );
    require_malformed_order(
        order_with("source", Json{{"kind", "manual"}}),
        MalformedRequestReason::MissingField,
        "source.id"
    );
    for (const std::string_view field : {
             "client_order_id",
             "source",
             "instrument",
             "side",
             "type",
             "quantity",
             "time_in_force",
         }) {
        require_malformed_order(
            order_without(field),
            MalformedRequestReason::MissingField,
            field
        );
    }
}

// ---------------------------------------------------------------------------
// Response rendering

void test_timestamps_render_as_rfc3339_with_nanoseconds() {
    using chronos::paper_trading::format_timestamp;
    const auto at = [](std::int64_t nanoseconds) {
        return format_timestamp(Timestamp::from_nanoseconds(nanoseconds));
    };
    REQUIRE(at(0) == "1970-01-01T00:00:00.000000000Z");
    REQUIRE(at(1) == "1970-01-01T00:00:00.000000001Z");
    REQUIRE(at(-1) == "1969-12-31T23:59:59.999999999Z");
    REQUIRE(
        at(kMarketOpenNanoseconds + 123'456'789) ==
        "2026-09-25T13:30:00.123456789Z"
    );
    REQUIRE(at(1'709'251'199'000'000'000) == "2024-02-29T23:59:59.000000000Z");
    REQUIRE(
        at(std::numeric_limits<std::int64_t>::max()) ==
        "2262-04-11T23:47:16.854775807Z"
    );
    REQUIRE(
        at(std::numeric_limits<std::int64_t>::min()) ==
        "1677-09-21T00:12:43.145224192Z"
    );
}

void test_executed_orders_render_exact_strings_and_status_201() {
    auto engine = processor();
    const TopOfBookEvent market = quote(7, 0);
    const RecordedPaperOrder filled = record(
        engine,
        intent("fill", Side::Buy, "2.5"),
        market,
        1
    );
    const HttpResponse response =
        chronos::paper_trading::render_order_submission(filled);
    REQUIRE(response.status == 201);
    const Json body = response_json(response);
    REQUIRE(!body.contains("error"));

    const Json& order = body.at("order");
    REQUIRE(order.at("trading_mode") == "paper");
    REQUIRE(order.at("client_order_id") == "fill");
    REQUIRE(order.at("source") == Json({{"kind", "model"}, {"id", "momentum-v1"}}));
    REQUIRE(order.at("type") == "market");
    REQUIRE(order.at("limit_price").is_null());
    REQUIRE(order.at("time_in_force") == "ioc");
    REQUIRE(order.at("submitted_at") == "2026-09-25T13:30:00.000000000Z");
    REQUIRE(order.at("status") == "filled");
    REQUIRE(order.at("filled_quantity") == "2.5");
    REQUIRE(order.at("unfilled_quantity") == "0");
    REQUIRE(order.at("cancellation_reason").is_null());
    REQUIRE(order.at("fill") == Json({
        {"fill_id", "fill:fill:0"},
        {"side", "buy"},
        {"price", "101"},
        {"quantity", "2.5"},
        {"notional", "252.5"},
        {"fee", "0.01"},
        {"filled_at", "2026-09-25T13:30:00.000000000Z"},
    }));
    REQUIRE(order.at("market").at("stream_id") == chronos::test::kTestStream);
    REQUIRE(order.at("market").at("ingest_sequence") == "7");
    REQUIRE(order.at("market").at("venue").is_null());
    REQUIRE(
        order.at("market").at("quality_flags") == Json::array({"replay"})
    );
    REQUIRE(
        order.at("market").at("event_timestamp") ==
        "2026-09-25T13:29:59.999999750Z"
    );
    REQUIRE(order.at("market").at("ask") == Json({
        {"price", "101"},
        {"quantity", "5"},
    }));

    const Json& account = body.at("account");
    REQUIRE(account.at("version") == "1");
    REQUIRE(account.at("status") == "active");
    REQUIRE(account.at("cash") == "747.49");
    REQUIRE(account.at("position") == "2.5");
    REQUIRE(account.at("open_cost_basis") == "252.5");
    REQUIRE(account.at("average_entry_price") == "101");
    REQUIRE(account.at("gross_realized_pnl") == "0");
    REQUIRE(account.at("fees_paid") == "0.01");
    REQUIRE(account.at("last_fill_at") == "2026-09-25T13:30:00.000000000Z");
    REQUIRE(account.at("risk_limits").at("maximum_quote_age_ns") == "1000000000");

    const Json partial = response_json(
        chronos::paper_trading::render_order_submission(record(
            engine,
            intent("partial", Side::Buy, "4"),
            market,
            2
        ))
    ).at("order");
    REQUIRE(partial.at("status") == "partially_filled_and_cancelled");
    REQUIRE(partial.at("filled_quantity") == "2.5");
    REQUIRE(partial.at("unfilled_quantity") == "1.5");
    REQUIRE(
        partial.at("cancellation_reason") == "displayed_liquidity_exhausted"
    );

    const HttpResponse cancelled_response =
        chronos::paper_trading::render_order_submission(record(
            engine,
            intent("passive", Side::Buy, "1", 0, "100.5"),
            market,
            3
        ));
    REQUIRE(cancelled_response.status == 201);
    const Json cancelled = response_json(cancelled_response).at("order");
    REQUIRE(cancelled.at("status") == "cancelled");
    REQUIRE(cancelled.at("limit_price") == "100.5");
    REQUIRE(cancelled.at("cancellation_reason") == "limit_not_marketable");
    REQUIRE(cancelled.at("fill").is_null());
}

void test_recorded_rejections_render_422_with_order_and_account() {
    auto engine = processor();
    const TopOfBookEvent market = quote(0, 0);

    const Json risk = require_error_body(
        chronos::paper_trading::render_order_submission(record(
            engine,
            intent("too-large", Side::Buy, "50"),
            market,
            1
        )),
        422,
        "risk_rejected"
    );
    REQUIRE(risk.at("error").at("reason") == "insufficient_cash");
    REQUIRE(risk.at("error").at("field") == "quantity");
    REQUIRE(risk.at("order").at("status") == "rejected");
    REQUIRE(risk.at("order").at("filled_quantity") == "0");
    REQUIRE(risk.at("order").at("unfilled_quantity") == "50");
    REQUIRE(risk.at("account").at("version") == "1");

    const Json stale = require_error_body(
        chronos::paper_trading::render_order_submission(record(
            engine,
            intent(
                "stale",
                Side::Buy,
                "1",
                chronos::test::kMaximumQuoteAgeNanoseconds + 1
            ),
            market,
            2
        )),
        422,
        "market_data_stale"
    );
    REQUIRE(stale.at("error").at("reason") == "stale_market_state");
    REQUIRE(stale.at("error").at("field").is_null());

    static_cast<void>(record(engine, intent("seed", Side::Buy, "1"), market, 3));
    TopOfBookEvent other_stream = quote(1, 0);
    other_stream.header.stream = MarketDataStreamId("another-stream");
    const Json execution = require_error_body(
        chronos::paper_trading::render_order_submission(record(
            engine,
            intent("wrong-stream", Side::Buy, "1"),
            other_stream,
            4
        )),
        422,
        "execution_rejected"
    );
    REQUIRE(execution.at("error").at("reason") == "market_stream_mismatch");
    REQUIRE(execution.at("error").at("field").is_null());
    REQUIRE(execution.at("order").at("market").at("stream_id") == "another-stream");

    RecordedPaperOrder faulted = record(
        engine,
        intent("faulted", Side::Buy, "1"),
        market,
        5
    );
    faulted.result = PaperOrderProcessingError{
        .code = PaperOrderProcessingErrorCode::AccountingInvariantViolation,
        .message = "risk-approved simulated fill was rejected by accounting",
        .execution = std::nullopt,
        .accounting_error = std::nullopt,
    };
    faulted.account.is_healthy = false;
    const Json fault = require_error_body(
        chronos::paper_trading::render_order_submission(faulted),
        500,
        "processing_fault"
    );
    REQUIRE(fault.at("error").at("reason") == "accounting_invariant_violation");
    REQUIRE(fault.at("order").at("status") == "faulted");
    REQUIRE(fault.at("order").at("fill").is_null());
    REQUIRE(fault.at("account").at("status") == "halted");
}

void test_unrecorded_service_errors_map_to_stable_status_codes() {
    struct Expected {
        PaperTradingErrorCode code;
        int status;
        std::string_view error_code;
    };
    for (const Expected& expected : {
             Expected{PaperTradingErrorCode::InvalidIdentifier, 400,
                      "malformed_request"},
             Expected{PaperTradingErrorCode::UnsupportedInstrument, 422,
                      "unsupported_instrument"},
             Expected{PaperTradingErrorCode::InvalidStartingCash, 422,
                      "invalid_starting_cash"},
             Expected{PaperTradingErrorCode::AccountAlreadyExists, 409,
                      "account_already_exists"},
             Expected{PaperTradingErrorCode::AccountNotFound, 404,
                      "account_not_found"},
             Expected{PaperTradingErrorCode::MarketDataUnavailable, 503,
                      "market_data_unavailable"},
             Expected{PaperTradingErrorCode::DuplicateClientOrderId, 409,
                      "duplicate_client_order_id"},
             Expected{PaperTradingErrorCode::AccountVersionConflict, 409,
                      "account_version_conflict"},
             Expected{PaperTradingErrorCode::PersistenceUnavailable, 503,
                      "persistence_unavailable"},
         }) {
        const PaperTradingError failure{
            .code = expected.code,
            .field = "field-name",
            .message = "internal detail from host=db password=secret",
            .market_state = MarketStateUnavailableCode::StreamInterrupted,
        };
        const Json body = require_error_body(
            chronos::paper_trading::render_order_submission(failure),
            expected.status,
            expected.error_code
        );
        REQUIRE(!body.contains("order"));
        REQUIRE(!body.contains("account"));
        const Json account_body = require_error_body(
            chronos::paper_trading::render_account(failure, 200),
            expected.status,
            expected.error_code
        );
        REQUIRE(account_body == body);
    }

    const auto render = [](PaperTradingErrorCode code) {
        return response_json(chronos::paper_trading::render_order_submission(
            PaperTradingError{
                .code = code,
                .field = "client_order_id",
                .message = "internal detail from host=db password=secret",
                .market_state = MarketStateUnavailableCode::NoTopOfBook,
            }
        )).at("error");
    };
    REQUIRE(render(PaperTradingErrorCode::InvalidIdentifier).at("reason") ==
            "invalid_identifier");
    REQUIRE(render(PaperTradingErrorCode::MarketDataUnavailable).at("reason") ==
            "no_top_of_book");
    const std::string persistence_message =
        render(PaperTradingErrorCode::PersistenceUnavailable).at("message");
    REQUIRE(persistence_message.find("secret") == std::string::npos);
    REQUIRE(persistence_message.find("same identifiers") != std::string::npos);
    REQUIRE(persistence_message.find("client_order_id") == std::string::npos);
}

void test_account_and_health_render_without_numbers() {
    auto engine = processor();
    const Json flat = response_json(chronos::paper_trading::render_account(
        view_of(engine, 0),
        201
    )).at("account");
    REQUIRE(flat.at("average_entry_price").is_null());
    REQUIRE(flat.at("last_fill_at").is_null());
    REQUIRE(flat.at("version") == "0");
    REQUIRE(flat.at("starting_cash") == "1000");

    MarketStateStatus market{
        .stream = MarketDataStreamId("stream-a"),
        .last_ingest_sequence = std::numeric_limits<std::uint64_t>::max(),
        .market_time = Timestamp::from_nanoseconds(kMarketOpenNanoseconds),
        .interruption = std::nullopt,
    };
    const HttpResponse healthy = chronos::paper_trading::render_health(
        PaperTradingHealth{.persistence_available = true, .market = market}
    );
    REQUIRE(healthy.status == 200);
    const Json healthy_body = response_json(healthy);
    REQUIRE(healthy_body.at("status") == "ok");
    REQUIRE(healthy_body.at("trading_mode") == "paper");
    REQUIRE(
        healthy_body.at("market_data").at("last_ingest_sequence") ==
        "18446744073709551615"
    );
    REQUIRE(healthy_body.at("market_data").at("state") == "accepting");

    market.interruption = MarketReplayError{
        .code = MarketReplayErrorCode::ReceiveTimeMovedBackward,
        .field = "header.receive_timestamp",
        .message = "market replay rejected receive time",
        .stream_error = std::nullopt,
        .clock_error = std::nullopt,
    };
    const HttpResponse unavailable = chronos::paper_trading::render_health(
        PaperTradingHealth{.persistence_available = false, .market = market}
    );
    REQUIRE(unavailable.status == 503);
    const Json unavailable_body = response_json(unavailable);
    REQUIRE(unavailable_body.at("persistence") == "unavailable");
    REQUIRE(unavailable_body.at("market_data").at("state") == "interrupted");
}

void test_malformed_requests_render_reason_and_field() {
    const Json body = require_error_body(
        chronos::paper_trading::render_malformed_request(MalformedRequest{
            .reason = MalformedRequestReason::TooManyFractionalDigits,
            .field = "quantity",
            .message = "quantity supports at most 8 fractional digits",
        }),
        400,
        "malformed_request"
    );
    REQUIRE(body.at("error").at("reason") == "too_many_fractional_digits");
    REQUIRE(body.at("error").at("field") == "quantity");

    const Json whole_body = require_error_body(
        chronos::paper_trading::render_malformed_request(MalformedRequest{
            .reason = MalformedRequestReason::InvalidJson,
            .field = "",
            .message = "request body is not valid UTF-8 JSON",
        }),
        400,
        "malformed_request"
    );
    REQUIRE(whole_body.at("error").at("field").is_null());
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "create account request parses exact values",
            test_create_account_request_parses_exact_values,
        },
        {
            "order request parses every field",
            test_order_request_parses_every_field_and_optional_limit,
        },
        {"decimal grammar is strict", test_decimal_text_grammar_is_strict},
        {
            "decimal precision and range per type",
            test_decimal_precision_and_range_are_enforced_per_type,
        },
        {"non-string decimals are rejected", test_non_string_decimals_are_rejected},
        {
            "enum values are closed and case sensitive",
            test_enum_values_are_closed_and_case_sensitive,
        },
        {"request structure is strict", test_request_structure_is_strict},
        {
            "timestamps render as RFC 3339 nanoseconds",
            test_timestamps_render_as_rfc3339_with_nanoseconds,
        },
        {
            "executed orders render exact strings",
            test_executed_orders_render_exact_strings_and_status_201,
        },
        {
            "recorded rejections render 422",
            test_recorded_rejections_render_422_with_order_and_account,
        },
        {
            "unrecorded errors map to stable codes",
            test_unrecorded_service_errors_map_to_stable_status_codes,
        },
        {
            "account and health render without numbers",
            test_account_and_health_render_without_numbers,
        },
        {
            "malformed requests render reason and field",
            test_malformed_requests_render_reason_and_field,
        },
    });
}
