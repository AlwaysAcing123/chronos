#pragma once

#include "chronos/postgresql_model_deployments.hpp"

#include "chronos/paper_order_processor.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos {

enum class PaperPersistenceConflictCode {
    AccountAlreadyExists,
    AccountNotFound,
    DuplicateClientOrderId,
    VersionConflict,
    // Another order of the account was already cancelled by this request.
    DuplicateCancelRequestId,
};

struct PaperPersistenceConflict {
    PaperPersistenceConflictCode code;
    std::string message;
};

// PostgreSQL could not be reached, or the connection was lost during an
// operation. When the loss interrupts a commit, whether the transaction
// committed is unknown; resubmitting the same client order ID is safe because
// the database accepts it at most once. Other database failures remain
// std::runtime_error because they indicate defects or corrupt state.
class PaperPersistenceUnavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using CreatePaperAccountResult = std::variant<
    std::int64_t,
    PaperPersistenceConflict
>;

using CommitPaperOrderResult = std::variant<
    std::int64_t,
    PaperPersistenceConflict
>;

struct StoredPaperOrderProcessor {
    PaperOrderProcessor processor;
    std::int64_t version;
};

struct PaperAccountJournalSummary {
    std::int64_t order_count;
    std::int64_t event_count;
    std::int64_t applied_fill_count;
    std::int64_t account_version;
};

// ---------------------------------------------------------------------------
// Journal read model
//
// Records are mapped from persisted rows alone; risk, execution, and
// accounting are never re-run to reconstruct them. A row whose fields could
// not have been written together fails closed with std::runtime_error.

// A processing fault as journaled. The accounting error behind an invariant
// violation is not journaled, so it is not part of the record.
struct PaperOrderFaultRecord {
    PaperOrderProcessingErrorCode code;
    std::string message;
    // The simulated execution that accounting rejected, when the order
    // reached the simulator. Its fill was never booked.
    std::optional<SimulatedExecutionReport> unbooked_execution;
};

// How a journaled submission ended. A SimulatedExecutionReport is an
// executed order; its fill, when present, was booked.
using PaperOrderRecordOutcome = std::variant<
    SimulatedExecutionReport,
    PreTradeRiskRejection,
    SimulatedExecutionError,
    PaperOrderFaultRecord
>;

// The current state of an order that reached execution. The journal
// record keeps how its submission ended; this is where the order is now.
struct PaperOrderState {
    PaperOrderStatus status;
    Quantity filled_quantity;
    Quantity remaining_quantity;
    // The sum of the booked notionals of its fills.
    Money filled_notional;
    std::uint32_t fill_count{0};
    // The flat fee its first fill pays, and what its fills have paid.
    Money order_fee;
    Money cumulative_fee;
    // What the order holds back while it works; zero once terminal.
    OrderReservation reservation;
    std::optional<SimulatedCancellationReason> cancellation_reason;
    // The request that cancelled it, for a client cancellation.
    std::optional<std::string> cancel_request_id;
    // Market time of its latest change.
    Timestamp updated_at;

    [[nodiscard]] bool fee_charged() const noexcept {
        return filled_quantity.raw() > 0;
    }

    friend bool operator==(const PaperOrderState&, const PaperOrderState&) =
        default;
};

struct PaperOrderRecord {
    // Journal identity. Within one account it increases in commit order,
    // because every order record is inserted while the account row is locked.
    // It is also a working order's priority: lower fills first.
    std::int64_t record_id;
    OrderIntent intent;
    TopOfBookEvent market;
    PaperOrderRecordOutcome outcome;
    // Present exactly when the submission reached execution.
    std::optional<PaperOrderState> state;
};

// The record a submission produces when it commits with `record_id`,
// before any later fill or cancellation changes the order's state. The
// repository persists exactly this; transports use it to describe an order
// they have not read back.
[[nodiscard]] PaperOrderRecord submitted_order_record(
    std::int64_t record_id,
    const OrderIntent& intent,
    const TopOfBookEvent& market,
    const PaperOrderResult& result,
    Money account_fee
);

// A fill that accounting booked, linked to the order that produced it.
struct PaperFillRecord {
    // Fill identity. Within one account it increases in commit order. A fill
    // recorded before migration 004 has its order's journal record ID.
    std::int64_t record_id;
    std::string account_id;
    std::string client_order_id;
    OrderSource source;
    Fill fill;
    // Its position among its order's fills, from 0.
    std::uint32_t fill_index{0};
    // The observation it executed against.
    std::uint64_t market_ingest_sequence{0};
};

// What happened to an order, in the order it happened. Names are persisted
// and public, like the order vocabulary in paper_order_codes.hpp.
enum class PaperOrderEventType {
    OrderSubmitted,
    RiskRejected,
    ExecutionRejected,
    OrderCompleted,
    FillApplied,
    AccountStateUpdated,
    ProcessorFaulted,
    OrderAccepted,
    OrderFilled,
    OrderCancelled,
};

[[nodiscard]] std::optional<std::string_view> canonical_name(
    PaperOrderEventType type
) noexcept;

struct PaperOrderEventRecord {
    // The event's position among its order's events, from 1.
    std::int64_t sequence;
    PaperOrderEventType type;
    // Market time. An event from before migration 004 has its submission's
    // time, when every event of that order was written.
    Timestamp occurred_at;
    // The fill a FillApplied event booked.
    std::optional<Fill> fill;
};

// A newest-first page. `before_record_id` excludes that record and every
// newer one, so a page chain stays stable while new orders are recorded.
// `instrument`, when present, keeps only the records for that instrument; a
// chain of pages must use the same filter throughout.
struct PaperJournalPageRequest {
    std::size_t limit;
    std::optional<std::int64_t> before_record_id;
    std::optional<InstrumentId> instrument;
};

template <typename Record>
struct PaperJournalPage {
    std::vector<Record> records;
    // Present when older records exist; the next request's before_record_id.
    std::optional<std::int64_t> next_before_record_id;
};

// An oldest-first page, for sequences read in the order they happened: one
// order's fills or events, and an account's working orders in priority
// order. `after` excludes that position and every earlier one, so a chain
// of pages stays stable while the sequence grows.
struct PaperForwardPageRequest {
    std::size_t limit;
    std::optional<std::int64_t> after;
    // Working orders only: keep one instrument's.
    std::optional<InstrumentId> instrument;
};

template <typename Record>
struct PaperForwardPage {
    std::vector<Record> records;
    // Present when later records exist; the next request's `after`.
    std::optional<std::int64_t> next_after;
};

enum class PaperJournalReadMiss {
    AccountNotFound,
    OrderNotFound,
};

using PaperOrderPageResult = std::variant<
    PaperJournalPage<PaperOrderRecord>,
    PaperJournalReadMiss
>;

using PaperFillPageResult = std::variant<
    PaperJournalPage<PaperFillRecord>,
    PaperJournalReadMiss
>;

using PaperOrderLookupResult = std::variant<
    PaperOrderRecord,
    PaperJournalReadMiss
>;

using PaperWorkingOrderPageResult = std::variant<
    PaperForwardPage<PaperOrderRecord>,
    PaperJournalReadMiss
>;

using PaperOrderFillPageResult = std::variant<
    PaperForwardPage<PaperFillRecord>,
    PaperJournalReadMiss
>;

using PaperOrderEventPageResult = std::variant<
    PaperForwardPage<PaperOrderEventRecord>,
    PaperJournalReadMiss
>;

class PostgresPaperOrderRepository;

struct CreatedDeployment {
    ModelDeploymentRecord deployment;
    // False when an identical deployment already existed.
    bool created{false};
};

// One account's row lock, held by an open transaction, with the account
// recovered inside it. Every mutation of an account (a submission, a match
// of its working orders, a cancellation) runs in one of these, so they are
// serialized per account by PostgreSQL, across connections and processes,
// and each sees every change committed before it. The row lock is always
// the transaction's first lock; the account's other rows are written after
// it, so two such transactions never wait on each other in a cycle.
//
// Obtain one from PostgresPaperOrderRepository::lock_account. It must not
// outlive the repository, and while it exists the repository must not be
// used for anything else. Destroying it without committing rolls back.
class PaperAccountLock final {
public:
    ~PaperAccountLock();
    PaperAccountLock(PaperAccountLock&& other) noexcept;
    PaperAccountLock& operator=(PaperAccountLock&&) = delete;
    PaperAccountLock(const PaperAccountLock&) = delete;
    PaperAccountLock& operator=(const PaperAccountLock&) = delete;

    // The account as recovered under the lock. Operate on it, then commit
    // what changed with exactly one commit call.
    [[nodiscard]] PaperOrderProcessor& processor() noexcept;
    [[nodiscard]] std::int64_t version() const noexcept;

    // Reads in the locked transaction.
    [[nodiscard]] std::optional<PaperOrderRecord> find_order(
        std::string_view client_order_id
    );
    // The order a cancel request ID already cancelled, if any.
    [[nodiscard]] std::optional<std::string> order_cancelled_by(
        std::string_view cancel_request_id
    );

    // Each writes what the processor changed since the lock was taken and
    // commits. The match is the working orders' fills on the submission's
    // observation, which precede the new order. On a conflict nothing is
    // written; the lock is released either way.
    [[nodiscard]] CommitPaperOrderResult commit_submission(
        const WorkingOrderMatch& earlier_orders,
        const OrderIntent& intent,
        const TopOfBookEvent& market,
        const PaperOrderResult& result
    );
    [[nodiscard]] CommitPaperOrderResult commit_match(
        const WorkingOrderMatch& match
    );
    [[nodiscard]] CommitPaperOrderResult commit_cancellation(
        const WorkingOrderCancellation& cancellation,
        std::string_view cancel_request_id
    );

    // Ends the transaction without writing.
    void release();

    // Model deployments (migration 005, ADR 0018).

    // The deployment that owns this account, if any. Read under the account
    // lock, so ownership cannot change before this transaction ends.
    [[nodiscard]] std::optional<std::string> owning_deployment();
    // Locks the deployment's row, then its decision's, after the account's:
    // the global order. Nullopt when either does not exist.
    [[nodiscard]] std::optional<LockedDeploymentDecision> lock_deployment_decision(
        std::string_view deployment_id,
        std::int64_t decision_sequence
    );
    // A deployment's order: what commit_submission writes and, in the same
    // transaction, the pending decision resolved as recorded and the
    // deployment's order counters updated. Either all of it commits or
    // none of it does, so a pending decision never has an order.
    [[nodiscard]] CommitPaperOrderResult commit_deployment_submission(
        const WorkingOrderMatch& earlier_orders,
        const OrderIntent& intent,
        const TopOfBookEvent& market,
        const PaperOrderResult& result,
        std::string_view deployment_id,
        std::int64_t decision_sequence,
        const DeploymentOrderResolution& resolution
    );
    // Resolves the pending decision as abandoned, with why, and commits. No
    // order exists for it, and none can be recorded afterwards.
    void commit_deployment_abandonment(
        std::string_view deployment_id,
        std::int64_t decision_sequence,
        std::string_view reason,
        Timestamp market_time
    );

    // Opaque; defined by the repository.
    struct State;

private:
    friend class PostgresPaperOrderRepository;
    explicit PaperAccountLock(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

class PostgresPaperOrderRepository final {
public:
    // With a lock timeout, waiting longer than that for another session's
    // lock fails the operation with PaperPersistenceUnavailable, which
    // callers already retry, instead of blocking without bound.
    explicit PostgresPaperOrderRepository(
        std::string connection_string,
        std::optional<std::chrono::milliseconds> lock_timeout = std::nullopt
    );
    ~PostgresPaperOrderRepository();

    PostgresPaperOrderRepository(
        const PostgresPaperOrderRepository&
    ) = delete;
    PostgresPaperOrderRepository& operator=(
        const PostgresPaperOrderRepository&
    ) = delete;
    PostgresPaperOrderRepository(
        PostgresPaperOrderRepository&&
    ) = delete;
    PostgresPaperOrderRepository& operator=(
        PostgresPaperOrderRepository&&
    ) = delete;

    void apply_migration(std::string_view sql);

    [[nodiscard]] std::optional<StoredPaperOrderProcessor> load_account(
        std::string_view account_id
    );

    [[nodiscard]] PaperAccountJournalSummary journal_summary(
        std::string_view account_id
    );

    // Journal reads. Each runs in one repeatable-read, read-only snapshot,
    // so a missing account is distinguished from an empty history
    // consistently. Pages fetch at most `limit + 1` rows through an index.
    [[nodiscard]] PaperOrderPageResult list_orders(
        std::string_view account_id,
        const PaperJournalPageRequest& request
    );

    [[nodiscard]] PaperFillPageResult list_fills(
        std::string_view account_id,
        const PaperJournalPageRequest& request
    );

    [[nodiscard]] PaperOrderLookupResult find_order(
        std::string_view account_id,
        std::string_view client_order_id
    );

    // The account's working orders, in priority order.
    [[nodiscard]] PaperWorkingOrderPageResult list_working_orders(
        std::string_view account_id,
        const PaperForwardPageRequest& request
    );

    // One order's fills and lifecycle events, oldest first.
    [[nodiscard]] PaperOrderFillPageResult list_order_fills(
        std::string_view account_id,
        std::string_view client_order_id,
        const PaperForwardPageRequest& request
    );

    [[nodiscard]] PaperOrderEventPageResult list_order_events(
        std::string_view account_id,
        std::string_view client_order_id,
        const PaperForwardPageRequest& request
    );

    // Takes the account's row lock and recovers the account under it;
    // nullopt when the account does not exist.
    [[nodiscard]] std::optional<PaperAccountLock> lock_account(
        std::string_view account_id
    );

    // The healthy accounts on `observation`'s stream with a working order
    // that the observation could fill: accepted before it, on its
    // instrument, and marketable at its opposite best. In account order.
    // A candidate is only a hint; matching decides under the account lock.
    [[nodiscard]] std::vector<std::string> accounts_to_match(
        const TopOfBookEvent& observation
    );

    // Probes the connection, reconnecting once if an earlier operation lost
    // it. Returns false only when PostgreSQL is unavailable.
    [[nodiscard]] bool check_availability();

    // Model deployments (migration 005, ADR 0018). Each runs in its own
    // transaction; a write locks the deployment's row first.

    // Creates the deployment, stopped, together with its new account, or
    // returns the existing deployment when one with this ID was created
    // from the identical request.
    [[nodiscard]] std::variant<CreatedDeployment, DeploymentConflict> create_deployment(
        const ModelDeploymentConfig& config
    );
    [[nodiscard]] std::optional<ModelDeploymentRecord> find_deployment(
        std::string_view deployment_id
    );
    // Newest first, continuing with those created before
    // `before_sequence`.
    [[nodiscard]] DeploymentPage list_deployments(
        std::size_t limit,
        std::optional<std::int64_t> before_sequence
    );
    // Moves the deployment from one of `from` to `to`, with a reason. A
    // start (`begin_run`) also begins a new run generation and resets the
    // run's order counters.
    [[nodiscard]] std::variant<ModelDeploymentRecord, DeploymentConflict> transition_deployment(
        std::string_view deployment_id,
        const std::vector<DeploymentState>& from,
        DeploymentState to,
        std::string_view reason,
        bool begin_run
    );
    // Records a decision under the deployment's row lock: only while it is
    // running in this generation, with no decision pending, and for an
    // observation strictly after the watermark, which advances. An order
    // decision is pending, with its deterministic client order ID and its
    // eligibility window; a hold is resolved at once.
    [[nodiscard]] std::variant<DeploymentDecisionRecord, DeploymentConflict> record_decision(
        const NewDeploymentDecision& decision
    );
    // Newest first.
    [[nodiscard]] DecisionPage list_decisions(
        std::string_view deployment_id,
        std::size_t limit,
        std::optional<std::int64_t> before_sequence
    );
    [[nodiscard]] std::vector<DeploymentDecisionRecord> pending_decisions();
    // The deployment owning an account, read outside any lock, for display.
    [[nodiscard]] std::optional<std::string> account_owner(std::string_view account_id);

private:
    friend class PersistentPaperOrderSession;
    friend class PaperAccountLock;

    [[nodiscard]] CreatePaperAccountResult create_account(
        const PaperOrderProcessorRecoveryState& state
    );

    // Writes the account row and only the position and execution rows that
    // differ between `state_before`, the state at `expected_version`, and
    // `state_after`, together with the working orders' fills, the journal
    // record, and their events, in one transaction.
    [[nodiscard]] CommitPaperOrderResult commit_order(
        std::string_view account_id,
        std::int64_t expected_version,
        const WorkingOrderMatch& earlier_orders,
        const OrderIntent& intent,
        const TopOfBookEvent& market,
        const PaperOrderResult& result,
        const PaperOrderProcessorRecoveryState& state_before,
        const PaperOrderProcessorRecoveryState& state_after
    );

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PersistentPaperOrderReceipt {
    PaperOrderResult result;
    std::int64_t account_version;
    // Earlier working orders filled by the order's observation first.
    WorkingOrderMatch earlier_orders;
};

using PersistentPaperOrderResult = std::variant<
    PersistentPaperOrderReceipt,
    PaperPersistenceConflict
>;

class PersistentPaperOrderSession final {
public:
    [[nodiscard]] static std::variant<
        PersistentPaperOrderSession,
        PaperPersistenceConflict
    > create(
        PostgresPaperOrderRepository& repository,
        std::string account_id,
        Money starting_cash,
        PreTradeRiskLimits risk_limits
    );

    [[nodiscard]] static std::optional<PersistentPaperOrderSession> load(
        PostgresPaperOrderRepository& repository,
        std::string_view account_id
    );

    PersistentPaperOrderSession(const PersistentPaperOrderSession&) = delete;
    PersistentPaperOrderSession& operator=(
        const PersistentPaperOrderSession&
    ) = delete;
    PersistentPaperOrderSession(PersistentPaperOrderSession&&) noexcept =
        default;
    PersistentPaperOrderSession& operator=(
        PersistentPaperOrderSession&&
    ) noexcept = default;

    [[nodiscard]] const PaperAccount& account() const noexcept {
        return processor_.account();
    }

    [[nodiscard]] const PreTradeRiskLimits& risk_limits() const noexcept {
        return processor_.risk_limits();
    }

    [[nodiscard]] bool is_healthy() const noexcept {
        return processor_.is_healthy();
    }

    [[nodiscard]] const InstrumentExecutionStates& executions()
        const noexcept {
        return processor_.executions();
    }

    [[nodiscard]] std::int64_t version() const noexcept { return version_; }

    [[nodiscard]] const std::vector<WorkingOrder>& working_orders()
        const noexcept {
        return processor_.working_orders();
    }

    [[nodiscard]] const AccountReservations& reservations() const noexcept {
        return processor_.reservations();
    }

    // Offers `market` to the working orders first, at the order's
    // submission time, then processes the order; both commit together.
    [[nodiscard]] PersistentPaperOrderResult submit(
        const OrderIntent& intent,
        const TopOfBookEvent& market
    );

private:
    PersistentPaperOrderSession(
        PostgresPaperOrderRepository& repository,
        PaperOrderProcessor processor,
        std::int64_t version
    );

    PostgresPaperOrderRepository* repository_;
    PaperOrderProcessor processor_;
    std::int64_t version_;
};

}  // namespace chronos
