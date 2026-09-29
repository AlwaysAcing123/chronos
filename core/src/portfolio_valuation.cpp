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
        .instruments = {},
    };
}

struct MarkIneligibility {
    PortfolioValuationErrorCode code;
    std::string message;
};

// Whether `market` may mark the open position in `instrument`, checked
// before any arithmetic.
std::optional<MarkIneligibility> mark_ineligibility(
    const InstrumentId& instrument,
    const InstrumentExecutionStates& executions,
    const TopOfBookEvent& market,
    Timestamp market_time,
    std::int64_t maximum_quote_age_nanoseconds
) {
    if (std::holds_alternative<MarketEventValidationFailure>(
            validate_market_event(MarketEvent(market))
        )) {
        return MarkIneligibility{
            PortfolioValuationErrorCode::InvalidMarketState,
            "top-of-book state failed the market-data contract",
        };
    }
    if (market.header.instrument != instrument) {
        return MarkIneligibility{
            PortfolioValuationErrorCode::MarketInstrumentMismatch,
            "top-of-book instrument does not match the position",
        };
    }
    if (market.header.quality.is_suspect()) {
        return MarkIneligibility{
            PortfolioValuationErrorCode::SuspectMarketState,
            "suspect top-of-book state cannot mark a position",
        };
    }
    if (!market.bid) {
        return MarkIneligibility{
            PortfolioValuationErrorCode::MissingBestBid,
            "top of book has no best bid to mark the position",
        };
    }
    if (market_lineage_violation(executions, market)) {
        return MarkIneligibility{
            PortfolioValuationErrorCode::MarketTimelineMismatch,
            "the quote does not continue the instrument's market-data "
            "timeline",
        };
    }
    switch (classify_market_observation_recency(
        market.header.receive_timestamp,
        market_time,
        maximum_quote_age_nanoseconds
    )) {
        case MarketObservationRecency::Current:
            return std::nullopt;
        case MarketObservationRecency::FromFuture:
            return MarkIneligibility{
                PortfolioValuationErrorCode::MarketTimelineMismatch,
                "market time precedes the quote",
            };
        case MarketObservationRecency::Stale:
            return MarkIneligibility{
                PortfolioValuationErrorCode::StaleMarketState,
                "top-of-book state is older than the account's configured "
                "risk limit",
            };
    }
    throw std::logic_error("unknown market observation recency");
}

// The account-wide timeline rules, which apply with or without positions:
// the market must be the account's stream, and a market clock must not
// precede the account's latest fill. Without any market clock, open
// positions are reported as unmarkable instead, which says more.
std::optional<PortfolioValuationError> timeline_violation(
    const PaperAccount& account,
    const InstrumentExecutionStates& executions,
    const MarketQuotes& market
) {
    const auto bound_stream = bound_market_stream(executions);
    if (bound_stream && *bound_stream != market.stream) {
        return valuation_error(
            PortfolioValuationErrorCode::MarketTimelineMismatch,
            "the account trades on another market-data stream"
        );
    }
    const auto latest_fill = account.state().latest_fill_timestamp();
    if (latest_fill && market.market_time &&
        *market.market_time < *latest_fill) {
        return valuation_error(
            PortfolioValuationErrorCode::MarketTimelineMismatch,
            "market time precedes the account's latest fill"
        );
    }
    return std::nullopt;
}

}  // namespace

PortfolioValuationResult value_portfolio(
    const PaperAccount& account,
    const InstrumentExecutionStates& executions,
    const MarketQuotes& market,
    std::int64_t maximum_quote_age_nanoseconds
) {
    if (auto violation = timeline_violation(account, executions, market)) {
        return std::move(*violation);
    }

    std::map<InstrumentId, TopOfBookEvent> marks;
    std::vector<PositionMarkFailure> failures;
    for (const auto& [instrument, position] : account.state().positions) {
        if (!position.is_open()) {
            continue;
        }
        const auto found = market.quotes.find(instrument);
        if (found == market.quotes.end()) {
            failures.push_back(PositionMarkFailure{
                .instrument = instrument,
                .reason = MarketStateUnavailableCode::NoTopOfBook,
                .message = "the market was not asked for the instrument",
            });
            continue;
        }
        if (const auto* unavailable =
                std::get_if<MarketStateUnavailable>(&found->second)) {
            failures.push_back(PositionMarkFailure{
                .instrument = instrument,
                .reason = unavailable->code,
                .message = unavailable->message,
            });
            continue;
        }
        const auto& quote = std::get<TopOfBookEvent>(found->second);
        if (!market.market_time) {
            failures.push_back(PositionMarkFailure{
                .instrument = instrument,
                .reason = PortfolioValuationErrorCode::MarketTimelineMismatch,
                .message = "a quote has no market time to be current at",
            });
            continue;
        }
        if (auto ineligible = mark_ineligibility(
                instrument,
                executions,
                quote,
                *market.market_time,
                maximum_quote_age_nanoseconds
            )) {
            failures.push_back(PositionMarkFailure{
                .instrument = instrument,
                .reason = ineligible->code,
                .message = std::move(ineligible->message),
            });
            continue;
        }
        marks.emplace(instrument, quote);
    }
    if (!failures.empty()) {
        PortfolioValuationError error = valuation_error(
            PortfolioValuationErrorCode::PositionMarkUnavailable,
            failures.size() == 1
                ? "an open position cannot be marked"
                : std::to_string(failures.size()) +
                    " open positions cannot be marked"
        );
        error.instruments = std::move(failures);
        return error;
    }
    // Reached only without open positions: history cannot be valued before
    // the market clock has started.
    if (account.state().latest_fill_timestamp() && !market.market_time) {
        return valuation_error(
            PortfolioValuationErrorCode::MarketTimelineMismatch,
            "market time precedes the account's latest fill"
        );
    }
    if (account.state().starting_cash.raw() <= 0) {
        return valuation_error(
            PortfolioValuationErrorCode::NonpositiveStartingCash,
            "a return percentage requires positive starting cash"
        );
    }

    std::map<InstrumentId, Price> mark_prices;
    for (const auto& [instrument, quote] : marks) {
        mark_prices.emplace(instrument, quote.bid->price);
    }
    // PaperAccount::snapshot computes every position and total with checked
    // arithmetic and verifies that equity less starting cash equals realized
    // plus unrealized PnL less fees.
    try {
        AccountSnapshot snapshot = account.snapshot(mark_prices);
        const Percentage return_percent = percentage_of(
            snapshot.net_total_pnl,
            snapshot.state.starting_cash
        );
        return PortfolioValuation{
            .snapshot = std::move(snapshot),
            .return_percent = return_percent,
            .marks = std::move(marks),
            .valued_at = market.market_time,
        };
    } catch (const std::overflow_error&) {
        return valuation_error(
            PortfolioValuationErrorCode::ArithmeticOverflow,
            "portfolio valuation exceeds the supported fixed-point range"
        );
    } catch (const std::invalid_argument&) {
        return valuation_error(
            PortfolioValuationErrorCode::AccountingIdentityFailed,
            "an open position has no eligible mark"
        );
    } catch (const std::logic_error&) {
        return valuation_error(
            PortfolioValuationErrorCode::AccountingIdentityFailed,
            "portfolio values do not reconcile"
        );
    }
}

}  // namespace chronos
