#pragma once

#include "chronos/paper_account.hpp"
#include "chronos/top_of_book_execution.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace chronos {

// Mark policy: a long-only position is marked at the displayed best bid, the
// price at which it could begin to be sold. This is a conservative
// mark-to-market value, not a promise of liquidation proceeds: it ignores
// displayed size, queue position, market impact, and exit fees.

enum class PortfolioValuationErrorCode {
    // The market cannot supply a trustworthy mark for this account.
    InvalidMarketState,
    MarketInstrumentMismatch,
    SuspectMarketState,
    MissingBestBid,
    StaleMarketState,
    MarketTimelineMismatch,
    // The account cannot be valued exactly.
    NonpositiveStartingCash,
    ArithmeticOverflow,
    AccountingIdentityFailed,
};

struct PortfolioValuationError {
    PortfolioValuationErrorCode code;
    std::string message;
};

// Every field reconciles exactly: market value is the best bid times the
// position, unrealized PnL is market value less open cost basis, equity is
// cash plus market value, and total PnL is both equity less starting cash
// and realized plus unrealized PnL less fees.
struct PortfolioValuation {
    AccountSnapshot snapshot;
    // snapshot.net_total_pnl as a percentage of starting cash.
    Percentage return_percent;
    // The observation whose best bid is the mark.
    TopOfBookEvent market;
    // The market clock when the observation was read.
    Timestamp valued_at;
};

using PortfolioValuationResult = std::variant<
    PortfolioValuation,
    PortfolioValuationError
>;

// Values `account` against a server-owned observation read at
// `market_time`. `active_market` is the observation of the account's latest
// execution: the valuation market must continue that timeline, and market
// time must not precede the latest fill. The observation must also be within
// `maximum_quote_age_nanoseconds` of market time. Checked arithmetic that
// overflows and identities that disagree are reported, never approximated.
[[nodiscard]] PortfolioValuationResult value_portfolio(
    const PaperAccount& account,
    const std::optional<TopOfBookEvent>& active_market,
    const TopOfBookEvent& market,
    Timestamp market_time,
    std::int64_t maximum_quote_age_nanoseconds
);

}  // namespace chronos
