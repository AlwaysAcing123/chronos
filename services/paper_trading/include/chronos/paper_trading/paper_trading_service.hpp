#pragma once

#include "chronos/market_state.hpp"
#include "chronos/postgresql_paper_order_repository.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chronos::paper_trading {

// Identifiers accepted from clients: 1 to 64 characters from
// [A-Za-z0-9._-], starting with a letter or digit. The rule keeps
// identifiers unambiguous in URL paths, logs, and PostgreSQL text parameters.
inline constexpr std::size_t kMaximumIdentifierLength = 64;

[[nodiscard]] bool is_valid_identifier(std::string_view value) noexcept;

struct PaperTradingServiceConfig {
    std::vector<InstrumentId> tradeable_instruments;
    // Applied to every new account and persisted with it. Changing the
    // configuration later does not alter existing accounts.
    PreTradeRiskLimits account_risk_limits;
};

struct CreatePaperAccountRequest {
    std::string account_id;
    std::string instrument;
    Money starting_cash;
};

// Everything a client controls about an order. The service adds the
// submission time from the market clock and the execution quote from market
// state, so a client can choose neither. Manual and model orders differ only
// in their source.
struct PaperOrderRequest {
    std::string account_id;
    std::string client_order_id;
    OrderSource source;
    std::string instrument;
    Side side{Side::Buy};
    OrderType type{OrderType::Market};
    Quantity quantity;
    std::optional<Price> limit_price;
    TimeInForce time_in_force{TimeInForce::ImmediateOrCancel};
};

enum class PaperTradingErrorCode {
    InvalidIdentifier,
    UnsupportedInstrument,
    InvalidStartingCash,
    AccountAlreadyExists,
    AccountNotFound,
    MarketDataUnavailable,
    DuplicateClientOrderId,
    AccountVersionConflict,
    PersistenceUnavailable,
};

// A request the service did not record. Retrying an order with the same
// client order ID is safe after any of these errors; after
// PersistenceUnavailable the database accepts that ID at most once.
struct PaperTradingError {
    PaperTradingErrorCode code;
    std::string field;
    std::string message;
    std::optional<MarketStateUnavailableCode> market_state;
};

struct PaperAccountView {
    std::string account_id;
    InstrumentId instrument;
    AccountState state;
    PreTradeRiskLimits risk_limits;
    bool is_healthy;
    std::int64_t version;
};

// A submission committed to the durable journal, whatever its outcome. Its
// client order ID is consumed even when risk or execution rejected it.
struct RecordedPaperOrder {
    OrderIntent intent;
    TopOfBookEvent market;
    PaperOrderResult result;
    PaperAccountView account;
};

using PaperAccountResult = std::variant<PaperAccountView, PaperTradingError>;

using PaperOrderSubmissionResult = std::variant<
    RecordedPaperOrder,
    PaperTradingError
>;

struct PaperTradingHealth {
    bool persistence_available;
    MarketStateStatus market;
};

// The application boundary shared by the HTTP API and future transports.
//
// The service exclusively owns one PostgreSQL connection and serializes every
// operation on it. Sessions are request-scoped: each operation loads the
// account from a consistent database snapshot, so the database is the only
// source of account truth between requests and optimistic concurrency
// resolves races with other processes. `market_state` must outlive the
// service.
class PaperTradingService final {
public:
    PaperTradingService(
        std::unique_ptr<PostgresPaperOrderRepository> repository,
        const MarketState& market_state,
        PaperTradingServiceConfig config
    );

    PaperTradingService(const PaperTradingService&) = delete;
    PaperTradingService& operator=(const PaperTradingService&) = delete;
    PaperTradingService(PaperTradingService&&) = delete;
    PaperTradingService& operator=(PaperTradingService&&) = delete;

    [[nodiscard]] PaperAccountResult create_account(
        const CreatePaperAccountRequest& request
    );

    [[nodiscard]] PaperAccountResult get_account(std::string_view account_id);

    [[nodiscard]] PaperOrderSubmissionResult submit_order(
        const PaperOrderRequest& request
    );

    [[nodiscard]] PaperTradingHealth health();

private:
    std::mutex mutex_;
    std::unique_ptr<PostgresPaperOrderRepository> repository_;
    const MarketState& market_state_;
    PaperTradingServiceConfig config_;
};

}  // namespace chronos::paper_trading
