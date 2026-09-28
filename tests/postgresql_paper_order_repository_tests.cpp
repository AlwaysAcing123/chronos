#include "chronos/postgresql_paper_order_repository.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using chronos::BookLevel;
using chronos::InstrumentId;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperFillRecord;
using chronos::PaperJournalPage;
using chronos::PaperJournalPageRequest;
using chronos::PaperJournalReadMiss;
using chronos::PaperOrderFaultRecord;
using chronos::PaperOrderProcessingErrorCode;
using chronos::PaperOrderReceipt;
using chronos::PaperOrderRecord;
using chronos::PaperPersistenceConflict;
using chronos::PaperPersistenceConflictCode;
using chronos::PaperPersistenceUnavailable;
using chronos::PersistentPaperOrderReceipt;
using chronos::PersistentPaperOrderResult;
using chronos::PersistentPaperOrderSession;
using chronos::PostgresPaperOrderRepository;
using chronos::PreTradeRiskErrorCode;
using chronos::PreTradeRiskLimits;
using chronos::PreTradeRiskRejection;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedExecutionError;
using chronos::SimulatedExecutionErrorCode;
using chronos::SimulatedExecutionReport;
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::test::all_migrations_sql;
using chronos::test::PostgresTestConnection;
using chronos::test::terminate_backends;
using chronos::test::unique_test_id;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;

std::string connection_string;

static_assert(!std::is_copy_constructible_v<PostgresPaperOrderRepository>);
static_assert(!std::is_move_constructible_v<PostgresPaperOrderRepository>);

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

PreTradeRiskLimits risk_limits(
    std::int64_t maximum_quote_age_nanoseconds = 1'000
) {
    return PreTradeRiskLimits{
        .maximum_order_quantity = quantity("100"),
        .maximum_order_notional = money("100000"),
        .maximum_position_quantity = quantity("100"),
        .maximum_quote_age_nanoseconds = maximum_quote_age_nanoseconds,
        .fee_per_fill = money("0.01"),
    };
}

PostgresPaperOrderRepository repository() {
    return PostgresPaperOrderRepository(connection_string);
}

TopOfBookEvent market(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view bid_price = "99",
    std::string_view bid_quantity = "5",
    std::string_view ask_price = "101",
    std::string_view ask_quantity = "5"
) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = InstrumentId("AAPL"),
            .stream = chronos::MarketDataStreamId("market-stream"),
            .venue = chronos::VenueId("XNAS"),
            .event_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp - 1),
            .receive_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp),
            .ingest_sequence = sequence,
            .venue_sequence = sequence + 1'000,
            .quality = chronos::MarketDataQuality{}.with(
                chronos::MarketDataQualityFlag::Replay
            ),
        },
        .bid = BookLevel{
            .price = price(bid_price),
            .quantity = quantity(bid_quantity),
        },
        .ask = BookLevel{
            .price = price(ask_price),
            .quantity = quantity(ask_quantity),
        },
    };
}

OrderIntent order(
    std::string account_id,
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at
) {
    return OrderIntent{
        .client_order_id = std::move(client_order_id),
        .account_id = std::move(account_id),
        .source = OrderSource{
            .kind = OrderSourceKind::Manual,
            .source_id = "integration-user",
        },
        .instrument = InstrumentId("AAPL"),
        .side = side,
        .type = OrderType::Market,
        .quantity = quantity(order_quantity),
        .limit_price = std::nullopt,
        .time_in_force = TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(submitted_at),
    };
}

PersistentPaperOrderSession create_session(
    PostgresPaperOrderRepository& store,
    const std::string& account_id,
    PreTradeRiskLimits limits = risk_limits()
) {
    auto created = PersistentPaperOrderSession::create(
        store,
        account_id,
        InstrumentId("AAPL"),
        money("1000"),
        limits
    );
    REQUIRE(std::holds_alternative<PersistentPaperOrderSession>(created));
    return std::get<PersistentPaperOrderSession>(std::move(created));
}

PersistentPaperOrderReceipt require_persisted_receipt(
    PersistentPaperOrderResult result
) {
    REQUIRE(std::holds_alternative<PersistentPaperOrderReceipt>(result));
    return std::get<PersistentPaperOrderReceipt>(std::move(result));
}

const PaperPersistenceConflict& require_conflict(
    const PersistentPaperOrderResult& result,
    PaperPersistenceConflictCode code
) {
    REQUIRE(std::holds_alternative<PaperPersistenceConflict>(result));
    const auto& conflict = std::get<PaperPersistenceConflict>(result);
    REQUIRE(conflict.code == code);
    REQUIRE(!conflict.message.empty());
    return conflict;
}

PaperOrderReceipt require_order_receipt(PersistentPaperOrderReceipt receipt) {
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(receipt.result));
    return std::get<PaperOrderReceipt>(std::move(receipt.result));
}

void test_fills_and_shadow_liquidity_survive_restart() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("restart");
    auto session = create_session(store, account_id);
    const OrderIntent first_order = order(
        account_id,
        "first",
        Side::Buy,
        "3",
        110
    );
    auto first_persisted = require_persisted_receipt(session.submit(
        first_order,
        market(10, 100)
    ));
    REQUIRE(first_persisted.account_version == 1);
    const auto first = require_order_receipt(std::move(first_persisted));
    REQUIRE(first.account_after.position == quantity("3"));

    auto restarted_store = repository();
    restarted_store.apply_migration(all_migrations_sql());
    auto recovered = PersistentPaperOrderSession::load(
        restarted_store,
        account_id
    );
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->version() == 1);
    REQUIRE(recovered->account().state().position == quantity("3"));
    const auto second = require_order_receipt(require_persisted_receipt(
        recovered->submit(
            order(account_id, "second", Side::Buy, "3", 110),
            market(10, 100)
        )
    ));
    REQUIRE(
        second.execution.status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(second.execution.fill->quantity == quantity("2"));
    REQUIRE(second.execution.unfilled_quantity == quantity("1"));
    REQUIRE(second.account_after.position == quantity("5"));

    const auto summary = restarted_store.journal_summary(account_id);
    REQUIRE(summary.order_count == 2);
    REQUIRE(summary.event_count == 8);
    REQUIRE(summary.applied_fill_count == 2);
    REQUIRE(summary.account_version == 2);

    auto recovered_again = PersistentPaperOrderSession::load(
        restarted_store,
        account_id
    );
    REQUIRE(recovered_again.has_value());
    REQUIRE(recovered_again->account().state().cash == money("494.98"));
    REQUIRE(
        recovered_again->account().state().open_cost_basis == money("505")
    );
    REQUIRE(recovered_again->account().state().fees_paid == money("0.02"));
}

void test_duplicate_submission_rolls_back_every_database_change() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("duplicate");
    auto session = create_session(store, account_id);
    const OrderIntent intent = order(
        account_id,
        "one-client-id",
        Side::Buy,
        "1",
        110
    );
    require_persisted_receipt(session.submit(intent, market(10, 100)));
    const Money cash_after_first = session.account().state().cash;

    const PersistentPaperOrderResult duplicate = session.submit(
        intent,
        market(10, 100)
    );
    require_conflict(
        duplicate,
        PaperPersistenceConflictCode::DuplicateClientOrderId
    );
    REQUIRE(session.version() == 1);
    REQUIRE(session.account().state().cash == cash_after_first);
    REQUIRE(session.account().state().position == quantity("1"));

    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 1);
    REQUIRE(summary.event_count == 4);
    REQUIRE(summary.applied_fill_count == 1);
    REQUIRE(summary.account_version == 1);
}

void test_risk_rejection_is_durable_and_consumes_database_idempotency_key() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("rejection");
    auto session = create_session(store, account_id, risk_limits(5));
    const OrderIntent rejected_order = order(
        account_id,
        "rejected-id",
        Side::Buy,
        "1",
        110
    );
    auto rejected = require_persisted_receipt(session.submit(
        rejected_order,
        market(10, 100)
    ));
    REQUIRE(rejected.account_version == 1);
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(rejected.result));
    REQUIRE(
        std::get<PreTradeRiskRejection>(rejected.result).code ==
        PreTradeRiskErrorCode::StaleMarketState
    );
    REQUIRE(session.account().state().position == Quantity{});

    auto recovered = PersistentPaperOrderSession::load(store, account_id);
    REQUIRE(recovered.has_value());
    const PersistentPaperOrderResult duplicate = recovered->submit(
        rejected_order,
        market(11, 109)
    );
    require_conflict(
        duplicate,
        PaperPersistenceConflictCode::DuplicateClientOrderId
    );
    REQUIRE(recovered->version() == 1);
    REQUIRE(recovered->account().state().position == Quantity{});

    const auto accepted = require_order_receipt(require_persisted_receipt(
        recovered->submit(
            order(account_id, "new-id", Side::Buy, "1", 110),
            market(11, 109)
        )
    ));
    REQUIRE(accepted.account_after.position == quantity("1"));
    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 2);
    REQUIRE(summary.event_count == 6);
    REQUIRE(summary.applied_fill_count == 1);
    REQUIRE(summary.account_version == 2);
}

void test_optimistic_version_conflict_preserves_committed_account() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("concurrency");
    auto first_session = create_session(store, account_id);
    auto second_session = PersistentPaperOrderSession::load(store, account_id);
    REQUIRE(second_session.has_value());

    require_persisted_receipt(first_session.submit(
        order(account_id, "winner", Side::Buy, "1", 110),
        market(10, 100)
    ));
    const PersistentPaperOrderResult stale = second_session->submit(
        order(account_id, "loser", Side::Buy, "2", 110),
        market(10, 100)
    );
    require_conflict(stale, PaperPersistenceConflictCode::VersionConflict);
    REQUIRE(second_session->version() == 0);
    REQUIRE(second_session->account().state().position == Quantity{});

    auto recovered = PersistentPaperOrderSession::load(store, account_id);
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->version() == 1);
    REQUIRE(recovered->account().state().position == quantity("1"));
    REQUIRE(recovered->account().state().cash == money("898.99"));
    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 1);
    REQUIRE(summary.event_count == 4);
    REQUIRE(summary.account_version == 1);
}

void test_processed_client_ids_survive_restart() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("processed-id");
    auto session = create_session(store, account_id);
    const OrderIntent intent = order(
        account_id,
        "processed",
        Side::Buy,
        "1",
        110
    );
    require_persisted_receipt(session.submit(intent, market(10, 100)));

    auto stored = store.load_account(account_id);
    REQUIRE(stored.has_value());
    const auto result = stored->processor.submit(intent, market(10, 100));
    REQUIRE(std::holds_alternative<SimulatedExecutionError>(result));
    REQUIRE(
        std::get<SimulatedExecutionError>(result).code ==
        SimulatedExecutionErrorCode::DuplicateClientOrderId
    );
}

void test_malformed_order_is_preserved_as_a_risk_rejection() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("malformed");
    auto session = create_session(store, account_id);
    OrderIntent malformed = order(
        account_id,
        "malformed-order",
        Side::Buy,
        "1",
        110
    );
    malformed.side = static_cast<Side>(99);
    malformed.quantity = Quantity{};
    malformed.submitted_at = Timestamp::from_nanoseconds(-1);

    auto persisted = require_persisted_receipt(session.submit(
        malformed,
        market(10, 100)
    ));
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(persisted.result));
    REQUIRE(
        std::get<PreTradeRiskRejection>(persisted.result).code ==
        PreTradeRiskErrorCode::UnknownSide
    );
    REQUIRE(persisted.account_version == 1);
    REQUIRE(session.account().state().position == Quantity{});
    REQUIRE(session.account().state().cash == money("1000"));

    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 1);
    REQUIRE(summary.event_count == 2);
    REQUIRE(summary.applied_fill_count == 0);
    REQUIRE(summary.account_version == 1);
}

void test_missing_client_order_ids_remain_auditable() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("missing-client-id");
    auto session = create_session(store, account_id);
    OrderIntent first = order(account_id, "", Side::Buy, "1", 110);
    OrderIntent second = order(account_id, "", Side::Buy, "1", 111);

    const auto first_result = require_persisted_receipt(session.submit(
        first,
        market(10, 100)
    ));
    const auto second_result = require_persisted_receipt(session.submit(
        second,
        market(11, 101)
    ));
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(first_result.result));
    REQUIRE(std::holds_alternative<PreTradeRiskRejection>(second_result.result));
    REQUIRE(
        std::get<PreTradeRiskRejection>(first_result.result).code ==
        PreTradeRiskErrorCode::MissingClientOrderId
    );
    REQUIRE(
        std::get<PreTradeRiskRejection>(second_result.result).code ==
        PreTradeRiskErrorCode::MissingClientOrderId
    );
    REQUIRE(first_result.account_version == 1);
    REQUIRE(second_result.account_version == 2);

    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 2);
    REQUIRE(summary.event_count == 4);
    REQUIRE(summary.applied_fill_count == 0);
    REQUIRE(summary.account_version == 2);
}

void test_failed_migration_does_not_poison_connection() {
    auto store = repository();
    REQUIRE_THROWS_AS(
        store.apply_migration(
            "BEGIN; SELECT * FROM chronos_missing_table; COMMIT;"
        ),
        std::runtime_error
    );

    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("migration-recovery");
    auto session = create_session(store, account_id);
    REQUIRE(session.version() == 0);
    REQUIRE(session.account().state().cash == money("1000"));
}

void test_text_with_nul_bytes_is_never_persisted() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("nul-text");
    const std::string account_id_with_nul = account_id + std::string("\0x", 2);
    REQUIRE_THROWS_AS(
        PersistentPaperOrderSession::create(
            store,
            account_id_with_nul,
            InstrumentId("AAPL"),
            money("1000"),
            risk_limits()
        ),
        std::invalid_argument
    );
    REQUIRE(!store.load_account(account_id).has_value());

    auto session = create_session(store, account_id);
    const std::string client_order_id_with_nul("order\0suffix", 12);
    REQUIRE_THROWS_AS(
        session.submit(
            order(account_id, client_order_id_with_nul, Side::Buy, "1", 110),
            market(10, 100)
        ),
        std::invalid_argument
    );
    REQUIRE(session.version() == 0);
    REQUIRE(session.account().state().position == Quantity{});
    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 0);
    REQUIRE(summary.account_version == 0);

    require_persisted_receipt(session.submit(
        order(account_id, "order", Side::Buy, "1", 110),
        market(10, 100)
    ));
}

void test_lost_connection_is_reported_and_then_reconnected() {
    const std::string application = unique_test_id("lost-connection");
    PostgresPaperOrderRepository store(
        with_application_name(connection_string, application)
    );
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("reconnect");
    auto session = create_session(store, account_id);
    require_persisted_receipt(session.submit(
        order(account_id, "before-loss", Side::Buy, "1", 110),
        market(10, 100)
    ));

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    REQUIRE_THROWS_AS(
        store.load_account(account_id),
        PaperPersistenceUnavailable
    );

    auto recovered = PersistentPaperOrderSession::load(store, account_id);
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->version() == 1);
    REQUIRE(recovered->account().state().position == quantity("1"));

    terminate_backends(observer, application);
    REQUIRE(store.check_availability());
}

void test_connection_lost_during_commit_records_nothing() {
    const std::string application = unique_test_id("commit-loss");
    PostgresPaperOrderRepository store(
        with_application_name(connection_string, application)
    );
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("commit-loss");
    auto session = create_session(store, account_id);
    const OrderIntent intent = order(
        account_id,
        "interrupted",
        Side::Buy,
        "1",
        110
    );

    PostgresTestConnection lock_holder(connection_string);
    PostgresTestConnection observer(connection_string);
    static_cast<void>(lock_holder.query("BEGIN"));
    static_cast<void>(lock_holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));

    std::string submitter_outcome = "returned without an exception";
    std::thread submitter([&] {
        try {
            static_cast<void>(session.submit(intent, market(10, 100)));
        } catch (const PaperPersistenceUnavailable&) {
            submitter_outcome = "unavailable";
        } catch (const std::exception& error) {
            submitter_outcome = error.what();
        }
    });
    wait_until_blocked_on_lock(observer, application);
    terminate_backends(observer, application);
    submitter.join();
    static_cast<void>(lock_holder.query("ROLLBACK"));

    REQUIRE(submitter_outcome == "unavailable");
    REQUIRE(session.version() == 0);
    REQUIRE(session.account().state().position == Quantity{});
    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 0);
    REQUIRE(summary.event_count == 0);
    REQUIRE(summary.account_version == 0);

    const auto retried = require_persisted_receipt(
        session.submit(intent, market(10, 100))
    );
    REQUIRE(retried.account_version == 1);
}

// ---------------------------------------------------------------------------
// Journal read model

PaperJournalPage<PaperOrderRecord> require_order_page(
    PostgresPaperOrderRepository& store,
    const std::string& account_id,
    std::size_t limit,
    std::optional<std::int64_t> before = std::nullopt
) {
    auto page = store.list_orders(
        account_id,
        PaperJournalPageRequest{.limit = limit, .before_record_id = before}
    );
    REQUIRE(std::holds_alternative<PaperJournalPage<PaperOrderRecord>>(page));
    return std::get<PaperJournalPage<PaperOrderRecord>>(std::move(page));
}

PaperJournalPage<PaperFillRecord> require_fill_page(
    PostgresPaperOrderRepository& store,
    const std::string& account_id,
    std::size_t limit,
    std::optional<std::int64_t> before = std::nullopt
) {
    auto page = store.list_fills(
        account_id,
        PaperJournalPageRequest{.limit = limit, .before_record_id = before}
    );
    REQUIRE(std::holds_alternative<PaperJournalPage<PaperFillRecord>>(page));
    return std::get<PaperJournalPage<PaperFillRecord>>(std::move(page));
}

PaperOrderRecord require_order_record(
    PostgresPaperOrderRepository& store,
    const std::string& account_id,
    const std::string& client_order_id
) {
    auto lookup = store.find_order(account_id, client_order_id);
    REQUIRE(std::holds_alternative<PaperOrderRecord>(lookup));
    return std::get<PaperOrderRecord>(std::move(lookup));
}

std::vector<std::string> client_order_ids(
    const PaperJournalPage<PaperOrderRecord>& page
) {
    std::vector<std::string> ids;
    for (const auto& record : page.records) {
        ids.push_back(record.intent.client_order_id);
    }
    return ids;
}

void test_empty_history_is_distinct_from_a_missing_account() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("empty-history");
    create_session(store, account_id);

    const auto orders = require_order_page(store, account_id, 10);
    REQUIRE(orders.records.empty());
    REQUIRE(!orders.next_before_record_id.has_value());
    const auto fills = require_fill_page(store, account_id, 10);
    REQUIRE(fills.records.empty());
    REQUIRE(!fills.next_before_record_id.has_value());
    REQUIRE(std::get<PaperJournalReadMiss>(store.find_order(account_id, "none")) ==
            PaperJournalReadMiss::OrderNotFound);

    const std::string missing = unique_test_id("missing-history");
    const PaperJournalPageRequest request{.limit = 10, .before_record_id = {}};
    REQUIRE(std::get<PaperJournalReadMiss>(store.list_orders(missing, request)) ==
            PaperJournalReadMiss::AccountNotFound);
    REQUIRE(std::get<PaperJournalReadMiss>(store.list_fills(missing, request)) ==
            PaperJournalReadMiss::AccountNotFound);
    REQUIRE(std::get<PaperJournalReadMiss>(store.find_order(missing, "none")) ==
            PaperJournalReadMiss::AccountNotFound);
}

void test_executed_orders_are_reconstructed_exactly() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("reconstruct-executed");
    auto session = create_session(store, account_id);
    const TopOfBookEvent quote = market(
        7,
        1'790'343'000'123'456'789,
        "99.123456789",
        "5",
        "101.987654321",
        "3.12345678"
    );
    const std::int64_t submitted_at = 1'790'343'000'123'456'790;

    OrderIntent filled = order(account_id, "filled", Side::Buy, "1.5", submitted_at);
    OrderIntent partial = order(account_id, "partial", Side::Buy, "4", submitted_at);
    OrderIntent passive = order(account_id, "passive", Side::Buy, "1", submitted_at);
    passive.type = OrderType::Limit;
    passive.limit_price = price("100.000000001");
    passive.source = OrderSource{
        .kind = OrderSourceKind::Model,
        .source_id = "momentum-v1",
    };

    std::vector<SimulatedExecutionReport> expected;
    for (const OrderIntent& intent : {filled, partial, passive}) {
        expected.push_back(require_order_receipt(require_persisted_receipt(
            session.submit(intent, quote)
        )).execution);
    }
    REQUIRE(expected[0].status == SimulatedExecutionStatus::Filled);
    REQUIRE(
        expected[1].status ==
        SimulatedExecutionStatus::PartiallyFilledAndCancelled
    );
    REQUIRE(expected[2].status == SimulatedExecutionStatus::Cancelled);

    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto record = require_order_record(
            store,
            account_id,
            expected[index].intent.client_order_id
        );
        REQUIRE(record.intent == expected[index].intent);
        REQUIRE(record.market == quote);
        REQUIRE(std::holds_alternative<SimulatedExecutionReport>(record.outcome));
        REQUIRE(std::get<SimulatedExecutionReport>(record.outcome) == expected[index]);
    }

    const auto page = require_order_page(store, account_id, 10);
    REQUIRE((client_order_ids(page) ==
             std::vector<std::string>{"passive", "partial", "filled"}));
    REQUIRE(page.records[0].intent.source.kind == OrderSourceKind::Model);
    REQUIRE(
        page.records[0].intent.limit_price->raw() == 100'000'000'001
    );
    REQUIRE(
        std::get<SimulatedExecutionReport>(page.records[1].outcome)
            .fill->quantity == quantity("1.62345678")
    );
}

void test_rejections_and_faults_are_reconstructed_exactly() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("reconstruct-rejected");
    auto session = create_session(store, account_id, risk_limits(5));

    const auto stale = require_persisted_receipt(session.submit(
        order(account_id, "stale", Side::Buy, "1", 110),
        market(10, 100)
    ));
    const auto risk_record = require_order_record(store, account_id, "stale");
    REQUIRE(std::get<PreTradeRiskRejection>(risk_record.outcome) ==
            std::get<PreTradeRiskRejection>(stale.result));

    require_persisted_receipt(session.submit(
        order(account_id, "seed", Side::Buy, "1", 110),
        market(11, 108)
    ));
    TopOfBookEvent other_stream = market(12, 109);
    other_stream.header.stream = chronos::MarketDataStreamId("other-stream");
    const auto rejected = require_persisted_receipt(session.submit(
        order(account_id, "other-stream", Side::Buy, "1", 110),
        other_stream
    ));
    const auto execution_record = require_order_record(
        store,
        account_id,
        "other-stream"
    );
    REQUIRE(std::get<SimulatedExecutionError>(execution_record.outcome) ==
            std::get<SimulatedExecutionError>(rejected.result));
    REQUIRE(
        std::get<SimulatedExecutionError>(execution_record.outcome).code ==
        SimulatedExecutionErrorCode::MarketStreamMismatch
    );
    REQUIRE(execution_record.market == other_stream);
}

// The whole order passes risk because its maximum notional is representable,
// but the one displayed lot fills for a notional that rounds to zero, so
// accounting rejects the risk-approved fill and the processor halts.
void test_unbooked_fill_of_a_fault_is_never_reported_as_booked() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("unbooked-fault");
    auto session = create_session(store, account_id);
    require_persisted_receipt(session.submit(
        order(account_id, "booked", Side::Buy, "1", 110),
        market(10, 100)
    ));
    const TopOfBookEvent dust = market(
        11,
        105,
        "0.000000001",
        "1",
        "0.000000001",
        "1"
    );
    const auto faulted = require_persisted_receipt(session.submit(
        order(account_id, "dust", Side::Buy, "10", 110),
        dust
    ));
    REQUIRE(
        std::get<chronos::PaperOrderProcessingError>(faulted.result).code ==
        PaperOrderProcessingErrorCode::AccountingInvariantViolation
    );
    require_persisted_receipt(session.submit(
        order(account_id, "halted", Side::Buy, "1", 110),
        dust
    ));

    PostgresTestConnection inspector(connection_string);
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM paper_order_records "
        "WHERE account_id = $1 AND fill_id IS NOT NULL",
        {account_id}
    ) == "2");

    const auto fault = require_order_record(store, account_id, "dust");
    const auto& fault_outcome = std::get<PaperOrderFaultRecord>(fault.outcome);
    REQUIRE(
        fault_outcome.code ==
        PaperOrderProcessingErrorCode::AccountingInvariantViolation
    );
    REQUIRE(fault_outcome.unbooked_execution.has_value());
    REQUIRE(fault_outcome.unbooked_execution->fill->quantity == quantity("1"));
    const auto halted = require_order_record(store, account_id, "halted");
    const auto& halted_outcome = std::get<PaperOrderFaultRecord>(halted.outcome);
    REQUIRE(
        halted_outcome.code == PaperOrderProcessingErrorCode::ProcessorFaulted
    );
    REQUIRE(!halted_outcome.unbooked_execution.has_value());

    const auto fills = require_fill_page(store, account_id, 10);
    REQUIRE(fills.records.size() == 1);
    REQUIRE(fills.records.front().client_order_id == "booked");
    REQUIRE(fills.records.front().fill.fill_id == "booked:fill:0");
}

void test_fill_history_links_booked_fills_newest_first() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("fill-history");
    auto session = create_session(store, account_id);
    const TopOfBookEvent quote = market(10, 100, "99", "5", "101", "5");
    const auto first = require_order_receipt(require_persisted_receipt(
        session.submit(order(account_id, "buy-a", Side::Buy, "2", 110), quote)
    ));
    OrderIntent passive = order(account_id, "passive", Side::Buy, "1", 110);
    passive.type = OrderType::Limit;
    passive.limit_price = price("100");
    require_persisted_receipt(session.submit(passive, quote));
    require_persisted_receipt(session.submit(
        order(account_id, "too-big", Side::Buy, "90", 110),
        quote
    ));
    OrderIntent model_sell = order(account_id, "sell-b", Side::Sell, "1.5", 111);
    model_sell.source = OrderSource{
        .kind = OrderSourceKind::Model,
        .source_id = "mean-reversion-v2",
    };
    const auto second = require_order_receipt(require_persisted_receipt(
        session.submit(model_sell, quote)
    ));

    const auto fills = require_fill_page(store, account_id, 10);
    REQUIRE(fills.records.size() == 2);
    REQUIRE(fills.records[0].account_id == account_id);
    REQUIRE(fills.records[0].client_order_id == "sell-b");
    REQUIRE(fills.records[0].source.kind == OrderSourceKind::Model);
    REQUIRE(fills.records[0].source.source_id == "mean-reversion-v2");
    REQUIRE(fills.records[0].fill == *second.execution.fill);
    REQUIRE(fills.records[1].client_order_id == "buy-a");
    REQUIRE(fills.records[1].fill == *first.execution.fill);
    REQUIRE(fills.records[0].record_id > fills.records[1].record_id);
}

void test_pages_are_bounded_and_stable_while_orders_arrive() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("keyset-pages");
    auto session = create_session(store, account_id);
    std::vector<std::string> submitted;
    for (int index = 0; index < 5; ++index) {
        const std::string id = "order-" + std::to_string(index);
        OrderIntent passive = order(account_id, id, Side::Buy, "1", 110);
        passive.type = OrderType::Limit;
        passive.limit_price = price("100");
        require_persisted_receipt(session.submit(passive, market(10, 100)));
        submitted.push_back(id);
    }

    const auto first = require_order_page(store, account_id, 2);
    REQUIRE((client_order_ids(first) ==
             std::vector<std::string>{"order-4", "order-3"}));
    REQUIRE(first.next_before_record_id == first.records.back().record_id);

    OrderIntent newer = order(account_id, "newer", Side::Buy, "1", 110);
    newer.type = OrderType::Limit;
    newer.limit_price = price("100");
    require_persisted_receipt(session.submit(newer, market(10, 100)));

    const auto second = require_order_page(
        store,
        account_id,
        2,
        first.next_before_record_id
    );
    REQUIRE((client_order_ids(second) ==
             std::vector<std::string>{"order-2", "order-1"}));
    const auto last = require_order_page(
        store,
        account_id,
        2,
        second.next_before_record_id
    );
    REQUIRE((client_order_ids(last) == std::vector<std::string>{"order-0"}));
    REQUIRE(!last.next_before_record_id.has_value());

    const auto exact = require_order_page(store, account_id, 6);
    REQUIRE(exact.records.size() == 6);
    REQUIRE(!exact.next_before_record_id.has_value());
    REQUIRE(exact.records.front().intent.client_order_id == "newer");
    const auto one_short = require_order_page(store, account_id, 5);
    REQUIRE(one_short.next_before_record_id.has_value());
    const auto past_oldest = require_order_page(
        store,
        account_id,
        10,
        exact.records.back().record_id
    );
    REQUIRE(past_oldest.records.empty());
    REQUIRE(!past_oldest.next_before_record_id.has_value());
}

void test_invalid_read_requests_are_rejected_before_querying() {
    auto store = repository();
    const std::string account_id = unique_test_id("invalid-read");
    REQUIRE_THROWS_AS(
        store.list_orders(
            account_id,
            PaperJournalPageRequest{.limit = 0, .before_record_id = {}}
        ),
        std::invalid_argument
    );
    for (const std::int64_t before : {std::int64_t{0}, std::int64_t{-1}}) {
        REQUIRE_THROWS_AS(
            store.list_fills(
                account_id,
                PaperJournalPageRequest{.limit = 1, .before_record_id = before}
            ),
            std::invalid_argument
        );
    }
    REQUIRE_THROWS_AS(store.find_order(account_id, ""), std::invalid_argument);
    REQUIRE_THROWS_AS(
        store.find_order(account_id, std::string("order\0suffix", 12)),
        std::invalid_argument
    );
    REQUIRE_THROWS_AS(
        store.list_orders(
            account_id + std::string("\0x", 2),
            PaperJournalPageRequest{.limit = 1, .before_record_id = {}}
        ),
        std::invalid_argument
    );
}

void test_malformed_intents_round_trip_through_the_journal() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("malformed-read");
    auto session = create_session(store, account_id);
    OrderIntent malformed = order(account_id, "malformed", Side::Buy, "1", 110);
    malformed.side = static_cast<Side>(99);
    malformed.quantity = Quantity{};
    malformed.submitted_at = Timestamp::from_nanoseconds(-1);
    require_persisted_receipt(session.submit(malformed, market(10, 100)));

    const auto record = require_order_record(store, account_id, "malformed");
    REQUIRE(record.intent == malformed);
    REQUIRE(
        std::get<PreTradeRiskRejection>(record.outcome).code ==
        PreTradeRiskErrorCode::UnknownSide
    );

    // The writer spells an undeclared value exactly one way.
    PostgresTestConnection editor(connection_string);
    static_cast<void>(editor.query(
        "UPDATE paper_order_records SET side = 'unknown:099' "
        "WHERE account_id = $1",
        {account_id}
    ));
    REQUIRE_THROWS_AS(
        store.find_order(account_id, "malformed"),
        std::runtime_error
    );
}

void test_inconsistent_journal_rows_fail_closed() {
    auto store = repository();
    store.apply_migration(all_migrations_sql());
    PostgresTestConnection editor(connection_string);
    const auto corrupted_read = [&](const std::string& prefix,
                                    std::string_view update) {
        const std::string account_id = unique_test_id(prefix);
        auto session = create_session(store, account_id);
        require_persisted_receipt(session.submit(
            order(account_id, "subject", Side::Buy, "1", 110),
            market(10, 100)
        ));
        static_cast<void>(editor.query(update, {account_id}));
        REQUIRE_THROWS_AS(
            store.find_order(account_id, "subject"),
            std::runtime_error
        );
        REQUIRE_THROWS_AS(
            store.list_orders(
                account_id,
                PaperJournalPageRequest{.limit = 5, .before_record_id = {}}
            ),
            std::runtime_error
        );
    };
    corrupted_read(
        "corrupt-status",
        "UPDATE paper_order_records SET execution_status = 'cancelled' "
        "WHERE account_id = $1"
    );
    corrupted_read(
        "corrupt-quantity",
        "UPDATE paper_order_records SET unfilled_quantity_raw = 1 "
        "WHERE account_id = $1"
    );
    corrupted_read(
        "corrupt-side",
        "UPDATE paper_order_records SET side = 'unknown:1' "
        "WHERE account_id = $1"
    );
    corrupted_read(
        "corrupt-code",
        "UPDATE paper_order_records SET outcome_code = 'insufficient_cash' "
        "WHERE account_id = $1"
    );
    corrupted_read(
        "corrupt-market",
        "UPDATE paper_order_records "
        "SET execution_market_ingest_sequence = 11 WHERE account_id = $1"
    );
}

void test_journal_reads_report_connection_loss_and_reconnect() {
    const std::string application = unique_test_id("read-outage");
    PostgresPaperOrderRepository store(
        with_application_name(connection_string, application)
    );
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("read-outage");
    auto session = create_session(store, account_id);
    require_persisted_receipt(session.submit(
        order(account_id, "before-outage", Side::Buy, "1", 110),
        market(10, 100)
    ));

    PostgresTestConnection observer(connection_string);
    terminate_backends(observer, application);
    REQUIRE_THROWS_AS(
        store.list_orders(
            account_id,
            PaperJournalPageRequest{.limit = 5, .before_record_id = {}}
        ),
        PaperPersistenceUnavailable
    );
    REQUIRE(require_order_page(store, account_id, 5).records.size() == 1);

    terminate_backends(observer, application);
    REQUIRE_THROWS_AS(
        store.find_order(account_id, "before-outage"),
        PaperPersistenceUnavailable
    );
    REQUIRE(require_fill_page(store, account_id, 5).records.size() == 1);
}

}  // namespace

int main() {
    const char* configured_connection =
        std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "PostgreSQL integration test skipped: "
                     "CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {
            "fills and shadow liquidity survive restart",
            test_fills_and_shadow_liquidity_survive_restart,
        },
        {
            "duplicate submission rolls back transaction",
            test_duplicate_submission_rolls_back_every_database_change,
        },
        {
            "risk rejection is durable and idempotent",
            test_risk_rejection_is_durable_and_consumes_database_idempotency_key,
        },
        {
            "optimistic version conflict preserves committed account",
            test_optimistic_version_conflict_preserves_committed_account,
        },
        {
            "processed client ids survive restart",
            test_processed_client_ids_survive_restart,
        },
        {
            "malformed order is durably rejected",
            test_malformed_order_is_preserved_as_a_risk_rejection,
        },
        {
            "missing client order IDs remain auditable",
            test_missing_client_order_ids_remain_auditable,
        },
        {
            "failed migration leaves connection usable",
            test_failed_migration_does_not_poison_connection,
        },
        {
            "text with NUL bytes is never persisted",
            test_text_with_nul_bytes_is_never_persisted,
        },
        {
            "lost connection is reported then reconnected",
            test_lost_connection_is_reported_and_then_reconnected,
        },
        {
            "connection lost during commit records nothing",
            test_connection_lost_during_commit_records_nothing,
        },
        {
            "empty history is distinct from a missing account",
            test_empty_history_is_distinct_from_a_missing_account,
        },
        {
            "executed orders are reconstructed exactly",
            test_executed_orders_are_reconstructed_exactly,
        },
        {
            "rejections are reconstructed exactly",
            test_rejections_and_faults_are_reconstructed_exactly,
        },
        {
            "unbooked fault fill is never reported as booked",
            test_unbooked_fill_of_a_fault_is_never_reported_as_booked,
        },
        {
            "fill history links booked fills newest first",
            test_fill_history_links_booked_fills_newest_first,
        },
        {
            "pages are bounded and stable while orders arrive",
            test_pages_are_bounded_and_stable_while_orders_arrive,
        },
        {
            "invalid read requests are rejected",
            test_invalid_read_requests_are_rejected_before_querying,
        },
        {
            "malformed intents round trip through the journal",
            test_malformed_intents_round_trip_through_the_journal,
        },
        {
            "inconsistent journal rows fail closed",
            test_inconsistent_journal_rows_fail_closed,
        },
        {
            "journal reads survive connection loss",
            test_journal_reads_report_connection_loss_and_reconnect,
        },
    });
}
