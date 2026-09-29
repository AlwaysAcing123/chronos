# ADR 0006: Best-bid portfolio valuation

- Status: Proposed
- Date: 2026-09-28

> **Amended by [ADR 0010](0010-multi-instrument-accounts.md).** Accounts now
> hold several instruments. Each open position is marked at its own
> instrument's best bid, and only open positions need a mark, so a flat
> account is valued at its cash without a quote; the "valuing a flat account
> without a quote" alternative below is now the rule. Every other decision
> here stands, applied per position and then summed.

## Context

A client needs a trustworthy current portfolio snapshot: market value,
unrealized and total PnL, equity, and return. The accounting core already
computes a mark-to-market snapshot for a caller-supplied mark, but deliberately
leaves the mark policy to the caller. The API must choose one, must never let
a client supply the mark, and must never return values that do not reconcile.

## Decision

### Mark at the displayed best bid

Chronos accounts are long-only, so the position is marked at the current best
bid from the server-owned `MarketState`. It is the price at which the
position could begin to be sold, and so a conservative mark-to-market value.
It ignores displayed size, queue position, market impact, and exit fees, so it
is not a promise of real-world liquidation proceeds.

A bid is required even when the account is flat, so every portfolio names a
real observation, market timestamp, and mark. There is no fallback to the
ask, midpoint, last trade, cost basis, or zero.

### Valuation must be eligible

`value_portfolio` refuses to value when the market cannot supply a
trustworthy mark:

- the observation fails the market-data contract, belongs to another
  instrument, or carries a suspect quality flag (the same rule execution
  uses, `MarketDataQuality::is_suspect`);
- the book has no bid;
- the observation does not continue the account's market timeline: once an
  account has executed, its market must be the same stream at a
  non-decreasing ingest sequence with one state per sequence (the same rule
  execution uses, `market_lineage_violation`), and market time must not
  precede the account's latest fill;
- the observation is older than the account's configured maximum quote age
  relative to market time (the same recency classifier execution uses).

These surface as `503 market_data_unavailable` with a specific reason. A
failure to value exactly (non-positive starting cash, fixed-point overflow, or
an identity that does not reconcile) is `500 valuation_failed`.

### Exact identities

`PaperAccount::snapshot` computes, with checked fixed-point arithmetic:

```text
position_market_value = round(mark_price × position)      to 0.00000001
unrealized_pnl        = position_market_value − open_cost_basis
equity                = cash + position_market_value
total_pnl             = gross_realized_pnl + unrealized_pnl − fees_paid
```

and verifies `total_pnl = equity − starting_cash`. The notional rounds half
away from zero, like every fill. Because every fill keeps
`cash + open_cost_basis = starting_cash + gross_realized_pnl − fees_paid`,
the two definitions of total PnL agree exactly for any valid account.

### Return as a fixed-point percentage

`Percentage` is a new fixed-point type with eight decimal places, measured in
percentage points (`1.5` means 1.5%):

```text
return_percent raw = round(total_pnl raw × 100 × 10^8 / starting_cash raw)
```

It rounds half away from zero, using the same checked 128-bit
`rounded_divide` as other fixed-point conversions. The ratio of two `Money`
values is scale-free, so no money-scale factor appears. Starting cash must be
positive; the API cannot create an account without it.

### Read-only

Valuation loads the durable account in a consistent snapshot and reads the
market; it journals nothing and consumes no liquidity or client order ID.

## Alternatives considered

- **Midpoint.** Common for display, but it overstates what a long position
  can be sold for by half the spread.
- **Last trade.** It can be arbitrarily stale and is not a price anyone is
  currently bidding.
- **Size-aware liquidation value.** Walking the book for the full position
  needs depth that the top-of-book slice does not have. It belongs with
  Level-2 execution.
- **Valuing a flat account without a quote.** It would work, but it would make
  the response's mark and timestamp optional and let a stale or broken feed
  look healthy.
- **Returning floating-point return percentages.** They are simpler for
  clients, but they would reintroduce binary rounding into a financial
  resource.
- **Basis points or a fraction instead of percentage points.** Percentage
  points match the field name and how people read returns; eight decimal
  places keep rounding error below 5 × 10⁻⁹ percentage points.

## Consequences

- A portfolio is valued only against a market that could also execute for the
  account, so a server restarted on a different recording refuses to value
  existing accounts rather than inventing a value, and an observation that
  is too stale to trade against is also too stale to mark the account.
- The mark policy is a single documented rule; a second policy would be a new
  field or endpoint, not a query parameter.
- The value is optimistic for positions larger than the displayed bid size.
