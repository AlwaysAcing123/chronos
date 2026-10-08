// live_stream_sequences in PostgreSQL (migration 006, ADR 0020): writer
// leases that are never taken before they expire, reservations that never
// overlap or wrap, and durable history from before the table that is
// continued only after an explicit adoption. Expiry is forced with SQL, so
// nothing waits for a clock.
#include "chronos/market_state.hpp"
#include "chronos/paper_trading/paper_trading_service.hpp"
#include "chronos/postgresql_live_stream_sequences.hpp"
#include "chronos/postgresql_paper_order_repository.hpp"
#include "paper_trading_test_support.hpp"
#include "postgres_test_support.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace {

using chronos::BookLevel;
using chronos::CreatedDeployment;
using chronos::DeploymentState;
using chronos::InstrumentId;
using chronos::MarketDataQuality;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketEventHeader;
using chronos::MarketState;
using chronos::ModelDeploymentConfig;
using chronos::ModelDeploymentRecord;
using chronos::Money;
using chronos::NewDeploymentDecision;
using chronos::PaperPersistenceUnavailable;
using chronos::PostgresIngestSequenceLeaseStore;
using chronos::PostgresPaperOrderRepository;
using chronos::PreTradeRiskLimits;
using chronos::Price;
using chronos::Quantity;
using chronos::Side;
using chronos::Timestamp;
using chronos::TopOfBookEvent;
using chronos::market_data::LeaseAcquisition;
using chronos::market_data::LeaseGranted;
using chronos::market_data::LeaseHeld;
using chronos::market_data::LeaseLost;
using chronos::market_data::LeaseRefused;
using chronos::market_data::LeaseRenewed;
using chronos::market_data::SequenceBlock;
using chronos::market_data::StreamWriter;
using chronos::test::PostgresTestConnection;
using namespace std::chrono_literals;

std::string connection_string;

constexpr std::uint64_t kLast = std::numeric_limits<std::uint64_t>::max();
const MarketDataStreamId kStream("alpaca-iex-store-test");
const StreamWriter kA{.id = "writer-a", .label = "pid 1 on host-a"};
const StreamWriter kB{.id = "writer-b", .label = "pid 2 on host-b"};

class ScratchSchema final : public chronos::test::ScratchSchema {
public:
    explicit ScratchSchema(bool with_live_sequences = true)
        : chronos::test::ScratchSchema(connection_string) {
        PostgresPaperOrderRepository store(connection());
        if (with_live_sequences) {
            store.apply_migration(chronos::test::all_migrations_sql());
            return;
        }
        for (const char* file : {"001_paper_trading.sql", "002_paper_order_read_indexes.sql",
                                 "003_multi_instrument_accounts.sql", "004_working_orders.sql",
                                 "005_model_deployments.sql"}) {
            store.apply_migration(chronos::test::migration_sql(file));
        }
    }
};

LeaseGranted granted(const LeaseAcquisition& acquisition) {
    if (const auto* refused = std::get_if<LeaseRefused>(&acquisition)) {
        throw chronos::test::TestFailure("refused: " + refused->reason);
    }
    REQUIRE(std::holds_alternative<LeaseGranted>(acquisition));
    return std::get<LeaseGranted>(acquisition);
}

std::string refusal(const LeaseAcquisition& acquisition) {
    REQUIRE(std::holds_alternative<LeaseRefused>(acquisition));
    return std::get<LeaseRefused>(acquisition).reason;
}

LeaseRenewed renewed(const chronos::market_data::LeaseRenewal& renewal) {
    if (const auto* lost = std::get_if<LeaseLost>(&renewal)) {
        throw chronos::test::TestFailure("lost: " + lost->reason);
    }
    return std::get<LeaseRenewed>(renewal);
}

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

std::string row(PostgresTestConnection& inspector, std::string_view stream = kStream.value()) {
    return inspector.scalar(
        "SELECT next_sequence::text || '|' || coalesce(writer_id, '-') || '|' || origin || '|' || "
        "coalesce(adopted_maximum::text, '-') || '|' || acquisitions FROM live_stream_sequences "
        "WHERE stream_id = '" + std::string(stream) + "'"
    );
}

void expire_leases(PostgresTestConnection& inspector) {
    inspector.run_script(
        "UPDATE live_stream_sequences SET lease_expires_at = clock_timestamp() - interval '1 millisecond' "
        "WHERE writer_id IS NOT NULL"
    );
}

void test_a_new_stream_starts_at_zero_and_reserves_blocks_in_order() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    const LeaseGranted first = granted(store.acquire(kStream, kA, 15'000ms, 8, false));
    REQUIRE(first.block == (SequenceBlock{.first = 0, .last = 7}));
    REQUIRE(!first.adopted_maximum.has_value());
    REQUIRE(row(inspector) == "8|writer-a|new|-|1");
    REQUIRE(inspector.scalar(
        "SELECT lease_expires_at > clock_timestamp() + interval '14 seconds' FROM live_stream_sequences"
    ) == "t");

    const std::string expiry = inspector.scalar("SELECT lease_expires_at::text FROM live_stream_sequences");
    const LeaseRenewed refill = renewed(store.renew(kStream, kA, 15'000ms, 8));
    REQUIRE(refill.block == std::optional(SequenceBlock{.first = 8, .last = 15}));
    REQUIRE(!refill.exhausted);
    const LeaseRenewed extended = renewed(store.renew(kStream, kA, 15'000ms, 0));
    REQUIRE(!extended.block.has_value());
    REQUIRE(row(inspector) == "16|writer-a|new|-|1");
    REQUIRE(inspector.scalar(
        "SELECT lease_expires_at > '" + expiry + "'::timestamptz FROM live_stream_sequences"
    ) == "t");
}

void test_a_lease_is_never_taken_before_it_expires() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore a(schema.connection());
    PostgresIngestSequenceLeaseStore b(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    static_cast<void>(granted(a.acquire(kStream, kA, 15'000ms, 8, false)));

    const auto waiting = b.acquire(kStream, kB, 15'000ms, 8, false);
    REQUIRE(std::holds_alternative<LeaseHeld>(waiting));
    REQUIRE(std::get<LeaseHeld>(waiting).writer_label == "pid 1 on host-a");
    REQUIRE(std::get<LeaseHeld>(waiting).expires_at.ends_with("Z"));
    REQUIRE(row(inspector) == "8|writer-a|new|-|1");
    // Releasing or renewing as someone else changes nothing.
    b.release(kStream, kB);
    REQUIRE(std::holds_alternative<LeaseLost>(b.renew(kStream, kB, 15'000ms, 8)));
    REQUIRE(row(inspector) == "8|writer-a|new|-|1");

    // Expired: b takes over after everything a reserved, and a finds out.
    expire_leases(inspector);
    REQUIRE(granted(b.acquire(kStream, kB, 15'000ms, 8, false)).block == (SequenceBlock{.first = 8, .last = 15}));
    const auto lost = a.renew(kStream, kA, 15'000ms, 8);
    REQUIRE(std::holds_alternative<LeaseLost>(lost));
    REQUIRE(contains(std::get<LeaseLost>(lost).reason, "pid 2 on host-b"));
    REQUIRE(row(inspector) == "16|writer-b|new|-|2");

    b.release(kStream, kB);
    REQUIRE(inspector.scalar(
        "SELECT (writer_id IS NULL AND writer_label IS NULL AND lease_expires_at IS NULL)::text "
        "FROM live_stream_sequences"
    ) == "true");
    REQUIRE(contains(std::get<LeaseLost>(b.renew(kStream, kB, 15'000ms, 0)).reason, "released"));
    REQUIRE(granted(a.acquire(kStream, kA, 15'000ms, 8, false)).block == (SequenceBlock{.first = 16, .last = 23}));
}

// An acquisition whose answer was lost is repeated: the row is already the
// writer's, so it gets a new block, and the unknown one is only a gap.
void test_repeating_an_unanswered_acquisition_only_leaves_a_gap() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore store(schema.connection());
    static_cast<void>(granted(store.acquire(kStream, kA, 15'000ms, 8, false)));
    REQUIRE(granted(store.acquire(kStream, kA, 15'000ms, 8, false)).block == (SequenceBlock{.first = 8, .last = 15}));
}

void test_two_writers_starting_together_get_one_lease() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore a(schema.connection());
    PostgresIngestSequenceLeaseStore b(schema.connection());
    std::promise<void> go;
    std::shared_future<void> started = go.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        started.wait();
        return a.acquire(kStream, kA, 15'000ms, 8, false);
    });
    auto second = std::async(std::launch::async, [&] {
        started.wait();
        return b.acquire(kStream, kB, 15'000ms, 8, false);
    });
    go.set_value();
    const LeaseAcquisition one = first.get();
    const LeaseAcquisition two = second.get();
    REQUIRE(std::holds_alternative<LeaseGranted>(one) != std::holds_alternative<LeaseGranted>(two));
    REQUIRE(std::holds_alternative<LeaseHeld>(one) || std::holds_alternative<LeaseHeld>(two));
    PostgresTestConnection inspector(schema.connection());
    REQUIRE(inspector.scalar("SELECT count(*) FROM live_stream_sequences") == "1");
}

void test_sequences_end_at_the_largest_value_without_wrapping() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) VALUES ('" + kStream.value() +
        "', 18446744073709551613, 'new')"
    );
    REQUIRE(granted(store.acquire(kStream, kA, 15'000ms, 8, false)).block ==
        (SequenceBlock{.first = kLast - 2, .last = kLast}));
    REQUIRE(row(inspector) == "18446744073709551616|writer-a|new|-|1");
    const LeaseRenewed nothing_left = renewed(store.renew(kStream, kA, 15'000ms, 8));
    REQUIRE(nothing_left.exhausted);
    REQUIRE(!nothing_left.block.has_value());
    store.release(kStream, kA);
    REQUIRE(contains(refusal(store.acquire(kStream, kB, 15'000ms, 8, false)), "new stream ID"));
}

// Durable history written before migration 006 on one stream: an account's
// fill and execution state, a resting working order, and a deployment's
// decision and watermark, each later than the one before.
void leave_legacy_history(const ScratchSchema& schema, const MarketDataStreamId& stream) {
    MarketState market(stream, 1'087);
    chronos::paper_trading::PaperTradingService service(
        std::make_unique<PostgresPaperOrderRepository>(schema.connection()),
        market,
        chronos::test::paper_service_config()
    );
    REQUIRE(std::holds_alternative<chronos::paper_trading::PaperAccountView>(
        service.create_account({.account_id = "legacy", .starting_cash = Money::parse("10000")})
    ));
    const auto quote = [&](std::uint64_t sequence, std::int64_t at) {
        chronos::test::apply(market, TopOfBookEvent{
            .header = MarketEventHeader{
                .instrument = InstrumentId("AAPL"),
                .stream = stream,
                .venue = chronos::VenueId("IEX"),
                .event_timestamp = Timestamp::from_nanoseconds(at - 1),
                .receive_timestamp = Timestamp::from_nanoseconds(at),
                .ingest_sequence = sequence,
                .venue_sequence = std::nullopt,
                .quality = MarketDataQuality{}.with(MarketDataQualityFlag::SourceSpecific),
            },
            .bid = BookLevel{.price = Price::parse("334"), .quantity = Quantity::parse("100")},
            .ask = BookLevel{.price = Price::parse("334.02"), .quantity = Quantity::parse("100")},
        });
    };
    const std::int64_t at = chronos::test::kMarketOpenNanoseconds;
    quote(1'087, at);
    REQUIRE(std::holds_alternative<chronos::paper_trading::RecordedPaperOrder>(service.submit_order(
        chronos::test::order_request("legacy", "buy-1", Side::Buy, "1")
    )));
    PostgresIngestSequenceLeaseStore probe(schema.connection());
    REQUIRE(probe.durable_maximum(stream) == std::optional<std::uint64_t>(1'087));
    for (std::uint64_t sequence = 1'088; sequence <= 1'090; ++sequence) {
        quote(sequence, at + static_cast<std::int64_t>(sequence - 1'087) * 1'000'000);
    }
    auto resting = chronos::test::order_request("legacy", "bid-300", Side::Buy, "1", chronos::OrderType::Limit, "300");
    resting.time_in_force = chronos::TimeInForce::GoodTilCancelled;
    REQUIRE(std::holds_alternative<chronos::paper_trading::RecordedPaperOrder>(service.submit_order(resting)));
    REQUIRE(probe.durable_maximum(stream) == std::optional<std::uint64_t>(1'090));

    PostgresPaperOrderRepository repository(schema.connection());
    ModelDeploymentConfig config;
    config.deployment_id = "legacy-model";
    config.account_id = "legacy-model-account";
    config.starting_cash = Money::parse("1000");
    config.model = chronos::ExperimentModel{.experiment_id = std::string(64, 'e'), .name = "logistic"};
    config.family = "logistic";
    config.parameters_sha256 = std::string(64, 'f');
    config.instrument = InstrumentId("AAPL");
    config.training_cutoff = Timestamp::from_nanoseconds(100);
    config.threshold_ppb = 550'000'000;
    config.target_quantity = Quantity::parse("1");
    config.decision_interval_ns = 1'000;
    config.latency_ns = 10;
    config.max_signal_age_ns = 1'000;
    config.max_orders_per_run = 10;
    config.max_consecutive_rejections = 3;
    config.stream = stream;
    config.risk_limits = PreTradeRiskLimits{
        .maximum_order_quantity = Quantity::parse("1"),
        .maximum_order_notional = Money::parse("500"),
        .maximum_position_quantity = Quantity::parse("1"),
        .maximum_quote_age_nanoseconds = 1'000,
        .fee_per_fill = Money::parse("0.01"),
    };
    config.request_sha256 = std::string(64, 'a');
    auto created = repository.create_deployment(config);
    REQUIRE(std::holds_alternative<CreatedDeployment>(created));
    REQUIRE(std::holds_alternative<ModelDeploymentRecord>(repository.transition_deployment(
        "legacy-model", {DeploymentState::Stopped}, DeploymentState::Running, "started", true
    )));
    // A hold: the decision and the watermark reference 2000, no order does.
    auto decided = repository.record_decision(NewDeploymentDecision{
        .deployment_id = "legacy-model",
        .run_generation = 1,
        .stream = stream,
        .instrument = InstrumentId("AAPL"),
        .observation_ingest_sequence = 2'000,
        .observation_event = Timestamp::from_nanoseconds(at + 9'999),
        .observation_receive = Timestamp::from_nanoseconds(at + 10'000),
        .decided_at_market = Timestamp::from_nanoseconds(at + 10'000),
        .inputs = {1, -2, 0, 500'000'000, 400'000'000},
        .predicted_class = "flat",
        .probabilities = {"0.1", "0.7", "0.2"},
        .confidence_ppb = 700'000'000,
        .position_before = Quantity::parse("0"),
        .target = std::nullopt,
        .policy_reason = "hold_flat_prediction",
        .side = std::nullopt,
        .quantity = std::nullopt,
    });
    REQUIRE(std::holds_alternative<chronos::DeploymentDecisionRecord>(decided));
    REQUIRE(probe.durable_maximum(stream) == std::optional<std::uint64_t>(2'000));
}

void test_legacy_history_is_refused_until_adopted_and_streams_stay_separate() {
    const ScratchSchema schema;
    leave_legacy_history(schema, kStream);
    PostgresIngestSequenceLeaseStore store(schema.connection());
    PostgresTestConnection inspector(schema.connection());

    const std::string refused = refusal(store.acquire(kStream, kA, 15'000ms, 8, false));
    REQUIRE(contains(refused, "up to ingest sequence 2000"));
    REQUIRE(contains(refused, "--adopt-live-stream-history"));
    REQUIRE(inspector.scalar("SELECT count(*) FROM live_stream_sequences") == "0");

    // Another stream has no history: it starts at zero, untouched by this one.
    const MarketDataStreamId other("alpaca-iex-other");
    REQUIRE(granted(store.acquire(other, kA, 15'000ms, 8, false)).block == (SequenceBlock{.first = 0, .last = 7}));

    const LeaseGranted adopted = granted(store.acquire(kStream, kB, 15'000ms, 8, true));
    REQUIRE(adopted.block == (SequenceBlock{.first = 2'001, .last = 2'008}));
    REQUIRE(adopted.adopted_maximum == std::optional<std::uint64_t>(2'000));
    REQUIRE(row(inspector) == "2009|writer-b|adopted|2000|1");
    REQUIRE(row(inspector, "alpaca-iex-other") == "8|writer-a|new|-|1");
    // Adoption happens once: the flag changes nothing for an existing row.
    store.release(kStream, kB);
    const LeaseGranted again = granted(store.acquire(kStream, kA, 15'000ms, 8, true));
    REQUIRE(again.block == (SequenceBlock{.first = 2'009, .last = 2'016}));
    REQUIRE(!again.adopted_maximum.has_value());
}

// A writer without durable sequencing, or a replay under a live stream ID,
// wrote beyond the reservations: refused, never repaired.
void test_history_beyond_the_reservations_is_refused() {
    const ScratchSchema schema;
    PostgresTestConnection inspector(schema.connection());
    // Written while the stream had no record, then a record behind it: the
    // fence would refuse that history once a record exists.
    leave_legacy_history(schema, kStream);
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) VALUES ('" + kStream.value() +
        "', 1500, 'new')"
    );
    PostgresIngestSequenceLeaseStore store(schema.connection());
    const std::string refused = refusal(store.acquire(kStream, kA, 15'000ms, 8, true));
    REQUIRE(contains(refused, "2000, at or beyond its next reservable sequence 1500"));
    REQUIRE(row(inspector) == "1500|-|new|-|0");
}

void test_a_database_without_migration_006_is_refused_with_its_remedy() {
    const ScratchSchema schema(false);
    PostgresIngestSequenceLeaseStore store(schema.connection());
    REQUIRE(contains(refusal(store.acquire(kStream, kA, 15'000ms, 8, false)), "006_live_stream_sequences.sql"));
}

// The fence follows the stream's current writer generation (ADR 0020,
// migration 007): what the writer reserved since it acquired passes, an
// earlier writer's sequences never do, and neither does anything once the
// stream is released. A stream without a record is not live.
void test_the_fence_follows_the_current_writer_generation() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore a(schema.connection());
    PostgresIngestSequenceLeaseStore b(schema.connection());
    PostgresPaperOrderRepository repository(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    const auto fence = [&](std::uint64_t first, std::uint64_t last) {
        return repository.live_stream_writer_refusal(kStream, first, last);
    };
    REQUIRE(!fence(0, 0).has_value());  // no record yet: not a live stream

    static_cast<void>(granted(a.acquire(kStream, kA, 15'000ms, 8, false)));
    REQUIRE(inspector.scalar("SELECT writer_first_sequence::text FROM live_stream_sequences") == "0");
    REQUIRE(!fence(0, 7).has_value());
    REQUIRE(contains(*fence(7, 8), "never reserved by its current writer"));
    static_cast<void>(renewed(a.renew(kStream, kA, 15'000ms, 8)));
    REQUIRE(!fence(0, 15).has_value());  // a reservation is the same generation

    expire_leases(inspector);
    static_cast<void>(granted(b.acquire(kStream, kB, 15'000ms, 8, false)));
    REQUIRE(inspector.scalar("SELECT writer_first_sequence::text FROM live_stream_sequences") == "16");
    for (const auto& [first, last] : {std::pair<std::uint64_t, std::uint64_t>{0, 0}, {15, 15}, {15, 16}}) {
        const auto refused = fence(first, last);
        REQUIRE(refused.has_value());
        REQUIRE(contains(*refused, "belongs to an earlier writer; the stream's current writer began at 16"));
    }
    REQUIRE(!fence(16, 23).has_value());

    // The earlier writer acquiring again starts yet another generation: its
    // own earlier sequences stay refused.
    b.release(kStream, kB);
    REQUIRE(inspector.scalar("SELECT coalesce(writer_first_sequence::text, '-') FROM live_stream_sequences") == "-");
    REQUIRE(contains(*fence(16, 16), "no writer holds the stream now"));
    static_cast<void>(granted(a.acquire(kStream, kA, 15'000ms, 8, false)));
    REQUIRE(contains(*fence(0, 0), "began at 24"));
    REQUIRE(!fence(24, 24).has_value());
    REQUIRE(!repository.live_stream_writer_refusal(MarketDataStreamId("replay-stream"), 0, 0).has_value());
}

// The fence's share lock serializes with a takeover: while a fenced change
// is in flight, a successor cannot begin its generation; once it has, the
// deposed writer's next change sees it. Here the in-flight change is the
// fence's own statement, held open in a transaction.
void test_a_takeover_waits_for_a_fenced_change_in_flight() {
    const ScratchSchema schema;
    PostgresIngestSequenceLeaseStore a(schema.connection());
    PostgresIngestSequenceLeaseStore b(schema.connection(), 300ms);
    PostgresPaperOrderRepository repository(schema.connection());
    PostgresTestConnection in_flight(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    static_cast<void>(granted(a.acquire(kStream, kA, 15'000ms, 8, false)));
    expire_leases(inspector);

    in_flight.run_script(
        "BEGIN; SELECT 1 FROM live_stream_sequences WHERE stream_id = '" + kStream.value() + "' FOR SHARE"
    );
    bool waited = false;
    try {
        const auto outcome = b.acquire(kStream, kB, 15'000ms, 8, false);
        std::cout << "takeover while a fenced change is in flight: " << outcome.index() << '\n';
    } catch (const PaperPersistenceUnavailable& error) {
        std::cout << "takeover while a fenced change is in flight: " << error.what() << '\n';
        // Its row lock waited until its timeout (lock or statement, both
        // 300 ms here) cancelled it.
        waited = contains(error.what(), "lock failed") && contains(error.what(), "canceling statement due to");
    }
    REQUIRE(waited);
    REQUIRE(row(inspector) == "8|writer-a|new|-|1");
    REQUIRE(!repository.live_stream_writer_refusal(kStream, 3, 3).has_value());

    in_flight.run_script("COMMIT");
    REQUIRE(granted(b.acquire(kStream, kB, 15'000ms, 8, false)).block == (SequenceBlock{.first = 8, .last = 15}));
    REQUIRE(contains(*repository.live_stream_writer_refusal(kStream, 3, 3), "earlier writer"));
}

// Before migration 007 a current server takes no stream, so nothing can be
// deposed and the fence refuses nothing; it finds the column once it is
// added, without a restart.
void test_a_database_without_migration_007_is_refused_and_the_fence_appears_with_it() {
    const ScratchSchema schema(false);
    PostgresPaperOrderRepository repository(schema.connection());
    repository.apply_migration(chronos::test::migration_sql("006_live_stream_sequences.sql"));
    PostgresIngestSequenceLeaseStore store(schema.connection());
    PostgresTestConnection inspector(schema.connection());
    const std::string refused = refusal(store.acquire(kStream, kA, 15'000ms, 8, false));
    REQUIRE(contains(refused, "007_live_stream_writer_fence.sql"));
    REQUIRE(inspector.scalar("SELECT count(*) FROM live_stream_sequences") == "0");
    inspector.run_script(
        "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin) VALUES ('" + kStream.value() +
        "', 8, 'new')"
    );
    REQUIRE(!repository.live_stream_writer_refusal(kStream, 0, 0).has_value());

    repository.apply_migration(chronos::test::migration_sql("007_live_stream_writer_fence.sql"));
    REQUIRE(contains(*repository.live_stream_writer_refusal(kStream, 0, 0), "no writer holds the stream now"));
    REQUIRE(granted(store.acquire(kStream, kA, 15'000ms, 8, false)).block == (SequenceBlock{.first = 8, .last = 15}));
    REQUIRE(!repository.live_stream_writer_refusal(kStream, 8, 8).has_value());
}

void test_an_unreachable_database_fails_at_once() {
    PostgresIngestSequenceLeaseStore store("postgresql://chronos@127.0.0.1:1/none");
    const auto started = std::chrono::steady_clock::now();
    try {
        static_cast<void>(store.acquire(kStream, kA, 15'000ms, 8, false));
        REQUIRE(false);
    } catch (const PaperPersistenceUnavailable& error) {
        REQUIRE(contains(error.what(), "could not connect"));
    }
    REQUIRE(std::chrono::steady_clock::now() - started < 6s);
}

}  // namespace

int main() {
    const char* configured_connection = std::getenv("CHRONOS_TEST_POSTGRES_URL");
    if (configured_connection == nullptr || std::string_view(configured_connection).empty()) {
        std::cout << "live stream sequence store test skipped: CHRONOS_TEST_POSTGRES_URL is not set\n";
        return 77;
    }
    connection_string = configured_connection;
    return chronos::test::run_tests({
        {
            "a new stream starts at zero and reserves blocks in order",
            test_a_new_stream_starts_at_zero_and_reserves_blocks_in_order,
        },
        {"a lease is never taken before it expires", test_a_lease_is_never_taken_before_it_expires},
        {
            "repeating an unanswered acquisition only leaves a gap",
            test_repeating_an_unanswered_acquisition_only_leaves_a_gap,
        },
        {"two writers starting together get one lease", test_two_writers_starting_together_get_one_lease},
        {
            "sequences end at the largest value without wrapping",
            test_sequences_end_at_the_largest_value_without_wrapping,
        },
        {
            "legacy history is refused until adopted, and streams stay separate",
            test_legacy_history_is_refused_until_adopted_and_streams_stay_separate,
        },
        {"history beyond the reservations is refused", test_history_beyond_the_reservations_is_refused},
        {
            "a database without migration 006 is refused with its remedy",
            test_a_database_without_migration_006_is_refused_with_its_remedy,
        },
        {"the fence follows the current writer generation", test_the_fence_follows_the_current_writer_generation},
        {"a takeover waits for a fenced change in flight", test_a_takeover_waits_for_a_fenced_change_in_flight},
        {
            "a database without migration 007 is refused and the fence appears with it",
            test_a_database_without_migration_007_is_refused_and_the_fence_appears_with_it,
        },
        {"an unreachable database fails at once", test_an_unreachable_database_fails_at_once},
    });
}
