#include "chronos/postgresql_paper_order_repository.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
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

// Migration 003 turns single-instrument accounts into multi-instrument ones.
// Each test builds its own database in a scratch schema of the shared test
// database, so it can start empty or from rows in the old format.

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
using chronos::test::with_search_path;

std::string connection_string;

const InstrumentId kAapl("AAPL");
const InstrumentId kSpy("SPY");

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

// A schema of its own, dropped with everything in it at scope exit.
class ScratchSchema final {
public:
    ScratchSchema()
        : name_(schema_name()),
          admin_(connection_string) {
        static_cast<void>(admin_.query("SET client_min_messages = warning"));
        static_cast<void>(admin_.query("CREATE SCHEMA " + name_));
    }

    ~ScratchSchema() {
        try {
            static_cast<void>(
                admin_.query("DROP SCHEMA " + name_ + " CASCADE")
            );
        } catch (const std::exception& error) {
            std::cerr << "could not drop " << name_ << ": " << error.what()
                      << '\n';
        }
    }

    ScratchSchema(const ScratchSchema&) = delete;
    ScratchSchema& operator=(const ScratchSchema&) = delete;

    [[nodiscard]] std::string connection() const {
        return with_search_path(connection_string, name_);
    }

private:
    static std::string schema_name() {
        std::string name = unique_test_id("chronos_migration");
        std::replace(name.begin(), name.end(), '-', '_');
        return name;
    }

    std::string name_;
    PostgresTestConnection admin_;
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
    ) == "1,2,3");
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
    const auto everything = [&] {
        return inspector.scalar(kJournalChecksum) + "/" + inspector.scalar(
            "SELECT md5(string_agg(row_text, ',' ORDER BY row_text)) FROM ("
            "  SELECT paper_accounts::text AS row_text FROM paper_accounts"
            "  UNION ALL SELECT paper_positions::text FROM paper_positions"
            "  UNION ALL SELECT paper_execution_states::text"
            "    FROM paper_execution_states"
            "  UNION ALL SELECT chronos_schema_migrations::text"
            "    FROM chronos_schema_migrations) AS all_rows"
        );
    };
    const std::string migrated = everything();

    // As the integration tests and a Compose volume apply them: every file.
    store.apply_migration(all_migrations_sql());
    apply(store, "003_multi_instrument_accounts.sql");
    REQUIRE(everything() == migrated);
}

// Two servers starting together each apply every migration.
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
            try {
                PostgresPaperOrderRepository store(schema.connection());
                store.apply_migration(all_migrations_sql());
                outcomes[index] = "applied";
            } catch (const std::exception& error) {
                outcomes[index] = error.what();
            }
        });
    }
    for (auto& applier : appliers) {
        applier.join();
    }
    REQUIRE(outcomes[0] == "applied");
    REQUIRE(outcomes[1] == "applied");
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar(
        "SELECT count(*) FROM chronos_schema_migrations WHERE version = 3"
    ) == "1");
    REQUIRE(inspector.scalar("SELECT count(*) FROM paper_positions") == "2");
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
        REQUIRE(PersistentPaperOrderSession::load(store, "legacy-open").has_value());
    }
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
    });
}
