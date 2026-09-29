#include "chronos/paper_trading/history_cursor.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::InstrumentId;
using chronos::MarketDataStreamId;
using chronos::MarketState;
using chronos::MarketStateUnavailableCode;
using chronos::MarketStateUpdateError;
using chronos::Money;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperOrderReceipt;
using chronos::PostgresPaperOrderRepository;
using chronos::PreTradeRiskErrorCode;
using chronos::PreTradeRiskLimits;
using chronos::PreTradeRiskRejection;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedCancellationReason;
using chronos::SimulatedExecutionStatus;
using chronos::Timestamp;
using chronos::MarketDataQualityFlag;
using chronos::PaperFillRecord;
using chronos::Percentage;
using chronos::PersistentPaperOrderSession;
using chronos::PortfolioValuationErrorCode;
using chronos::TopOfBookEvent;
using chronos::PaperOrderRecord;
using chronos::SimulatedExecutionReport;
using chronos::paper_trading::CreatePaperAccountRequest;
using chronos::paper_trading::encode_history_cursor;
using chronos::paper_trading::HistoryCursorKind;
using chronos::paper_trading::HistoryPageRequest;
using chronos::paper_trading::PaperFillHistoryPage;
using chronos::paper_trading::PaperFillHistoryResult;
using chronos::paper_trading::PaperOrderHistoryPage;
using chronos::paper_trading::PaperOrderHistoryResult;
using chronos::paper_trading::PaperOrderRecordResult;
using chronos::paper_trading::PaperPortfolioResult;
using chronos::paper_trading::PaperPortfolioView;
using chronos::paper_trading::PaperAccountResult;
using chronos::paper_trading::PaperAccountView;
using chronos::paper_trading::PaperOrderSubmissionResult;
using chronos::paper_trading::PaperTradingError;
using chronos::paper_trading::PaperTradingErrorCode;
using chronos::paper_trading::PaperTradingService;
using chronos::paper_trading::RecordedPaperOrder;
using chronos::test::apply;
using chronos::test::kMarketOpenNanoseconds;
using chronos::test::kMaximumQuoteAgeNanoseconds;
using chronos::test::market_state;
using chronos::test::order_request;
using chronos::test::paper_risk_limits;
using chronos::test::paper_service_config;
using chronos::test::PostgresTestConnection;
using chronos::test::quote;
using chronos::test::QuoteLevels;
using chronos::test::terminate_backends;
using chronos::test::unique_test_id;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;

std::string connection_string;
const InstrumentId kAapl("AAPL");
const InstrumentId kSpy("SPY");

chronos::paper_trading::PaperOrderRequest for_instrument(
    chronos::paper_trading::PaperOrderRequest request,
    std::string instrument
) {
    request.instrument = std::move(instrument);
    return request;
}

std::unique_ptr<PaperTradingService> make_service(
    const MarketState& market,
    const std::string& connection = connection_string
) {
    return std::make_unique<PaperTradingService>(
        std::make_unique<PostgresPaperOrderRepository>(connection),
        market,
        paper_service_config()
    );
}

PaperAccountView require_account(PaperAccountResult result) {
    REQUIRE(std::holds_alternative<PaperAccountView>(result));
    return std::get<PaperAccountView>(std::move(result));
}

template <typename Result>
PaperTradingError require_error(Result result, PaperTradingErrorCode code) {
    REQUIRE(std::holds_alternative<PaperTradingError>(result));
    PaperTradingError error = std::get<PaperTradingError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.message.empty());
    return error;
}

RecordedPaperOrder require_recorded(PaperOrderSubmissionResult result) {
    REQUIRE(std::holds_alternative<RecordedPaperOrder>(result));
    return std::get<RecordedPaperOrder>(std::move(result));
}

PaperOrderReceipt require_receipt(const RecordedPaperOrder& recorded) {
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(recorded.result));
    return std::get<PaperOrderReceipt>(recorded.result);
}

std::string create_account(
    PaperTradingService& service,
    std::string_view prefix,
    std::string_view starting_cash = "1000"
) {
    std::string account_id = unique_test_id(prefix);
    require_account(service.create_account(CreatePaperAccountRequest{
        .account_id = account_id,
        .starting_cash = Money::parse(starting_cash),
    }));
    return account_id;
}

std::int64_t recorded_orders(const std::string& account_id) {
    PostgresPaperOrderRepository inspector(connection_string);
    return inspector.journal_summary(account_id).order_count;
}

bool same_limits(const PreTradeRiskLimits& left, const PreTradeRiskLimits& right) {
    return left.maximum_order_quantity == right.maximum_order_quantity &&
        left.maximum_order_notional == right.maximum_order_notional &&
        left.maximum_position_quantity == right.maximum_position_quantity &&
        left.maximum_quote_age_nanoseconds ==
            right.maximum_quote_age_nanoseconds &&
        left.fee_per_fill == right.fee_per_fill;
}

void test_account_is_created_with_server_risk_limits_and_retrieved() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const std::string account_id = unique_test_id("create");

    const PaperAccountView created = require_account(service->create_account(
        CreatePaperAccountRequest{
            .account_id = account_id,
            .starting_cash = Money::parse("1000.5"),
        }
    ));
    REQUIRE(created.account_id == account_id);
    REQUIRE(created.version == 0);
    REQUIRE(created.is_healthy);
    REQUIRE(created.state.starting_cash == Money::parse("1000.5"));
    REQUIRE(created.state.cash == Money::parse("1000.5"));
    REQUIRE(created.state.positions.empty());
    REQUIRE(!created.market_stream.has_value());
    REQUIRE(same_limits(created.risk_limits, paper_risk_limits()));

    const PaperAccountView loaded = require_account(
        service->get_account(account_id)
    );
    REQUIRE(loaded.state == created.state);
    REQUIRE(loaded.version == 0);
    REQUIRE(same_limits(loaded.risk_limits, created.risk_limits));
}

void test_duplicate_account_leaves_the_original_unchanged() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "duplicate");

    const PaperTradingError duplicate = require_error(
        service->create_account(CreatePaperAccountRequest{
            .account_id = account_id,
            .starting_cash = Money::parse("5000"),
        }),
        PaperTradingErrorCode::AccountAlreadyExists
    );
    REQUIRE(duplicate.field == "account_id");

    const PaperAccountView original = require_account(
        service->get_account(account_id)
    );
    REQUIRE(original.state.starting_cash == Money::parse("1000"));
    REQUIRE(original.state.cash == Money::parse("1000"));
}

void test_account_requests_are_validated_before_persistence() {
    const auto market = market_state();
    const auto service = make_service(*market);

    const auto create = [&](std::string account_id, std::string_view cash) {
        return service->create_account(CreatePaperAccountRequest{
            .account_id = std::move(account_id),
            .starting_cash = Money::parse(cash),
        });
    };
    REQUIRE(require_error(
        create(unique_test_id("zero-cash"), "0"),
        PaperTradingErrorCode::InvalidStartingCash
    ).field == "starting_cash");
    REQUIRE(require_error(
        create(unique_test_id("negative-cash"), "-1"),
        PaperTradingErrorCode::InvalidStartingCash
    ).field == "starting_cash");

    for (const std::string& invalid : {
             std::string(),
             std::string("has space"),
             std::string("nul\0id", 6),
             std::string(".hidden"),
             std::string("-leading"),
             std::string("../escape"),
             std::string("slash/inside"),
             std::string(65, 'a'),
         }) {
        REQUIRE(require_error(
            create(invalid, "1000"),
            PaperTradingErrorCode::InvalidIdentifier
        ).field == "account_id");
        REQUIRE(require_error(
            service->get_account(invalid),
            PaperTradingErrorCode::InvalidIdentifier
        ).field == "account_id");
    }

    std::string longest = unique_test_id("max-length");
    longest.resize(chronos::paper_trading::kMaximumIdentifierLength, 'x');
    require_account(create(longest, "1000"));
    require_error(
        service->get_account(unique_test_id("missing")),
        PaperTradingErrorCode::AccountNotFound
    );
}

void test_marketable_order_fills_at_the_server_quote_and_market_time() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "fill");

    const RecordedPaperOrder recorded = require_recorded(service->submit_order(
        order_request(account_id, "fill-1", Side::Buy, "2")
    ));
    REQUIRE(
        recorded.intent.submitted_at ==
        Timestamp::from_nanoseconds(kMarketOpenNanoseconds)
    );
    REQUIRE(recorded.market == quote(0, 0));

    const PaperOrderReceipt receipt = require_receipt(recorded);
    REQUIRE(receipt.execution.status == SimulatedExecutionStatus::Filled);
    REQUIRE(receipt.execution.fill->price == Price::parse("101"));
    REQUIRE(receipt.execution.fill->quantity == Quantity::parse("2"));
    REQUIRE(receipt.execution.fill->fee == Money::parse("0.01"));
    REQUIRE(receipt.accounting->notional == Money::parse("202"));
    REQUIRE(recorded.account.state == receipt.account_after);
    REQUIRE(recorded.account.state.cash == Money::parse("797.99"));
    REQUIRE(recorded.account.state.quantity(kAapl) == Quantity::parse("2"));
    REQUIRE(recorded.account.version == 1);
}

void test_order_beyond_displayed_liquidity_partially_fills() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "partial");

    const PaperOrderReceipt receipt = require_receipt(require_recorded(
        service->submit_order(order_request(account_id, "p-1", Side::Buy, "8"))
    ));
    REQUIRE(
        receipt.execution.status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(receipt.execution.fill->quantity == Quantity::parse("5"));
    REQUIRE(receipt.execution.unfilled_quantity == Quantity::parse("3"));
    REQUIRE(
        receipt.execution.cancellation_reason ==
        SimulatedCancellationReason::DisplayedLiquidityExhausted
    );
    REQUIRE(receipt.account_after.quantity(kAapl) == Quantity::parse("5"));
}

void test_non_marketable_limit_is_cancelled_and_books_nothing() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "limit");

    const RecordedPaperOrder recorded = require_recorded(service->submit_order(
        order_request(
            account_id,
            "limit-1",
            Side::Buy,
            "1",
            OrderType::Limit,
            "100.5"
        )
    ));
    const PaperOrderReceipt receipt = require_receipt(recorded);
    REQUIRE(receipt.execution.status == SimulatedExecutionStatus::Cancelled);
    REQUIRE(
        receipt.execution.cancellation_reason ==
        SimulatedCancellationReason::LimitNotMarketable
    );
    REQUIRE(!receipt.execution.fill.has_value());
    REQUIRE(!receipt.accounting.has_value());
    REQUIRE(recorded.account.state.cash == Money::parse("1000"));
    REQUIRE(recorded.account.version == 1);
    REQUIRE(recorded_orders(account_id) == 1);
}

void test_risk_rejection_is_recorded_and_consumes_the_client_order_id() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "risk");

    const RecordedPaperOrder rejected = require_recorded(service->submit_order(
        order_request(account_id, "too-large", Side::Buy, "50")
    ));
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(rejected.result));
    REQUIRE(
        std::get<PreTradeRiskRejection>(rejected.result).code ==
        PreTradeRiskErrorCode::InsufficientCash
    );
    REQUIRE(rejected.account.version == 1);
    REQUIRE(rejected.account.state.cash == Money::parse("1000"));

    REQUIRE(require_error(
        service->submit_order(
            order_request(account_id, "too-large", Side::Buy, "1")
        ),
        PaperTradingErrorCode::DuplicateClientOrderId
    ).field == "client_order_id");
    REQUIRE(recorded_orders(account_id) == 1);
}

void test_missing_market_state_records_nothing() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "no-market");

    const PaperTradingError empty = require_error(
        service->submit_order(order_request(account_id, "early", Side::Buy, "1")),
        PaperTradingErrorCode::MarketDataUnavailable
    );
    REQUIRE(empty.market_state == MarketStateUnavailableCode::NoTopOfBook);

    apply(*market, quote(0, 0, "SPY"));
    require_error(
        service->submit_order(order_request(account_id, "early", Side::Buy, "1")),
        PaperTradingErrorCode::MarketDataUnavailable
    );
    REQUIRE(recorded_orders(account_id) == 0);
    REQUIRE(require_account(service->get_account(account_id)).version == 0);

    apply(*market, quote(1, 10));
    const RecordedPaperOrder recorded = require_recorded(service->submit_order(
        order_request(account_id, "early", Side::Buy, "1")
    ));
    REQUIRE(require_receipt(recorded).execution.status ==
            SimulatedExecutionStatus::Filled);
}

void test_interrupted_market_stream_blocks_orders() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    auto gap = chronos::validate_market_event(quote(2, 10));
    REQUIRE(std::holds_alternative<MarketStateUpdateError>(market->apply(
        std::get<chronos::ValidatedMarketEvent>(gap)
    )));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "interrupted");

    const PaperTradingError interrupted = require_error(
        service->submit_order(order_request(account_id, "gap", Side::Buy, "1")),
        PaperTradingErrorCode::MarketDataUnavailable
    );
    REQUIRE(
        interrupted.market_state ==
        MarketStateUnavailableCode::StreamInterrupted
    );
    REQUIRE(recorded_orders(account_id) == 0);
}

void test_stale_quote_is_a_recorded_risk_rejection_at_market_time() {
    const auto stale_market = market_state();
    apply(*stale_market, quote(0, 0));
    apply(*stale_market, quote(1, kMaximumQuoteAgeNanoseconds + 1, "SPY"));
    const auto service = make_service(*stale_market);
    const std::string stale_account = create_account(*service, "stale");

    const RecordedPaperOrder stale = require_recorded(service->submit_order(
        order_request(stale_account, "stale-1", Side::Buy, "1")
    ));
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(stale.result));
    REQUIRE(
        std::get<PreTradeRiskRejection>(stale.result).code ==
        PreTradeRiskErrorCode::StaleMarketState
    );
    REQUIRE(stale.intent.submitted_at == Timestamp::from_nanoseconds(
        kMarketOpenNanoseconds + kMaximumQuoteAgeNanoseconds + 1
    ));
    REQUIRE(recorded_orders(stale_account) == 1);

    const auto boundary_market = market_state();
    apply(*boundary_market, quote(0, 0));
    apply(*boundary_market, quote(1, kMaximumQuoteAgeNanoseconds, "SPY"));
    const auto boundary_service = make_service(*boundary_market);
    const std::string boundary_account = create_account(
        *boundary_service,
        "quote-age-boundary"
    );
    const RecordedPaperOrder at_limit = require_recorded(
        boundary_service->submit_order(
            order_request(boundary_account, "at-limit", Side::Buy, "1")
        )
    );
    REQUIRE(require_receipt(at_limit).execution.status ==
            SimulatedExecutionStatus::Filled);
}

void test_duplicate_client_order_id_is_not_recorded_twice() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "duplicate-order");

    require_recorded(service->submit_order(
        order_request(account_id, "once", Side::Buy, "1")
    ));
    require_error(
        service->submit_order(order_request(account_id, "once", Side::Buy, "1")),
        PaperTradingErrorCode::DuplicateClientOrderId
    );
    const PaperAccountView account = require_account(
        service->get_account(account_id)
    );
    REQUIRE(account.state.quantity(kAapl) == Quantity::parse("1"));
    REQUIRE(account.version == 1);
    REQUIRE(recorded_orders(account_id) == 1);
}

void test_concurrent_writer_causes_a_version_conflict_that_records_nothing() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const std::string application = unique_test_id("service-conflict");
    const auto service = make_service(
        *market,
        with_application_name(connection_string, application)
    );
    const std::string account_id = create_account(*service, "conflict");

    PostgresTestConnection lock_holder(connection_string);
    PostgresTestConnection observer(connection_string);
    static_cast<void>(lock_holder.query("BEGIN"));
    static_cast<void>(lock_holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));

    std::optional<PaperOrderSubmissionResult> contended;
    std::string submitter_failure;
    std::thread submitter([&] {
        try {
            contended = service->submit_order(
                order_request(account_id, "contended", Side::Buy, "1")
            );
        } catch (const std::exception& error) {
            submitter_failure = error.what();
        }
    });
    wait_until_blocked_on_lock(observer, application);
    static_cast<void>(lock_holder.query(
        "UPDATE paper_accounts SET version = version + 1 "
        "WHERE account_id = $1",
        {account_id}
    ));
    static_cast<void>(lock_holder.query("COMMIT"));
    submitter.join();

    REQUIRE(submitter_failure.empty());
    REQUIRE(contended.has_value());
    require_error(
        std::move(*contended),
        PaperTradingErrorCode::AccountVersionConflict
    );
    REQUIRE(recorded_orders(account_id) == 0);

    const RecordedPaperOrder retried = require_recorded(service->submit_order(
        order_request(account_id, "contended", Side::Buy, "1")
    ));
    REQUIRE(retried.account.version == 2);
    REQUIRE(recorded_orders(account_id) == 1);
}

void test_durable_state_and_shadow_liquidity_survive_a_new_service() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    std::string account_id;
    {
        const auto first_service = make_service(*market);
        account_id = create_account(*first_service, "restart");
        require_recorded(first_service->submit_order(
            order_request(account_id, "before-restart", Side::Buy, "3")
        ));
    }

    const auto restarted = make_service(*market);
    const PaperAccountView recovered = require_account(
        restarted->get_account(account_id)
    );
    REQUIRE(recovered.version == 1);
    REQUIRE(recovered.state.quantity(kAapl) == Quantity::parse("3"));
    REQUIRE(recovered.state.cash == Money::parse("696.99"));

    const PaperOrderReceipt after_restart = require_receipt(require_recorded(
        restarted->submit_order(
            order_request(account_id, "after-restart", Side::Buy, "3")
        )
    ));
    REQUIRE(
        after_restart.execution.status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(after_restart.execution.fill->quantity == Quantity::parse("2"));
}

void test_shadow_liquidity_is_isolated_between_accounts() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string first = create_account(*service, "isolated-a");
    const std::string second = create_account(*service, "isolated-b");

    REQUIRE(require_receipt(require_recorded(service->submit_order(
        order_request(first, "all", Side::Buy, "5")
    ))).execution.status == SimulatedExecutionStatus::Filled);
    const PaperOrderReceipt exhausted = require_receipt(require_recorded(
        service->submit_order(order_request(first, "more", Side::Buy, "1"))
    ));
    REQUIRE(exhausted.execution.status == SimulatedExecutionStatus::Cancelled);
    REQUIRE(
        exhausted.execution.cancellation_reason ==
        SimulatedCancellationReason::DisplayedLiquidityExhausted
    );

    const PaperOrderReceipt independent = require_receipt(require_recorded(
        service->submit_order(order_request(second, "all", Side::Buy, "5"))
    ));
    REQUIRE(independent.execution.status == SimulatedExecutionStatus::Filled);
    REQUIRE(independent.execution.fill->quantity == Quantity::parse("5"));
}

void test_manual_and_model_orders_receive_identical_treatment() {
    const auto market = market_state();
    apply(*market, quote(0, 0, "AAPL", QuoteLevels{.ask_quantity = "4"}));
    const auto service = make_service(*market);
    const std::string manual_account = create_account(*service, "manual");
    const std::string model_account = create_account(*service, "model");

    const RecordedPaperOrder manual = require_recorded(service->submit_order(
        order_request(manual_account, "same-id", Side::Buy, "6")
    ));
    const RecordedPaperOrder model = require_recorded(service->submit_order(
        order_request(
            model_account,
            "same-id",
            Side::Buy,
            "6",
            OrderType::Market,
            std::nullopt,
            OrderSourceKind::Model
        )
    ));
    REQUIRE(manual.intent.source.kind == OrderSourceKind::Manual);
    REQUIRE(model.intent.source.kind == OrderSourceKind::Model);

    const PaperOrderReceipt manual_receipt = require_receipt(manual);
    const PaperOrderReceipt model_receipt = require_receipt(model);
    REQUIRE(manual_receipt.execution.status == model_receipt.execution.status);
    REQUIRE(manual_receipt.execution.fill == model_receipt.execution.fill);
    REQUIRE(
        manual_receipt.execution.unfilled_quantity ==
        model_receipt.execution.unfilled_quantity
    );
    REQUIRE(manual_receipt.account_after == model_receipt.account_after);
    REQUIRE(manual.market == model.market);
}

void test_lost_database_connection_is_unavailable_then_recovers() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const std::string application = unique_test_id("service-outage");
    const auto service = make_service(
        *market,
        with_application_name(connection_string, application)
    );
    const std::string account_id = create_account(*service, "outage");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    require_error(
        service->submit_order(
            order_request(account_id, "during-outage", Side::Buy, "1")
        ),
        PaperTradingErrorCode::PersistenceUnavailable
    );
    REQUIRE(recorded_orders(account_id) == 0);
    REQUIRE(service->health().persistence_available);

    require_recorded(service->submit_order(
        order_request(account_id, "during-outage", Side::Buy, "1")
    ));
    REQUIRE(recorded_orders(account_id) == 1);
}

void test_health_reports_persistence_and_market_stream() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const auto before = service->health();
    REQUIRE(before.persistence_available);
    const std::vector<InstrumentId> configured_instruments{
        InstrumentId("AAPL"),
        InstrumentId("SPY"),
    };
    REQUIRE(before.tradeable_instruments == configured_instruments);
    REQUIRE(
        before.market.stream ==
        MarketDataStreamId(std::string(chronos::test::kTestStream))
    );
    REQUIRE(!before.market.last_ingest_sequence.has_value());

    apply(*market, quote(0, 0));
    const auto after = service->health();
    REQUIRE(after.market.last_ingest_sequence == 0U);
    REQUIRE(
        after.market.market_time ==
        Timestamp::from_nanoseconds(kMarketOpenNanoseconds)
    );
    REQUIRE(!after.market.interruption.has_value());
}

// ---------------------------------------------------------------------------
// Journal reads

PaperOrderHistoryPage require_order_history(PaperOrderHistoryResult result) {
    REQUIRE(std::holds_alternative<PaperOrderHistoryPage>(result));
    return std::get<PaperOrderHistoryPage>(std::move(result));
}

PaperFillHistoryPage require_fill_history(PaperFillHistoryResult result) {
    REQUIRE(std::holds_alternative<PaperFillHistoryPage>(result));
    return std::get<PaperFillHistoryPage>(std::move(result));
}

PaperOrderRecord require_order_record(PaperOrderRecordResult result) {
    REQUIRE(std::holds_alternative<PaperOrderRecord>(result));
    return std::get<PaperOrderRecord>(std::move(result));
}

std::vector<std::string> client_order_ids(const PaperOrderHistoryPage& page) {
    std::vector<std::string> ids;
    for (const auto& record : page.orders) {
        ids.push_back(record.intent.client_order_id);
    }
    return ids;
}

std::vector<std::string> client_order_ids(const PaperFillHistoryPage& page) {
    std::vector<std::string> ids;
    for (const auto& record : page.fills) {
        ids.push_back(record.client_order_id);
    }
    return ids;
}

HistoryPageRequest page(
    std::optional<std::size_t> limit,
    std::optional<std::string> cursor = std::nullopt,
    std::optional<std::string> instrument = std::nullopt
) {
    return HistoryPageRequest{
        .limit = limit,
        .cursor = std::move(cursor),
        .instrument = std::move(instrument),
    };
}

void test_history_distinguishes_missing_accounts_from_empty_history() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "empty-reads");

    const auto orders = require_order_history(
        service->list_orders(account_id, {})
    );
    REQUIRE(orders.orders.empty());
    REQUIRE(!orders.next_cursor.has_value());
    const auto fills = require_fill_history(service->list_fills(account_id, {}));
    REQUIRE(fills.fills.empty());
    REQUIRE(!fills.next_cursor.has_value());
    REQUIRE(require_error(
        service->get_order(account_id, "never-sent"),
        PaperTradingErrorCode::OrderNotFound
    ).field == "client_order_id");

    const std::string missing = unique_test_id("missing-reads");
    require_error(
        service->list_orders(missing, {}),
        PaperTradingErrorCode::AccountNotFound
    );
    require_error(
        service->list_fills(missing, {}),
        PaperTradingErrorCode::AccountNotFound
    );
    require_error(
        service->get_order(missing, "never-sent"),
        PaperTradingErrorCode::AccountNotFound
    );

    REQUIRE(require_error(
        service->list_orders("bad id", {}),
        PaperTradingErrorCode::InvalidIdentifier
    ).field == "account_id");
    for (const std::string& invalid : {
             std::string("bad id"),
             std::string("nul\0id", 6),
             std::string(65, 'x'),
         }) {
        REQUIRE(require_error(
            service->get_order(account_id, invalid),
            PaperTradingErrorCode::InvalidIdentifier
        ).field == "client_order_id");
    }
}

void test_history_pages_follow_opaque_cursors_newest_first() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "history-pages");
    for (const std::string_view id : {"fill-0", "fill-1", "fill-2"}) {
        require_recorded(service->submit_order(
            order_request(account_id, std::string(id), Side::Buy, "1")
        ));
    }
    require_recorded(service->submit_order(order_request(
        account_id,
        "passive",
        Side::Buy,
        "1",
        OrderType::Limit,
        "100"
    )));
    require_recorded(service->submit_order(
        order_request(account_id, "too-big", Side::Buy, "50")
    ));

    const auto first = require_order_history(
        service->list_orders(account_id, page(2))
    );
    REQUIRE((client_order_ids(first) ==
             std::vector<std::string>{"too-big", "passive"}));
    REQUIRE(first.next_cursor.has_value());
    const auto second = require_order_history(
        service->list_orders(account_id, page(2, first.next_cursor))
    );
    REQUIRE((client_order_ids(second) ==
             std::vector<std::string>{"fill-2", "fill-1"}));
    const auto last = require_order_history(
        service->list_orders(account_id, page(2, second.next_cursor))
    );
    REQUIRE((client_order_ids(last) == std::vector<std::string>{"fill-0"}));
    REQUIRE(!last.next_cursor.has_value());
    REQUIRE(require_order_history(service->list_orders(account_id, {}))
                .orders.size() == 5);

    const auto fills = require_fill_history(
        service->list_fills(account_id, page(2))
    );
    REQUIRE((client_order_ids(fills) ==
             std::vector<std::string>{"fill-2", "fill-1"}));
    REQUIRE(fills.fills.front().account_id == account_id);
    const auto older_fills = require_fill_history(
        service->list_fills(account_id, page(2, fills.next_cursor))
    );
    REQUIRE((client_order_ids(older_fills) ==
             std::vector<std::string>{"fill-0"}));
    REQUIRE(!older_fills.next_cursor.has_value());
}

void test_page_limits_and_cursors_are_validated_by_the_service() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "page-rules");
    const std::string other_account = create_account(*service, "page-other");
    using chronos::paper_trading::kMaximumHistoryPageLimit;

    for (const std::size_t invalid : {std::size_t{0}, kMaximumHistoryPageLimit + 1}) {
        REQUIRE(require_error(
            service->list_orders(account_id, page(invalid)),
            PaperTradingErrorCode::InvalidPageLimit
        ).field == "limit");
        require_error(
            service->list_fills(account_id, page(invalid)),
            PaperTradingErrorCode::InvalidPageLimit
        );
    }
    require_order_history(
        service->list_orders(account_id, page(kMaximumHistoryPageLimit))
    );

    const std::string fills_cursor = encode_history_cursor(
        HistoryCursorKind::Fills,
        account_id,
        std::nullopt,
        5
    );
    REQUIRE(require_error(
        service->list_orders(account_id, page(std::nullopt, fills_cursor)),
        PaperTradingErrorCode::InvalidCursor
    ).field == "cursor");
    require_error(
        service->list_orders(
            account_id,
            page(
                std::nullopt,
                encode_history_cursor(HistoryCursorKind::Orders, other_account, std::nullopt, 5)
            )
        ),
        PaperTradingErrorCode::InvalidCursor
    );
    for (const std::string& cursor : {
             std::string(),
             std::string("not a cursor"),
             encode_history_cursor(HistoryCursorKind::Fills, account_id, std::nullopt, 0),
             encode_history_cursor(HistoryCursorKind::Fills, account_id, std::nullopt, -1),
         }) {
        require_error(
            service->list_fills(account_id, page(std::nullopt, cursor)),
            PaperTradingErrorCode::InvalidCursor
        );
    }
    require_fill_history(service->list_fills(
        account_id,
        page(std::nullopt, fills_cursor)
    ));

    // Request validation needs no database, so it precedes the account check.
    require_error(
        service->list_orders(unique_test_id("missing-page"), page(0)),
        PaperTradingErrorCode::InvalidPageLimit
    );
}

void test_default_page_holds_fifty_orders() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "default-page");
    for (int index = 0; index < 51; ++index) {
        require_recorded(service->submit_order(order_request(
            account_id,
            "passive-" + std::to_string(index),
            Side::Buy,
            "1",
            OrderType::Limit,
            "100"
        )));
    }

    const auto first = require_order_history(
        service->list_orders(account_id, {})
    );
    REQUIRE(first.orders.size() ==
            chronos::paper_trading::kDefaultHistoryPageLimit);
    REQUIRE(first.orders.front().intent.client_order_id == "passive-50");
    const auto rest = require_order_history(
        service->list_orders(account_id, page(std::nullopt, first.next_cursor))
    );
    REQUIRE((client_order_ids(rest) == std::vector<std::string>{"passive-0"}));
    REQUIRE(!rest.next_cursor.has_value());
}

void test_uncertain_submission_is_reconciled_by_lookup() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const std::string application = unique_test_id("reconcile");
    const auto service = make_service(
        *market,
        with_application_name(connection_string, application)
    );
    const std::string account_id = create_account(*service, "reconcile");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    require_error(
        service->submit_order(order_request(account_id, "uncertain", Side::Buy, "2")),
        PaperTradingErrorCode::PersistenceUnavailable
    );
    require_error(
        service->get_order(account_id, "uncertain"),
        PaperTradingErrorCode::OrderNotFound
    );

    const RecordedPaperOrder recorded = require_recorded(service->submit_order(
        order_request(account_id, "uncertain", Side::Buy, "2")
    ));
    const PaperOrderRecord reconciled = require_order_record(
        service->get_order(account_id, "uncertain")
    );
    REQUIRE(reconciled.intent == recorded.intent);
    REQUIRE(reconciled.market == recorded.market);
    REQUIRE(std::get<SimulatedExecutionReport>(reconciled.outcome) ==
            require_receipt(recorded).execution);
}

void test_manual_and_model_orders_read_back_identically() {
    const auto market = market_state();
    apply(*market, quote(0, 0, "AAPL", QuoteLevels{.ask_quantity = "4"}));
    const auto service = make_service(*market);
    const std::string manual_account = create_account(*service, "read-manual");
    const std::string model_account = create_account(*service, "read-model");
    require_recorded(service->submit_order(
        order_request(manual_account, "same-id", Side::Buy, "6")
    ));
    require_recorded(service->submit_order(order_request(
        model_account,
        "same-id",
        Side::Buy,
        "6",
        OrderType::Market,
        std::nullopt,
        OrderSourceKind::Model
    )));

    const auto manual = require_order_record(
        service->get_order(manual_account, "same-id")
    );
    const auto model = require_order_record(
        service->get_order(model_account, "same-id")
    );
    REQUIRE(manual.intent.source.kind == OrderSourceKind::Manual);
    REQUIRE(model.intent.source.kind == OrderSourceKind::Model);
    REQUIRE(manual.market == model.market);
    const auto& manual_execution =
        std::get<SimulatedExecutionReport>(manual.outcome);
    const auto& model_execution =
        std::get<SimulatedExecutionReport>(model.outcome);
    REQUIRE(manual_execution.status == model_execution.status);
    REQUIRE(manual_execution.fill == model_execution.fill);
    REQUIRE(
        manual_execution.unfilled_quantity == model_execution.unfilled_quantity
    );

    const PaperFillRecord manual_fill = require_fill_history(
        service->list_fills(manual_account, {})
    ).fills.front();
    const PaperFillRecord model_fill = require_fill_history(
        service->list_fills(model_account, {})
    ).fills.front();
    REQUIRE(manual_fill.fill == model_fill.fill);
    REQUIRE(manual_fill.source.kind == OrderSourceKind::Manual);
    REQUIRE(model_fill.source.kind == OrderSourceKind::Model);
}

void test_history_reads_report_outages_then_recover() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const std::string application = unique_test_id("read-outage-service");
    const auto service = make_service(
        *market,
        with_application_name(connection_string, application)
    );
    const std::string account_id = create_account(*service, "read-outage");
    require_recorded(service->submit_order(
        order_request(account_id, "before", Side::Buy, "1")
    ));

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    require_error(
        service->list_orders(account_id, {}),
        PaperTradingErrorCode::PersistenceUnavailable
    );
    REQUIRE(require_order_history(service->list_orders(account_id, {}))
                .orders.size() == 1);
    // Each outage needs a live connection to lose, so a successful read
    // re-establishes it before the next backend is terminated.
    terminate_backends(observer, application);
    require_error(
        service->list_fills(account_id, {}),
        PaperTradingErrorCode::PersistenceUnavailable
    );
    REQUIRE(require_fill_history(service->list_fills(account_id, {}))
                .fills.size() == 1);
    terminate_backends(observer, application);
    require_error(
        service->get_order(account_id, "before"),
        PaperTradingErrorCode::PersistenceUnavailable
    );
    require_order_record(service->get_order(account_id, "before"));
}

// ---------------------------------------------------------------------------
// Portfolio valuation

PaperPortfolioView require_portfolio(PaperPortfolioResult result) {
    REQUIRE(std::holds_alternative<PaperPortfolioView>(result));
    return std::get<PaperPortfolioView>(std::move(result));
}

PaperTradingError require_market_unavailable(
    PaperPortfolioResult result,
    PortfolioValuationErrorCode reason
) {
    PaperTradingError failure = require_error(
        std::move(result),
        PaperTradingErrorCode::MarketDataUnavailable
    );
    REQUIRE(failure.valuation == reason);
    return failure;
}

// Buy 2 at the ask of 101, then sell 0.5 at the bid of 99 through a model
// source. Independently: cost basis 202 - 50.5 = 151.5, realized PnL
// 49.5 - 50.5 = -1, cash 1000 - 202.01 + 49.49 = 847.48, fees 0.02. Marked at
// the bid of 99, 1.5 is worth 148.5, unrealized PnL is -3, total PnL is
// -4.02 = 995.98 - 1000, and the return is -0.402%.
void submit_valuation_history(PaperTradingService& service, const std::string& account_id) {
    require_recorded(service.submit_order(
        order_request(account_id, "buy", Side::Buy, "2")
    ));
    require_recorded(service.submit_order(order_request(
        account_id,
        "model-sell",
        Side::Sell,
        "0.5",
        OrderType::Market,
        std::nullopt,
        OrderSourceKind::Model
    )));
}

void require_expected_portfolio(const PaperPortfolioView& portfolio) {
    const auto& snapshot = portfolio.valuation.snapshot;
    REQUIRE(portfolio.account.version == 2);
    REQUIRE(snapshot.state == portfolio.account.state);
    REQUIRE(snapshot.open_positions.size() == 1);
    REQUIRE(snapshot.open_positions[0].instrument == kAapl);
    REQUIRE(snapshot.open_positions[0].mark_price == Price::parse("99"));
    REQUIRE(snapshot.state.cash == Money::parse("847.48"));
    REQUIRE(snapshot.state.quantity(kAapl) == Quantity::parse("1.5"));
    REQUIRE(snapshot.open_cost_basis == Money::parse("151.5"));
    REQUIRE(snapshot.gross_realized_pnl == Money::parse("-1"));
    REQUIRE(snapshot.fees_paid == Money::parse("0.02"));
    REQUIRE(snapshot.market_value == Money::parse("148.5"));
    REQUIRE(snapshot.gross_unrealized_pnl == Money::parse("-3"));
    REQUIRE(snapshot.net_total_pnl == Money::parse("-4.02"));
    REQUIRE(snapshot.equity == Money::parse("995.98"));
    REQUIRE(portfolio.valuation.return_percent == Percentage::parse("-0.402"));
    REQUIRE(portfolio.valuation.marks.at(kAapl) == quote(0, 0));
    REQUIRE(
        portfolio.valuation.valued_at ==
        Timestamp::from_nanoseconds(kMarketOpenNanoseconds)
    );
}

void test_portfolio_values_the_durable_account_at_the_best_bid() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "portfolio");
    submit_valuation_history(*service, account_id);

    require_expected_portfolio(require_portfolio(service->get_portfolio(account_id)));
}

void test_portfolio_requires_a_valid_existing_account() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    require_error(
        service->get_portfolio(unique_test_id("no-portfolio")),
        PaperTradingErrorCode::AccountNotFound
    );
    REQUIRE(require_error(
        service->get_portfolio("bad id"),
        PaperTradingErrorCode::InvalidIdentifier
    ).field == "account_id");
}

// Every held instrument that cannot be marked, in instrument order, with
// the reason for each.
using MarkReason = std::variant<MarketStateUnavailableCode, PortfolioValuationErrorCode>;

void require_unmarkable(
    PaperPortfolioResult result,
    std::initializer_list<std::pair<std::string_view, MarkReason>> expected
) {
    const PaperTradingError failure = require_market_unavailable(
        std::move(result),
        PortfolioValuationErrorCode::PositionMarkUnavailable
    );
    REQUIRE(failure.instruments.size() == expected.size());
    std::size_t index = 0;
    for (const auto& [instrument, reason] : expected) {
        REQUIRE(failure.instruments[index].instrument ==
            InstrumentId(std::string(instrument)));
        REQUIRE(failure.instruments[index].reason == reason);
        ++index;
    }
}

void test_a_flat_account_is_valued_without_any_quote() {
    const auto market = market_state();
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "portfolio-flat");

    // No event has arrived, so there is no market time to report.
    const auto before_market = require_portfolio(service->get_portfolio(account_id));
    REQUIRE(before_market.valuation.snapshot.equity == Money::parse("1000"));
    REQUIRE(before_market.valuation.snapshot.open_positions.empty());
    REQUIRE(!before_market.valuation.valued_at.has_value());

    apply(*market, quote(0, 0, "SPY"));
    const auto with_market = require_portfolio(service->get_portfolio(account_id));
    REQUIRE(with_market.valuation.valued_at ==
        Timestamp::from_nanoseconds(kMarketOpenNanoseconds));
    REQUIRE(with_market.valuation.return_percent == Percentage{});
}

void test_portfolio_requires_a_current_quote_for_each_held_instrument() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "portfolio-quote");
    require_recorded(service->submit_order(
        order_request(account_id, "hold-aapl", Side::Buy, "1")
    ));
    require_portfolio(service->get_portfolio(account_id));

    // Another instrument's newer quote ages the AAPL mark, and never marks
    // it.
    apply(*market, quote(1, kMaximumQuoteAgeNanoseconds + 1, "SPY"));
    require_unmarkable(
        service->get_portfolio(account_id),
        {{"AAPL", PortfolioValuationErrorCode::StaleMarketState}}
    );

    auto gap = chronos::validate_market_event(quote(
        4,
        kMaximumQuoteAgeNanoseconds + 20
    ));
    REQUIRE(std::holds_alternative<MarketStateUpdateError>(market->apply(
        std::get<chronos::ValidatedMarketEvent>(gap)
    )));
    require_unmarkable(
        service->get_portfolio(account_id),
        {{"AAPL", MarketStateUnavailableCode::StreamInterrupted}}
    );
}

void test_portfolio_rejects_a_quote_without_a_usable_bid() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "portfolio-no-bid");
    require_recorded(service->submit_order(
        order_request(account_id, "hold", Side::Buy, "1")
    ));

    TopOfBookEvent ask_only = quote(1, 1);
    ask_only.bid = std::nullopt;
    apply(*market, ask_only);
    require_unmarkable(
        service->get_portfolio(account_id),
        {{"AAPL", PortfolioValuationErrorCode::MissingBestBid}}
    );

    TopOfBookEvent suspect = quote(2, 2);
    suspect.header.quality =
        suspect.header.quality.with(MarketDataQualityFlag::PotentiallyInvalidBook);
    apply(*market, suspect);
    require_unmarkable(
        service->get_portfolio(account_id),
        {{"AAPL", PortfolioValuationErrorCode::SuspectMarketState}}
    );
}

void test_portfolio_rejects_an_incompatible_market_timeline_after_restart() {
    const auto original = market_state();
    for (std::uint64_t sequence = 0; sequence <= 5; ++sequence) {
        apply(*original, quote(sequence, static_cast<std::int64_t>(sequence)));
    }
    std::string account_id;
    {
        const auto service = make_service(*original);
        account_id = create_account(*service, "portfolio-timeline");
        require_recorded(service->submit_order(
            order_request(account_id, "at-five", Side::Buy, "1")
        ));
        require_portfolio(service->get_portfolio(account_id));
    }

    const auto other_stream = std::make_unique<MarketState>(
        MarketDataStreamId("another-recording")
    );
    TopOfBookEvent foreign = quote(0, 10);
    foreign.header.stream = MarketDataStreamId("another-recording");
    apply(*other_stream, foreign);
    require_market_unavailable(
        make_service(*other_stream)->get_portfolio(account_id),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );

    const auto earlier_replay = market_state();
    for (std::uint64_t sequence = 0; sequence <= 3; ++sequence) {
        apply(*earlier_replay, quote(sequence, static_cast<std::int64_t>(sequence)));
    }
    require_market_unavailable(
        make_service(*earlier_replay)->get_portfolio(account_id),
        PortfolioValuationErrorCode::MarketTimelineMismatch
    );
}

void test_portfolio_is_valued_identically_by_a_new_service() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    std::string account_id;
    {
        const auto service = make_service(*market);
        account_id = create_account(*service, "portfolio-restart");
        submit_valuation_history(*service, account_id);
    }
    require_expected_portfolio(
        require_portfolio(make_service(*market)->get_portfolio(account_id))
    );
}

void test_portfolio_valuation_is_read_only() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "portfolio-read-only");
    require_recorded(service->submit_order(
        order_request(account_id, "first", Side::Buy, "3")
    ));

    PostgresPaperOrderRepository inspector(connection_string);
    const auto journal_before = inspector.journal_summary(account_id);
    const auto consumed_ask = [&] {
        return inspector.load_account(account_id)
            ->processor.recovery_state()
            .execution.instruments.at(kAapl)
            .consumed_ask_quantity;
    };
    REQUIRE(consumed_ask() == Quantity::parse("3"));
    for (int valuation = 0; valuation < 3; ++valuation) {
        require_portfolio(service->get_portfolio(account_id));
    }
    const auto journal_after = inspector.journal_summary(account_id);
    REQUIRE(journal_after.order_count == journal_before.order_count);
    REQUIRE(journal_after.event_count == journal_before.event_count);
    REQUIRE(journal_after.applied_fill_count == journal_before.applied_fill_count);
    REQUIRE(journal_after.account_version == journal_before.account_version);
    REQUIRE(consumed_ask() == Quantity::parse("3"));

    // The remaining displayed ask is still 2 of the original 5.
    const auto next = require_receipt(require_recorded(service->submit_order(
        order_request(account_id, "second", Side::Buy, "3")
    )));
    REQUIRE(next.execution.fill->quantity == Quantity::parse("2"));
}

void test_portfolio_reports_persistence_outages_then_recovers() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const std::string application = unique_test_id("portfolio-outage");
    const auto service = make_service(
        *market,
        with_application_name(connection_string, application)
    );
    const std::string account_id = create_account(*service, "portfolio-outage");

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    require_error(
        service->get_portfolio(account_id),
        PaperTradingErrorCode::PersistenceUnavailable
    );
    const auto recovered = require_portfolio(service->get_portfolio(account_id));
    REQUIRE(recovered.valuation.snapshot.equity == Money::parse("1000"));
}

// The API cannot create an account without positive starting cash, but an
// in-process caller can; such an account has no defined return.
void test_portfolio_without_starting_cash_is_a_valuation_failure() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const std::string account_id = unique_test_id("portfolio-unfunded");
    {
        PostgresPaperOrderRepository direct(connection_string);
        REQUIRE(std::holds_alternative<PersistentPaperOrderSession>(
            PersistentPaperOrderSession::create(
                direct,
                account_id,
                Money{},
                paper_risk_limits()
            )
        ));
    }
    const PaperTradingError failure = require_error(
        make_service(*market)->get_portfolio(account_id),
        PaperTradingErrorCode::ValuationFailed
    );
    REQUIRE(failure.valuation == PortfolioValuationErrorCode::NonpositiveStartingCash);
}

// ---------------------------------------------------------------------------
// Multi-instrument accounts

void test_an_account_trades_several_instruments_with_shared_cash() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    apply(*market, quote(1, 1, "SPY", QuoteLevels{
        .bid_price = "499",
        .ask_price = "501",
    }));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "multi", "5000");

    require_recorded(service->submit_order(
        order_request(account_id, "aapl", Side::Buy, "2")
    ));
    const RecordedPaperOrder spy = require_recorded(service->submit_order(
        for_instrument(order_request(account_id, "spy", Side::Buy, "3"), "SPY")
    ));
    // SPY executed against its own quote: 3 x 501 + 0.01.
    REQUIRE(spy.market.header.instrument == kSpy);
    REQUIRE(require_receipt(spy).execution.fill->price == Price::parse("501"));
    // 5000 - 202.01 - 1503.01 = 3294.98, shared by both positions.
    REQUIRE(spy.account.state.cash == Money::parse("3294.98"));
    REQUIRE(spy.account.state.quantity(kAapl) == Quantity::parse("2"));
    REQUIRE(spy.account.state.quantity(kSpy) == Quantity::parse("3"));
    REQUIRE(spy.account.market_stream ==
        MarketDataStreamId(std::string(chronos::test::kTestStream)));

    const PaperPortfolioView portfolio = require_portfolio(
        service->get_portfolio(account_id)
    );
    const auto& snapshot = portfolio.valuation.snapshot;
    REQUIRE(snapshot.open_positions.size() == 2);
    REQUIRE(snapshot.open_positions[0].instrument == kAapl);
    REQUIRE(snapshot.open_positions[0].market_value == Money::parse("198"));
    REQUIRE(snapshot.open_positions[1].instrument == kSpy);
    REQUIRE(snapshot.open_positions[1].market_value == Money::parse("1497"));
    REQUIRE(snapshot.market_value == Money::parse("1695"));
    REQUIRE(snapshot.gross_unrealized_pnl == Money::parse("-10"));
    REQUIRE(snapshot.net_total_pnl == Money::parse("-10.02"));
    REQUIRE(snapshot.equity == Money::parse("4989.98"));
    REQUIRE(portfolio.valuation.marks.at(kSpy) == quote(1, 1, "SPY", QuoteLevels{
        .bid_price = "499",
        .ask_price = "501",
    }));
}

void test_an_order_executes_only_against_its_own_instrument() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "own-quote");

    // An AAPL quote cannot fill SPY: nothing is recorded.
    const PaperTradingError unavailable = require_error(
        service->submit_order(
            for_instrument(order_request(account_id, "spy", Side::Buy, "1"), "SPY")
        ),
        PaperTradingErrorCode::MarketDataUnavailable
    );
    REQUIRE(unavailable.market_state == MarketStateUnavailableCode::NoTopOfBook);
    REQUIRE(recorded_orders(account_id) == 0);

    apply(*market, quote(1, 1, "SPY", QuoteLevels{.ask_price = "450"}));
    const RecordedPaperOrder filled = require_recorded(service->submit_order(
        for_instrument(order_request(account_id, "spy", Side::Buy, "1"), "SPY")
    ));
    REQUIRE(filled.market == quote(1, 1, "SPY", QuoteLevels{.ask_price = "450"}));
    REQUIRE(require_receipt(filled).execution.fill->price == Price::parse("450"));
    REQUIRE(filled.account.state.position(kAapl) == nullptr);
}

void test_an_unsupported_instrument_is_rejected_before_recording() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    apply(*market, quote(1, 1, "MSFT"));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "unsupported");

    // MSFT has a quote, but this server does not trade it.
    REQUIRE(require_error(
        service->submit_order(
            for_instrument(order_request(account_id, "msft", Side::Buy, "1"), "MSFT")
        ),
        PaperTradingErrorCode::UnsupportedInstrument
    ).field == "instrument");
    REQUIRE(require_error(
        service->submit_order(
            for_instrument(order_request(account_id, "bad", Side::Buy, "1"), "AA PL")
        ),
        PaperTradingErrorCode::InvalidIdentifier
    ).field == "instrument");
    REQUIRE(recorded_orders(account_id) == 0);
    // The client order ID was not consumed.
    require_recorded(service->submit_order(
        order_request(account_id, "msft", Side::Buy, "1")
    ));
}

void test_displayed_liquidity_is_consumed_per_instrument() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    apply(*market, quote(1, 1, "SPY"));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "per-instrument", "5000");

    REQUIRE(require_receipt(require_recorded(service->submit_order(
        order_request(account_id, "aapl-all", Side::Buy, "5")
    ))).execution.status == SimulatedExecutionStatus::Filled);
    // AAPL consumption did not reduce SPY's displayed size.
    const PaperOrderReceipt spy = require_receipt(require_recorded(service->submit_order(
        for_instrument(order_request(account_id, "spy-all", Side::Buy, "5"), "SPY")
    )));
    REQUIRE(spy.execution.status == SimulatedExecutionStatus::Filled);
    REQUIRE(spy.execution.fill->quantity == Quantity::parse("5"));
    const PaperOrderReceipt exhausted = require_receipt(require_recorded(
        service->submit_order(order_request(account_id, "aapl-more", Side::Buy, "1"))
    ));
    REQUIRE(exhausted.execution.status == SimulatedExecutionStatus::Cancelled);
}

void test_a_client_order_id_repeated_for_another_symbol_is_a_duplicate() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    apply(*market, quote(1, 1, "SPY"));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "repeat-id");

    require_recorded(service->submit_order(
        order_request(account_id, "once", Side::Buy, "1")
    ));
    REQUIRE(require_error(
        service->submit_order(
            for_instrument(order_request(account_id, "once", Side::Buy, "1"), "SPY")
        ),
        PaperTradingErrorCode::DuplicateClientOrderId
    ).field == "client_order_id");
    // The original reconciles the retry.
    const PaperOrderRecord original = require_order_record(
        service->get_order(account_id, "once")
    );
    REQUIRE(original.intent.instrument == kAapl);
    REQUIRE(recorded_orders(account_id) == 1);
    REQUIRE(require_account(service->get_account(account_id))
        .state.position(kSpy) == nullptr);
}

// Each order costs 4 x 101 + 0.01 = 404.01, so the shared 1000 of cash buys
// exactly two whichever services and instruments win the races.
void test_concurrent_services_cannot_double_spend_shared_cash() {
    const auto market = market_state();
    apply(*market, quote(0, 0, "AAPL", QuoteLevels{.ask_quantity = "10"}));
    apply(*market, quote(1, 1, "SPY", QuoteLevels{.ask_quantity = "10"}));
    const auto first_service = make_service(*market);
    const auto second_service = make_service(*market);
    const std::string account_id = create_account(*first_service, "double-spend");

    const std::vector<std::pair<std::string, std::string>> orders{
        {"aapl-1", "AAPL"},
        {"spy-1", "SPY"},
        {"aapl-2", "AAPL"},
        {"spy-2", "SPY"},
    };
    std::vector<std::string> outcomes(orders.size());
    std::vector<std::thread> traders;
    for (std::size_t index = 0; index < orders.size(); ++index) {
        traders.emplace_back([&, index] {
            PaperTradingService& service =
                index % 2 == 0 ? *first_service : *second_service;
            try {
                for (;;) {
                    auto result = service.submit_order(for_instrument(
                        order_request(account_id, orders[index].first, Side::Buy, "4"),
                        orders[index].second
                    ));
                    if (const auto* failure = std::get_if<PaperTradingError>(&result)) {
                        if (failure->code ==
                            PaperTradingErrorCode::AccountVersionConflict) {
                            continue;
                        }
                        outcomes[index] = failure->message;
                        return;
                    }
                    const auto& recorded = std::get<RecordedPaperOrder>(result);
                    outcomes[index] =
                        std::holds_alternative<PaperOrderReceipt>(recorded.result)
                        ? "filled"
                        : std::get<PreTradeRiskRejection>(recorded.result).code ==
                                PreTradeRiskErrorCode::InsufficientCash
                            ? "insufficient cash"
                            : "other rejection";
                    return;
                }
            } catch (const std::exception& error) {
                outcomes[index] = error.what();
            }
        });
    }
    for (auto& trader : traders) {
        trader.join();
    }
    REQUIRE(std::count(outcomes.begin(), outcomes.end(), "filled") == 2);
    REQUIRE(std::count(outcomes.begin(), outcomes.end(), "insufficient cash") == 2);
    const PaperAccountView account = require_account(
        first_service->get_account(account_id)
    );
    REQUIRE(account.state.cash == Money::parse("191.98"));
    REQUIRE(account.version == 4);
}

void test_portfolio_reports_every_position_it_cannot_mark() {
    const auto market = market_state();
    apply(*market, quote(0, 0));
    apply(*market, quote(1, 1, "SPY"));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "unmarkable", "5000");
    require_recorded(service->submit_order(
        order_request(account_id, "aapl", Side::Buy, "1")
    ));
    require_recorded(service->submit_order(
        for_instrument(order_request(account_id, "spy", Side::Buy, "1"), "SPY")
    ));

    // An unheld symbol ages both marks; neither is valued as zero.
    apply(*market, quote(2, kMaximumQuoteAgeNanoseconds + 2, "MSFT"));
    require_unmarkable(
        service->get_portfolio(account_id),
        {
            {"AAPL", PortfolioValuationErrorCode::StaleMarketState},
            {"SPY", PortfolioValuationErrorCode::StaleMarketState},
        }
    );
    apply(*market, quote(3, kMaximumQuoteAgeNanoseconds + 3));
    require_unmarkable(
        service->get_portfolio(account_id),
        {{"SPY", PortfolioValuationErrorCode::StaleMarketState}}
    );

    // Once SPY is closed, only AAPL needs a mark.
    const auto sold = require_recorded(service->submit_order(
        for_instrument(order_request(account_id, "spy-close", Side::Sell, "1"), "SPY")
    ));
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(sold.result));
    apply(*market, quote(4, kMaximumQuoteAgeNanoseconds + 4, "SPY"));
    require_recorded(service->submit_order(
        for_instrument(order_request(account_id, "spy-close-fresh", Side::Sell, "1"), "SPY")
    ));
    const PaperPortfolioView valued = require_portfolio(service->get_portfolio(account_id));
    REQUIRE(valued.valuation.snapshot.open_positions.size() == 1);
    REQUIRE(valued.valuation.snapshot.open_positions[0].instrument == kAapl);
    REQUIRE(!valued.account.state.positions.at(kSpy).is_open());
}

void test_history_can_be_filtered_by_instrument() {
    const auto market = market_state();
    apply(*market, quote(0, 0, "AAPL", QuoteLevels{.ask_quantity = "50"}));
    apply(*market, quote(1, 1, "SPY", QuoteLevels{.ask_quantity = "50"}));
    const auto service = make_service(*market);
    const std::string account_id = create_account(*service, "filtered", "5000");
    for (const auto& [id, instrument] : std::vector<std::pair<std::string, std::string>>{
             {"spy-0", "SPY"},
             {"aapl-0", "AAPL"},
             {"spy-1", "SPY"},
             {"aapl-1", "AAPL"},
             {"spy-2", "SPY"},
         }) {
        require_recorded(service->submit_order(
            for_instrument(order_request(account_id, id, Side::Buy, "1"), instrument)
        ));
    }

    const auto first = require_order_history(
        service->list_orders(account_id, page(2, std::nullopt, "SPY"))
    );
    REQUIRE((client_order_ids(first) == std::vector<std::string>{"spy-2", "spy-1"}));
    const auto rest = require_order_history(
        service->list_orders(account_id, page(2, first.next_cursor, "SPY"))
    );
    REQUIRE((client_order_ids(rest) == std::vector<std::string>{"spy-0"}));
    REQUIRE(!rest.next_cursor.has_value());

    // A filtered cursor pages only the same filter.
    for (const std::optional<std::string>& other : {
             std::optional<std::string>(),
             std::optional<std::string>("AAPL"),
         }) {
        REQUIRE(require_error(
            service->list_orders(account_id, page(2, first.next_cursor, other)),
            PaperTradingErrorCode::InvalidCursor
        ).field == "cursor");
    }
    const auto unfiltered = require_order_history(service->list_orders(account_id, page(2)));
    require_error(
        service->list_orders(account_id, page(2, unfiltered.next_cursor, "SPY")),
        PaperTradingErrorCode::InvalidCursor
    );
    // An orders cursor is not a fills cursor, even with the same filter.
    require_error(
        service->list_fills(account_id, page(2, first.next_cursor, "SPY")),
        PaperTradingErrorCode::InvalidCursor
    );

    const auto aapl_fills = require_fill_history(
        service->list_fills(account_id, page(std::nullopt, std::nullopt, "AAPL"))
    );
    REQUIRE((client_order_ids(aapl_fills) ==
        std::vector<std::string>{"aapl-1", "aapl-0"}));
    REQUIRE(require_order_history(
        service->list_orders(account_id, page(std::nullopt, std::nullopt, "MSFT"))
    ).orders.empty());
    REQUIRE(require_error(
        service->list_orders(account_id, page(std::nullopt, std::nullopt, "bad symbol")),
        PaperTradingErrorCode::InvalidIdentifier
    ).field == "instrument");
}

}  // namespace

int main() {
    const char* configured_connection =
        std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "paper-trading service test skipped: "
                     "CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    PostgresPaperOrderRepository(connection_string)
        .apply_migration(chronos::test::all_migrations_sql());

    return chronos::test::run_tests({
        {
            "account creation applies server risk limits",
            test_account_is_created_with_server_risk_limits_and_retrieved,
        },
        {
            "duplicate account leaves original unchanged",
            test_duplicate_account_leaves_the_original_unchanged,
        },
        {
            "account requests are validated",
            test_account_requests_are_validated_before_persistence,
        },
        {
            "marketable order fills at server quote",
            test_marketable_order_fills_at_the_server_quote_and_market_time,
        },
        {
            "order beyond displayed liquidity partially fills",
            test_order_beyond_displayed_liquidity_partially_fills,
        },
        {
            "non-marketable limit is cancelled",
            test_non_marketable_limit_is_cancelled_and_books_nothing,
        },
        {
            "risk rejection is recorded",
            test_risk_rejection_is_recorded_and_consumes_the_client_order_id,
        },
        {
            "missing market state records nothing",
            test_missing_market_state_records_nothing,
        },
        {
            "interrupted stream blocks orders",
            test_interrupted_market_stream_blocks_orders,
        },
        {
            "stale quote is a recorded rejection",
            test_stale_quote_is_a_recorded_risk_rejection_at_market_time,
        },
        {
            "duplicate client order ID is not recorded twice",
            test_duplicate_client_order_id_is_not_recorded_twice,
        },
        {
            "version conflict records nothing",
            test_concurrent_writer_causes_a_version_conflict_that_records_nothing,
        },
        {
            "durable state survives a new service",
            test_durable_state_and_shadow_liquidity_survive_a_new_service,
        },
        {
            "shadow liquidity is isolated between accounts",
            test_shadow_liquidity_is_isolated_between_accounts,
        },
        {
            "manual and model orders are treated identically",
            test_manual_and_model_orders_receive_identical_treatment,
        },
        {
            "lost database connection recovers",
            test_lost_database_connection_is_unavailable_then_recovers,
        },
        {
            "health reports persistence and market stream",
            test_health_reports_persistence_and_market_stream,
        },
        {
            "history distinguishes missing accounts",
            test_history_distinguishes_missing_accounts_from_empty_history,
        },
        {
            "history pages follow opaque cursors",
            test_history_pages_follow_opaque_cursors_newest_first,
        },
        {
            "page limits and cursors are validated",
            test_page_limits_and_cursors_are_validated_by_the_service,
        },
        {
            "default page holds fifty orders",
            test_default_page_holds_fifty_orders,
        },
        {
            "uncertain submission is reconciled by lookup",
            test_uncertain_submission_is_reconciled_by_lookup,
        },
        {
            "manual and model orders read back identically",
            test_manual_and_model_orders_read_back_identically,
        },
        {
            "history reads report outages then recover",
            test_history_reads_report_outages_then_recover,
        },
        {
            "portfolio values the durable account at the bid",
            test_portfolio_values_the_durable_account_at_the_best_bid,
        },
        {
            "portfolio requires a valid existing account",
            test_portfolio_requires_a_valid_existing_account,
        },
        {
            "a flat account is valued without any quote",
            test_a_flat_account_is_valued_without_any_quote,
        },
        {
            "portfolio requires a current quote per holding",
            test_portfolio_requires_a_current_quote_for_each_held_instrument,
        },
        {
            "portfolio rejects a quote without a usable bid",
            test_portfolio_rejects_a_quote_without_a_usable_bid,
        },
        {
            "portfolio rejects an incompatible timeline",
            test_portfolio_rejects_an_incompatible_market_timeline_after_restart,
        },
        {
            "portfolio is valued identically by a new service",
            test_portfolio_is_valued_identically_by_a_new_service,
        },
        {
            "portfolio valuation is read only",
            test_portfolio_valuation_is_read_only,
        },
        {
            "portfolio reports persistence outages",
            test_portfolio_reports_persistence_outages_then_recovers,
        },
        {
            "portfolio without starting cash fails valuation",
            test_portfolio_without_starting_cash_is_a_valuation_failure,
        },
        {
            "an account trades several instruments with shared cash",
            test_an_account_trades_several_instruments_with_shared_cash,
        },
        {
            "an order executes only against its own instrument",
            test_an_order_executes_only_against_its_own_instrument,
        },
        {
            "an unsupported instrument is rejected before recording",
            test_an_unsupported_instrument_is_rejected_before_recording,
        },
        {
            "displayed liquidity is consumed per instrument",
            test_displayed_liquidity_is_consumed_per_instrument,
        },
        {
            "a client order ID repeated for another symbol is a duplicate",
            test_a_client_order_id_repeated_for_another_symbol_is_a_duplicate,
        },
        {
            "concurrent services cannot double-spend shared cash",
            test_concurrent_services_cannot_double_spend_shared_cash,
        },
        {
            "portfolio reports every position it cannot mark",
            test_portfolio_reports_every_position_it_cannot_mark,
        },
        {
            "history can be filtered by instrument",
            test_history_can_be_filtered_by_instrument,
        },
    });
}
