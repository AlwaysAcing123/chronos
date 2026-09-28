#pragma once

#include "chronos/paper_order_processor.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

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
