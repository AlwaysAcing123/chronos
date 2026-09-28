#include "chronos/paper_trading/paper_trading_service.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

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
using chronos::paper_trading::CreatePaperAccountRequest;
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
        .instrument = "AAPL",
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
            .instrument = "AAPL",
            .starting_cash = Money::parse("1000.5"),
        }
    ));
    REQUIRE(created.account_id == account_id);
    REQUIRE(created.instrument == InstrumentId("AAPL"));
    REQUIRE(created.version == 0);
    REQUIRE(created.is_healthy);
    REQUIRE(created.state.starting_cash == Money::parse("1000.5"));
    REQUIRE(created.state.cash == Money::parse("1000.5"));
    REQUIRE(created.state.position == Quantity{});
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
            .instrument = "SPY",
            .starting_cash = Money::parse("5000"),
        }),
        PaperTradingErrorCode::AccountAlreadyExists
    );
    REQUIRE(duplicate.field == "account_id");

    const PaperAccountView original = require_account(
        service->get_account(account_id)
    );
    REQUIRE(original.instrument == InstrumentId("AAPL"));
    REQUIRE(original.state.cash == Money::parse("1000"));
}

void test_account_requests_are_validated_before_persistence() {
    const auto market = market_state();
    const auto service = make_service(*market);

    const auto create = [&](std::string account_id,
                            std::string instrument,
                            std::string_view cash) {
        return service->create_account(CreatePaperAccountRequest{
            .account_id = std::move(account_id),
            .instrument = std::move(instrument),
            .starting_cash = Money::parse(cash),
        });
    };
    REQUIRE(require_error(
        create(unique_test_id("unsupported"), "MSFT", "1000"),
        PaperTradingErrorCode::UnsupportedInstrument
    ).field == "instrument");
    REQUIRE(require_error(
        create(unique_test_id("zero-cash"), "AAPL", "0"),
        PaperTradingErrorCode::InvalidStartingCash
    ).field == "starting_cash");
    REQUIRE(require_error(
        create(unique_test_id("negative-cash"), "AAPL", "-1"),
        PaperTradingErrorCode::InvalidStartingCash
    ).field == "starting_cash");
    REQUIRE(require_error(
        create(unique_test_id("bad-instrument"), "AA PL", "1000"),
        PaperTradingErrorCode::InvalidIdentifier
    ).field == "instrument");

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
            create(invalid, "AAPL", "1000"),
            PaperTradingErrorCode::InvalidIdentifier
        ).field == "account_id");
        REQUIRE(require_error(
            service->get_account(invalid),
            PaperTradingErrorCode::InvalidIdentifier
        ).field == "account_id");
    }

    std::string longest = unique_test_id("max-length");
    longest.resize(chronos::paper_trading::kMaximumIdentifierLength, 'x');
    require_account(create(longest, "AAPL", "1000"));
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
    REQUIRE(recorded.account.state.position == Quantity::parse("2"));
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
    REQUIRE(receipt.account_after.position == Quantity::parse("5"));
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
    REQUIRE(account.state.position == Quantity::parse("1"));
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
    REQUIRE(recovered.state.position == Quantity::parse("3"));
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
        .apply_migration(chronos::test::migration_sql());

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
    });
}
