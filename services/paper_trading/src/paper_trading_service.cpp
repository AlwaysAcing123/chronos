#include "chronos/paper_trading/paper_trading_service.hpp"

#include "chronos/paper_trading/history_cursor.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::paper_trading {
namespace {

bool is_ascii_alphanumeric(char character) noexcept {
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9');
}

PaperTradingError error(
    PaperTradingErrorCode code,
    std::string field,
    std::string message
) {
    return PaperTradingError{
        .code = code,
        .field = std::move(field),
        .message = std::move(message),
        .market_state = std::nullopt,
    };
}

std::optional<PaperTradingError> invalid_identifier(
    std::string_view value,
    std::string_view field
) {
    if (is_valid_identifier(value)) {
        return std::nullopt;
    }
    return error(
        PaperTradingErrorCode::InvalidIdentifier,
        std::string(field),
        std::string(field) +
            " must be 1 to 64 characters from [A-Za-z0-9._-] starting with "
            "a letter or digit"
    );
}

PaperTradingError account_not_found() {
    return error(
        PaperTradingErrorCode::AccountNotFound,
        "account_id",
        "paper account does not exist"
    );
}

PaperTradingError market_data_unavailable(
    const MarketStateUnavailable& unavailable
) {
    PaperTradingError failure = error(
        PaperTradingErrorCode::MarketDataUnavailable,
        "",
        unavailable.message
    );
    failure.market_state = unavailable.code;
    return failure;
}

// A market that cannot mark the account is unavailable market data; an
// account that cannot be valued exactly is a valuation failure.
PaperTradingErrorCode service_code(PortfolioValuationErrorCode code) {
    switch (code) {
        case PortfolioValuationErrorCode::InvalidMarketState:
        case PortfolioValuationErrorCode::MarketInstrumentMismatch:
        case PortfolioValuationErrorCode::SuspectMarketState:
        case PortfolioValuationErrorCode::MissingBestBid:
        case PortfolioValuationErrorCode::StaleMarketState:
        case PortfolioValuationErrorCode::MarketTimelineMismatch:
        case PortfolioValuationErrorCode::PositionMarkUnavailable:
            return PaperTradingErrorCode::MarketDataUnavailable;
        case PortfolioValuationErrorCode::NonpositiveStartingCash:
        case PortfolioValuationErrorCode::ArithmeticOverflow:
        case PortfolioValuationErrorCode::AccountingIdentityFailed:
            return PaperTradingErrorCode::ValuationFailed;
    }
    throw std::logic_error("unknown portfolio valuation error");
}

PaperTradingError portfolio_valuation_failure(
    PortfolioValuationError failure
) {
    PaperTradingError mapped = error(
        service_code(failure.code),
        "",
        std::move(failure.message)
    );
    mapped.valuation = failure.code;
    mapped.instruments = std::move(failure.instruments);
    return mapped;
}

PaperTradingError persistence_unavailable(
    const PaperPersistenceUnavailable& failure
) {
    return error(
        PaperTradingErrorCode::PersistenceUnavailable,
        "",
        failure.what()
    );
}

PaperTradingError persistence_conflict(PaperPersistenceConflict conflict) {
    switch (conflict.code) {
        case PaperPersistenceConflictCode::AccountAlreadyExists:
            return error(
                PaperTradingErrorCode::AccountAlreadyExists,
                "account_id",
                std::move(conflict.message)
            );
        case PaperPersistenceConflictCode::AccountNotFound:
            return error(
                PaperTradingErrorCode::AccountNotFound,
                "account_id",
                std::move(conflict.message)
            );
        case PaperPersistenceConflictCode::DuplicateClientOrderId:
            return error(
                PaperTradingErrorCode::DuplicateClientOrderId,
                "client_order_id",
                std::move(conflict.message)
            );
        case PaperPersistenceConflictCode::VersionConflict:
            return error(
                PaperTradingErrorCode::AccountVersionConflict,
                "",
                std::move(conflict.message)
            );
        case PaperPersistenceConflictCode::DuplicateCancelRequestId:
            return error(
                PaperTradingErrorCode::DuplicateCancelRequestId,
                "cancel_request_id",
                std::move(conflict.message)
            );
    }
    throw std::logic_error("unknown paper persistence conflict");
}

PaperTradingError order_matching_busy() {
    return error(
        PaperTradingErrorCode::OrderMatchingBusy,
        "",
        "working orders are still being matched against the latest market "
        "event"
    );
}

PaperTradingError order_not_found() {
    return error(
        PaperTradingErrorCode::OrderNotFound,
        "client_order_id",
        "no order with this client_order_id is recorded for the account"
    );
}

// Validates a page request and resolves its cursor to a journal position.
std::variant<PaperJournalPageRequest, PaperTradingError> journal_page_request(
    std::string_view account_id,
    const HistoryPageRequest& request,
    HistoryCursorKind kind
) {
    if (request.instrument) {
        if (auto invalid = invalid_identifier(*request.instrument, "instrument")) {
            return *invalid;
        }
    }
    const std::optional<std::string_view> instrument = request.instrument
        ? std::optional<std::string_view>(*request.instrument)
        : std::nullopt;
    const std::size_t limit = request.limit.value_or(kDefaultHistoryPageLimit);
    if (limit == 0 || limit > kMaximumHistoryPageLimit) {
        return error(
            PaperTradingErrorCode::InvalidPageLimit,
            "limit",
            "limit must be from 1 to " +
                std::to_string(kMaximumHistoryPageLimit)
        );
    }
    std::optional<std::int64_t> before_record_id;
    if (request.cursor) {
        before_record_id = decode_history_cursor(
            *request.cursor,
            kind,
            account_id,
            instrument
        );
        if (!before_record_id) {
            return error(
                PaperTradingErrorCode::InvalidCursor,
                "cursor",
                "cursor was not issued for this history of this account"
            );
        }
    }
    return PaperJournalPageRequest{
        .limit = limit,
        .before_record_id = before_record_id,
        .instrument = request.instrument
            ? std::optional<InstrumentId>(InstrumentId(*request.instrument))
            : std::nullopt,
    };
}

std::optional<std::string> next_cursor(
    HistoryCursorKind kind,
    std::string_view account_id,
    const std::optional<std::string>& instrument,
    const std::optional<std::int64_t>& next_before_record_id
) {
    if (!next_before_record_id) {
        return std::nullopt;
    }
    return encode_history_cursor(
        kind,
        account_id,
        instrument ? std::optional<std::string_view>(*instrument)
                   : std::nullopt,
        *next_before_record_id
    );
}

PaperAccountView account_view(
    const PaperOrderProcessor& processor,
    std::int64_t version
) {
    return PaperAccountView{
        .account_id = processor.account().account_id(),
        .state = processor.account().state(),
        .market_stream = bound_market_stream(processor.executions()),
        .risk_limits = processor.risk_limits(),
        .is_healthy = processor.is_healthy(),
        .version = version,
        .reservations = processor.reservations(),
        .working_orders = processor.working_orders().size(),
    };
}

PaperAccountView account_view(const PersistentPaperOrderSession& session) {
    return PaperAccountView{
        .account_id = session.account().account_id(),
        .state = session.account().state(),
        .market_stream = bound_market_stream(session.executions()),
        .risk_limits = session.risk_limits(),
        .is_healthy = session.is_healthy(),
        .version = session.version(),
        .reservations = session.reservations(),
        .working_orders = session.working_orders().size(),
    };
}

PaperTradingError order_not_cancellable(PaperOrderRecord order) {
    PaperTradingError failure = error(
        PaperTradingErrorCode::OrderNotCancellable,
        "client_order_id",
        "the order has no working remainder to cancel"
    );
    failure.order = std::move(order);
    return failure;
}

PaperTradingError invalid_cursor() {
    return error(
        PaperTradingErrorCode::InvalidCursor,
        "cursor",
        "cursor was not issued for this history of this account"
    );
}

std::optional<PaperTradingError> invalid_page_limit(std::size_t limit) {
    if (limit != 0 && limit <= kMaximumHistoryPageLimit) {
        return std::nullopt;
    }
    return error(
        PaperTradingErrorCode::InvalidPageLimit,
        "limit",
        "limit must be from 1 to " + std::to_string(kMaximumHistoryPageLimit)
    );
}

// Validates a per-order page request and resolves its cursor.
std::variant<PaperForwardPageRequest, PaperTradingError> order_page_request(
    std::string_view account_id,
    std::string_view client_order_id,
    const ForwardPageRequest& request,
    OrderCursorKind kind
) {
    if (request.instrument) {
        throw std::invalid_argument("an order's pages have no instrument filter");
    }
    const std::size_t limit = request.limit.value_or(kDefaultHistoryPageLimit);
    if (auto invalid = invalid_page_limit(limit)) {
        return *invalid;
    }
    std::optional<std::int64_t> after;
    if (request.cursor) {
        after = decode_order_cursor(
            *request.cursor,
            kind,
            account_id,
            client_order_id
        );
        if (!after) {
            return invalid_cursor();
        }
    }
    return PaperForwardPageRequest{
        .limit = limit,
        .after = after,
        .instrument = std::nullopt,
    };
}

void validate_config(const PaperTradingServiceConfig& config) {
    if (config.tradeable_instruments.empty()) {
        throw std::invalid_argument(
            "paper-trading service requires a tradeable instrument"
        );
    }
    std::set<InstrumentId> unique_instruments;
    for (const InstrumentId& instrument : config.tradeable_instruments) {
        if (!is_valid_identifier(instrument.value())) {
            throw std::invalid_argument(
                "tradeable instrument is not a valid identifier"
            );
        }
        if (!unique_instruments.insert(instrument).second) {
            throw std::invalid_argument(
                "tradeable instruments must be unique"
            );
        }
    }
    static_cast<void>(PreTradeRiskEngine(config.account_risk_limits));
}

}  // namespace

bool is_valid_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumIdentifierLength ||
        !is_ascii_alphanumeric(value.front())) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char character) {
        return is_ascii_alphanumeric(character) || character == '.' ||
            character == '_' || character == '-';
    });
}

PaperTradingService::PaperTradingService(
    std::unique_ptr<PostgresPaperOrderRepository> repository,
    const MarketState& market_state,
    PaperTradingServiceConfig config,
    ActivityCallback on_activity,
    market_data::MarketEventGate* gate
)
    : repository_(std::move(repository)),
      market_state_(market_state),
      config_(std::move(config)),
      on_activity_(std::move(on_activity)),
      gate_(gate) {
    if (repository_ == nullptr) {
        throw std::invalid_argument(
            "paper-trading service requires a repository"
        );
    }
    validate_config(config_);
}

PaperAccountResult PaperTradingService::create_account(
    const CreatePaperAccountRequest& request
) {
    if (auto invalid = invalid_identifier(request.account_id, "account_id")) {
        return *invalid;
    }
    if (request.starting_cash.raw() <= 0) {
        return error(
            PaperTradingErrorCode::InvalidStartingCash,
            "starting_cash",
            "starting cash must be positive"
        );
    }

    const std::lock_guard lock(mutex_);
    try {
        auto created = PersistentPaperOrderSession::create(
            *repository_,
            request.account_id,
            request.starting_cash,
            config_.account_risk_limits
        );
        if (auto* conflict = std::get_if<PaperPersistenceConflict>(&created)) {
            return persistence_conflict(std::move(*conflict));
        }
        return account_view(std::get<PersistentPaperOrderSession>(created));
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

PaperAccountResult PaperTradingService::get_account(
    std::string_view account_id
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }

    const std::lock_guard lock(mutex_);
    try {
        const auto session = PersistentPaperOrderSession::load(
            *repository_,
            account_id
        );
        if (!session) {
            return account_not_found();
        }
        return account_view(*session);
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

void PaperTradingService::notify_activity() const {
    if (on_activity_) {
        on_activity_();
    }
}

PaperOrderSubmissionResult PaperTradingService::submit_order(
    const PaperOrderRequest& request
) {
    for (const auto& [value, field] : {
             std::pair<std::string_view, std::string_view>{
                 request.account_id,
                 "account_id",
             },
             {request.client_order_id, "client_order_id"},
             {request.source.source_id, "source.id"},
             {request.instrument, "instrument"},
         }) {
        if (auto invalid = invalid_identifier(value, field)) {
            return *invalid;
        }
    }
    // Checked before anything is recorded: an order for an instrument the
    // server does not trade has no quote to execute or journal against.
    const InstrumentId instrument(request.instrument);
    if (std::find(
            config_.tradeable_instruments.begin(),
            config_.tradeable_instruments.end(),
            instrument
        ) == config_.tradeable_instruments.end()) {
        return error(
            PaperTradingErrorCode::UnsupportedInstrument,
            "instrument",
            "instrument is not tradeable on this server"
        );
    }

    // Submissions wait for the market one at a time, and without the
    // service lock: a queue of submissions keeps the feed waiting for at
    // most one of them, and reads and cancellations never wait for the gate.
    // The whole wait is bounded by one deadline.
    const auto deadline =
        std::chrono::steady_clock::now() + config_.order_matching_wait;
    const std::unique_lock submitting(submission_mutex_, deadline);
    if (!submitting.owns_lock()) {
        return order_matching_busy();
    }
    // Before the account lock, as the feed takes it: no event is applied or
    // matched from here until the order is recorded and read back.
    std::optional<std::shared_lock<market_data::MarketEventGate::Mutex>> market;
    if (gate_ != nullptr) {
        market = gate_->hold_market(deadline);
        if (!market) {
            return order_matching_busy();
        }
    }
    const std::lock_guard lock(mutex_);
    try {
        std::optional<PaperAccountLock> locked =
            repository_->lock_account(request.account_id);
        if (!locked) {
            return account_not_found();
        }

        // The durable journal records the observation every order used, so
        // an order cannot be recorded until the server has a quote for its
        // instrument. No other instrument's quote is read.
        ExecutionQuoteResult quote_result =
            market_state_.execution_quote(instrument);
        if (const auto* unavailable =
                std::get_if<MarketStateUnavailable>(&quote_result)) {
            locked->release();
            return market_data_unavailable(*unavailable);
        }
        ExecutionQuote quote =
            std::get<ExecutionQuote>(std::move(quote_result));

        OrderIntent intent{
            .client_order_id = request.client_order_id,
            .account_id = request.account_id,
            .source = request.source,
            .instrument = instrument,
            .side = request.side,
            .type = request.type,
            .quantity = request.quantity,
            .limit_price = request.limit_price,
            .time_in_force = request.time_in_force,
            .submitted_at = quote.market_time,
        };
        PaperOrderProcessor& processor = locked->processor();
        // The matcher already offered this quote to every working order
        // that existed when it was applied, so the new order simply follows
        // them; it is offered no working order's liquidity twice.
        const WorkingOrderMatch earlier_orders{
            .observation = quote.top_of_book,
            .fills = {},
            .skipped = {},
            .fault = std::nullopt,
        };
        PaperOrderResult result = processor.submit(intent, quote.top_of_book);
        CommitPaperOrderResult committed = locked->commit_submission(
            earlier_orders,
            intent,
            quote.top_of_book,
            result
        );
        if (auto* conflict = std::get_if<PaperPersistenceConflict>(&committed)) {
            return persistence_conflict(std::move(*conflict));
        }
        PaperAccountView account =
            account_view(processor, std::get<std::int64_t>(committed));
        notify_activity();
        // The response describes the order as recorded, read back while the
        // market is still held, so no fill can come between it and the
        // account above. If this read fails, the order is still recorded,
        // and the client reconciles with get_order as after any uncertain
        // result.
        auto recorded = repository_->find_order(
            request.account_id,
            request.client_order_id
        );
        market.reset();
        if (!std::holds_alternative<PaperOrderRecord>(recorded)) {
            throw std::runtime_error("a committed order could not be read back");
        }
        return RecordedPaperOrder{
            .intent = std::move(intent),
            .market = std::move(quote.top_of_book),
            .result = std::move(result),
            .account = std::move(account),
            .order = std::get<PaperOrderRecord>(std::move(recorded)),
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

CancelPaperOrderResult PaperTradingService::cancel_order(
    const CancelPaperOrderRequest& request
) {
    for (const auto& [value, field] : {
             std::pair<std::string_view, std::string_view>{
                 request.account_id,
                 "account_id",
             },
             {request.client_order_id, "client_order_id"},
             {request.cancel_request_id, "cancel_request_id"},
         }) {
        if (auto invalid = invalid_identifier(value, field)) {
            return *invalid;
        }
    }

    const std::lock_guard lock(mutex_);
    try {
        std::optional<PaperAccountLock> locked =
            repository_->lock_account(request.account_id);
        if (!locked) {
            return account_not_found();
        }
        PaperOrderProcessor& processor = locked->processor();
        // A retry of a request that already cancelled the order returns it
        // unchanged; the same ID cannot cancel a second order.
        if (const auto owner =
                locked->order_cancelled_by(request.cancel_request_id)) {
            if (*owner != request.client_order_id) {
                locked->release();
                return error(
                    PaperTradingErrorCode::DuplicateCancelRequestId,
                    "cancel_request_id",
                    "cancel_request_id already cancelled another order of "
                    "this account"
                );
            }
            std::optional<PaperOrderRecord> order =
                locked->find_order(request.client_order_id);
            PaperAccountView account = account_view(processor, locked->version());
            locked->release();
            if (!order) {
                throw std::runtime_error("a cancelled order is not recorded");
            }
            return CancelledPaperOrder{
                .order = std::move(*order),
                .account = std::move(account),
                .already_cancelled = true,
            };
        }
        std::optional<PaperOrderRecord> order =
            locked->find_order(request.client_order_id);
        if (!order) {
            locked->release();
            return order_not_found();
        }
        const std::optional<Timestamp> market_time =
            market_state_.status().market_time;
        const Timestamp at = market_time
            ? *market_time
            : (order->state ? order->state->updated_at
                            : order->intent.submitted_at);
        PaperOrderCancelResult cancelled =
            processor.cancel(request.client_order_id, at);
        if (std::holds_alternative<PaperOrderCancelError>(cancelled)) {
            locked->release();
            return order_not_cancellable(std::move(*order));
        }
        CommitPaperOrderResult committed = locked->commit_cancellation(
            std::get<WorkingOrderCancellation>(cancelled),
            request.cancel_request_id
        );
        if (auto* conflict = std::get_if<PaperPersistenceConflict>(&committed)) {
            return persistence_conflict(std::move(*conflict));
        }
        PaperAccountView account =
            account_view(processor, std::get<std::int64_t>(committed));
        notify_activity();
        auto recorded = repository_->find_order(
            request.account_id,
            request.client_order_id
        );
        if (!std::holds_alternative<PaperOrderRecord>(recorded)) {
            throw std::runtime_error(
                "a cancelled order could not be read back"
            );
        }
        return CancelledPaperOrder{
            .order = std::get<PaperOrderRecord>(std::move(recorded)),
            .account = std::move(account),
            .already_cancelled = false,
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

PaperOrderHistoryResult PaperTradingService::list_orders(
    std::string_view account_id,
    const HistoryPageRequest& request
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }
    auto page_request = journal_page_request(
        account_id,
        request,
        HistoryCursorKind::Orders
    );
    if (auto* invalid = std::get_if<PaperTradingError>(&page_request)) {
        return std::move(*invalid);
    }

    const std::lock_guard lock(mutex_);
    try {
        auto page = repository_->list_orders(
            account_id,
            std::get<PaperJournalPageRequest>(page_request)
        );
        if (std::holds_alternative<PaperJournalReadMiss>(page)) {
            return account_not_found();
        }
        auto& journal = std::get<PaperJournalPage<PaperOrderRecord>>(page);
        return PaperOrderHistoryPage{
            .orders = std::move(journal.records),
            .next_cursor = next_cursor(
                HistoryCursorKind::Orders,
                account_id,
                request.instrument,
                journal.next_before_record_id
            ),
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

PaperOrderRecordResult PaperTradingService::get_order(
    std::string_view account_id,
    std::string_view client_order_id
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }
    if (auto invalid = invalid_identifier(client_order_id, "client_order_id")) {
        return *invalid;
    }

    const std::lock_guard lock(mutex_);
    try {
        auto lookup = repository_->find_order(account_id, client_order_id);
        if (const auto* miss = std::get_if<PaperJournalReadMiss>(&lookup)) {
            return *miss == PaperJournalReadMiss::AccountNotFound
                ? account_not_found()
                : order_not_found();
        }
        return std::get<PaperOrderRecord>(std::move(lookup));
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

PaperFillHistoryResult PaperTradingService::list_fills(
    std::string_view account_id,
    const HistoryPageRequest& request
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }
    auto page_request = journal_page_request(
        account_id,
        request,
        HistoryCursorKind::Fills
    );
    if (auto* invalid = std::get_if<PaperTradingError>(&page_request)) {
        return std::move(*invalid);
    }

    const std::lock_guard lock(mutex_);
    try {
        auto page = repository_->list_fills(
            account_id,
            std::get<PaperJournalPageRequest>(page_request)
        );
        if (std::holds_alternative<PaperJournalReadMiss>(page)) {
            return account_not_found();
        }
        auto& journal = std::get<PaperJournalPage<PaperFillRecord>>(page);
        return PaperFillHistoryPage{
            .fills = std::move(journal.records),
            .next_cursor = next_cursor(
                HistoryCursorKind::Fills,
                account_id,
                request.instrument,
                journal.next_before_record_id
            ),
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

WorkingOrderPageResult PaperTradingService::list_working_orders(
    std::string_view account_id,
    const ForwardPageRequest& request
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }
    if (request.instrument) {
        if (auto invalid = invalid_identifier(*request.instrument, "instrument")) {
            return *invalid;
        }
    }
    const std::size_t limit = request.limit.value_or(kDefaultHistoryPageLimit);
    if (auto invalid = invalid_page_limit(limit)) {
        return *invalid;
    }
    const std::optional<std::string_view> instrument = request.instrument
        ? std::optional<std::string_view>(*request.instrument)
        : std::nullopt;
    std::optional<std::int64_t> after;
    if (request.cursor) {
        after = decode_history_cursor(
            *request.cursor,
            HistoryCursorKind::WorkingOrders,
            account_id,
            instrument
        );
        if (!after) {
            return invalid_cursor();
        }
    }

    const std::lock_guard lock(mutex_);
    try {
        auto page = repository_->list_working_orders(
            account_id,
            PaperForwardPageRequest{
                .limit = limit,
                .after = after,
                .instrument = request.instrument
                    ? std::optional<InstrumentId>(InstrumentId(*request.instrument))
                    : std::nullopt,
            }
        );
        if (std::holds_alternative<PaperJournalReadMiss>(page)) {
            return account_not_found();
        }
        auto& orders = std::get<PaperForwardPage<PaperOrderRecord>>(page);
        return WorkingOrderPage{
            .orders = std::move(orders.records),
            .next_cursor = orders.next_after
                ? std::optional<std::string>(encode_history_cursor(
                      HistoryCursorKind::WorkingOrders,
                      account_id,
                      instrument,
                      *orders.next_after
                  ))
                : std::nullopt,
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

OrderFillPageResult PaperTradingService::list_order_fills(
    std::string_view account_id,
    std::string_view client_order_id,
    const ForwardPageRequest& request
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }
    if (auto invalid = invalid_identifier(client_order_id, "client_order_id")) {
        return *invalid;
    }
    auto page_request = order_page_request(
        account_id,
        client_order_id,
        request,
        OrderCursorKind::Fills
    );
    if (auto* invalid = std::get_if<PaperTradingError>(&page_request)) {
        return std::move(*invalid);
    }

    const std::lock_guard lock(mutex_);
    try {
        auto page = repository_->list_order_fills(
            account_id,
            client_order_id,
            std::get<PaperForwardPageRequest>(page_request)
        );
        if (const auto* miss = std::get_if<PaperJournalReadMiss>(&page)) {
            return *miss == PaperJournalReadMiss::AccountNotFound
                ? account_not_found()
                : order_not_found();
        }
        auto& fills = std::get<PaperForwardPage<PaperFillRecord>>(page);
        return OrderFillPage{
            .fills = std::move(fills.records),
            .next_cursor = fills.next_after
                ? std::optional<std::string>(encode_order_cursor(
                      OrderCursorKind::Fills,
                      account_id,
                      client_order_id,
                      *fills.next_after
                  ))
                : std::nullopt,
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

OrderEventPageResult PaperTradingService::list_order_events(
    std::string_view account_id,
    std::string_view client_order_id,
    const ForwardPageRequest& request
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }
    if (auto invalid = invalid_identifier(client_order_id, "client_order_id")) {
        return *invalid;
    }
    auto page_request = order_page_request(
        account_id,
        client_order_id,
        request,
        OrderCursorKind::Events
    );
    if (auto* invalid = std::get_if<PaperTradingError>(&page_request)) {
        return std::move(*invalid);
    }

    const std::lock_guard lock(mutex_);
    try {
        auto page = repository_->list_order_events(
            account_id,
            client_order_id,
            std::get<PaperForwardPageRequest>(page_request)
        );
        if (const auto* miss = std::get_if<PaperJournalReadMiss>(&page)) {
            return *miss == PaperJournalReadMiss::AccountNotFound
                ? account_not_found()
                : order_not_found();
        }
        auto& events = std::get<PaperForwardPage<PaperOrderEventRecord>>(page);
        return OrderEventPage{
            .events = std::move(events.records),
            .next_cursor = events.next_after
                ? std::optional<std::string>(encode_order_cursor(
                      OrderCursorKind::Events,
                      account_id,
                      client_order_id,
                      *events.next_after
                  ))
                : std::nullopt,
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

PaperPortfolioResult PaperTradingService::get_portfolio(
    std::string_view account_id
) {
    if (auto invalid = invalid_identifier(account_id, "account_id")) {
        return *invalid;
    }

    const std::lock_guard lock(mutex_);
    try {
        const auto session = PersistentPaperOrderSession::load(
            *repository_,
            account_id
        );
        if (!session) {
            return account_not_found();
        }
        // Only open positions need a mark; an instrument the account does not
        // hold never prevents valuation.
        std::vector<InstrumentId> held;
        for (const auto& [instrument, position] :
             session->account().state().positions) {
            if (position.is_open()) {
                held.push_back(instrument);
            }
        }
        PortfolioValuationResult valuation = value_portfolio(
            session->account(),
            session->executions(),
            market_state_.quotes(held),
            session->risk_limits().maximum_quote_age_nanoseconds
        );
        if (auto* failure = std::get_if<PortfolioValuationError>(&valuation)) {
            return portfolio_valuation_failure(std::move(*failure));
        }
        return PaperPortfolioView{
            .account = account_view(*session),
            .valuation = std::get<PortfolioValuation>(std::move(valuation)),
        };
    } catch (const PaperPersistenceUnavailable& failure) {
        return persistence_unavailable(failure);
    }
}

MarketAvailability PaperTradingService::market_availability() const {
    MarketAvailability availability{
        .stream = market_state_.status(),
        .instruments = {},
    };
    // One consistent read, so every instrument's quote is from one instant.
    const MarketQuotes market = market_state_.quotes(config_.tradeable_instruments);
    for (const InstrumentId& instrument : config_.tradeable_instruments) {
        const InstrumentQuote& quote = market.quotes.at(instrument);
        const auto* unavailable = std::get_if<MarketStateUnavailable>(&quote);
        availability.instruments.push_back(InstrumentAvailability{
            .instrument = instrument,
            .unavailable = unavailable
                ? std::optional<MarketStateUnavailableCode>(unavailable->code)
                : std::nullopt,
            .quote = unavailable
                ? std::nullopt
                : std::optional<TopOfBookEvent>(std::get<TopOfBookEvent>(quote)),
        });
    }
    return availability;
}

PaperTradingHealth PaperTradingService::health() {
    const std::lock_guard lock(mutex_);
    return PaperTradingHealth{
        .persistence_available = repository_->check_availability(),
        .market = market_state_.status(),
        .tradeable_instruments = config_.tradeable_instruments,
    };
}

}  // namespace chronos::paper_trading
