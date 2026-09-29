#pragma once

#include "chronos/market_quotes.hpp"
#include "chronos/paper_account.hpp"
#include "chronos/top_of_book_execution.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace chronos {

// Mark policy: a long-only position is marked at the displayed best bid, the
// price at which it could begin to be sold. This is a conservative
// mark-to-market value, not a promise of liquidation proceeds: it ignores
// displayed size, queue position, market impact, and exit fees.

enum class PortfolioValuationErrorCode {
    // The market cannot supply a trustworthy mark for a position.
    InvalidMarketState,
    MarketInstrumentMismatch,
    SuspectMarketState,
    MissingBestBid,
    StaleMarketState,
    // The market does not continue the account's timeline, for the account
    // as a whole or for one position.
    MarketTimelineMismatch,
    // The account cannot be valued exactly.
    NonpositiveStartingCash,
    ArithmeticOverflow,
    AccountingIdentityFailed,
    // One or more open positions cannot be marked; `instruments` says why.
    PositionMarkUnavailable,
};

// Why one open position cannot be marked: the market has no quote for it,
// or its quote is not eligible to mark it.
struct PositionMarkFailure {
    InstrumentId instrument;
    std::variant<MarketStateUnavailableCode, PortfolioValuationErrorCode> reason;
    std::string message;
};

struct PortfolioValuationError {
    PortfolioValuationErrorCode code;
    std::string message;
    // For PositionMarkUnavailable: every open position that cannot be
    // marked, in instrument order.
    std::vector<PositionMarkFailure> instruments;
};

// Every field reconciles exactly (see AccountSnapshot): each open position
// is marked at its best bid, and equity - starting cash = net total PnL.
struct PortfolioValuation {
    AccountSnapshot snapshot;
    // snapshot.net_total_pnl as a percentage of starting cash.
    Percentage return_percent;
    // The observation whose best bid marks each open position.
    std::map<InstrumentId, TopOfBookEvent> marks;
    // The market clock of the valuation; absent only before the stream's
    // first event, when no position could have been marked.
    std::optional<Timestamp> valued_at;
};

using PortfolioValuationResult = std::variant<
    PortfolioValuation,
    PortfolioValuationError
>;

// Values `account` against one consistent read of the market. Only open
// positions need a quote, so an instrument the account does not hold never
// prevents valuation; every open position that cannot be marked is
// reported, never valued as zero. A quote must be a valid, unsuspect top of
// book with a bid, no older than `maximum_quote_age_nanoseconds`, and must
// continue the instrument's execution timeline (market_lineage_violation).
// The account's timeline must hold even without positions: the market must
// be the stream the account is bound to, and market time must not precede
// the account's latest fill. The checks run in that order: the stream, a
// market clock behind the latest fill, each open position, and finally a
// missing market clock, so that before any market event an open position
// reports that it has no quote. Checked arithmetic that overflows and totals
// that disagree are reported, never approximated.
[[nodiscard]] PortfolioValuationResult value_portfolio(
    const PaperAccount& account,
    const InstrumentExecutionStates& executions,
    const MarketQuotes& market,
    std::int64_t maximum_quote_age_nanoseconds
);

}  // namespace chronos
