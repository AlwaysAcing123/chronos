#include "chronos/portfolio_valuation.hpp"

#include "chronos/market_data_validation.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace chronos {
namespace {

PortfolioValuationError valuation_error(
    PortfolioValuationErrorCode code,
    std::string message
) {
    return PortfolioValuationError{
        .code = code,
        .message = std::move(message),
    };
}

// Whether `market` may mark `account` at all, checked before any arithmetic.
std::optional<PortfolioValuationError> mark_ineligibility(
    const PaperAccount& account,
    const std::optional<TopOfBookEvent>& active_market,
    const TopOfBookEvent& market,
    Timestamp market_time,
    std::int64_t maximum_quote_age_nanoseconds
) {
    if (std::holds_alternative<MarketEventValidationFailure>(
            validate_market_event(MarketEvent(market))
        )) {
        return valuation_error(
            PortfolioValuationErrorCode::InvalidMarketState,
            "top-of-book state failed the market-data contract"
        );
    }
    if (market.header.instrument != account.instrument()) {
        return valuation_error(
            PortfolioValuationErrorCode::MarketInstrumentMismatch,
            "top-of-book instrument does not match the account"
        );
    }
    if (market.header.quality.is_suspect()) {
        return valuation_error(
            PortfolioValuationErrorCode::SuspectMarketState,
            "suspect top-of-book state cannot mark a portfolio"
        );
    }
    if (!market.bid) {
        return valuation_error(
            PortfolioValuationErrorCode::MissingBestBid,
            "top of book has no best bid to mark the portfolio"
        );
    }
    if (market_lineage_violation(active_market, market)) {
        return valuation_error(
            PortfolioValuationErrorCode::MarketTimelineMismatch,
            "the current market does not continue the account's market-data "
            "timeline"
        );
    }
    const MarketObservationRecency recency =
        classify_market_observation_recency(
            market.header.receive_timestamp,
            market_time,
            maximum_quote_age_nanoseconds
        );
    const auto& last_fill = account.state().last_fill_timestamp;
    if (recency == MarketObservationRecency::FromFuture ||
        (last_fill && market_time < *last_fill)) {
        return valuation_error(
            PortfolioValuationErrorCode::MarketTimelineMismatch,
            "market time precedes the observation or the account's latest "
            "fill"
        );
    }
    if (recency == MarketObservationRecency::Stale) {
        return valuation_error(
            PortfolioValuationErrorCode::StaleMarketState,
            "top-of-book state is older than the account's configured risk "
            "limit"
        );
    }
    return std::nullopt;
}

}  // namespace

PortfolioValuationResult value_portfolio(
    const PaperAccount& account,
    const std::optional<TopOfBookEvent>& active_market,
    const TopOfBookEvent& market,
    Timestamp market_time,
    std::int64_t maximum_quote_age_nanoseconds
) {
    if (auto ineligible = mark_ineligibility(
            account,
            active_market,
            market,
            market_time,
            maximum_quote_age_nanoseconds
        )) {
        return std::move(*ineligible);
    }
    if (account.state().starting_cash.raw() <= 0) {
        return valuation_error(
            PortfolioValuationErrorCode::NonpositiveStartingCash,
            "a return percentage requires positive starting cash"
        );
    }

    // PaperAccount::snapshot computes market value, unrealized PnL, and
    // equity with checked arithmetic and verifies that equity less starting
    // cash equals realized plus unrealized PnL less fees.
    try {
        AccountSnapshot snapshot = account.snapshot(market.bid->price);
        const Percentage return_percent = percentage_of(
            snapshot.net_total_pnl,
            snapshot.state.starting_cash
        );
        return PortfolioValuation{
            .snapshot = std::move(snapshot),
            .return_percent = return_percent,
            .market = market,
            .valued_at = market_time,
        };
    } catch (const std::overflow_error&) {
        return valuation_error(
            PortfolioValuationErrorCode::ArithmeticOverflow,
            "portfolio valuation exceeds the supported fixed-point range"
        );
    } catch (const std::logic_error&) {
        return valuation_error(
            PortfolioValuationErrorCode::AccountingIdentityFailed,
            "portfolio values do not reconcile"
        );
    }
}

}  // namespace chronos
