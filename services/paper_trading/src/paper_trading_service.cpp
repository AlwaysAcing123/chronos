#include "chronos/paper_trading/paper_trading_service.hpp"

#include <algorithm>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

PaperAccountView account_view(const PersistentPaperOrderSession& session) {
    return PaperAccountView{
        .account_id = session.account().account_id(),
        .instrument = session.account().instrument(),
        .state = session.account().state(),
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
    if (auto invalid = invalid_identifier(request.instrument, "instrument")) {
        return *invalid;
    }
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
            instrument,
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
        // an order cannot be recorded until the server has a quote for the
        // account's instrument.
        ExecutionQuoteResult quote_result =
            market_state_.execution_quote(session->account().instrument());
        if (const auto* unavailable =
                std::get_if<MarketStateUnavailable>(&quote_result)) {
            PaperTradingError unavailable_error = error(
                PaperTradingErrorCode::MarketDataUnavailable,
                "",
                unavailable->message
            );
            unavailable_error.market_state = unavailable->code;
            return unavailable_error;
        }
        ExecutionQuote quote =
            std::get<ExecutionQuote>(std::move(quote_result));

        OrderIntent intent{
            .client_order_id = request.client_order_id,
            .account_id = request.account_id,
            .source = request.source,
            .instrument = InstrumentId(request.instrument),
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

PaperTradingHealth PaperTradingService::health() {
    const std::lock_guard lock(mutex_);
    return PaperTradingHealth{
        .persistence_available = repository_->check_availability(),
        .market = market_state_.status(),
    };
}

}  // namespace chronos::paper_trading
