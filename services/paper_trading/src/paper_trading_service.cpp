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
    }
    throw std::logic_error("unknown paper persistence conflict");
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

PaperAccountView account_view(const PersistentPaperOrderSession& session) {
    return PaperAccountView{
        .account_id = session.account().account_id(),
        .state = session.account().state(),
        .market_stream = bound_market_stream(session.executions()),
        .risk_limits = session.risk_limits(),
        .is_healthy = session.is_healthy(),
        .version = session.version(),
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
    PaperTradingServiceConfig config
)
    : repository_(std::move(repository)),
      market_state_(market_state),
      config_(std::move(config)) {
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

    const std::lock_guard lock(mutex_);
    try {
        auto session = PersistentPaperOrderSession::load(
            *repository_,
            request.account_id
        );
        if (!session) {
            return account_not_found();
        }

        // The durable journal records the observation every order used, so
        // an order cannot be recorded until the server has a quote for its
        // instrument. No other instrument's quote is read.
        ExecutionQuoteResult quote_result =
            market_state_.execution_quote(instrument);
        if (const auto* unavailable =
                std::get_if<MarketStateUnavailable>(&quote_result)) {
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
        PersistentPaperOrderResult persisted = session->submit(
            intent,
            quote.top_of_book
        );
        if (auto* conflict =
                std::get_if<PaperPersistenceConflict>(&persisted)) {
            return persistence_conflict(std::move(*conflict));
        }
        return RecordedPaperOrder{
            .intent = std::move(intent),
            .market = std::move(quote.top_of_book),
            .result = std::get<PersistentPaperOrderReceipt>(
                std::move(persisted)
            ).result,
            .account = account_view(*session),
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
