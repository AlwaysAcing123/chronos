#include "chronos/postgresql_paper_order_repository.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using chronos::BookLevel;
using chronos::InstrumentId;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperOrderReceipt;
using chronos::PaperPersistenceConflict;
using chronos::PaperPersistenceConflictCode;
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
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;

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

std::string unique_account_id(std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::system_clock::now()
        .time_since_epoch()
        .count();
    return std::string(prefix) + "-" + std::to_string(now) + "-" +
        std::to_string(counter.fetch_add(1));
}

std::string migration_sql() {
    const std::string path =
        std::string(CHRONOS_SOURCE_DIR) +
        "/migrations/001_paper_trading.sql";
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open migration: " + path);
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("restart");
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
    restarted_store.apply_migration(migration_sql());
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("duplicate");
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("rejection");
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("concurrency");
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("processed-id");
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("malformed");
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
    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("missing-client-id");
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

    store.apply_migration(migration_sql());
    const std::string account_id = unique_account_id("migration-recovery");
    auto session = create_session(store, account_id);
    REQUIRE(session.version() == 0);
    REQUIRE(session.account().state().cash == money("1000"));
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
    });
}
