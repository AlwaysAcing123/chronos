#include "chronos/postgresql_paper_order_repository.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

// Migration 003 turns single-instrument accounts into multi-instrument ones;
// migration 004 adds durable order state, fills, and reservations for
// good-til-cancelled working orders; migration 005 adds model deployments
// and the guard on the accounts they own. Each test builds its own database in a
// scratch schema of the shared test database, so it can start empty or from
// rows in an old format.

namespace {

using chronos::InstrumentId;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperJournalPage;
using chronos::PaperJournalPageRequest;
using chronos::PaperOrderReceipt;
using chronos::PaperOrderRecord;
using chronos::PaperPersistenceConflict;
using chronos::PaperPersistenceConflictCode;
using chronos::PersistentPaperOrderReceipt;
using chronos::PersistentPaperOrderSession;
using chronos::PositionState;
using chronos::PostgresPaperOrderRepository;
using chronos::PreTradeRiskErrorCode;
using chronos::PreTradeRiskLimits;
using chronos::PreTradeRiskRejection;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::test::all_migrations_sql;
using chronos::test::migration_sql;
using chronos::test::PostgresTestConnection;
using chronos::test::unique_test_id;

std::string connection_string;

const InstrumentId kAapl("AAPL");
const InstrumentId kSpy("SPY");

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

// A scratch schema of the shared test database.
class ScratchSchema final : public chronos::test::ScratchSchema {
public:
    ScratchSchema() : chronos::test::ScratchSchema(connection_string) {}
};

void apply(PostgresPaperOrderRepository& store, std::string_view file_name) {
    store.apply_migration(migration_sql(file_name));
}

// A database as the single-instrument release left it.
void apply_single_instrument_schema(PostgresPaperOrderRepository& store) {
    apply(store, "001_paper_trading.sql");
    apply(store, "002_paper_order_read_indexes.sql");
}

// Accounts in every state the single-instrument release could persist:
//
//   legacy-open       AAPL: bought 3 @ 101, sold 1 @ 102; holds 2, and a
//                     rejected SPY order that did not match its instrument
//   legacy-closed     SPY: bought 2 @ 101, sold 2 @ 99; flat, unhealthy
//   legacy-cancelled  AAPL: one passive order cancelled, never filled
//   legacy-empty      SPY: never traded
//
// Every fill paid a fee of 0.01. Prices are scaled by 1e9, quantities and
// money by 1e8.
constexpr std::string_view kSingleInstrumentRows = R"SQL(
INSERT INTO paper_accounts (
    account_id, instrument, starting_cash_raw, cash_raw, position_raw,
    open_cost_basis_raw, gross_realized_pnl_raw, fees_paid_raw,
    last_fill_timestamp_ns, maximum_order_quantity_raw,
    maximum_order_notional_raw, maximum_position_quantity_raw,
    maximum_quote_age_ns, fee_per_fill_raw, is_healthy, active_stream_id,
    active_venue, active_event_timestamp_ns, active_receive_timestamp_ns,
    active_ingest_sequence, active_venue_sequence, active_quality_flags,
    active_bid_price_raw, active_bid_quantity_raw, active_ask_price_raw,
    active_ask_quantity_raw, consumed_bid_quantity_raw,
    consumed_ask_quantity_raw, version
) VALUES
    ('legacy-open', 'AAPL', 100000000000, 79898000000, 200000000,
     20200000000, 100000000, 2000000, 120, 10000000000, 10000000000000,
     10000000000, 1000, 1000000, true, 'legacy-stream', 'XNAS', 114, 115,
     11, 1011, 0, 102000000000, 500000000, 103000000000, 500000000,
     100000000, 0, 3),
    ('legacy-closed', 'SPY', 100000000000, 99598000000, 0, 0, -400000000,
     2000000, 220, 10000000000, 10000000000000, 10000000000, 1000, 1000000,
     false, 'legacy-stream', 'XNAS', 204, 205, 21, 1021, 0, 99000000000,
     500000000, 101000000000, 500000000, 200000000, 0, 2),
    ('legacy-cancelled', 'AAPL', 100000000000, 100000000000, 0, 0, 0, 0,
     NULL, 10000000000, 10000000000000, 10000000000, 1000, 1000000, true,
     'legacy-stream', NULL, 299, 300, 30, NULL, 0, 99000000000, 500000000,
     101000000000, 500000000, 0, 0, 1),
    ('legacy-empty', 'SPY', 100000000000, 100000000000, 0, 0, 0, 0, NULL,
     10000000000, 10000000000000, 10000000000, 1000, 1000000, true, NULL,
     NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0);

INSERT INTO paper_order_records (
    account_id, client_order_id, intent_account_id, instrument, source_kind,
    source_id, side, order_type, quantity_raw, limit_price_raw,
    time_in_force, submitted_at_ns, market_instrument, market_stream_id,
    market_venue, market_event_timestamp_ns, market_receive_timestamp_ns,
    market_ingest_sequence, market_venue_sequence, market_quality_flags,
    market_bid_price_raw, market_bid_quantity_raw, market_ask_price_raw,
    market_ask_quantity_raw, outcome_kind, outcome_code, outcome_field,
    outcome_message, execution_status, unfilled_quantity_raw,
    cancellation_reason, execution_market_ingest_sequence, fill_id,
    fill_side, fill_price_raw, fill_quantity_raw, fill_fee_raw,
    fill_timestamp_ns, account_cash_after_raw, account_position_after_raw,
    account_open_cost_basis_after_raw, account_gross_realized_pnl_after_raw,
    account_fees_paid_after_raw, account_last_fill_timestamp_after_ns,
    processed_by_execution
) VALUES
    ('legacy-open', 'buy-1', 'legacy-open', 'AAPL', 'manual', 'legacy-user',
     'buy', 'market', 300000000, NULL, 'ioc', 110, 'AAPL', 'legacy-stream',
     'XNAS', 99, 100, 10, 1010, 0, 99000000000, 500000000, 101000000000,
     500000000, 'receipt', NULL, NULL, NULL, 'filled', 0, NULL, 10,
     'buy-1:fill:0', 'buy', 101000000000, 300000000, 1000000, 110,
     69699000000, 300000000, 30300000000, 0, 1000000, 110, true),
    ('legacy-open', 'sell-1', 'legacy-open', 'AAPL', 'manual', 'legacy-user',
     'sell', 'market', 100000000, NULL, 'ioc', 120, 'AAPL', 'legacy-stream',
     'XNAS', 114, 115, 11, 1011, 0, 102000000000, 500000000, 103000000000,
     500000000, 'receipt', NULL, NULL, NULL, 'filled', 0, NULL, 11,
     'sell-1:fill:0', 'sell', 102000000000, 100000000, 1000000, 120,
     79898000000, 200000000, 20200000000, 100000000, 2000000, 120, true),
    ('legacy-open', 'wrong-symbol', 'legacy-open', 'SPY', 'manual',
     'legacy-user', 'buy', 'market', 100000000, NULL, 'ioc', 121, 'SPY',
     'legacy-stream', 'XNAS', 115, 116, 12, 1012, 0, 99000000000, 500000000,
     101000000000, 500000000, 'risk_rejection', 'instrument_mismatch',
     'instrument', 'order instrument does not match the account', NULL,
     NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 79898000000,
     200000000, 20200000000, 100000000, 2000000, 120, false),
    ('legacy-closed', 'buy-2', 'legacy-closed', 'SPY', 'manual',
     'legacy-user', 'buy', 'market', 200000000, NULL, 'ioc', 210, 'SPY',
     'legacy-stream', 'XNAS', 199, 200, 20, 1020, 0, 99000000000, 500000000,
     101000000000, 500000000, 'receipt', NULL, NULL, NULL, 'filled', 0,
     NULL, 20, 'buy-2:fill:0', 'buy', 101000000000, 200000000, 1000000, 210,
     79799000000, 200000000, 20200000000, 0, 1000000, 210, true),
    ('legacy-closed', 'sell-2', 'legacy-closed', 'SPY', 'manual',
     'legacy-user', 'sell', 'market', 200000000, NULL, 'ioc', 220, 'SPY',
     'legacy-stream', 'XNAS', 204, 205, 21, 1021, 0, 99000000000, 500000000,
     101000000000, 500000000, 'receipt', NULL, NULL, NULL, 'filled', 0,
     NULL, 21, 'sell-2:fill:0', 'sell', 99000000000, 200000000, 1000000, 220,
     99598000000, 0, 0, -400000000, 2000000, 220, true),
    ('legacy-cancelled', 'passive', 'legacy-cancelled', 'AAPL', 'model',
     'momentum-v1', 'buy', 'limit', 100000000, 100000000000, 'ioc', 310,
     'AAPL', 'legacy-stream', NULL, 299, 300, 30, NULL, 0, 99000000000,
     500000000, 101000000000, 500000000, 'receipt', NULL, NULL, NULL,
     'cancelled', 100000000, 'limit_not_marketable', 30, NULL, NULL, NULL,
     NULL, NULL, NULL, 100000000000, 0, 0, 0, 0, NULL, true);

INSERT INTO paper_events (
    account_id, order_record_id, client_order_id, event_order, event_type
)
SELECT
    record.account_id, record.order_record_id, record.client_order_id,
    event.event_order, event.event_type
FROM paper_order_records AS record
JOIN (VALUES
    ('buy-1', 1, 'order_submitted'), ('buy-1', 2, 'order_completed'),
    ('buy-1', 3, 'fill_applied'), ('buy-1', 4, 'account_state_updated'),
    ('sell-1', 1, 'order_submitted'), ('sell-1', 2, 'order_completed'),
    ('sell-1', 3, 'fill_applied'), ('sell-1', 4, 'account_state_updated'),
    ('wrong-symbol', 1, 'order_submitted'),
    ('wrong-symbol', 2, 'risk_rejected'),
    ('buy-2', 1, 'order_submitted'), ('buy-2', 2, 'order_completed'),
    ('buy-2', 3, 'fill_applied'), ('buy-2', 4, 'account_state_updated'),
    ('sell-2', 1, 'order_submitted'), ('sell-2', 2, 'order_completed'),
    ('sell-2', 3, 'fill_applied'), ('sell-2', 4, 'account_state_updated'),
    ('passive', 1, 'order_submitted'), ('passive', 2, 'order_completed'),
    ('passive', 3, 'account_state_updated')
) AS event (client_order_id, event_order, event_type)
    ON event.client_order_id = record.client_order_id
ORDER BY record.order_record_id, event.event_order;
)SQL";

// Every row the journal holds, which migration 003 must not change.
constexpr std::string_view kJournalChecksum = R"SQL(
SELECT md5(
    (SELECT string_agg(concat_ws('|', order_record_id, account_id,
        client_order_id, instrument, side, quantity_raw, submitted_at_ns,
        market_instrument, market_ingest_sequence, outcome_kind,
        coalesce(outcome_code, '-'), coalesce(execution_status, '-'),
        coalesce(fill_id, '-'), coalesce(fill_price_raw::text, '-'),
        coalesce(fill_quantity_raw::text, '-'), account_cash_after_raw,
        processed_by_execution, created_at), ',' ORDER BY order_record_id)
     FROM paper_order_records) || '/' ||
    (SELECT string_agg(concat_ws('|', event_sequence, account_id,
        order_record_id, event_order, event_type, created_at), ','
        ORDER BY event_sequence)
     FROM paper_events)
)
)SQL";

PreTradeRiskLimits legacy_limits() {
    return PreTradeRiskLimits{
        .maximum_order_quantity = quantity("100"),
        .maximum_order_notional = money("100000"),
        .maximum_position_quantity = quantity("100"),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = money("0.01"),
    };
}

TopOfBookEvent quote(
    const InstrumentId& instrument,
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view bid = "99",
    std::string_view ask = "101"
) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = instrument,
            .stream = chronos::MarketDataStreamId("legacy-stream"),
            .venue = chronos::VenueId("XNAS"),
            .event_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp - 1),
            .receive_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp),
            .ingest_sequence = sequence,
            .venue_sequence = sequence + 1'000,
            .quality = chronos::MarketDataQuality{},
        },
        .bid = chronos::BookLevel{
            .price = price(bid),
            .quantity = quantity("5"),
        },
        .ask = chronos::BookLevel{
            .price = price(ask),
            .quantity = quantity("5"),
        },
    };
}

OrderIntent market_order(
    const std::string& account_id,
    const InstrumentId& instrument,
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at
) {
    return OrderIntent{
        .client_order_id = std::move(client_order_id),
        .account_id = account_id,
        .source = OrderSource{
            .kind = OrderSourceKind::Manual,
            .source_id = "upgraded-user",
        },
        .instrument = instrument,
        .side = side,
        .type = OrderType::Market,
        .quantity = quantity(order_quantity),
        .limit_price = std::nullopt,
        .time_in_force = TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(submitted_at),
    };
}

PaperOrderReceipt require_filled(chronos::PersistentPaperOrderResult result) {
    REQUIRE(std::holds_alternative<PersistentPaperOrderReceipt>(result));
    auto receipt = std::get<PersistentPaperOrderReceipt>(std::move(result));
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(receipt.result));
    return std::get<PaperOrderReceipt>(std::move(receipt.result));
}

std::string required_migration_error(
    PostgresPaperOrderRepository& store,
    std::string_view file_name
) {
    try {
        apply(store, file_name);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    throw chronos::test::TestFailure("migration unexpectedly succeeded");
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
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

void test_a_fresh_database_gets_the_multi_instrument_schema() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    store.apply_migration(all_migrations_sql());
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar(
        "SELECT string_agg(version::text, ',' ORDER BY version) "
        "FROM chronos_schema_migrations"
    ) == "1,2,3,4,5,6,7");
    REQUIRE(inspector.scalar(
        "SELECT string_agg(column_name, ',' ORDER BY ordinal_position) "
        "FROM information_schema.columns "
        "WHERE table_schema = current_schema() "
        "  AND table_name = 'paper_accounts'"
    ) == "account_id,starting_cash_raw,cash_raw,maximum_order_quantity_raw,"
         "maximum_order_notional_raw,maximum_position_quantity_raw,"
         "maximum_quote_age_ns,fee_per_fill_raw,is_healthy,version,"
         "created_at,updated_at,market_stream_id");

    auto created = PersistentPaperOrderSession::create(
        store,
        "fresh",
        money("1000"),
        legacy_limits()
    );
    auto& session = std::get<PersistentPaperOrderSession>(created);
    require_filled(session.submit(
        market_order("fresh", kAapl, "aapl", Side::Buy, "1", 110),
        quote(kAapl, 1, 100)
    ));
    require_filled(session.submit(
        market_order("fresh", kSpy, "spy", Side::Buy, "1", 110),
        quote(kSpy, 2, 101)
    ));
    PostgresPaperOrderRepository restarted(schema.connection());
    const auto recovered = PersistentPaperOrderSession::load(restarted, "fresh");
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->account().state() == session.account().state());
    REQUIRE(recovered->executions() == session.executions());
    REQUIRE(recovered->version() == 2);
}

void test_single_instrument_accounts_upgrade_without_loss() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    apply_single_instrument_schema(store);
    PostgresTestConnection inspector(schema.connection());
    inspector.run_script(kSingleInstrumentRows);
    const std::string journal_before = inspector.scalar(kJournalChecksum);

    apply(store, "003_multi_instrument_accounts.sql");

    REQUIRE(inspector.scalar(
        "SELECT string_agg(version::text, ',' ORDER BY version) "
        "FROM chronos_schema_migrations"
    ) == "1,2,3");
    // Cash, limits, health, and version are unchanged; the stream binding
    // comes from the old active market.
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws(':', account_id, starting_cash_raw, "
        "cash_raw, maximum_order_quantity_raw, maximum_order_notional_raw, "
        "maximum_position_quantity_raw, maximum_quote_age_ns, "
        "fee_per_fill_raw, is_healthy, version, "
        "coalesce(market_stream_id, '-')), ',' ORDER BY account_id) "
        "FROM paper_accounts"
    ) == "legacy-cancelled:100000000000:100000000000:10000000000:"
         "10000000000000:10000000000:1000:1000000:t:1:legacy-stream,"
         "legacy-closed:100000000000:99598000000:10000000000:"
         "10000000000000:10000000000:1000:1000000:f:2:legacy-stream,"
         "legacy-empty:100000000000:100000000000:10000000000:"
         "10000000000000:10000000000:1000:1000000:t:0:-,"
         "legacy-open:100000000000:79898000000:10000000000:"
         "10000000000000:10000000000:1000:1000000:t:3:legacy-stream");
    // A ledger exists exactly for the accounts that had a fill.
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws(':', account_id, instrument, "
        "quantity_raw, open_cost_basis_raw, gross_realized_pnl_raw, "
        "fees_paid_raw, last_fill_timestamp_ns), ',' ORDER BY account_id) "
        "FROM paper_positions"
    ) == "legacy-closed:SPY:0:0:-400000000:2000000:220,"
         "legacy-open:AAPL:200000000:20200000000:100000000:2000000:120");
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws(':', account_id, instrument, "
        "market_stream_id, coalesce(market_venue, '-'), "
        "market_event_timestamp_ns, market_receive_timestamp_ns, "
        "market_ingest_sequence, coalesce(market_venue_sequence::text, '-'), "
        "market_quality_flags, market_bid_price_raw, "
        "market_bid_quantity_raw, market_ask_price_raw, "
        "market_ask_quantity_raw, consumed_bid_quantity_raw, "
        "consumed_ask_quantity_raw), ',' ORDER BY account_id) "
        "FROM paper_execution_states"
    ) == "legacy-cancelled:AAPL:legacy-stream:-:299:300:30:-:0:99000000000:"
         "500000000:101000000000:500000000:0:0,"
         "legacy-closed:SPY:legacy-stream:XNAS:204:205:21:1021:0:"
         "99000000000:500000000:101000000000:500000000:200000000:0,"
         "legacy-open:AAPL:legacy-stream:XNAS:114:115:11:1011:0:"
         "102000000000:500000000:103000000000:500000000:100000000:0");
    // The journal and events are untouched; their "after" snapshot names
    // the account's old instrument, even for the order that named another.
    REQUIRE(inspector.scalar(kJournalChecksum) == journal_before);
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws(':', client_order_id, instrument, "
        "position_after_instrument, position_quantity_after_raw, "
        "position_open_cost_basis_after_raw, "
        "position_gross_realized_pnl_after_raw, "
        "position_fees_paid_after_raw, "
        "coalesce(position_last_fill_timestamp_after_ns::text, '-')), ',' "
        "ORDER BY order_record_id) FROM paper_order_records"
    ) == "buy-1:AAPL:AAPL:300000000:30300000000:0:1000000:110,"
         "sell-1:AAPL:AAPL:200000000:20200000000:100000000:2000000:120,"
         "wrong-symbol:SPY:AAPL:200000000:20200000000:100000000:2000000:120,"
         "buy-2:SPY:SPY:200000000:20200000000:0:1000000:210,"
         "sell-2:SPY:SPY:0:0:-400000000:2000000:220,"
         "passive:AAPL:AAPL:0:0:0:0:-");

    // The current release reads version 4, which copies all of this.
    apply(store, "004_working_orders.sql");

    // Recovery restores every account exactly.
    const auto open = PersistentPaperOrderSession::load(store, "legacy-open");
    REQUIRE(open.has_value());
    REQUIRE(open->version() == 3);
    REQUIRE(open->is_healthy());
    REQUIRE(open->account().state().starting_cash == money("1000"));
    REQUIRE(open->account().state().cash == money("798.98"));
    REQUIRE(open->account().state().positions.size() == 1);
    const PositionState held_aapl{
        .quantity = quantity("2"),
        .open_cost_basis = money("202"),
        .gross_realized_pnl = money("1"),
        .fees_paid = money("0.02"),
        .last_fill_timestamp = Timestamp::from_nanoseconds(120),
    };
    REQUIRE(open->account().state().positions.at(kAapl) == held_aapl);
    REQUIRE(open->executions().size() == 1);
    REQUIRE(open->executions().at(kAapl).active_market == quote(kAapl, 11, 115, "102", "103"));
    REQUIRE(open->executions().at(kAapl).consumed_bid_quantity == quantity("1"));
    REQUIRE(open->risk_limits().fee_per_fill == money("0.01"));

    const auto closed = PersistentPaperOrderSession::load(store, "legacy-closed");
    REQUIRE(closed.has_value());
    REQUIRE(!closed->is_healthy());
    REQUIRE(closed->version() == 2);
    REQUIRE(!closed->account().state().positions.at(kSpy).is_open());
    REQUIRE(closed->account().state().gross_realized_pnl() == money("-4"));

    const auto cancelled = PersistentPaperOrderSession::load(store, "legacy-cancelled");
    REQUIRE(cancelled.has_value());
    REQUIRE(cancelled->account().state().positions.empty());
    REQUIRE(cancelled->executions().at(kAapl).active_market.header.ingest_sequence == 30);

    const auto empty = PersistentPaperOrderSession::load(store, "legacy-empty");
    REQUIRE(empty.has_value());
    REQUIRE(empty->version() == 0);
    REQUIRE(empty->executions().empty());

    // The old journal reads back exactly, including the retired rejection.
    const auto rejected = std::get<PaperOrderRecord>(
        store.find_order("legacy-open", "wrong-symbol")
    );
    REQUIRE(rejected.intent.instrument == kSpy);
    REQUIRE(std::get<PreTradeRiskRejection>(rejected.outcome).code ==
        PreTradeRiskErrorCode::InstrumentMismatch);
}

void test_an_upgraded_account_trades_several_instruments() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    apply_single_instrument_schema(store);
    PostgresTestConnection inspector(schema.connection());
    inspector.run_script(kSingleInstrumentRows);
    apply(store, "003_multi_instrument_accounts.sql");
    apply(store, "004_working_orders.sql");

    auto session = PersistentPaperOrderSession::load(store, "legacy-open");
    REQUIRE(session.has_value());
    // A new instrument on the account's stream, then the old one continues
    // from its recovered execution state.
    require_filled(session->submit(
        market_order("legacy-open", kSpy, "buy-spy", Side::Buy, "2", 130),
        quote(kSpy, 40, 125)
    ));
    require_filled(session->submit(
        market_order("legacy-open", kAapl, "sell-rest", Side::Sell, "2", 131),
        quote(kAapl, 41, 126, "104", "105")
    ));
    // 798.98 - 202.01 + 207.99 = 804.96, and AAPL realized 1 + 6 = 7.
    REQUIRE(session->account().state().cash == money("804.96"));
    REQUIRE(!session->account().state().positions.at(kAapl).is_open());
    REQUIRE(session->account().state().positions.at(kAapl).gross_realized_pnl == money("7"));
    REQUIRE(session->account().state().quantity(kSpy) == quantity("2"));
    REQUIRE(session->version() == 5);

    // A client order ID from before the upgrade is still taken, whatever
    // the symbol.
    const auto repeated = session->submit(
        market_order("legacy-open", kSpy, "buy-1", Side::Buy, "1", 132),
        quote(kSpy, 42, 127)
    );
    REQUIRE(std::get<PaperPersistenceConflict>(repeated).code ==
        PaperPersistenceConflictCode::DuplicateClientOrderId);

    PostgresPaperOrderRepository restarted(schema.connection());
    const auto recovered = PersistentPaperOrderSession::load(restarted, "legacy-open");
    REQUIRE(recovered->account().state() == session->account().state());
    REQUIRE(recovered->executions() == session->executions());

    const auto aapl_history = std::get<PaperJournalPage<PaperOrderRecord>>(
        restarted.list_orders("legacy-open", PaperJournalPageRequest{
            .limit = 10,
            .before_record_id = {},
            .instrument = kAapl,
        })
    );
    REQUIRE((client_order_ids(aapl_history) ==
        std::vector<std::string>{"sell-rest", "sell-1", "buy-1"}));
    const auto spy_history = std::get<PaperJournalPage<PaperOrderRecord>>(
        restarted.list_orders("legacy-open", PaperJournalPageRequest{
            .limit = 10,
            .before_record_id = {},
            .instrument = kSpy,
        })
    );
    REQUIRE((client_order_ids(spy_history) ==
        std::vector<std::string>{"buy-spy", "wrong-symbol"}));

    // Fills booked before the upgrade read back with their instrument and
    // exact values, beside the new ones.
    const auto aapl_fills = std::get<PaperJournalPage<chronos::PaperFillRecord>>(
        restarted.list_fills("legacy-open", PaperJournalPageRequest{
            .limit = 10,
            .before_record_id = {},
            .instrument = kAapl,
        })
    );
    REQUIRE(aapl_fills.records.size() == 3);
    REQUIRE(aapl_fills.records[2].fill.fill_id == "buy-1:fill:0");
    REQUIRE(aapl_fills.records[2].fill.instrument == kAapl);
    REQUIRE(aapl_fills.records[2].fill.price == price("101"));
    REQUIRE(aapl_fills.records[2].fill.quantity == quantity("3"));
    REQUIRE(aapl_fills.records[2].fill.timestamp == Timestamp::from_nanoseconds(110));
    REQUIRE(aapl_fills.records[1].fill.fill_id == "sell-1:fill:0");
    const auto closed_fills = std::get<PaperJournalPage<chronos::PaperFillRecord>>(
        restarted.list_fills("legacy-closed", PaperJournalPageRequest{
            .limit = 10,
            .before_record_id = {},
            .instrument = {},
        })
    );
    REQUIRE(closed_fills.records.size() == 2);
    REQUIRE(closed_fills.records[0].fill.instrument == kSpy);
    REQUIRE(closed_fills.records[0].fill.side == Side::Sell);
}

void test_the_migration_applies_once() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    apply_single_instrument_schema(store);
    PostgresTestConnection inspector(schema.connection());
    inspector.run_script(kSingleInstrumentRows);
    apply(store, "003_multi_instrument_accounts.sql");
    apply(store, "004_working_orders.sql");
    apply(store, "005_model_deployments.sql");
    apply(store, "006_live_stream_sequences.sql");
    apply(store, "007_live_stream_writer_fence.sql");
    const auto everything = [&] {
        return inspector.scalar(kJournalChecksum) + "/" + inspector.scalar(
            "SELECT md5(string_agg(row_text, ',' ORDER BY row_text)) FROM ("
            "  SELECT paper_accounts::text AS row_text FROM paper_accounts"
            "  UNION ALL SELECT paper_positions::text FROM paper_positions"
            "  UNION ALL SELECT paper_execution_states::text"
            "    FROM paper_execution_states"
            "  UNION ALL SELECT paper_orders::text FROM paper_orders"
            "  UNION ALL SELECT paper_fills::text FROM paper_fills"
            "  UNION ALL SELECT paper_events::text FROM paper_events"
            "  UNION ALL SELECT model_deployments::text FROM model_deployments"
            "  UNION ALL SELECT model_deployment_decisions::text"
            "    FROM model_deployment_decisions"
            "  UNION ALL SELECT live_stream_sequences::text"
            "    FROM live_stream_sequences"
            "  UNION ALL SELECT chronos_schema_migrations::text"
            "    FROM chronos_schema_migrations) AS all_rows"
        ) + "/" + inspector.scalar(
            "SELECT nextval(pg_get_serial_sequence('paper_fills', "
            "'fill_record_id'))"
        );
    };
    const std::string migrated = everything();

    // As the integration tests and a Compose volume apply them: every file.
    store.apply_migration(all_migrations_sql());
    apply(store, "003_multi_instrument_accounts.sql");
    apply(store, "004_working_orders.sql");
    apply(store, "005_model_deployments.sql");
    apply(store, "006_live_stream_sequences.sql");
    apply(store, "007_live_stream_writer_fence.sql");
    // Reading the identity advanced it once.
    const std::string rerun = everything();
    REQUIRE(rerun.substr(0, rerun.rfind('/')) ==
        migrated.substr(0, migrated.rfind('/')));
    REQUIRE(std::stoll(rerun.substr(rerun.rfind('/') + 1)) ==
        std::stoll(migrated.substr(migrated.rfind('/') + 1)) + 1);
}

// Two servers starting together each apply every migration.
//
// Migration 003, which cannot change, locks paper_order_records before
// paper_events, while a rerun of 001 takes its index locks in the other
// order, so the two can deadlock. PostgreSQL then aborts one transaction and
// its client runs the files again, as each applier here does; the other
// commits. Migration 004 cannot take part in such a cycle (see "migration
// 004 cannot deadlock with a rerun of 001").
void test_concurrent_upgrades_apply_the_migration_once() {
    const ScratchSchema schema;
    {
        PostgresPaperOrderRepository store(schema.connection());
        apply_single_instrument_schema(store);
        PostgresTestConnection inspector(schema.connection());
        inspector.run_script(kSingleInstrumentRows);
    }
    std::vector<std::string> outcomes(2);
    std::vector<std::thread> appliers;
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        appliers.emplace_back([&, index] {
            for (int attempt = 1;; ++attempt) {
                try {
                    PostgresPaperOrderRepository store(schema.connection());
                    store.apply_migration(all_migrations_sql());
                    outcomes[index] = "applied";
                    return;
                } catch (const std::exception& error) {
                    const bool deadlock_victim =
                        std::string_view(error.what()).find("deadlock detected") !=
                        std::string_view::npos;
                    if (!deadlock_victim || attempt == 3) {
                        outcomes[index] = error.what();
                        return;
                    }
                }
            }
        });
    }
    for (auto& applier : appliers) {
        applier.join();
    }
    for (const std::string& outcome : outcomes) {
        if (outcome != "applied") {
            throw chronos::test::TestFailure("a concurrent upgrade failed: " + outcome);
        }
    }
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM chronos_schema_migrations WHERE version = 3"
    ) == "1");
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM chronos_schema_migrations WHERE version = 4"
    ) == "1");
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_positions") == "2");
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_orders") == "5");
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_fills") == "4");
}

// Nothing is changed unless everything can be: the old schema, rows, and
// version history survive a migration that fails.
void require_single_instrument_schema_unchanged(
    PostgresTestConnection& inspector,
    const std::string& accounts_before
) {
    REQUIRE(inspector.scalar(
        "SELECT string_agg(version::text, ',' ORDER BY version) "
        "FROM chronos_schema_migrations"
    ) == "1,2");
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM information_schema.columns "
        "WHERE table_schema = current_schema() "
        "  AND table_name = 'paper_accounts' "
        "  AND column_name IN ('instrument', 'position_raw', "
        "                      'active_stream_id', 'market_stream_id')"
    ) == "3");
    REQUIRE(inspector.scalar(
        "SELECT (to_regclass('paper_positions') IS NULL AND "
        "        to_regclass('paper_execution_states') IS NULL AND "
        "        to_regprocedure('paper_account_reconciles()') IS NULL)::text"
    ) == "true");
    REQUIRE(inspector.scalar(
        "SELECT string_agg(paper_accounts::text, ',' ORDER BY account_id) "
        "FROM paper_accounts"
    ) == accounts_before);
}

void test_an_account_that_cannot_migrate_rolls_everything_back() {
    struct Case {
        std::string_view description;
        std::string_view rows;
        std::string_view error;
    };
    const std::vector<Case> cases{
        {
            "fills without an execution state",
            "INSERT INTO paper_accounts (account_id, instrument, "
            "starting_cash_raw, cash_raw, position_raw, open_cost_basis_raw, "
            "gross_realized_pnl_raw, fees_paid_raw, last_fill_timestamp_ns, "
            "maximum_order_quantity_raw, maximum_order_notional_raw, "
            "maximum_position_quantity_raw, maximum_quote_age_ns, "
            "fee_per_fill_raw, is_healthy) VALUES ('unexecuted', 'AAPL', "
            "100000000000, 69699000000, 300000000, 30300000000, 0, 1000000, "
            "110, 1, 1, 1, 0, 0, true)",
            "have no execution state",
        },
        {
            "position state without a fill",
            "INSERT INTO paper_accounts (account_id, instrument, "
            "starting_cash_raw, cash_raw, position_raw, open_cost_basis_raw, "
            "gross_realized_pnl_raw, fees_paid_raw, last_fill_timestamp_ns, "
            "maximum_order_quantity_raw, maximum_order_notional_raw, "
            "maximum_position_quantity_raw, maximum_quote_age_ns, "
            "fee_per_fill_raw, is_healthy) VALUES ('unfilled', 'AAPL', "
            "100000000000, 100100000000, 0, 0, 100000000, 0, NULL, 1, 1, 1, "
            "0, 0, true)",
            "without a fill carry position state",
        },
        {
            // The old CHECK let a NULL through; the copy fails after the
            // migration has already added columns and tables.
            "an incomplete execution state",
            "INSERT INTO paper_accounts (account_id, instrument, "
            "starting_cash_raw, cash_raw, position_raw, open_cost_basis_raw, "
            "gross_realized_pnl_raw, fees_paid_raw, last_fill_timestamp_ns, "
            "maximum_order_quantity_raw, maximum_order_notional_raw, "
            "maximum_position_quantity_raw, maximum_quote_age_ns, "
            "fee_per_fill_raw, is_healthy, active_stream_id, "
            "active_event_timestamp_ns, active_receive_timestamp_ns, "
            "active_ingest_sequence, active_quality_flags) VALUES "
            "('incomplete', 'AAPL', 100000000000, 100000000000, 0, 0, 0, 0, "
            "NULL, 1, 1, 1, 0, 0, true, 'legacy-stream', NULL, 100, 1, 0)",
            "market_event_timestamp_ns",
        },
    };
    for (const Case& failing : cases) {
        const ScratchSchema schema;
        PostgresPaperOrderRepository store(schema.connection());
        apply_single_instrument_schema(store);
        PostgresTestConnection inspector(schema.connection());
        inspector.run_script(kSingleInstrumentRows);
        static_cast<void>(inspector.query(failing.rows));
        const std::string accounts_before = inspector.scalar(
            "SELECT string_agg(paper_accounts::text, ',' ORDER BY account_id) "
            "FROM paper_accounts"
        );
        const std::string journal_before = inspector.scalar(kJournalChecksum);

        const std::string error = required_migration_error(
            store,
            "003_multi_instrument_accounts.sql"
        );
        if (!contains(error, failing.error)) {
            throw chronos::test::TestFailure(
                std::string(failing.description) + ": unexpected error: " +
                error
            );
        }
        require_single_instrument_schema_unchanged(inspector, accounts_before);
        REQUIRE(inspector.scalar(kJournalChecksum) == journal_before);
        // The connection is still usable, and the migration applies once
        // the data is repaired.
        static_cast<void>(inspector.query(
            "DELETE FROM paper_accounts WHERE account_id NOT LIKE 'legacy-%'"
        ));
        apply(store, "003_multi_instrument_accounts.sql");
        apply(store, "004_working_orders.sql");
        REQUIRE(PersistentPaperOrderSession::load(store, "legacy-open").has_value());
    }
}

// ---------------------------------------------------------------------------
// Migration 004: working orders

// The legacy accounts as the multi-instrument release left them.
void apply_multi_instrument_rows(
    PostgresPaperOrderRepository& store,
    PostgresTestConnection& inspector
) {
    apply_single_instrument_schema(store);
    inspector.run_script(kSingleInstrumentRows);
    apply(store, "003_multi_instrument_accounts.sql");
}

// Every version 3 row, which migration 004 must not change.
constexpr std::string_view kVersion3Checksum = R"SQL(
SELECT md5(string_agg(row_text, ',' ORDER BY row_text)) FROM (
    SELECT paper_accounts::text AS row_text FROM paper_accounts
    UNION ALL SELECT paper_positions::text FROM paper_positions
    UNION ALL SELECT paper_execution_states::text FROM paper_execution_states
    UNION ALL SELECT paper_order_records::text FROM paper_order_records
    UNION ALL SELECT concat_ws('|', event_sequence, account_id,
        order_record_id, client_order_id, event_order, event_type, created_at)
        FROM paper_events
) AS all_rows
)SQL";

// Whether a PostgreSQL backend of the schema's database waits for a lock on
// `table`, polled for up to ten seconds.
bool waits_for_lock_on(PostgresTestConnection& inspector, std::string_view table) {
    for (int poll = 0; poll < 1'000; ++poll) {
        if (inspector.scalar(
                "SELECT count(*) FROM pg_locks WHERE NOT granted "
                "AND locktype = 'relation' AND relation = $1::regclass",
                {std::string(table)}
            ) != "0") {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Rerunning migration 001, as every test and a fresh Compose volume do,
// takes index locks in SHARE mode on paper_events and then on
// paper_order_records. Migration 004 locks paper_accounts first and those
// two in the same order, so it cannot complete a cycle with such a rerun:
// waiting for paper_events, it holds no lock the rerun needs next.
void test_migration_004_cannot_deadlock_with_a_rerun_of_001() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_multi_instrument_rows(store, inspector);

    PostgresTestConnection rerun(schema.connection());
    rerun.run_script("BEGIN; LOCK TABLE paper_events IN SHARE MODE");
    std::string upgrade = "running";
    std::thread upgrader([&schema, &upgrade] {
        try {
            PostgresPaperOrderRepository upgrading(schema.connection());
            apply(upgrading, "004_working_orders.sql");
            upgrade = "applied";
        } catch (const std::exception& error) {
            upgrade = error.what();
        }
    });
    const bool upgrade_waited = waits_for_lock_on(inspector, "paper_events");
    // The rerun's next index lock: granted at once unless the upgrade
    // already holds paper_order_records.
    std::string next_lock = "granted";
    try {
        rerun.run_script("LOCK TABLE paper_order_records IN SHARE MODE NOWAIT");
    } catch (const std::exception& error) {
        next_lock = error.what();
    }
    rerun.run_script("COMMIT");
    upgrader.join();
    REQUIRE(upgrade_waited);
    REQUIRE(next_lock == "granted");
    REQUIRE(upgrade == "applied");
}

void test_version_3_orders_become_order_state_without_loss() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_multi_instrument_rows(store, inspector);
    const std::string version_3 = inspector.scalar(kVersion3Checksum);
    const std::string journal_before = inspector.scalar(kJournalChecksum);

    apply(store, "004_working_orders.sql");

    REQUIRE(inspector.scalar(
        "SELECT string_agg(version::text, ',' ORDER BY version) "
        "FROM chronos_schema_migrations"
    ) == "1,2,3,4");
    REQUIRE(inspector.scalar(kVersion3Checksum) == version_3);
    REQUIRE(inspector.scalar(kJournalChecksum) == journal_before);
    // Every executed order has its state; the rejection has none.
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws(':', order_record_id, client_order_id, "
        "status, time_in_force, filled_quantity_raw, remaining_quantity_raw, "
        "filled_notional_raw, fill_count, order_fee_raw, cumulative_fee_raw, "
        "fee_charged, coalesce(cancellation_reason, '-'), reserved_cash_raw, "
        "reserved_quantity_raw, accepted_market_ingest_sequence, "
        "submitted_at_ns, updated_at_ns), ',' ORDER BY order_record_id) "
        "FROM paper_orders"
    ) == "1:buy-1:filled:ioc:300000000:0:30300000000:1:1000000:1000000:t:-:"
         "0:0:10:110:110,"
         "2:sell-1:filled:ioc:100000000:0:10200000000:1:1000000:1000000:t:-:"
         "0:0:11:120:120,"
         "4:buy-2:filled:ioc:200000000:0:20200000000:1:1000000:1000000:t:-:"
         "0:0:20:210:210,"
         "5:sell-2:filled:ioc:200000000:0:19800000000:1:1000000:1000000:t:-:"
         "0:0:21:220:220,"
         "6:passive:cancelled:ioc:0:100000000:0:0:1000000:0:f:"
         "limit_not_marketable:0:0:30:310:310");
    // Each booked fill keeps its ID and gets its journal record's identity.
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws('|', fill_record_id, order_record_id, "
        "fill_id, fill_index, side, price_raw, quantity_raw, fee_raw, "
        "notional_raw, filled_at_ns, market_ingest_sequence), ',' "
        "ORDER BY fill_record_id) FROM paper_fills"
    ) == "1|1|buy-1:fill:0|0|buy|101000000000|300000000|1000000|30300000000|110|10,"
         "2|2|sell-1:fill:0|0|sell|102000000000|100000000|1000000|10200000000|120|11,"
         "4|4|buy-2:fill:0|0|buy|101000000000|200000000|1000000|20200000000|210|20,"
         "5|5|sell-2:fill:0|0|sell|99000000000|200000000|1000000|19800000000|220|21");
    // Later fills are numbered after them.
    REQUIRE(inspector.scalar(
        "SELECT nextval(pg_get_serial_sequence('paper_fills', "
        "'fill_record_id'))"
    ) == "6");
    // Events are dated at their submission, and a fill event names its fill.
    REQUIRE(inspector.scalar(
        "SELECT string_agg(concat_ws(':', client_order_id, event_order, "
        "event_type, occurred_at_ns, coalesce(fill_id, '-')), ',' "
        "ORDER BY event_sequence) FROM paper_events "
        "WHERE client_order_id IN ('buy-1', 'wrong-symbol', 'passive')"
    ) == "buy-1:1:order_submitted:110:-,buy-1:2:order_completed:110:-,"
         "buy-1:3:fill_applied:110:buy-1:fill:0,"
         "buy-1:4:account_state_updated:110:-,"
         "wrong-symbol:1:order_submitted:121:-,"
         "wrong-symbol:2:risk_rejected:121:-,"
         "passive:1:order_submitted:310:-,passive:2:order_completed:310:-,"
         "passive:3:account_state_updated:310:-");

    // The read model returns the same history, now with current state.
    const auto buy = std::get<PaperOrderRecord>(store.find_order("legacy-open", "buy-1"));
    REQUIRE(buy.state.has_value());
    REQUIRE(buy.state->status == chronos::PaperOrderStatus::Filled);
    REQUIRE(buy.state->filled_quantity == quantity("3"));
    REQUIRE(buy.state->remaining_quantity == Quantity{});
    REQUIRE(buy.state->filled_notional == money("303"));
    REQUIRE(buy.state->cumulative_fee == money("0.01"));
    REQUIRE(buy.state->reservation == chronos::OrderReservation{});
    const auto rejected =
        std::get<PaperOrderRecord>(store.find_order("legacy-open", "wrong-symbol"));
    REQUIRE(!rejected.state.has_value());
    const auto passive =
        std::get<PaperOrderRecord>(store.find_order("legacy-cancelled", "passive"));
    REQUIRE(passive.state->status == chronos::PaperOrderStatus::Cancelled);
    REQUIRE(passive.state->cancellation_reason ==
        chronos::SimulatedCancellationReason::LimitNotMarketable);
    const auto events = std::get<chronos::PaperForwardPage<chronos::PaperOrderEventRecord>>(
        store.list_order_events("legacy-open", "buy-1", chronos::PaperForwardPageRequest{
            .limit = 10,
            .after = {},
            .instrument = {},
        })
    );
    REQUIRE(events.records.size() == 4);
    REQUIRE(events.records[2].type == chronos::PaperOrderEventType::FillApplied);
    REQUIRE(events.records[2].fill->fill_id == "buy-1:fill:0");
    REQUIRE(events.records[2].fill->price == price("101"));

    // An upgraded account can rest a good-til-cancelled order.
    auto session = PersistentPaperOrderSession::load(store, "legacy-open");
    REQUIRE(session.has_value());
    OrderIntent resting = market_order("legacy-open", kAapl, "rest-1", Side::Buy, "1", 130);
    resting.type = OrderType::Limit;
    resting.limit_price = price("100");
    resting.time_in_force = TimeInForce::GoodTilCancelled;
    const auto accepted = session->submit(resting, quote(kAapl, 50, 125, "99", "101"));
    REQUIRE(std::holds_alternative<PersistentPaperOrderReceipt>(accepted));
    REQUIRE(session->working_orders().size() == 1);
    const auto recovered = PersistentPaperOrderSession::load(store, "legacy-open");
    REQUIRE(recovered->working_orders() == session->working_orders());
    REQUIRE(recovered->reservations().reserved_cash == money("100.01"));
}

// Nothing is changed unless everything can be: version 3 survives a
// migration 004 that refuses its data.
void require_version_3_unchanged(
    PostgresTestConnection& inspector,
    const std::string& version_3,
    const std::string& journal
) {
    REQUIRE(inspector.scalar(
        "SELECT string_agg(version::text, ',' ORDER BY version) "
        "FROM chronos_schema_migrations"
    ) == "1,2,3");
    REQUIRE(inspector.scalar(
        "SELECT (to_regclass('paper_orders') IS NULL AND "
        "        to_regclass('paper_fills') IS NULL AND "
        "        to_regprocedure('paper_order_fills_reconcile()') IS NULL)::text"
    ) == "true");
    REQUIRE(inspector.scalar(
        "SELECT string_agg(column_name || ' ' || data_type, ',' "
        "ORDER BY ordinal_position) FROM information_schema.columns "
        "WHERE table_schema = current_schema() AND table_name = 'paper_events'"
    ) == "event_sequence bigint,account_id text,order_record_id bigint,"
         "client_order_id text,event_order smallint,event_type text,"
         "created_at timestamp with time zone");
    REQUIRE(!contains(
        inspector.scalar(
            "SELECT pg_get_constraintdef(oid) FROM pg_constraint "
            "WHERE conname = 'paper_order_records_execution_status_check' "
            "  AND connamespace = current_schema()::regnamespace"
        ),
        "working"
    ));
    // The reconcile function is still version 3's.
    REQUIRE(!contains(
        inspector.scalar(
            "SELECT prosrc FROM pg_proc WHERE oid = "
            "'paper_account_reconciles()'::regprocedure"
        ),
        "reserves"
    ));
    REQUIRE(inspector.scalar(kVersion3Checksum) == version_3);
    REQUIRE(inspector.scalar(kJournalChecksum) == journal);
}

void test_corrupt_version_3_rows_roll_migration_004_back() {
    struct Case {
        std::string_view description;
        std::string_view corrupt;
        std::string_view repair;
        std::string_view error;
    };
    const std::vector<Case> cases{
        {
            "an undeclared side on an executed order",
            "UPDATE paper_order_records SET side = 'unknown:1' "
            "WHERE client_order_id = 'sell-1'",
            "UPDATE paper_order_records SET side = 'sell' "
            "WHERE client_order_id = 'sell-1'",
            "are not declared immediate-or-cancel orders",
        },
        {
            "an executed order that claims to be good-til-cancelled",
            "UPDATE paper_order_records SET time_in_force = 'gtc' "
            "WHERE client_order_id = 'passive'",
            "UPDATE paper_order_records SET time_in_force = 'ioc' "
            "WHERE client_order_id = 'passive'",
            "are not declared immediate-or-cancel orders",
        },
        {
            "quantities that do not add up",
            "UPDATE paper_order_records SET unfilled_quantity_raw = 1 "
            "WHERE client_order_id = 'buy-1'",
            "UPDATE paper_order_records SET unfilled_quantity_raw = 0 "
            "WHERE client_order_id = 'buy-1'",
            "have an inconsistent outcome, quantity, or fill",
        },
        {
            "a receipt that carries a rejection code",
            "UPDATE paper_order_records SET outcome_code = 'insufficient_cash' "
            "WHERE client_order_id = 'buy-2'",
            "UPDATE paper_order_records SET outcome_code = NULL "
            "WHERE client_order_id = 'buy-2'",
            "have an inconsistent outcome, quantity, or fill",
        },
        {
            "an execution on another observation",
            "UPDATE paper_order_records SET execution_market_ingest_sequence "
            "= 11 WHERE client_order_id = 'buy-1'",
            "UPDATE paper_order_records SET execution_market_ingest_sequence "
            "= 10 WHERE client_order_id = 'buy-1'",
            "have an inconsistent outcome, quantity, or fill",
        },
        {
            "a fill event for an order that booked none",
            "INSERT INTO paper_events (account_id, order_record_id, "
            "client_order_id, event_order, event_type) SELECT account_id, "
            "order_record_id, client_order_id, 9, 'fill_applied' "
            "FROM paper_order_records WHERE client_order_id = 'passive'",
            "DELETE FROM paper_events WHERE event_order = 9",
            "fill event(s) have no booked fill",
        },
    };
    for (const Case& failing : cases) {
        const ScratchSchema schema;
        PostgresPaperOrderRepository store(schema.connection());
        PostgresTestConnection inspector(schema.connection());
        apply_multi_instrument_rows(store, inspector);
        static_cast<void>(inspector.query(failing.corrupt));
        const std::string version_3 = inspector.scalar(kVersion3Checksum);
        const std::string journal = inspector.scalar(kJournalChecksum);

        const std::string error =
            required_migration_error(store, "004_working_orders.sql");
        if (!contains(error, failing.error)) {
            throw chronos::test::TestFailure(
                std::string(failing.description) + ": unexpected error: " +
                error
            );
        }
        require_version_3_unchanged(inspector, version_3, journal);
        // The connection is still usable, and the migration applies once
        // the data is repaired.
        static_cast<void>(inspector.query(failing.repair));
        apply(store, "004_working_orders.sql");
        REQUIRE(inspector.scalar("SELECT count(*) FROM paper_orders") == "5");
        REQUIRE(PersistentPaperOrderSession::load(store, "legacy-open").has_value());
    }
}

// Version 4 refuses rows that could not have been written together, even
// when each value alone is valid.
void test_order_state_constraints_hold() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_multi_instrument_rows(store, inspector);
    apply(store, "004_working_orders.sql");
    const auto rejects = [&](std::string_view statement, std::string_view constraint) {
        try {
            inspector.run_script(
                "BEGIN; " + std::string(statement) + "; COMMIT;"
            );
        } catch (const std::exception& error) {
            static_cast<void>(inspector.query("ROLLBACK"));
            if (!contains(error.what(), constraint)) {
                throw chronos::test::TestFailure(
                    std::string(statement) + ": unexpected error: " + error.what()
                );
            }
            return;
        }
        throw chronos::test::TestFailure(std::string(statement) + ": accepted");
    };
    // An immediate-or-cancel order cannot become working.
    rejects(
        "UPDATE paper_orders SET status = 'working', filled_quantity_raw = 0, "
        "remaining_quantity_raw = 300000000, filled_notional_raw = 0, "
        "fill_count = 0 WHERE client_order_id = 'buy-1'",
        "paper_orders_status_is_consistent"
    );
    // Quantities must add up.
    rejects(
        "UPDATE paper_orders SET remaining_quantity_raw = 1 "
        "WHERE client_order_id = 'buy-1'",
        "paper_orders_quantities_add_up"
    );
    // An order and its fills must agree, checked at commit.
    rejects(
        "UPDATE paper_orders SET filled_notional_raw = 30300000001 "
        "WHERE client_order_id = 'buy-1'",
        "disagrees with its fills"
    );
    // Only the first fill pays the fee, and a fill's notional is exact.
    rejects(
        "UPDATE paper_fills SET fill_index = 1, fill_id = 'buy-1:fill:1' "
        "WHERE fill_id = 'buy-1:fill:0'",
        "paper_fills_only_first_fill_pays_fee"
    );
    rejects(
        "UPDATE paper_fills SET notional_raw = notional_raw + 1 "
        "WHERE fill_id = 'buy-1:fill:0'",
        "paper_fills_notional_is_exact"
    );
    // A fill cannot name another order's side or instrument.
    rejects(
        "UPDATE paper_fills SET side = 'sell' WHERE fill_id = 'buy-1:fill:0'",
        "paper_fills_match_their_order"
    );
    // Reserved cash must be held: a working buy for more than the account
    // has fails at commit.
    rejects(
        "UPDATE paper_orders SET time_in_force = 'gtc', order_type = 'limit', "
        "limit_price_raw = 1000000000000000, status = 'working', "
        "cancellation_reason = NULL "
        "WHERE client_order_id = 'passive'",
        "reserves more cash than it holds"
    );
    // A user cancellation records its request, and only for a GTC order.
    rejects(
        "UPDATE paper_orders SET cancellation_reason = 'user_requested', "
        "cancel_request_id = 'cancel-1' WHERE client_order_id = 'passive'",
        "paper_orders_cancellation_matches_time_in_force"
    );
}


// ---------------------------------------------------------------------------
// Migration 005: model deployments

// Every version 4 row, which migration 005 must not change.
constexpr std::string_view kVersion4Checksum = R"SQL(
SELECT md5(string_agg(row_text, ',' ORDER BY row_text)) FROM (
    SELECT paper_accounts::text AS row_text FROM paper_accounts
    UNION ALL SELECT paper_positions::text FROM paper_positions
    UNION ALL SELECT paper_execution_states::text FROM paper_execution_states
    UNION ALL SELECT paper_order_records::text FROM paper_order_records
    UNION ALL SELECT paper_orders::text FROM paper_orders
    UNION ALL SELECT paper_fills::text FROM paper_fills
    UNION ALL SELECT paper_events::text FROM paper_events
) AS all_rows
)SQL";

constexpr std::string_view kMigrationVersions =
    "SELECT string_agg(version::text, ',' ORDER BY version) "
    "FROM chronos_schema_migrations";

// The legacy accounts as the working-order release left them.
void apply_working_order_rows(
    PostgresPaperOrderRepository& store,
    PostgresTestConnection& inspector
) {
    apply_multi_instrument_rows(store, inspector);
    apply(store, "004_working_orders.sql");
}

void test_migration_005_leaves_every_existing_account_manual() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);
    const std::string before = inspector.scalar(kVersion4Checksum);

    apply(store, "005_model_deployments.sql");

    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5");
    REQUIRE(inspector.scalar(kVersion4Checksum) == before);
    REQUIRE(inspector.scalar("SELECT count(*) FROM model_deployments") == "0");
    // No account is owned, so the ownership guard lets manual orders in.
    auto session = PersistentPaperOrderSession::load(store, "legacy-open");
    REQUIRE(session.has_value());
    const std::int64_t version = session->version();
    require_filled(session->submit(
        market_order("legacy-open", kAapl, "after-005", Side::Buy, "1", 131),
        quote(kAapl, 41, 126)
    ));
    REQUIRE(session->version() == version + 1);
}

void test_migration_005_requires_migration_004() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_multi_instrument_rows(store, inspector);

    REQUIRE(contains(
        required_migration_error(store, "005_model_deployments.sql"),
        "migration 005 requires migration 004"
    ));
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3");
    REQUIRE(inspector.scalar(
        "SELECT to_regclass('model_deployments') IS NULL"
    ) == "t");
}

void test_a_failed_migration_005_leaves_nothing_behind() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);
    const std::string before = inspector.scalar(kVersion4Checksum);
    // A table in the way of the second CREATE TABLE fails the migration
    // after it has created the first.
    inspector.run_script("CREATE TABLE model_deployment_decisions (squatter int)");

    REQUIRE(contains(
        required_migration_error(store, "005_model_deployments.sql"),
        "already exists"
    ));
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4");
    REQUIRE(inspector.scalar(
        "SELECT to_regclass('model_deployments') IS NULL"
    ) == "t");
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM pg_proc "
        "WHERE proname = 'paper_order_respects_deployment_ownership' "
        "  AND pronamespace = current_schema()::regnamespace"
    ) == "0");
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM pg_trigger "
        "WHERE tgname = 'paper_order_records_deployment_ownership' "
        "  AND tgrelid = 'paper_order_records'::regclass"
    ) == "0");
    REQUIRE(inspector.scalar(kVersion4Checksum) == before);

    inspector.run_script("DROP TABLE model_deployment_decisions");
    apply(store, "005_model_deployments.sql");
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5");
}

// Two servers starting together: both wait for the first lock, one
// creates the tables, and the other then finds version 5 and does nothing.
void test_concurrent_applications_of_migration_005_apply_it_once() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);

    PostgresTestConnection blocker(schema.connection());
    blocker.run_script("BEGIN; LOCK TABLE paper_accounts IN SHARE ROW EXCLUSIVE MODE");
    std::vector<std::string> outcomes(2, "running");
    std::vector<std::thread> appliers;
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        appliers.emplace_back([&schema, &outcomes, index] {
            try {
                PostgresPaperOrderRepository applying(schema.connection());
                apply(applying, "005_model_deployments.sql");
                outcomes[index] = "applied";
            } catch (const std::exception& error) {
                outcomes[index] = error.what();
            }
        });
    }
    bool both_waited = false;
    for (int poll = 0; poll < 1'000 && !both_waited; ++poll) {
        both_waited = inspector.scalar(
            "SELECT count(*) FROM pg_locks WHERE NOT granted "
            "AND locktype = 'relation' AND relation = 'paper_accounts'::regclass"
        ) == "2";
        if (!both_waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    blocker.run_script("COMMIT");
    for (auto& applier : appliers) {
        applier.join();
    }
    REQUIRE(both_waited);
    REQUIRE(outcomes == std::vector<std::string>(2, "applied"));
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5");
}

// `text` with its one occurrence of `from` replaced by `to`.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    if (at == std::string::npos || text.find(from, at + 1) != std::string::npos) {
        throw chronos::test::TestFailure("expected one occurrence of " + std::string(from));
    }
    return text.replace(at, from.size(), to);
}

// A deployment of `account`, as the repository creates one.
std::string deployment_insert(std::string_view deployment, std::string_view account) {
    return "INSERT INTO model_deployments (deployment_id, account_id, "
        "request_sha256, experiment_id, parameters_sha256, model_name, "
        "instrument, model_feature_set, policy, threshold_ppb, "
        "target_quantity_raw, decision_interval_ns, latency_ns, "
        "max_signal_age_ns, max_orders_per_run, max_consecutive_rejections, "
        "training_cutoff_ns, stream_id, state, state_reason) VALUES ('" +
        std::string(deployment) + "', '" + std::string(account) +
        "', repeat('a', 64), repeat('b', 64), repeat('c', 64), 'logistic', "
        "'AAPL', 'top_of_book_baseline/1', 'long_flat_threshold/1', "
        "550000000, 100000000, 1000, 10, 1000, 10, 3, 0, 'legacy-stream', "
        "'stopped', 'created')";
}

// Decision `sequence` of dep-1: a pending buy of one share unless changed.
std::string decision_insert(
    std::string_view sequence,
    std::string_view state = "pending",
    std::string_view client_order_id = "'dep-1-000000000001'"
) {
    const bool held = state == "held";
    return "INSERT INTO model_deployment_decisions (deployment_id, "
        "decision_sequence, run_generation, stream_id, instrument, "
        "observation_ingest_sequence, observation_event_ns, "
        "observation_receive_ns, decided_at_market_ns, inputs_raw, "
        "predicted_class, probabilities, confidence_ppb, position_before_raw, "
        "target_raw, policy_reason, client_order_id, side, quantity_raw, "
        "not_before_receive_ns, expires_at_market_ns, state) VALUES ('dep-1', " +
        std::string(sequence) + ", 1, 'legacy-stream', 'AAPL', " +
        std::string(sequence) + ", 99, 100, 100, '{1,0,0,1,1}', 'up', "
        "'{0.1,0.2,0.7}', 700000000, 0, 100000000, " +
        (held ? "'already_at_target', NULL, NULL, NULL, NULL, NULL, '"
              : "'order_scheduled', " + std::string(client_order_id) +
                  ", 'buy', 100000000, 110, 1100, '") +
        std::string(state) + "')";
}

void test_deployment_constraints_and_the_ownership_guard_hold() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    store.apply_migration(all_migrations_sql());
    for (const char* account : {"owned", "other", "manual"}) {
        REQUIRE(std::holds_alternative<PersistentPaperOrderSession>(
            PersistentPaperOrderSession::create(
                store, account, money("1000"), legacy_limits()
            )
        ));
    }
    inspector.run_script(deployment_insert("dep-1", "owned"));
    const auto rejects = [&](const std::string& statement, std::string_view expected) {
        try {
            inspector.run_script("BEGIN; " + statement + "; COMMIT;");
        } catch (const std::exception& error) {
            static_cast<void>(inspector.query("ROLLBACK"));
            if (!contains(error.what(), expected)) {
                throw chronos::test::TestFailure(
                    statement + ": unexpected error: " + error.what()
                );
            }
            return;
        }
        throw chronos::test::TestFailure(statement + ": accepted");
    };
    // An account belongs to one deployment, for ever.
    rejects(deployment_insert("dep-2", "owned"), "model_deployments_account_id_key");
    rejects(deployment_insert("bad id!", "other"), "model_deployments_deployment_id_check");
    // At most one share, a positive latency, and a signal that outlives it.
    rejects(
        "UPDATE model_deployments SET target_quantity_raw = 100000001",
        "model_deployments_target_quantity_raw_check"
    );
    rejects(
        "UPDATE model_deployments SET latency_ns = 0",
        "model_deployments_latency_ns_check"
    );
    rejects(
        "UPDATE model_deployments SET max_signal_age_ns = latency_ns",
        "model_deployments_signal_outlives_latency"
    );
    rejects("UPDATE model_deployments SET state = 'paused'", "model_deployments_state_check");
    rejects(
        "UPDATE model_deployments SET watermark_receive_ns = 1",
        "model_deployments_watermark_is_complete"
    );
    // A held decision intends no order; a pending one intends exactly one.
    rejects(replaced(
        decision_insert("1", "held"),
        "'already_at_target', NULL",
        "'already_at_target', 'dep-1-x'"
    ), "model_deployment_decisions_held_intends_no_order");
    rejects(
        replaced(decision_insert("1"), "'order_scheduled'", "'already_at_target'"),
        "model_deployment_decisions_reason_matches_state"
    );
    // Recorded means its order's execution is recorded with it, and only a
    // resolved decision has a resolution.
    rejects(
        replaced(
            replaced(decision_insert("1", "recorded"), "'recorded')", "'recorded', 'filled', 100)"),
            ", state)",
            ", state, resolution, resolved_at_market_ns)"
        ),
        "model_deployment_decisions_execution_matches_state"
    );
    rejects(
        replaced(
            replaced(decision_insert("1"), "'pending')", "'pending', 'filled')"),
            ", state)",
            ", state, resolution)"
        ),
        "model_deployment_decisions_resolution_matches_state"
    );
    rejects(
        replaced(decision_insert("1"), "'buy', 100000000, 110", "'buy', NULL, 110"),
        "model_deployment_decisions_order_is_complete"
    );
    inspector.run_script(decision_insert("1"));
    // One pending decision per deployment, and one decision per order.
    rejects(
        decision_insert("2", "pending", "'dep-1-000000000002'"),
        "model_deployment_decisions_one_pending"
    );
    rejects(
        replaced(
            replaced(decision_insert("2", "abandoned"), "'abandoned')", "'abandoned', 'stopped', 100)"),
            ", state)",
            ", state, resolution, resolved_at_market_ns)"
        ),
        "model_deployment_decisions_client_order"
    );

    // The ownership guard, in the database itself: only the owner's model
    // order for a pending decision is journaled into its account.
    auto owned = PersistentPaperOrderSession::load(store, "owned");
    REQUIRE(owned.has_value());
    const auto refused = [&](OrderIntent intent) {
        try {
            static_cast<void>(owned->submit(intent, quote(kAapl, 7, 100)));
        } catch (const std::exception& error) {
            REQUIRE(contains(error.what(), "belongs to model deployment dep-1"));
            return;
        }
        throw chronos::test::TestFailure("an order was journaled into an owned account");
    };
    refused(market_order("owned", kAapl, "dep-1-000000000001", Side::Buy, "1", 110));
    OrderIntent model = market_order("owned", kAapl, "dep-1-000000000001", Side::Buy, "1", 110);
    model.source = OrderSource{.kind = OrderSourceKind::Model, .source_id = "dep-2"};
    refused(model);
    model.source.source_id = "dep-1";
    model.client_order_id = "dep-1-000000000009";
    refused(model);
    owned = PersistentPaperOrderSession::load(store, "owned");
    REQUIRE(owned->version() == 0);
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM paper_order_records WHERE account_id = 'owned'"
    ) == "0");
    model.client_order_id = "dep-1-000000000001";
    // Even an exact pending ID/source cannot authorize a generic writer:
    // the deferred check requires its resolution in the same commit.
    refused(model);
    owned = PersistentPaperOrderSession::load(store, "owned");
    REQUIRE(owned->version() == 0);
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM paper_order_records WHERE account_id = 'owned'"
    ) == "0");
    // Other accounts are untouched by the guard.
    auto manual = PersistentPaperOrderSession::load(store, "manual");
    require_filled(manual->submit(
        market_order("manual", kAapl, "manual-1", Side::Buy, "1", 110),
        quote(kAapl, 9, 100)
    ));
}

// Migration 006 (ADR 0020) adds one table to a database that already has
// accounts, fills, working orders, and deployments, and changes none of them.
void test_migration_006_changes_nothing_that_exists_and_reruns_as_a_no_op() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);
    apply(store, "005_model_deployments.sql");
    const std::string before = inspector.scalar(kVersion4Checksum);

    apply(store, "006_live_stream_sequences.sql");
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5,6");
    REQUIRE(inspector.scalar(kVersion4Checksum) == before);
    // No stream is adopted or started by the migration itself.
    REQUIRE(inspector.scalar("SELECT count(*) FROM live_stream_sequences") == "0");

    apply(store, "006_live_stream_sequences.sql");
    store.apply_migration(all_migrations_sql());
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5,6,7");
    REQUIRE(inspector.scalar(kVersion4Checksum) == before);
}

void test_migration_006_requires_migration_005() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);

    REQUIRE(contains(
        required_migration_error(store, "006_live_stream_sequences.sql"),
        "migration 006 requires migration 005"
    ));
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4");
    REQUIRE(inspector.scalar("SELECT to_regclass('live_stream_sequences') IS NULL") == "t");
}

// Migration 007 (ADR 0020, revised) adds writer generations to a database
// that already has live streams, one released and one held, and changes
// nothing else. The held stream's writer predates the fence: its generation
// begins at next_sequence, so nothing it numbered passes until it acquires
// again.
void test_migration_007_starts_held_streams_at_their_next_sequence_and_reruns_as_a_no_op() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);
    apply(store, "005_model_deployments.sql");
    apply(store, "006_live_stream_sequences.sql");
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) VALUES ('released', 16, 'new');"
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, writer_id, writer_label, "
        "lease_expires_at) VALUES ('held', 18446744073709551616, 'new', 'w', 'pid 1', "
        "clock_timestamp() + interval '15 seconds')"
    );
    const std::string before = inspector.scalar(kVersion4Checksum);
    const std::string streams = "SELECT string_agg(stream_id || '=' || next_sequence::text || '/' || "
        "coalesce(writer_id, '-') || '/' || coalesce(writer_first_sequence::text, '-'), ',' ORDER BY stream_id) "
        "FROM live_stream_sequences";

    apply(store, "007_live_stream_writer_fence.sql");
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5,6,7");
    REQUIRE(inspector.scalar(kVersion4Checksum) == before);
    REQUIRE(inspector.scalar(streams) == "held=18446744073709551616/w/18446744073709551616,released=16/-/-");

    apply(store, "007_live_stream_writer_fence.sql");
    store.apply_migration(all_migrations_sql());
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5,6,7");
    REQUIRE(inspector.scalar(kVersion4Checksum) == before);
    REQUIRE(inspector.scalar(streams) == "held=18446744073709551616/w/18446744073709551616,released=16/-/-");
}

void test_migration_007_requires_migration_006() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    apply_working_order_rows(store, inspector);
    apply(store, "005_model_deployments.sql");
    REQUIRE(contains(
        required_migration_error(store, "007_live_stream_writer_fence.sql"),
        "migration 007 requires migration 006"
    ));
    REQUIRE(inspector.scalar(kMigrationVersions) == "1,2,3,4,5");
}

// What a server from before migration 007 would write, refused: it takes
// over a stream without beginning a generation, acquires one without a
// generation, or releases one leaving its generation behind. Each would
// leave a deposed writer's sequences current, or a stream without a fence.
void test_a_writer_change_must_begin_a_generation_where_reservations_ended() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    store.apply_migration(all_migrations_sql());
    PostgresTestConnection inspector(schema.connection());
    const auto refused = [&](std::string_view sql) {
        try {
            inspector.run_script(sql);
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) VALUES ('s', 0, 'new');"
        "UPDATE live_stream_sequences SET next_sequence = 8, writer_id = 'a', writer_label = 'pid 1', "
        "lease_expires_at = clock_timestamp(), writer_first_sequence = 0 WHERE stream_id = 's'"
    );
    // A takeover that keeps the deposed generation.
    REQUIRE(contains(refused(
        "UPDATE live_stream_sequences SET next_sequence = 16, writer_id = 'b', writer_label = 'pid 2', "
        "lease_expires_at = clock_timestamp() + interval '15 seconds' WHERE stream_id = 's'"
    ), "must begin its generation at 8"));
    // One that begins anywhere but where reservations ended.
    REQUIRE(contains(refused(
        "UPDATE live_stream_sequences SET next_sequence = 16, writer_id = 'b', writer_label = 'pid 2', "
        "lease_expires_at = clock_timestamp() + interval '15 seconds', writer_first_sequence = 7 "
        "WHERE stream_id = 's'"
    ), "must begin its generation at 8"));
    // A release that leaves the generation.
    REQUIRE(contains(refused(
        "UPDATE live_stream_sequences SET writer_id = NULL, writer_label = NULL, lease_expires_at = NULL "
        "WHERE stream_id = 's'"
    ), "live_stream_sequences_generation_complete"));
    // Renewals and reservations by the same writer keep its generation.
    inspector.run_script(
        "UPDATE live_stream_sequences SET next_sequence = 24, lease_expires_at = clock_timestamp() "
        "WHERE stream_id = 's'"
    );
    // A proper release, then an acquisition without a generation.
    inspector.run_script(
        "UPDATE live_stream_sequences SET writer_id = NULL, writer_label = NULL, lease_expires_at = NULL, "
        "writer_first_sequence = NULL WHERE stream_id = 's'"
    );
    REQUIRE(contains(refused(
        "UPDATE live_stream_sequences SET next_sequence = 32, writer_id = 'c', writer_label = 'pid 3', "
        "lease_expires_at = clock_timestamp() + interval '15 seconds' WHERE stream_id = 's'"
    ), "must begin its generation at 24"));
    REQUIRE(inspector.scalar(
        "SELECT next_sequence::text || '/' || coalesce(writer_id, '-') || '/' || "
        "coalesce(writer_first_sequence::text, '-') FROM live_stream_sequences"
    ) == "24/-/-");
}

// Sequences are exact unsigned 64-bit values: 2^64 marks an exhausted
// stream and nothing beyond it, or below zero, is stored. A lease is whole
// or absent, and an adoption records what it followed.
void test_live_stream_sequence_constraints_hold() {
    const ScratchSchema schema;
    PostgresPaperOrderRepository store(schema.connection());
    store.apply_migration(all_migrations_sql());
    PostgresTestConnection inspector(schema.connection());
    const auto refused = [&](std::string_view sql) {
        try {
            inspector.run_script(sql);
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
        return std::string();
    };
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) "
        "VALUES ('exhausted', 18446744073709551616, 'new')"
    );
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) "
        "VALUES ('beyond', 18446744073709551617, 'new')"
    ), "live_stream_sequences_next_sequence_check"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) "
        "VALUES ('negative', -1, 'new')"
    ), "live_stream_sequences_next_sequence_check"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, writer_id, writer_first_sequence) "
        "VALUES ('half-lease', 0, 'new', 'w', 0)"
    ), "live_stream_sequences_lease_complete"));
    // Migration 007: a writer has a generation, and only a writer does; it
    // never begins beyond what was reserved.
    const std::string lease = "'w', 'pid 1', clock_timestamp() + interval '15 seconds'";
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, writer_id, writer_label, "
        "lease_expires_at) VALUES ('no-generation', 8, 'new', " + lease + ")"
    ), "live_stream_sequences_generation_complete"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, writer_first_sequence) "
        "VALUES ('generation-without-writer', 8, 'new', 0)"
    ), "live_stream_sequences_generation_complete"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, writer_id, writer_label, "
        "lease_expires_at, writer_first_sequence) VALUES ('ahead', 8, 'new', " + lease + ", 9)"
    ), "live_stream_sequences_generation_complete"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, writer_id, writer_label, "
        "lease_expires_at, writer_first_sequence) VALUES ('negative-generation', 8, 'new', " + lease + ", -1)"
    ), "live_stream_sequences_writer_first_sequence_check"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) "
        "VALUES ('unrecorded', 10, 'adopted')"
    ), "live_stream_sequences_adoption_recorded"));
    REQUIRE(contains(refused(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, adopted_maximum) "
        "VALUES ('behind', 10, 'adopted', 10)"
    ), "live_stream_sequences_adoption_recorded"));
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, adopted_maximum) "
        "VALUES ('adopted', 1088, 'adopted', 1087)"
    );
    REQUIRE(inspector.scalar(
        "SELECT string_agg(stream_id || '=' || next_sequence::text, ',' ORDER BY stream_id) "
        "FROM live_stream_sequences"
    ) == "adopted=1088,exhausted=18446744073709551616");
}

}  // namespace

int main() {
    const char* configured_connection =
        std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "PostgreSQL migration test skipped: "
                     "CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {
            "a fresh database gets the multi-instrument schema",
            test_a_fresh_database_gets_the_multi_instrument_schema,
        },
        {
            "single-instrument accounts upgrade without loss",
            test_single_instrument_accounts_upgrade_without_loss,
        },
        {
            "an upgraded account trades several instruments",
            test_an_upgraded_account_trades_several_instruments,
        },
        {"the migration applies once", test_the_migration_applies_once},
        {
            "concurrent upgrades apply the migration once",
            test_concurrent_upgrades_apply_the_migration_once,
        },
        {
            "an account that cannot migrate rolls everything back",
            test_an_account_that_cannot_migrate_rolls_everything_back,
        },
        {
            "version 3 orders become order state without loss",
            test_version_3_orders_become_order_state_without_loss,
        },
        {
            "migration 004 cannot deadlock with a rerun of 001",
            test_migration_004_cannot_deadlock_with_a_rerun_of_001,
        },
        {
            "corrupt version 3 rows roll migration 004 back",
            test_corrupt_version_3_rows_roll_migration_004_back,
        },
        {"order state constraints hold", test_order_state_constraints_hold},
        {
            "migration 005 leaves every existing account manual",
            test_migration_005_leaves_every_existing_account_manual,
        },
        {
            "migration 005 requires migration 004",
            test_migration_005_requires_migration_004,
        },
        {
            "a failed migration 005 leaves nothing behind",
            test_a_failed_migration_005_leaves_nothing_behind,
        },
        {
            "concurrent applications of migration 005 apply it once",
            test_concurrent_applications_of_migration_005_apply_it_once,
        },
        {
            "deployment constraints and the ownership guard hold",
            test_deployment_constraints_and_the_ownership_guard_hold,
        },
        {
            "migration 006 changes nothing that exists and reruns as a no-op",
            test_migration_006_changes_nothing_that_exists_and_reruns_as_a_no_op,
        },
        {"migration 006 requires migration 005", test_migration_006_requires_migration_005},
        {
            "migration 007 starts held streams at their next sequence and reruns as a no-op",
            test_migration_007_starts_held_streams_at_their_next_sequence_and_reruns_as_a_no_op,
        },
        {"migration 007 requires migration 006", test_migration_007_requires_migration_006},
        {
            "a writer change must begin a generation where reservations ended",
            test_a_writer_change_must_begin_a_generation_where_reservations_ended,
        },
        {"live stream sequence constraints hold", test_live_stream_sequence_constraints_hold},
    });
}
