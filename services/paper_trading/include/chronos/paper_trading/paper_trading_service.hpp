#pragma once

#include "chronos/market_data/market_event_gate.hpp"
#include "chronos/market_state.hpp"
#include "chronos/portfolio_valuation.hpp"
#include "chronos/postgresql_paper_order_repository.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    // Every account may trade any of these, on the server's one market
    // stream.
    std::vector<InstrumentId> tradeable_instruments;
    // Applied to every new account and persisted with it. Changing the
    // configuration later does not alter existing accounts.
    PreTradeRiskLimits account_risk_limits;
    // How long a submission waits for the market-event gate while an event
    // is being applied and matched.
    std::chrono::milliseconds order_matching_wait{2'000};
};

// An account holds cash and a position per instrument it has traded, so it
// is not created for any one instrument.
struct CreatePaperAccountRequest {
    std::string account_id;
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
    InvalidPageLimit,
    InvalidCursor,
    AccountAlreadyExists,
    AccountNotFound,
    OrderNotFound,
    MarketDataUnavailable,
    DuplicateClientOrderId,
    AccountVersionConflict,
    ValuationFailed,
    PersistenceUnavailable,
    // The order has no working remainder, and this cancel request did not
    // cancel it.
    OrderNotCancellable,
    // The cancel request ID already cancelled another order.
    DuplicateCancelRequestId,
    // An event was still being applied and matched when the wait ended;
    // nothing was recorded.
    OrderMatchingBusy,
};

// A request the service did not record. Retrying an order with the same
// client order ID is safe after any of these errors; after
// PersistenceUnavailable the database accepts that ID at most once.
struct PaperTradingError {
    PaperTradingErrorCode code;
    std::string field;
    std::string message;
    // Why market data was unavailable: MarketState could not supply a
    // quote, or the quote could not mark this account's portfolio.
    std::optional<MarketStateUnavailableCode> market_state{};
    // The valuation rule that failed, for MarketDataUnavailable raised by
    // valuation and for ValuationFailed.
    std::optional<PortfolioValuationErrorCode> valuation{};
    // For PositionMarkUnavailable: every open position that cannot be
    // marked, in instrument order, and why.
    std::vector<PositionMarkFailure> instruments{};
    // For OrderNotCancellable: the order as it is now.
    std::optional<PaperOrderRecord> order{};
};

struct PaperAccountView {
    std::string account_id;
    // Cash and every instrument ledger, in instrument order. A closed
    // position keeps its ledger at zero quantity.
    AccountState state;
    // The stream the account executes on, fixed by its first executed order.
    std::optional<MarketDataStreamId> market_stream;
    PreTradeRiskLimits risk_limits;
    bool is_healthy;
    std::int64_t version;
    // What working orders hold back from new orders, and how many there are.
    AccountReservations reservations;
    std::size_t working_orders{0};
};

// A submission committed to the durable journal, whatever its outcome. Its
// client order ID is consumed even when risk or execution rejected it.
struct RecordedPaperOrder {
    OrderIntent intent;
    TopOfBookEvent market;
    PaperOrderResult result;
    PaperAccountView account;
    // The order as recorded, read back after the commit.
    PaperOrderRecord order;
};

// Cancels the working remainder of one order. The cancel request ID is the
// request's idempotency key: retrying it returns the order it cancelled.
struct CancelPaperOrderRequest {
    std::string account_id;
    std::string client_order_id;
    std::string cancel_request_id;
};

struct CancelledPaperOrder {
    // The order as it is now.
    PaperOrderRecord order;
    PaperAccountView account;
    // This request had already cancelled the order; nothing was written.
    bool already_cancelled;
};

using CancelPaperOrderResult = std::variant<
    CancelledPaperOrder,
    PaperTradingError
>;

using PaperAccountResult = std::variant<PaperAccountView, PaperTradingError>;

using PaperOrderSubmissionResult = std::variant<
    RecordedPaperOrder,
    PaperTradingError
>;

// A read-only valuation of a durable account against one consistent read of
// the server's quotes for the instruments it holds. Financial fields come
// from `valuation`, whose snapshot holds the same account state as
// `account`.
struct PaperPortfolioView {
    PaperAccountView account;
    PortfolioValuation valuation;
};

using PaperPortfolioResult = std::variant<
    PaperPortfolioView,
    PaperTradingError
>;

// History pages are newest first. See docs/paper-trading-api.md.
inline constexpr std::size_t kDefaultHistoryPageLimit = 50;
inline constexpr std::size_t kMaximumHistoryPageLimit = 200;

struct HistoryPageRequest {
    // Absent means kDefaultHistoryPageLimit.
    std::optional<std::size_t> limit;
    // Absent means the newest page; otherwise the next_cursor of a previous
    // page of the same account, kind, and instrument filter.
    std::optional<std::string> cursor;
    // Absent means every instrument; otherwise only the records for this
    // one. Any valid identifier is accepted, so the history of a symbol no
    // longer configured stays readable.
    std::optional<std::string> instrument;
};

struct PaperOrderHistoryPage {
    std::vector<PaperOrderRecord> orders;
    std::optional<std::string> next_cursor;
};

struct PaperFillHistoryPage {
    std::vector<PaperFillRecord> fills;
    std::optional<std::string> next_cursor;
};

using PaperOrderHistoryResult = std::variant<
    PaperOrderHistoryPage,
    PaperTradingError
>;

using PaperOrderRecordResult = std::variant<
    PaperOrderRecord,
    PaperTradingError
>;

using PaperFillHistoryResult = std::variant<
    PaperFillHistoryPage,
    PaperTradingError
>;

// An oldest-first page: working orders in priority order, or one order's
// fills or lifecycle events in the order they happened.
struct ForwardPageRequest {
    // Absent means kDefaultHistoryPageLimit.
    std::optional<std::size_t> limit;
    // Absent means the first page; otherwise the next_cursor of a previous
    // page of the same resource.
    std::optional<std::string> cursor;
    // Working orders only: keep one instrument's.
    std::optional<std::string> instrument;
};

struct WorkingOrderPage {
    std::vector<PaperOrderRecord> orders;
    std::optional<std::string> next_cursor;
};

struct OrderFillPage {
    std::vector<PaperFillRecord> fills;
    std::optional<std::string> next_cursor;
};

struct OrderEventPage {
    std::vector<PaperOrderEventRecord> events;
    std::optional<std::string> next_cursor;
};

using WorkingOrderPageResult = std::variant<WorkingOrderPage, PaperTradingError>;
using OrderFillPageResult = std::variant<OrderFillPage, PaperTradingError>;
using OrderEventPageResult = std::variant<OrderEventPage, PaperTradingError>;

// Whether an order for one tradeable instrument could execute now, and
// against which quote. The quote is a reference for clients: an order
// executes against the server's quote when it is submitted.
struct InstrumentAvailability {
    InstrumentId instrument;
    // Why no execution quote is available, or nullopt when one is.
    std::optional<MarketStateUnavailableCode> unavailable;
    // The current execution quote, exactly when one is available.
    std::optional<TopOfBookEvent> quote;
};

struct MarketAvailability {
    MarketStateStatus stream;
    std::vector<InstrumentAvailability> instruments;
};

struct PaperTradingHealth {
    bool persistence_available;
    MarketStateStatus market;
    // The instruments new accounts may trade, in configuration order, so a
    // client can offer them without hard-coding server configuration.
    std::vector<InstrumentId> tradeable_instruments;
};

// The application boundary shared by the HTTP API and future transports.
//
// The service exclusively owns one PostgreSQL connection and serializes every
// operation on it. Sessions are request-scoped: each read loads the account
// from a consistent database snapshot, so the database is the only source of
// account truth between requests. A submission or cancellation takes the
// account's row lock and recovers the account under it, so it is serialized
// with the working-order matcher, which uses its own connection, and with
// other processes.
//
// With a market-event gate (see MarketEventGate), a submission holds it
// from before the account lock until its commit, so its quote and every
// event before it were already matched, and every later event is offered
// to the order it records. `market_state` and `gate` must outlive the
// service.
class PaperTradingService final {
public:
    using ActivityCallback = std::function<void()>;

    // `on_activity` runs after a submission or cancellation commits.
    PaperTradingService(
        std::unique_ptr<PostgresPaperOrderRepository> repository,
        const MarketState& market_state,
        PaperTradingServiceConfig config,
        ActivityCallback on_activity = {},
        market_data::MarketEventGate* gate = nullptr
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

    // Needs no market data and works on a halted account. The cancellation
    // time is the market clock, or the order's latest change if the clock
    // is behind it or has not started.
    [[nodiscard]] CancelPaperOrderResult cancel_order(
        const CancelPaperOrderRequest& request
    );

    // Read the durable journal only; nothing is re-evaluated. An order that
    // returned an uncertain 500 or 503 can be reconciled with get_order.
    [[nodiscard]] PaperOrderHistoryResult list_orders(
        std::string_view account_id,
        const HistoryPageRequest& request
    );

    [[nodiscard]] PaperOrderRecordResult get_order(
        std::string_view account_id,
        std::string_view client_order_id
    );

    [[nodiscard]] PaperFillHistoryResult list_fills(
        std::string_view account_id,
        const HistoryPageRequest& request
    );

    [[nodiscard]] WorkingOrderPageResult list_working_orders(
        std::string_view account_id,
        const ForwardPageRequest& request
    );

    [[nodiscard]] OrderFillPageResult list_order_fills(
        std::string_view account_id,
        std::string_view client_order_id,
        const ForwardPageRequest& request
    );

    [[nodiscard]] OrderEventPageResult list_order_events(
        std::string_view account_id,
        std::string_view client_order_id,
        const ForwardPageRequest& request
    );

    // Marks every open position at the server's current best bid, read
    // together with the market clock. Only held instruments need a quote;
    // any that cannot be marked fails the whole valuation. Reads only:
    // nothing is journaled, and no liquidity or client order ID is consumed.
    [[nodiscard]] PaperPortfolioResult get_portfolio(
        std::string_view account_id
    );

    [[nodiscard]] PaperTradingHealth health();

    // Reads only the market state, so it neither waits for the database
    // nor takes the service lock; browsers refresh it on every change.
    [[nodiscard]] MarketAvailability market_availability() const;

private:
    void notify_activity() const;

    // Lock order: submission_mutex_, the market-event gate, mutex_, then an
    // account row. mutex_ serializes use of the one connection; only
    // submissions take the other two.
    std::timed_mutex submission_mutex_;
    std::mutex mutex_;
    std::unique_ptr<PostgresPaperOrderRepository> repository_;
    const MarketState& market_state_;
    PaperTradingServiceConfig config_;
    ActivityCallback on_activity_;
    market_data::MarketEventGate* gate_;
};

}  // namespace chronos::paper_trading
