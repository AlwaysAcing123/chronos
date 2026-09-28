#pragma once

#include "chronos/paper_order_processor.hpp"

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

struct PaperOrderRecord {
    // Journal identity. Within one account it increases in commit order,
    // because every order record is inserted while the account row is locked.
    std::int64_t record_id;
    OrderIntent intent;
    TopOfBookEvent market;
    PaperOrderRecordOutcome outcome;
};

// A fill that accounting booked, linked to the order that produced it.
struct PaperFillRecord {
    std::int64_t record_id;
    std::string account_id;
    std::string client_order_id;
    OrderSource source;
    Fill fill;
};

// A newest-first page. `before_record_id` excludes that record and every
// newer one, so a page chain stays stable while new orders are recorded.
struct PaperJournalPageRequest {
    std::size_t limit;
    std::optional<std::int64_t> before_record_id;
};

template <typename Record>
struct PaperJournalPage {
    std::vector<Record> records;
    // Present when older records exist; the next request's before_record_id.
    std::optional<std::int64_t> next_before_record_id;
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

class PostgresPaperOrderRepository final {
public:
    explicit PostgresPaperOrderRepository(std::string connection_string);
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

    // Probes the connection, reconnecting once if an earlier operation lost
    // it. Returns false only when PostgreSQL is unavailable.
    [[nodiscard]] bool check_availability();

private:
    friend class PersistentPaperOrderSession;

    [[nodiscard]] CreatePaperAccountResult create_account(
        const PaperOrderProcessorRecoveryState& state
    );

    [[nodiscard]] CommitPaperOrderResult commit_order(
        std::string_view account_id,
        std::int64_t expected_version,
        const OrderIntent& intent,
        const TopOfBookEvent& market,
        const PaperOrderResult& result,
        const PaperOrderProcessorRecoveryState& state_after
    );

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PersistentPaperOrderReceipt {
    PaperOrderResult result;
    std::int64_t account_version;
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
        InstrumentId instrument,
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

    [[nodiscard]] std::int64_t version() const noexcept { return version_; }

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
