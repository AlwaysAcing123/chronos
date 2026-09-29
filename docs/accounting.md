# Chronos paper-account accounting contract

## Scope

This document defines the first permanent accounting boundary in Chronos. The account accepts immutable simulated fills and updates one shared cash balance and, for the fill's instrument only, a long position, cost basis, gross realized profit and loss, and fees.

The multi-instrument rules are recorded in `decisions/0010-multi-instrument-accounts.md`.

The fixed-point representation and scale tradeoffs are recorded in `decisions/0002-fixed-point-scales.md`.

This layer does not decide whether an order should fill or what its price
should be. The paper-order processor now connects market data, order
validation, risk policy, and execution simulation to this accounting boundary.

## Version-one decisions

- One USD-denominated paper account holds one cash balance and zero or more positions, each keyed by instrument.
- Positions are long-only; margin, borrowing, and short selling are excluded.
- No deposits or withdrawals occur after account creation.
- Financial values use signed 64-bit fixed-point storage. Prices have nine decimal places so provider prices can be represented exactly; money and quantities have eight decimal places.
- Input decimals are parsed directly from text and never pass through binary floating point.
- Notional money and proportional cost-basis allocation round to the nearest `0.00000001`. Displayed average entry price rounds to the nearest `0.000000001`. Exact halfway values round away from zero.
- Gross PnL excludes fees. Fees are tracked separately. Net PnL equals gross PnL minus accumulated fees.
- Fee allocation: each fill's fee is paid from the shared cash and recorded in the fill instrument's ledger, so every fee belongs to exactly one position. Account fees are the sum over ledgers, including closed ones.
- Cost basis excludes fees so execution performance and costs remain separately inspectable.
- Equal fill timestamps are valid; a timestamp earlier than the instrument's previously applied fill is rejected. Fills for different instruments are not ordered against each other by accounting (the paper-order processor keeps the account's timeline; see `paper-execution.md`).
- A fill identifier may be applied at most once per account, whatever its instrument.
- A fill changes only its own instrument's ledger and the shared cash.
- A positive price and quantity whose product rounds to zero at the supported money precision is rejected.
- Rejected fills do not change account state.

## Authoritative state

The account stores:

- Starting cash
- Current cash
- A ledger per instrument it has filled, in instrument order:
  - Position quantity
  - Open cost basis
  - Gross realized PnL
  - Accumulated fees
  - Last applied fill timestamp
- Applied fill identifiers

A ledger is created by the instrument's first fill and is never removed. Selling a whole position leaves the ledger with zero quantity and zero open cost basis — the zero-quantity representation of a closed position — so its realized PnL, fees, and last fill time remain part of the account. An instrument never filled has no ledger. Iteration is always in instrument order (`std::map`), so snapshots, persistence, and API responses are deterministic.

Account totals — open cost basis, gross realized PnL, and fees — are derived: they are sums over every ledger, computed with checked arithmetic. A fill whose result would make any total unrepresentable is rejected as an arithmetic overflow and changes nothing. The account's latest fill timestamp is the maximum over its ledgers.

Average entry price is derived per position from open cost basis divided by open quantity. It is not the authoritative cost-basis value, and a closed position has none.

With no deposits or withdrawals, the account identity holds after every fill:

```text
cash + Σ open cost basis = starting cash + Σ gross realized PnL − Σ fees
```

Recovery re-checks it, and PostgreSQL enforces it at commit (see `postgresql-persistence.md`).

## Buy fill

For a buy fill:

```text
notional          = fill price × fill quantity
cash required     = notional + fee
new cash          = old cash − cash required
new quantity      = old quantity + fill quantity
new cost basis    = old cost basis + notional
realized PnL      = unchanged
new fees          = old fees + fee
```

The fill is rejected if cash required exceeds current cash. Cash is shared, so a buy in one instrument spends cash another instrument's order could have used.

## Sell fill

For a partial sale under average-cost accounting:

```text
removed cost basis = old cost basis × sold quantity / old quantity
sale notional      = fill price × sold quantity
realized PnL change = sale notional − removed cost basis
new cash           = old cash + sale notional − fee
new quantity       = old quantity − sold quantity
new cost basis     = old cost basis − removed cost basis
new realized PnL   = old realized PnL + realized PnL change
new fees           = old fees + fee
```

Selling the complete position removes the complete remaining cost basis, avoiding residual rounding dust. Selling more than the instrument's current position is rejected; a position in another instrument never covers it.

## Mark-to-market snapshot

Given an explicit mark price for every open position (a closed position needs none, and a missing mark is an error, never zero):

```text
position market value         = mark price × open quantity
position gross unrealized PnL = position market value − position open cost basis
market value                  = Σ position market value
gross unrealized PnL          = Σ position gross unrealized PnL
gross total PnL               = Σ gross realized PnL + gross unrealized PnL
net total PnL                 = gross total PnL − Σ fees
equity                        = cash + market value
```

Every sum is checked; a total that overflows fails the snapshot.

With no deposits or withdrawals after creation, this invariant must always hold:

```text
equity − starting cash = net total PnL
```

The caller must identify the mark-price policy, such as best bid, midpoint, or last trade. This accounting layer only evaluates the supplied mark.

The portfolio API's policy is each instrument's current best bid, chosen and validated by `value_portfolio` in `chronos/portfolio_valuation.hpp`. It adds a return percentage, `round(net total PnL / starting cash × 100)` to eight decimal places, half away from zero, with starting cash required to be positive. The rules are recorded in `decisions/0006-portfolio-valuation.md`.

## Primary example

Starting account:

```text
cash = $10,000.00
position = 0
```

After buying 10 shares at $100.05 with a $1.00 fee:

```text
notional = $1,000.50
cash = $8,998.50
position = 10
open cost basis = $1,000.50
average entry price = $100.05
gross realized PnL = $0.00
fees = $1.00
```

After selling 4 shares at $102.90 with a $0.50 fee:

```text
sale notional = $411.60
removed cost basis = $400.20
gross realized PnL change = $11.40
cash = $9,409.60
position = 6
open cost basis = $600.30
average entry price = $100.05
gross realized PnL = $11.40
fees = $1.50
```

At a $103.00 mark:

```text
market value = $618.00
gross unrealized PnL = $17.70
gross total PnL = $29.10
net total PnL = $27.60
equity = $10,027.60
```

## Multi-instrument example

The core tests reconcile this account to the last unit. Starting with $100,000.00:

```text
buy  AAPL 10 @ 200.50, fee 0.35
buy  SPY   4 @ 500.25, fee 0.35
buy  QQQ   2 @ 400.00, fee 0.35
sell QQQ   2 @ 410.00, fee 0.35     QQQ is closed: realized +20.00
sell AAPL  3 @ 210.00, fee 0.35     removed cost 601.50, realized +28.50
```

```text
cash = 96,642.25

AAPL  quantity 7  open cost 1,403.50  realized 28.50  fees 0.70
SPY   quantity 4  open cost 2,001.00  realized  0.00  fees 0.35
QQQ   quantity 0  open cost     0.00  realized 20.00  fees 0.70   (closed)

cash + Σ open cost = 96,642.25 + 3,404.50 = 100,046.75
starting + Σ realized − Σ fees = 100,000.00 + 48.50 − 1.75 = 100,046.75
```

Marked at AAPL 205.00 and SPY 499.75 (QQQ needs no mark):

```text
market value = 1,435.00 + 1,999.00 = 3,434.00
gross unrealized PnL = 31.50 − 2.00 = 29.50
net total PnL = 48.50 + 29.50 − 1.75 = 76.25
equity = 96,642.25 + 3,434.00 = 100,076.25
```

## Explicit exclusions

- Execution-price selection
- Order validation and order state transitions
- Market-data ingestion
- Risk limits beyond cash and owned quantity
- Instruments priced in other currencies, and cross-margining between positions
- Short positions, margin, leverage, borrowing, interest, and liquidation
- Deposits, withdrawals, dividends, splits, funding, and corporate actions
- Tax-lot accounting methods such as FIFO and LIFO
- Persistence, APIs, authentication, and user interfaces
