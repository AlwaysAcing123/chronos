// Durable good-til-cancelled orders in PostgreSQL: recovery, multiple fills,
// reservations, cancellation, idempotent matching, and account-level
// serialization across connections.
#include "chronos/postgresql_paper_order_repository.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <atomic>
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

namespace {

using chronos::BookLevel;
using chronos::InstrumentId;
using chronos::Money;
using chronos::OrderIntent;
using chronos::OrderSource;
using chronos::OrderSourceKind;
using chronos::OrderType;
using chronos::PaperAccountLock;
using chronos::PaperFillRecord;
using chronos::PaperForwardPage;
using chronos::PaperForwardPageRequest;
using chronos::PaperJournalPage;
using chronos::PaperJournalPageRequest;
using chronos::PaperJournalReadMiss;
using chronos::PaperOrderEventRecord;
using chronos::PaperOrderEventType;
using chronos::PaperOrderReceipt;
using chronos::PaperOrderRecord;
using chronos::PaperOrderStatus;
using chronos::PaperPersistenceConflict;
using chronos::PaperPersistenceConflictCode;
using chronos::PaperPersistenceUnavailable;
using chronos::PersistentPaperOrderReceipt;
using chronos::PersistentPaperOrderSession;
using chronos::PostgresPaperOrderRepository;
using chronos::PreTradeRiskLimits;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::SimulatedCancellationReason;
using chronos::SimulatedExecutionStatus;
using chronos::TimeInForce;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::WorkingOrderCancellation;
using chronos::WorkingOrderMatch;
using chronos::test::all_migrations_sql;
using chronos::test::PostgresTestConnection;
using chronos::test::terminate_backends;
using chronos::test::unique_test_id;
using chronos::test::wait_until_blocked_on_lock;
using chronos::test::with_application_name;

std::string connection_string;
const InstrumentId kAapl("AAPL");

Money money(std::string_view value) { return Money::parse(value); }
Price price(std::string_view value) { return Price::parse(value); }
Quantity quantity(std::string_view value) { return Quantity::parse(value); }

PreTradeRiskLimits limits() {
    return PreTradeRiskLimits{
        .maximum_order_quantity = quantity("100"),
        .maximum_order_notional = money("100000"),
        .maximum_position_quantity = quantity("100"),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = money("0.01"),
    };
}

TopOfBookEvent market(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view bid = "99",
    std::string_view bid_size = "5",
    std::string_view ask = "101",
    std::string_view ask_size = "5"
) {
    return TopOfBookEvent{
        .header = chronos::MarketEventHeader{
            .instrument = kAapl,
            .stream = chronos::MarketDataStreamId("market-stream"),
            .venue = chronos::VenueId("XNAS"),
            .event_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp - 1),
            .receive_timestamp =
                Timestamp::from_nanoseconds(receive_timestamp),
            .ingest_sequence = sequence,
            .venue_sequence = sequence + 1'000,
            .quality = chronos::MarketDataQuality{},
        },
        .bid = BookLevel{.price = price(bid), .quantity = quantity(bid_size)},
        .ask = BookLevel{.price = price(ask), .quantity = quantity(ask_size)},
    };
}

TopOfBookEvent asks(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view ask,
    std::string_view ask_size
) {
    return market(sequence, receive_timestamp, "90", "5", ask, ask_size);
}

TopOfBookEvent bids(
    std::uint64_t sequence,
    std::int64_t receive_timestamp,
    std::string_view bid,
    std::string_view bid_size
) {
    return market(sequence, receive_timestamp, bid, bid_size, "200", "5");
}

OrderIntent order(
    const std::string& account_id,
    std::string client_order_id,
    Side side,
    std::string_view order_quantity,
    std::int64_t submitted_at,
    std::optional<std::string_view> limit = std::nullopt
) {
    return OrderIntent{
        .client_order_id = std::move(client_order_id),
        .account_id = account_id,
        .source = OrderSource{
            .kind = OrderSourceKind::Manual,
            .source_id = "integration-user",
        },
        .instrument = kAapl,
        .side = side,
        .type = limit ? OrderType::Limit : OrderType::Market,
        .quantity = quantity(order_quantity),
        .limit_price = limit ? std::optional<Price>(price(*limit)) : std::nullopt,
        .time_in_force = limit ? TimeInForce::GoodTilCancelled
                               : TimeInForce::ImmediateOrCancel,
        .submitted_at = Timestamp::from_nanoseconds(submitted_at),
    };
}

PersistentPaperOrderSession create_session(
    PostgresPaperOrderRepository& store,
    const std::string& account_id
) {
    auto created = PersistentPaperOrderSession::create(
        store,
        account_id,
        money("10000"),
        limits()
    );
    REQUIRE(std::holds_alternative<PersistentPaperOrderSession>(created));
    return std::get<PersistentPaperOrderSession>(std::move(created));
}

PaperOrderReceipt submitted(chronos::PersistentPaperOrderResult result) {
    REQUIRE(std::holds_alternative<PersistentPaperOrderReceipt>(result));
    auto receipt = std::get<PersistentPaperOrderReceipt>(std::move(result));
    REQUIRE(std::holds_alternative<PaperOrderReceipt>(receipt.result));
    return std::get<PaperOrderReceipt>(std::move(receipt.result));
}

PaperForwardPageRequest forward(
    std::size_t limit = 50,
    std::optional<std::int64_t> after = std::nullopt
) {
    return PaperForwardPageRequest{
        .limit = limit,
        .after = after,
        .instrument = std::nullopt,
    };
}

PaperOrderRecord find(
    PostgresPaperOrderRepository& store,
    const std::string& account_id,
    std::string_view client_order_id
) {
    auto lookup = store.find_order(account_id, client_order_id);
    REQUIRE(std::holds_alternative<PaperOrderRecord>(lookup));
    return std::get<PaperOrderRecord>(std::move(lookup));
}

PaperAccountLock lock(
    PostgresPaperOrderRepository& store,
    const std::string& account_id
) {
    auto locked = store.lock_account(account_id);
    REQUIRE(locked.has_value());
    return std::move(*locked);
}

// Matches one observation under the account lock, as the matcher does.
WorkingOrderMatch match_and_commit(
    PostgresPaperOrderRepository& store,
    const std::string& account_id,
    const TopOfBookEvent& observation
) {
    PaperAccountLock locked = lock(store, account_id);
    WorkingOrderMatch report = locked.processor().match(
        observation,
        observation.header.receive_timestamp
    );
    if (!report.changed_account()) {
        locked.release();
        return report;
    }
    REQUIRE(std::holds_alternative<std::int64_t>(locked.commit_match(report)));
    return report;
}

std::vector<std::string> event_types(
    const PaperForwardPage<PaperOrderEventRecord>& page
) {
    std::vector<std::string> types;
    for (const auto& event : page.records) {
        types.emplace_back(*chronos::canonical_name(event.type));
    }
    return types;
}

void test_working_orders_and_reservations_survive_restart() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-restart");
    auto session = create_session(store, account_id);
    static_cast<void>(submitted(session.submit(
        order(account_id, "bought", Side::Buy, "5", 100),
        market(1, 100)
    )));
    const auto resting_buy = submitted(session.submit(
        order(account_id, "b1", Side::Buy, "10", 200, "100"),
        market(2, 200)
    ));
    REQUIRE(resting_buy.execution.status == SimulatedExecutionStatus::Working);
    static_cast<void>(submitted(session.submit(
        order(account_id, "s1", Side::Sell, "3", 200, "105"),
        market(2, 200)
    )));
    REQUIRE(session.reservations().reserved_cash == money("1000.02"));

    PostgresPaperOrderRepository restarted(connection_string);
    const auto recovered = PersistentPaperOrderSession::load(restarted, account_id);
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->working_orders() == session.working_orders());
    REQUIRE(recovered->reservations() == session.reservations());
    REQUIRE(recovered->account().state() == session.account().state());
    REQUIRE(recovered->version() == session.version());

    const PaperOrderRecord buy = find(restarted, account_id, "b1");
    REQUIRE(buy.state->status == PaperOrderStatus::Working);
    REQUIRE(buy.state->remaining_quantity == quantity("10"));
    REQUIRE(buy.state->reservation.cash == money("1000.01"));
    REQUIRE(!buy.state->fee_charged());
    const PaperOrderRecord sell = find(restarted, account_id, "s1");
    REQUIRE(sell.state->reservation.cash == money("0.01"));
    REQUIRE(sell.state->reservation.quantity == quantity("3"));
    REQUIRE(sell.record_id > buy.record_id);

    // Working orders page in priority order.
    const auto all = std::get<PaperForwardPage<PaperOrderRecord>>(
        restarted.list_working_orders(account_id, forward())
    );
    REQUIRE(all.records.size() == 2);
    REQUIRE(all.records[0].intent.client_order_id == "b1");
    REQUIRE(all.records[1].intent.client_order_id == "s1");
    REQUIRE(!all.next_after.has_value());
    const auto first = std::get<PaperForwardPage<PaperOrderRecord>>(
        restarted.list_working_orders(account_id, forward(1))
    );
    REQUIRE(first.records.size() == 1);
    REQUIRE(first.next_after == buy.record_id);
    const auto second = std::get<PaperForwardPage<PaperOrderRecord>>(
        restarted.list_working_orders(account_id, forward(1, first.next_after))
    );
    REQUIRE(second.records[0].intent.client_order_id == "s1");
    REQUIRE(!second.next_after.has_value());
    REQUIRE(std::get<PaperJournalReadMiss>(restarted.list_working_orders(
        "missing-account", forward()
    )) == PaperJournalReadMiss::AccountNotFound);

    {
        PaperAccountLock locked = lock(restarted, account_id);
        REQUIRE(locked.processor().working_orders() == session.working_orders());
        REQUIRE(locked.version() == session.version());
        locked.release();
    }

    // The working sale fills at a later bid and releases its shares.
    const WorkingOrderMatch sold = match_and_commit(restarted, account_id, bids(3, 300, "105.5", "5"));
    REQUIRE(sold.fills.size() == 1);
    REQUIRE(sold.fills[0].accounting.fill.price == price("105.5"));
    const PaperOrderRecord filled_sale = find(restarted, account_id, "s1");
    REQUIRE(filled_sale.state->status == PaperOrderStatus::Filled);
    REQUIRE(filled_sale.state->reservation == chronos::OrderReservation{});
    const auto after_sale = PersistentPaperOrderSession::load(restarted, account_id);
    REQUIRE(after_sale->account().state().quantity(kAapl) == quantity("2"));
    REQUIRE(after_sale->reservations().reserved_cash == money("1000.01"));
    REQUIRE(after_sale->reservations().reserved_sell_quantity(kAapl) == Quantity{});
}

void test_matching_books_several_fills_with_one_fee() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-fills");
    auto session = create_session(store, account_id);
    static_cast<void>(submitted(session.submit(
        order(account_id, "b1", Side::Buy, "10", 100, "100"),
        market(1, 100)
    )));

    REQUIRE(match_and_commit(store, account_id, asks(2, 200, "99.5", "4")).fills.size() == 1);
    REQUIRE(match_and_commit(store, account_id, asks(3, 300, "100", "4")).fills.size() == 1);
    const WorkingOrderMatch last = match_and_commit(store, account_id, asks(4, 400, "99", "10"));
    REQUIRE(last.fills.size() == 1);
    REQUIRE(last.fills[0].status == PaperOrderStatus::Filled);

    const PaperOrderRecord record = find(store, account_id, "b1");
    REQUIRE(record.state->status == PaperOrderStatus::Filled);
    REQUIRE(record.state->filled_quantity == quantity("10"));
    REQUIRE(record.state->fill_count == 3);
    REQUIRE(record.state->filled_notional == money("996"));
    REQUIRE(record.state->cumulative_fee == money("0.01"));
    REQUIRE(record.state->reservation == chronos::OrderReservation{});
    REQUIRE(record.state->updated_at == Timestamp::from_nanoseconds(400));
    // The submission itself is unchanged: it rested.
    REQUIRE(std::get<chronos::SimulatedExecutionReport>(record.outcome).status ==
        SimulatedExecutionStatus::Working);

    const auto fills = std::get<PaperForwardPage<PaperFillRecord>>(
        store.list_order_fills(account_id, "b1", forward())
    );
    REQUIRE(fills.records.size() == 3);
    const std::vector<std::string> ids{"b1:fill:0", "b1:fill:1", "b1:fill:2"};
    const std::vector<Money> fees{money("0.01"), Money{}, Money{}};
    const std::vector<std::uint64_t> observations{2, 3, 4};
    for (std::size_t index = 0; index < 3; ++index) {
        REQUIRE(fills.records[index].fill.fill_id == ids[index]);
        REQUIRE(fills.records[index].fill.fee == fees[index]);
        REQUIRE(fills.records[index].fill_index == index);
        REQUIRE(fills.records[index].market_ingest_sequence == observations[index]);
    }
    const auto paged = std::get<PaperForwardPage<PaperFillRecord>>(
        store.list_order_fills(account_id, "b1", forward(2))
    );
    REQUIRE(paged.records.size() == 2);
    REQUIRE(paged.next_after == 1);
    const auto rest = std::get<PaperForwardPage<PaperFillRecord>>(
        store.list_order_fills(account_id, "b1", forward(2, paged.next_after))
    );
    REQUIRE(rest.records.size() == 1);
    REQUIRE(rest.records[0].fill.fill_id == "b1:fill:2");

    const auto events = std::get<PaperForwardPage<PaperOrderEventRecord>>(
        store.list_order_events(account_id, "b1", forward())
    );
    REQUIRE((event_types(events) == std::vector<std::string>{
        "order_submitted", "order_accepted", "account_state_updated",
        "fill_applied", "account_state_updated",
        "fill_applied", "account_state_updated",
        "fill_applied", "order_filled", "account_state_updated",
    }));
    REQUIRE(events.records[3].fill->fill_id == "b1:fill:0");
    REQUIRE(events.records[3].occurred_at == Timestamp::from_nanoseconds(200));
    REQUIRE(events.records[8].occurred_at == Timestamp::from_nanoseconds(400));
    for (std::size_t index = 0; index < events.records.size(); ++index) {
        REQUIRE(events.records[index].sequence == static_cast<std::int64_t>(index + 1));
    }

    // The account's fill history lists every fill, newest first.
    const auto history = std::get<PaperJournalPage<PaperFillRecord>>(
        store.list_fills(account_id, PaperJournalPageRequest{
            .limit = 10,
            .before_record_id = {},
            .instrument = {},
        })
    );
    REQUIRE(history.records.size() == 3);
    REQUIRE(history.records[0].fill.fill_id == "b1:fill:2");
    REQUIRE(history.records[2].fill.fill_id == "b1:fill:0");

    const auto summary = store.journal_summary(account_id);
    REQUIRE(summary.order_count == 1);
    REQUIRE(summary.applied_fill_count == 3);
    REQUIRE(summary.event_count == 10);

    const auto recovered = PersistentPaperOrderSession::load(store, account_id);
    const auto& position = *recovered->account().state().position(kAapl);
    REQUIRE(position.quantity == quantity("10"));
    REQUIRE(position.open_cost_basis == money("996"));
    REQUIRE(position.fees_paid == money("0.01"));
    REQUIRE(recovered->account().state().cash == money("9003.99"));
    REQUIRE(recovered->working_orders().empty());
}

void test_matching_twice_never_books_twice() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-idempotent");
    auto session = create_session(store, account_id);
    static_cast<void>(submitted(session.submit(
        order(account_id, "b1", Side::Buy, "10", 100, "100"),
        market(1, 100)
    )));
    const TopOfBookEvent observation = asks(2, 200, "100", "4");
    const auto contains_account = [&](const std::vector<std::string>& accounts) {
        return std::find(accounts.begin(), accounts.end(), account_id) != accounts.end();
    };
    REQUIRE(contains_account(store.accounts_to_match(observation)));
    // The acceptance observation and older ones never select the order.
    REQUIRE(!contains_account(store.accounts_to_match(market(1, 100, "99", "5", "100", "5"))));
    // A limit the observation does not reach does not either.
    REQUIRE(!contains_account(store.accounts_to_match(asks(2, 200, "100.01", "4"))));
    TopOfBookEvent other_stream = observation;
    other_stream.header.stream = chronos::MarketDataStreamId("other-stream");
    REQUIRE(!contains_account(store.accounts_to_match(other_stream)));

    REQUIRE(match_and_commit(store, account_id, observation).fills.size() == 1);
    // Still a candidate, since a remainder is marketable, but the account
    // already used this observation: an uncertain commit's retry, or a
    // second server on the same stream, books nothing.
    REQUIRE(contains_account(store.accounts_to_match(observation)));
    REQUIRE(!match_and_commit(store, account_id, observation).changed_account());
    PostgresPaperOrderRepository second_server(connection_string);
    REQUIRE(!match_and_commit(second_server, account_id, observation).changed_account());
    REQUIRE(find(store, account_id, "b1").state->fill_count == 1);
    REQUIRE(store.journal_summary(account_id).applied_fill_count == 1);

    // A halted account is not a candidate.
    PostgresTestConnection editor(connection_string);
    static_cast<void>(editor.query(
        "UPDATE paper_accounts SET is_healthy = false WHERE account_id = $1",
        {account_id}
    ));
    REQUIRE(!contains_account(store.accounts_to_match(asks(3, 300, "100", "4"))));
}

void test_a_skipped_fill_durably_keeps_fifo_priority() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-skip-priority");
    PreTradeRiskLimits risk = limits();
    risk.fee_per_fill = Money{};
    auto created = PersistentPaperOrderSession::create(
        store,
        account_id,
        money("0.00000008"),
        risk
    );
    REQUIRE(std::holds_alternative<PersistentPaperOrderSession>(created));
    auto session = std::get<PersistentPaperOrderSession>(std::move(created));
    const TopOfBookEvent acceptance = market(
        1,
        100,
        "1.1",
        "1",
        "1.3",
        "1"
    );
    static_cast<void>(submitted(session.submit(
        order(account_id, "first", Side::Buy, "0.00000006", 100, "1.2"),
        acceptance
    )));
    static_cast<void>(submitted(session.submit(
        order(account_id, "second", Side::Buy, "0.00000001", 100, "1.2"),
        acceptance
    )));

    const TopOfBookEvent observation = market(
        2,
        200,
        "1.1",
        "1",
        "1.2",
        "0.00000003"
    );
    const WorkingOrderMatch skipped = match_and_commit(
        store,
        account_id,
        observation
    );
    REQUIRE(skipped.fills.empty());
    REQUIRE(skipped.skipped.size() == 1);

    const auto recovered = PersistentPaperOrderSession::load(store, account_id);
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->working_orders().size() == 2);
    const auto& execution = recovered->executions().at(kAapl);
    REQUIRE(execution.active_market.header.ingest_sequence == 2);
    REQUIRE(execution.consumed_ask_quantity == quantity("0.00000003"));
    REQUIRE(store.journal_summary(account_id).applied_fill_count == 0);
}

void test_a_lost_connection_while_matching_books_nothing_until_retried() {
    const std::string application = unique_test_id("gtc-match-loss");
    PostgresPaperOrderRepository store(
        with_application_name(connection_string, application)
    );
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-match-loss");
    {
        PostgresPaperOrderRepository setup(connection_string);
        auto session = create_session(setup, account_id);
        static_cast<void>(submitted(session.submit(
            order(account_id, "b1", Side::Buy, "10", 100, "100"),
            market(1, 100)
        )));
    }
    const TopOfBookEvent observation = asks(2, 200, "100", "4");

    // Another transaction holds the account, so the matcher waits for the
    // row lock; the connection is lost while it waits.
    PostgresTestConnection holder(connection_string);
    PostgresTestConnection observer(connection_string);
    static_cast<void>(holder.query("BEGIN"));
    static_cast<void>(holder.query(
        "SELECT 1 FROM paper_accounts WHERE account_id = $1 FOR UPDATE",
        {account_id}
    ));
    std::string outcome = "returned";
    std::thread matcher([&] {
        try {
            static_cast<void>(match_and_commit(store, account_id, observation));
        } catch (const PaperPersistenceUnavailable&) {
            outcome = "unavailable";
        } catch (const std::exception& error) {
            outcome = error.what();
        }
    });
    wait_until_blocked_on_lock(observer, application);
    terminate_backends(observer, application);
    matcher.join();
    static_cast<void>(holder.query("ROLLBACK"));
    REQUIRE(outcome == "unavailable");
    REQUIRE(find(store, account_id, "b1").state->fill_count == 0);

    // The retry reconnects and books the observation exactly once.
    REQUIRE(match_and_commit(store, account_id, observation).fills.size() == 1);
    REQUIRE(!match_and_commit(store, account_id, observation).changed_account());
    REQUIRE(find(store, account_id, "b1").state->filled_quantity == quantity("4"));
}

void test_cancellation_is_durable_idempotent_and_releases_reservations() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-cancel");
    auto session = create_session(store, account_id);
    for (const std::string& id : {std::string("b1"), std::string("b2"), std::string("b3")}) {
        static_cast<void>(submitted(session.submit(
            order(account_id, id, Side::Buy, "10", 100, "100"),
            market(1, 100)
        )));
    }

    {
        PaperAccountLock locked = lock(store, account_id);
        const auto cancelled = locked.processor().cancel("b1", Timestamp::from_nanoseconds(150));
        const auto result = locked.commit_cancellation(
            std::get<WorkingOrderCancellation>(cancelled),
            "cancel-1"
        );
        REQUIRE(std::holds_alternative<std::int64_t>(result));
    }
    const PaperOrderRecord cancelled = find(store, account_id, "b1");
    REQUIRE(cancelled.state->status == PaperOrderStatus::Cancelled);
    REQUIRE(cancelled.state->cancellation_reason == SimulatedCancellationReason::UserRequested);
    REQUIRE(cancelled.state->cancel_request_id == "cancel-1");
    REQUIRE(cancelled.state->reservation == chronos::OrderReservation{});
    REQUIRE(cancelled.state->cumulative_fee == Money{});
    REQUIRE(cancelled.state->updated_at == Timestamp::from_nanoseconds(150));
    {
        PaperAccountLock locked = lock(store, account_id);
        REQUIRE(locked.order_cancelled_by("cancel-1") == "b1");
        REQUIRE(!locked.order_cancelled_by("cancel-2").has_value());
        REQUIRE(locked.processor().reservations().reserved_cash == money("2000.02"));
        locked.release();
    }

    // One cancel request ID cancels one order: reusing it writes nothing.
    const std::int64_t version_before = store.journal_summary(account_id).account_version;
    {
        PaperAccountLock locked = lock(store, account_id);
        const auto reuse = locked.processor().cancel("b2", Timestamp::from_nanoseconds(160));
        const auto result = locked.commit_cancellation(
            std::get<WorkingOrderCancellation>(reuse),
            "cancel-1"
        );
        REQUIRE(std::get<PaperPersistenceConflict>(result).code ==
            PaperPersistenceConflictCode::DuplicateCancelRequestId);
    }
    REQUIRE(find(store, account_id, "b2").state->status == PaperOrderStatus::Working);
    REQUIRE(store.journal_summary(account_id).account_version == version_before);

    // A partially filled order keeps its fill and fee when its remainder is
    // cancelled, even on a halted account.
    REQUIRE(match_and_commit(store, account_id, asks(2, 200, "100", "4")).fills.size() == 1);
    PostgresTestConnection editor(connection_string);
    static_cast<void>(editor.query(
        "UPDATE paper_accounts SET is_healthy = false WHERE account_id = $1",
        {account_id}
    ));
    {
        PaperAccountLock locked = lock(store, account_id);
        const auto partial = locked.processor().cancel("b2", Timestamp::from_nanoseconds(250));
        REQUIRE(std::holds_alternative<std::int64_t>(locked.commit_cancellation(
            std::get<WorkingOrderCancellation>(partial),
            "cancel-2"
        )));
    }
    const PaperOrderRecord partial = find(store, account_id, "b2");
    REQUIRE(partial.state->status == PaperOrderStatus::PartiallyFilledAndCancelled);
    REQUIRE(partial.state->filled_quantity == quantity("4"));
    REQUIRE(partial.state->cumulative_fee == money("0.01"));
    REQUIRE(partial.state->reservation == chronos::OrderReservation{});
    const auto events = std::get<PaperForwardPage<PaperOrderEventRecord>>(
        store.list_order_events(account_id, "b2", forward())
    );
    REQUIRE((event_types(events) == std::vector<std::string>{
        "order_submitted", "order_accepted", "account_state_updated",
        "fill_applied", "account_state_updated",
        "order_cancelled", "account_state_updated",
    }));
    const auto recovered = PersistentPaperOrderSession::load(store, account_id);
    REQUIRE(recovered->working_orders().size() == 1);
    REQUIRE(recovered->working_orders()[0].intent.client_order_id == "b3");
    REQUIRE(recovered->reservations().reserved_cash == money("1000.01"));
    REQUIRE(recovered->account().state().quantity(kAapl) == quantity("4"));
}

void test_submission_gives_working_orders_priority_on_its_observation() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-priority");
    {
        auto session = create_session(store, account_id);
        static_cast<void>(submitted(session.submit(
            order(account_id, "b1", Side::Buy, "3", 100, "100"),
            market(1, 100)
        )));
    }
    // The submission's observation fills the earlier working order first;
    // the new order takes only what it left.
    auto session = PersistentPaperOrderSession::load(store, account_id);
    const auto persisted = session->submit(
        order(account_id, "t1", Side::Buy, "5", 200),
        asks(2, 200, "100", "4")
    );
    const auto& receipt = std::get<PersistentPaperOrderReceipt>(persisted);
    REQUIRE(receipt.earlier_orders.fills.size() == 1);
    REQUIRE(receipt.earlier_orders.fills[0].accounting.fill.quantity == quantity("3"));
    const auto& taker = std::get<PaperOrderReceipt>(receipt.result);
    REQUIRE(taker.execution.status == SimulatedExecutionStatus::PartiallyFilledAndCancelled);
    REQUIRE(taker.execution.fill->quantity == quantity("1"));

    // The earlier order's fill was recorded before the new order.
    PostgresTestConnection inspector(connection_string);
    REQUIRE(inspector.scalar(
        "SELECT string_agg(client_order_id || ':' || event_type, ',' "
        "ORDER BY event_sequence) FROM paper_events "
        "WHERE account_id = $1 AND event_sequence > ("
        "  SELECT max(event_sequence) FROM paper_events "
        "  WHERE account_id = $1 AND client_order_id = 'b1' "
        "    AND event_type = 'account_state_updated' "
        "    AND occurred_at_ns = 100)",
        {account_id}
    ) == "b1:fill_applied,b1:order_filled,b1:account_state_updated,"
         "t1:order_submitted,t1:order_completed,t1:fill_applied,"
         "t1:account_state_updated");
    REQUIRE(find(store, account_id, "b1").state->status == PaperOrderStatus::Filled);
}

// Two matchers, as two servers on one stream would run, race over the same
// observations while another connection cancels an order and a reader pages
// the account. Account locks serialize every mutation, so each observation
// is booked once and every reservation stays covered.
void test_concurrent_matchers_and_cancellation_serialize() {
    PostgresPaperOrderRepository setup(connection_string);
    setup.apply_migration(all_migrations_sql());
    const std::string account_id = unique_test_id("gtc-race");
    {
        auto session = create_session(setup, account_id);
        for (int index = 0; index < 4; ++index) {
            static_cast<void>(submitted(session.submit(
                order(account_id, "b" + std::to_string(index), Side::Buy, "10", 100, "100"),
                market(1, 100)
            )));
        }
    }
    std::vector<TopOfBookEvent> observations;
    for (std::uint64_t sequence = 2; sequence < 12; ++sequence) {
        observations.push_back(asks(
            sequence,
            static_cast<std::int64_t>(sequence) * 100,
            "100",
            "3"
        ));
    }
    std::atomic<int> failures{0};
    std::atomic<bool> matching{true};
    std::vector<std::thread> workers;
    for (int server = 0; server < 2; ++server) {
        workers.emplace_back([&] {
            try {
                PostgresPaperOrderRepository store(connection_string);
                for (const TopOfBookEvent& observation : observations) {
                    static_cast<void>(match_and_commit(store, account_id, observation));
                }
            } catch (...) {
                failures.fetch_add(1);
            }
        });
    }
    // The last order in priority never fills: the ten observations offer 30
    // shares and the first three orders want 30. Cancelling it at any point
    // changes nothing else.
    workers.emplace_back([&] {
        try {
            PostgresPaperOrderRepository store(connection_string);
            PaperAccountLock locked = lock(store, account_id);
            const auto cancelled =
                locked.processor().cancel("b3", Timestamp::from_nanoseconds(150));
            if (!std::holds_alternative<std::int64_t>(locked.commit_cancellation(
                    std::get<WorkingOrderCancellation>(cancelled),
                    "cancel-b3"
                ))) {
                failures.fetch_add(1);
            }
        } catch (...) {
            failures.fetch_add(1);
        }
    });
    std::thread reader([&] {
        try {
            PostgresPaperOrderRepository store(connection_string);
            while (matching.load()) {
                static_cast<void>(store.list_working_orders(account_id, forward()));
                static_cast<void>(store.find_order(account_id, "b0"));
            }
        } catch (...) {
            failures.fetch_add(1);
        }
    });
    for (auto& worker : workers) {
        worker.join();
    }
    matching.store(false);
    reader.join();
    REQUIRE(failures.load() == 0);

    PostgresTestConnection inspector(connection_string);
    REQUIRE(inspector.scalar(
        "SELECT sum(quantity_raw) FROM paper_fills WHERE account_id = $1",
        {account_id}
    ) == "3000000000");
    // One matcher booked each observation; the other found it used.
    REQUIRE(inspector.scalar(
        "SELECT count(DISTINCT market_ingest_sequence) FROM paper_fills "
        "WHERE account_id = $1",
        {account_id}
    ) == "10");
    REQUIRE(inspector.scalar(
        "SELECT string_agg(client_order_id || ':' || status || ':' || "
        "filled_quantity_raw, ',' ORDER BY order_record_id) "
        "FROM paper_orders WHERE account_id = $1",
        {account_id}
    ) == "b0:filled:1000000000,b1:filled:1000000000,b2:filled:1000000000,"
         "b3:cancelled:0");
    const auto recovered = PersistentPaperOrderSession::load(setup, account_id);
    REQUIRE(recovered.has_value());
    REQUIRE(recovered->working_orders().empty());
    REQUIRE(recovered->account().state().quantity(kAapl) == quantity("30"));
}

// Every query the matcher, recovery, and working-order pages run can use an
// index. Sequential scans are disabled so that an index path, if one exists,
// is chosen however small the test tables are.
void test_working_order_queries_can_use_indexes() {
    PostgresPaperOrderRepository store(connection_string);
    store.apply_migration(all_migrations_sql());
    PostgresTestConnection inspector(connection_string);
    static_cast<void>(inspector.query("SET enable_seqscan = off"));
    const auto plan = [&](std::string_view query) {
        std::string text;
        for (const auto& row : inspector.query("EXPLAIN " + std::string(query))) {
            text += row.front() + "\n";
        }
        return text;
    };
    const std::vector<std::string_view> queries{
        // The matcher's candidates.
        "SELECT DISTINCT latest.account_id FROM paper_orders AS latest "
        "JOIN paper_accounts AS owner ON owner.account_id = latest.account_id "
        "WHERE latest.status IN ('working', 'partially_filled') "
        "AND latest.instrument = 'AAPL' AND latest.market_stream_id = 's' "
        "AND latest.accepted_market_ingest_sequence < 10 "
        "AND ((latest.side = 'buy' AND latest.limit_price_raw >= 100) OR "
        "(latest.side = 'sell' AND latest.limit_price_raw <= 99)) "
        "AND owner.is_healthy",
        // Recovery and the working-orders page.
        "SELECT order_record_id FROM paper_orders WHERE account_id = 'a' "
        "AND status IN ('working', 'partially_filled') "
        "AND order_record_id > 5 ORDER BY order_record_id LIMIT 51",
        // Recovery's fill IDs and the account's fill history.
        "SELECT fill_id FROM paper_fills WHERE account_id = 'a' "
        "ORDER BY fill_record_id",
        "SELECT fill_record_id FROM paper_fills WHERE account_id = 'a' "
        "AND fill_record_id < 9 ORDER BY fill_record_id DESC LIMIT 51",
        "SELECT fill_record_id FROM paper_fills WHERE account_id = 'a' "
        "AND instrument = 'AAPL' AND fill_record_id < 9 "
        "ORDER BY fill_record_id DESC LIMIT 51",
        // One order's fills and events.
        "SELECT fill_index FROM paper_fills WHERE account_id = 'a' "
        "AND order_record_id = 7 AND fill_index > 0 ORDER BY fill_index LIMIT 51",
        "SELECT event_order FROM paper_events WHERE account_id = 'a' "
        "AND order_record_id = 7 AND event_order > 0 ORDER BY event_order LIMIT 51",
        // A cancel request's order, and the reconcile trigger's sums.
        "SELECT client_order_id FROM paper_orders WHERE account_id = 'a' "
        "AND cancel_request_id = 'cancel-1'",
        "SELECT sum(reserved_cash_raw) FROM paper_orders WHERE account_id = 'a' "
        "AND status IN ('working', 'partially_filled')",
        "SELECT count(*) FROM paper_fills WHERE order_record_id = 7",
    };
    for (const std::string_view query : queries) {
        const std::string text = plan(query);
        for (const std::string_view table : {"paper_orders", "paper_fills", "paper_events"}) {
            if (text.find("Seq Scan on " + std::string(table)) != std::string::npos) {
                throw chronos::test::TestFailure(
                    "full scan of " + std::string(table) + " in:\n" + text
                );
            }
        }
    }
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr ||
        std::string_view(configured_connection).empty()) {
        std::cout << "PostgreSQL working-order test skipped: "
                     "CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {"working orders survive restart", test_working_orders_and_reservations_survive_restart},
        {"several fills pay one fee", test_matching_books_several_fills_with_one_fee},
        {"matching twice books once", test_matching_twice_never_books_twice},
        {"skipped fill keeps durable priority", test_a_skipped_fill_durably_keeps_fifo_priority},
        {"lost connection while matching", test_a_lost_connection_while_matching_books_nothing_until_retried},
        {"cancellation is durable and idempotent", test_cancellation_is_durable_idempotent_and_releases_reservations},
        {"working orders keep priority at submission", test_submission_gives_working_orders_priority_on_its_observation},
        {"concurrent matchers serialize", test_concurrent_matchers_and_cancellation_serialize},
        {"working order queries use indexes", test_working_order_queries_can_use_indexes},
    });
}
