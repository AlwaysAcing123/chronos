#include "chronos/paper_trading/paper_trading_http_contract.hpp"
#include "json_test_support.hpp"
#include "paper_trading_test_support.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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
using chronos::PaperFillRecord;
using chronos::PaperOrderFaultRecord;
using chronos::PaperOrderProcessingError;
using chronos::PaperOrderRecord;
using chronos::PaperOrderReceipt;
using chronos::PortfolioValuation;
using chronos::PortfolioValuationErrorCode;
using chronos::PreTradeRiskRejection;
using chronos::PaperOrderProcessingErrorCode;
using chronos::PaperOrderProcessor;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::paper_trading::CreatePaperAccountRequest;
using chronos::paper_trading::HistoryPageRequest;
using chronos::paper_trading::PaperFillHistoryPage;
using chronos::paper_trading::PaperOrderHistoryPage;
using chronos::paper_trading::HttpResponse;
using chronos::paper_trading::MalformedRequest;
using chronos::paper_trading::MalformedRequestReason;
using chronos::paper_trading::PaperAccountView;
using chronos::paper_trading::PaperOrderRequest;
using chronos::paper_trading::PaperPortfolioView;
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
        PaperTradingHealth{
            .persistence_available = true,
            .market = market,
            .tradeable_instruments = {InstrumentId("SPY"), InstrumentId("AAPL")},
        }
    );
    REQUIRE(healthy.status == 200);
    const Json healthy_body = response_json(healthy);
    require_no_json_numbers(healthy_body);
    REQUIRE(healthy_body.at("status") == "ok");
    REQUIRE(healthy_body.at("trading_mode") == "paper");
    // Configuration order is preserved, not sorted.
    REQUIRE(
        healthy_body.at("tradeable_instruments") == Json::array({"SPY", "AAPL"})
    );
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
        PaperTradingHealth{
            .persistence_available = false,
            .market = market,
            .tradeable_instruments = {InstrumentId("AAPL")},
        }
    );
    REQUIRE(unavailable.status == 503);
    const Json unavailable_body = response_json(unavailable);
    REQUIRE(unavailable_body.at("persistence") == "unavailable");
    REQUIRE(
        unavailable_body.at("tradeable_instruments") == Json::array({"AAPL"})
    );
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

// ---------------------------------------------------------------------------
// History queries and resources

HistoryPageRequest require_history_query(std::string_view query) {
    auto parsed = chronos::paper_trading::parse_history_query(query);
    REQUIRE(std::holds_alternative<HistoryPageRequest>(parsed));
    return std::get<HistoryPageRequest>(std::move(parsed));
}

MalformedRequest require_bad_history_query(
    std::string_view query,
    MalformedRequestReason reason,
    std::string_view field
) {
    auto parsed = chronos::paper_trading::parse_history_query(query);
    REQUIRE(std::holds_alternative<MalformedRequest>(parsed));
    MalformedRequest malformed = std::get<MalformedRequest>(std::move(parsed));
    REQUIRE(malformed.reason == reason);
    REQUIRE(malformed.field == field);
    REQUIRE(!malformed.message.empty());
    return malformed;
}

void test_history_queries_parse_limit_and_cursor_strictly() {
    const HistoryPageRequest empty = require_history_query("");
    REQUIRE(!empty.limit.has_value());
    REQUIRE(!empty.cursor.has_value());
    const HistoryPageRequest both = require_history_query(
        "limit=25&cursor=djF8b3JkZXJzfGFjY3QtMXw0Mg"
    );
    REQUIRE(both.limit == 25U);
    REQUIRE(both.cursor == "djF8b3JkZXJzfGFjY3QtMXw0Mg");
    REQUIRE(require_history_query("cursor=abc&limit=1").limit == 1U);
    REQUIRE(require_history_query("%6Cimit=7").limit == 7U);
    REQUIRE(require_history_query("cursor=a%2Db").cursor == "a-b");
    // Range is the service's rule; syntax alone admits zero and large values.
    REQUIRE(require_history_query("limit=0").limit == 0U);
    REQUIRE(require_history_query("limit=100000").limit == 100'000U);
    REQUIRE(require_history_query("cursor=").cursor == "");

    for (const std::string_view limit : {
             "limit=",
             "limit=-1",
             "limit=+5",
             "limit=05",
             "limit=1.5",
             "limit=1e2",
             "limit=%205",
             "limit=five",
             "limit=99999999999999999999999",
         }) {
        require_bad_history_query(
            limit,
            MalformedRequestReason::InvalidLimit,
            "limit"
        );
    }
    require_bad_history_query(
        "limit=5&limit=6",
        MalformedRequestReason::DuplicateField,
        "limit"
    );
    require_bad_history_query(
        "cursor=a&limit=5&cursor=b",
        MalformedRequestReason::DuplicateField,
        "cursor"
    );
    require_bad_history_query(
        "limit=5&offset=10",
        MalformedRequestReason::UnexpectedField,
        "offset"
    );
    require_bad_history_query(
        "Limit=5",
        MalformedRequestReason::UnexpectedField,
        "Limit"
    );
    const MalformedRequest unsafe = require_bad_history_query(
        "%FF%00=1",
        MalformedRequestReason::UnexpectedField,
        ""
    );
    REQUIRE(unsafe.message == "query string contains an unrecognized parameter");
    for (const std::string_view syntax : {
             "&",
             "limit=5&",
             "&limit=5",
             "limit",
             "=5",
             "limit=5&&cursor=a",
             "limit=%",
             "limit=%4",
             "limit=%zz",
         }) {
        require_bad_history_query(syntax, MalformedRequestReason::InvalidQuery, "");
    }
}

void test_parameterless_routes_reject_every_query_parameter() {
    using chronos::paper_trading::reject_query_parameters;
    REQUIRE(!reject_query_parameters("").has_value());
    const auto unknown = reject_query_parameters("verbose=true");
    REQUIRE(unknown.has_value());
    REQUIRE(unknown->reason == MalformedRequestReason::UnexpectedField);
    REQUIRE(unknown->field == "verbose");
    const auto limit = reject_query_parameters("limit=5");
    REQUIRE(limit->reason == MalformedRequestReason::UnexpectedField);
    REQUIRE(reject_query_parameters("x")->reason ==
            MalformedRequestReason::InvalidQuery);
}

PaperOrderRecord journal_record(
    std::int64_t record_id,
    const RecordedPaperOrder& recorded
) {
    PaperOrderRecord record{
        .record_id = record_id,
        .intent = recorded.intent,
        .market = recorded.market,
        .outcome = PreTradeRiskRejection{},
    };
    std::visit(
        [&record](const auto& result) {
            using Result = std::decay_t<decltype(result)>;
            if constexpr (std::is_same_v<Result, PaperOrderReceipt>) {
                record.outcome = result.execution;
            } else if constexpr (std::is_same_v<Result, PaperOrderProcessingError>) {
                record.outcome = PaperOrderFaultRecord{
                    .code = result.code,
                    .message = result.message,
                    .unbooked_execution = result.execution,
                };
            } else {
                record.outcome = result;
            }
        },
        recorded.result
    );
    return record;
}

void test_journal_orders_render_the_same_resource_as_submissions() {
    auto engine = processor();
    const TopOfBookEvent market = quote(3, 0);
    for (const auto& order : {
             intent("filled", Side::Buy, "2.5"),
             intent("partial", Side::Buy, "4"),
             intent("passive", Side::Buy, "1", 0, "100.5"),
             intent("too-large", Side::Buy, "50"),
         }) {
        const RecordedPaperOrder recorded = record(engine, order, market, 1);
        const Json submitted = response_json(
            chronos::paper_trading::render_order_submission(recorded)
        );
        const HttpResponse lookup = chronos::paper_trading::render_order_record(
            journal_record(9, recorded)
        );
        REQUIRE(lookup.status == 200);
        const Json looked_up = response_json(lookup);
        REQUIRE(looked_up.at("order") == submitted.at("order"));
        REQUIRE(!looked_up.contains("account"));
        if (submitted.contains("error")) {
            REQUIRE(submitted.at("order").at("rejection") == submitted.at("error"));
        } else {
            REQUIRE(submitted.at("order").at("rejection").is_null());
        }
    }
}

void test_rejection_and_fault_records_render_exactly() {
    auto engine = processor();
    const TopOfBookEvent market = quote(0, 0);
    const RecordedPaperOrder stale = record(
        engine,
        intent(
            "stale",
            Side::Buy,
            "1",
            chronos::test::kMaximumQuoteAgeNanoseconds + 1
        ),
        market,
        1
    );
    const Json stale_order = response_json(
        chronos::paper_trading::render_order_record(journal_record(1, stale))
    ).at("order");
    REQUIRE(stale_order.at("status") == "rejected");
    REQUIRE(stale_order.at("rejection") == Json({
        {"code", "market_data_stale"},
        {"message", "top-of-book state is older than the configured risk limit"},
        {"field", nullptr},
        {"reason", "stale_market_state"},
    }));
    REQUIRE(stale_order.at("fill").is_null());

    // A fault keeps its unbooked simulated fill in the journal record, but
    // the resource reports no fill and no filled quantity.
    RecordedPaperOrder executed = record(
        engine,
        intent("faulted", Side::Buy, "2"),
        market,
        2
    );
    PaperOrderRecord fault = journal_record(2, executed);
    fault.outcome = PaperOrderFaultRecord{
        .code = PaperOrderProcessingErrorCode::AccountingInvariantViolation,
        .message = "risk-approved simulated fill was rejected by accounting",
        .unbooked_execution =
            std::get<PaperOrderReceipt>(executed.result).execution,
    };
    const Json fault_order = response_json(
        chronos::paper_trading::render_order_record(fault)
    ).at("order");
    REQUIRE(fault_order.at("status") == "faulted");
    REQUIRE(fault_order.at("fill").is_null());
    REQUIRE(fault_order.at("filled_quantity") == "0");
    REQUIRE(fault_order.at("unfilled_quantity") == "2");
    REQUIRE(fault_order.at("rejection").at("code") == "processing_fault");
    REQUIRE(
        fault_order.at("rejection").at("reason") ==
        "accounting_invariant_violation"
    );
}

void test_order_history_pages_render_cursor_or_null() {
    auto engine = processor();
    const TopOfBookEvent market = quote(0, 0);
    const RecordedPaperOrder first = record(
        engine,
        intent("first", Side::Buy, "1"),
        market,
        1
    );
    const RecordedPaperOrder second = record(
        engine,
        intent("second", Side::Buy, "1"),
        market,
        2
    );
    const HttpResponse response = chronos::paper_trading::render_order_history(
        PaperOrderHistoryPage{
            .orders = {journal_record(8, second), journal_record(7, first)},
            .next_cursor = "djF8b3JkZXJzfGFjY3QtMXw3",
        }
    );
    REQUIRE(response.status == 200);
    const Json body = response_json(response);
    REQUIRE(body.at("orders").size() == 2);
    REQUIRE(body.at("orders")[0].at("client_order_id") == "second");
    REQUIRE(body.at("orders")[1].at("client_order_id") == "first");
    REQUIRE(body.at("next_cursor") == "djF8b3JkZXJzfGFjY3QtMXw3");

    const Json empty = response_json(chronos::paper_trading::render_order_history(
        PaperOrderHistoryPage{.orders = {}, .next_cursor = std::nullopt}
    ));
    REQUIRE(empty.at("orders") == Json::array());
    REQUIRE(empty.at("next_cursor").is_null());
}

void test_fill_history_renders_linkage_and_exact_notional() {
    const PaperFillRecord rounded{
        .record_id = 5,
        .account_id = "acct-1",
        .client_order_id = "order-5",
        .source = OrderSource{
            .kind = OrderSourceKind::Model,
            .source_id = "momentum-v1",
        },
        .fill = chronos::Fill{
            .fill_id = "order-5:fill:0",
            .instrument = InstrumentId("AAPL"),
            .side = Side::Sell,
            .price = Price::parse("0.333333333"),
            .quantity = Quantity::parse("3"),
            .fee = Money::parse("0.00000001"),
            .timestamp = Timestamp::from_nanoseconds(kMarketOpenNanoseconds + 7),
        },
    };
    const HttpResponse response = chronos::paper_trading::render_fill_history(
        PaperFillHistoryPage{.fills = {rounded}, .next_cursor = std::nullopt}
    );
    REQUIRE(response.status == 200);
    const Json body = response_json(response);
    REQUIRE(body.at("next_cursor").is_null());
    REQUIRE(body.at("fills")[0] == Json({
        {"trading_mode", "paper"},
        {"account_id", "acct-1"},
        {"client_order_id", "order-5"},
        {"source", {{"kind", "model"}, {"id", "momentum-v1"}}},
        {"instrument", "AAPL"},
        {"fill_id", "order-5:fill:0"},
        {"side", "sell"},
        {"price", "0.333333333"},
        {"quantity", "3"},
        {"notional", "1"},
        {"fee", "0.00000001"},
        {"filled_at", "2026-09-25T13:30:00.000000007Z"},
    }));
}

void test_read_side_errors_map_to_stable_codes() {
    const auto render = [](PaperTradingErrorCode code, std::string field) {
        return chronos::paper_trading::render_order_history(PaperTradingError{
            .code = code,
            .field = std::move(field),
            .message = "detail",
            .market_state = std::nullopt,
        });
    };
    REQUIRE(require_error_body(
        render(PaperTradingErrorCode::OrderNotFound, "client_order_id"),
        404,
        "order_not_found"
    ).at("error").at("field") == "client_order_id");
    const Json limit = require_error_body(
        render(PaperTradingErrorCode::InvalidPageLimit, "limit"),
        400,
        "malformed_request"
    );
    REQUIRE(limit.at("error").at("reason") == "invalid_limit");
    const Json cursor = require_error_body(
        render(PaperTradingErrorCode::InvalidCursor, "cursor"),
        400,
        "malformed_request"
    );
    REQUIRE(cursor.at("error").at("reason") == "invalid_cursor");
    REQUIRE(cursor.at("error").at("field") == "cursor");
    require_error_body(
        chronos::paper_trading::render_fill_history(PaperTradingError{
            .code = PaperTradingErrorCode::AccountNotFound,
            .field = "account_id",
            .message = "paper account does not exist",
            .market_state = std::nullopt,
        }),
        404,
        "account_not_found"
    );
}

// The journal can preserve a malformed intent from an in-process caller.
// The v1 contract has no spelling for it, so rendering fails closed and the
// router reports a generic internal error.
void test_undeclared_journal_values_fail_closed_when_rendered() {
    auto engine = processor();
    OrderIntent malformed = intent("malformed", Side::Buy, "1");
    malformed.side = static_cast<Side>(99);
    const RecordedPaperOrder recorded = record(engine, malformed, quote(0, 0), 1);
    REQUIRE_THROWS_AS(
        chronos::paper_trading::render_order_record(journal_record(1, recorded)),
        std::logic_error
    );
}

// ---------------------------------------------------------------------------
// Portfolio

PaperPortfolioView portfolio_of(const PaperOrderProcessor& source, std::int64_t version) {
    auto valuation = chronos::value_portfolio(
        source.account(),
        source.active_market(),
        quote(0, 0),
        Timestamp::from_nanoseconds(kMarketOpenNanoseconds),
        chronos::test::kMaximumQuoteAgeNanoseconds
    );
    REQUIRE(std::holds_alternative<PortfolioValuation>(valuation));
    return PaperPortfolioView{
        .account = view_of(source, version),
        .valuation = std::get<PortfolioValuation>(std::move(valuation)),
    };
}

// Buying 2 at the ask of 101 and marking at the bid of 99: cost basis 202,
// market value 198, unrealized PnL -4, fees 0.01, total PnL -4.01, cash
// 797.99, equity 995.99, and a return of -0.401%.
void test_portfolio_renders_every_field_as_an_exact_string() {
    auto engine = processor();
    static_cast<void>(record(engine, intent("buy", Side::Buy, "2"), quote(0, 0), 1));
    const HttpResponse response =
        chronos::paper_trading::render_portfolio(portfolio_of(engine, 1));
    REQUIRE(response.status == 200);
    REQUIRE(response_json(response) == Json({
        {"portfolio", {
            {"trading_mode", "paper"},
            {"account_id", "account-1"},
            {"instrument", "AAPL"},
            {"account_version", "1"},
            {"status", "active"},
            {"valued_at", "2026-09-25T13:30:00.000000000Z"},
            {"mark_policy", "best_bid"},
            {"mark_price", "99"},
            {"market", {
                {"stream_id", chronos::test::kTestStream},
                {"ingest_sequence", "0"},
                {"venue", nullptr},
                {"event_timestamp", "2026-09-25T13:29:59.999999750Z"},
                {"receive_timestamp", "2026-09-25T13:30:00.000000000Z"},
                {"quality_flags", {"replay"}},
                {"bid", {{"price", "99"}, {"quantity", "5"}}},
                {"ask", {{"price", "101"}, {"quantity", "5"}}},
            }},
            {"starting_cash", "1000"},
            {"cash", "797.99"},
            {"position", "2"},
            {"open_cost_basis", "202"},
            {"average_entry_price", "101"},
            {"position_market_value", "198"},
            {"unrealized_pnl", "-4"},
            {"gross_realized_pnl", "0"},
            {"fees_paid", "0.01"},
            {"total_pnl", "-4.01"},
            {"equity", "995.99"},
            {"return_percent", "-0.401"},
        }},
    }));
}

void test_flat_portfolio_has_no_average_entry_price() {
    const Json portfolio = response_json(
        chronos::paper_trading::render_portfolio(portfolio_of(processor(), 0))
    ).at("portfolio");
    REQUIRE(portfolio.at("average_entry_price").is_null());
    REQUIRE(portfolio.at("position") == "0");
    REQUIRE(portfolio.at("position_market_value") == "0");
    REQUIRE(portfolio.at("total_pnl") == "0");
    REQUIRE(portfolio.at("equity") == "1000");
    REQUIRE(portfolio.at("return_percent") == "0");
    REQUIRE(portfolio.at("mark_price") == "99");
}

// A faulted processor keeps its last reconciled account, which is still
// valued; only the status reports the fault.
void test_halted_portfolio_is_valued_and_reports_its_status() {
    auto engine = processor();
    static_cast<void>(record(engine, intent("buy", Side::Buy, "2"), quote(0, 0), 1));
    PaperPortfolioView halted = portfolio_of(engine, 1);
    halted.account.is_healthy = false;
    const Json portfolio = response_json(
        chronos::paper_trading::render_portfolio(halted)
    ).at("portfolio");
    REQUIRE(portfolio.at("status") == "halted");
    REQUIRE(portfolio.at("equity") == "995.99");
    REQUIRE(portfolio.at("total_pnl") == "-4.01");
}

void test_portfolio_errors_map_to_stable_codes_and_reasons() {
    const auto render = [](PaperTradingErrorCode code,
                           std::optional<PortfolioValuationErrorCode> valuation) {
        return chronos::paper_trading::render_portfolio(PaperTradingError{
            .code = code,
            .field = "",
            .message = "internal detail from host=db password=secret",
            .market_state = std::nullopt,
            .valuation = valuation,
        });
    };
    for (const auto& [valuation, reason] : {
             std::pair{PortfolioValuationErrorCode::InvalidMarketState, "invalid_market_state"},
             std::pair{
                 PortfolioValuationErrorCode::MarketInstrumentMismatch,
                 "market_instrument_mismatch",
             },
             std::pair{PortfolioValuationErrorCode::SuspectMarketState, "suspect_market_state"},
             std::pair{PortfolioValuationErrorCode::MissingBestBid, "missing_best_bid"},
             std::pair{PortfolioValuationErrorCode::StaleMarketState, "stale_market_state"},
             std::pair{
                 PortfolioValuationErrorCode::MarketTimelineMismatch,
                 "incompatible_market_timeline",
             },
         }) {
        REQUIRE(require_error_body(
            render(PaperTradingErrorCode::MarketDataUnavailable, valuation),
            503,
            "market_data_unavailable"
        ).at("error").at("reason") == reason);
    }
    for (const auto& [valuation, reason] : {
             std::pair{
                 PortfolioValuationErrorCode::NonpositiveStartingCash,
                 "nonpositive_starting_cash",
             },
             std::pair{PortfolioValuationErrorCode::ArithmeticOverflow, "arithmetic_overflow"},
             std::pair{
                 PortfolioValuationErrorCode::AccountingIdentityFailed,
                 "accounting_identity_failed",
             },
         }) {
        const Json failed = require_error_body(
            render(PaperTradingErrorCode::ValuationFailed, valuation),
            500,
            "valuation_failed"
        );
        REQUIRE(failed.at("error").at("reason") == reason);
        REQUIRE(!failed.contains("portfolio"));
    }
    const Json outage = require_error_body(
        render(PaperTradingErrorCode::PersistenceUnavailable, std::nullopt),
        503,
        "persistence_unavailable"
    );
    REQUIRE(outage.at("error").at("message").get<std::string>().find("secret") ==
            std::string::npos);
    require_error_body(
        render(PaperTradingErrorCode::AccountNotFound, std::nullopt),
        404,
        "account_not_found"
    );
    require_error_body(
        render(PaperTradingErrorCode::InvalidIdentifier, std::nullopt),
        400,
        "malformed_request"
    );
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
        {
            "history queries parse strictly",
            test_history_queries_parse_limit_and_cursor_strictly,
        },
        {
            "parameterless routes reject query parameters",
            test_parameterless_routes_reject_every_query_parameter,
        },
        {
            "journal orders render the submission resource",
            test_journal_orders_render_the_same_resource_as_submissions,
        },
        {
            "rejection and fault records render exactly",
            test_rejection_and_fault_records_render_exactly,
        },
        {
            "order history renders cursor or null",
            test_order_history_pages_render_cursor_or_null,
        },
        {
            "fill history renders linkage and exact notional",
            test_fill_history_renders_linkage_and_exact_notional,
        },
        {
            "read-side errors map to stable codes",
            test_read_side_errors_map_to_stable_codes,
        },
        {
            "undeclared journal values fail closed",
            test_undeclared_journal_values_fail_closed_when_rendered,
        },
        {
            "portfolio renders every field as an exact string",
            test_portfolio_renders_every_field_as_an_exact_string,
        },
        {
            "flat portfolio has no average entry price",
            test_flat_portfolio_has_no_average_entry_price,
        },
        {
            "halted portfolio is valued and reports its status",
            test_halted_portfolio_is_valued_and_reports_its_status,
        },
        {
            "portfolio errors map to stable codes",
            test_portfolio_errors_map_to_stable_codes_and_reasons,
        },
    });
}
