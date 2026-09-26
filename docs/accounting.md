# Chronos paper-account accounting contract

## Scope

This document defines the first permanent accounting boundary in Chronos. The account accepts immutable simulated fills and updates cash, a single long position, cost basis, gross realized profit and loss, and fees.

The fixed-point representation and scale tradeoffs are recorded in `decisions/0002-fixed-point-scales.md`.

This layer does not decide whether an order should fill or what its price should be. Market data, order validation, risk policy, and execution simulation will produce fills in later stages.

## Version-one decisions

- One USD-denominated paper account holds one instrument.
- Positions are long-only; margin, borrowing, and short selling are excluded.
- No deposits or withdrawals occur after account creation.
- Financial values use signed 64-bit fixed-point storage. Prices have nine decimal places so provider prices can be represented exactly; money and quantities have eight decimal places.
- Input decimals are parsed directly from text and never pass through binary floating point.
- Notional money and proportional cost-basis allocation round to the nearest `0.00000001`. Displayed average entry price rounds to the nearest `0.000000001`. Exact halfway values round away from zero.
- Gross PnL excludes fees. Fees are tracked separately. Net PnL equals gross PnL minus accumulated fees.
- Cost basis excludes fees so execution performance and costs remain separately inspectable.
- Equal fill timestamps are valid; a timestamp earlier than the previously applied fill is rejected.
- A fill identifier may be applied at most once.
- A positive price and quantity whose product rounds to zero at the supported money precision is rejected.
- Rejected fills do not change account state.

## Authoritative state

The account stores:

- Starting cash
- Current cash
- Position quantity
- Open cost basis
- Gross realized PnL
- Accumulated fees
- Last applied fill timestamp
- Applied fill identifiers

Average entry price is derived from open cost basis divided by open quantity. It is not the authoritative cost-basis value.

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

The fill is rejected if cash required exceeds current cash.

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

Selling the complete position removes the complete remaining cost basis, avoiding residual rounding dust. Selling more than the current position is rejected.

## Mark-to-market snapshot

Given an explicit mark price:

```text
market value         = mark price × open quantity
gross unrealized PnL = market value − open cost basis
gross total PnL      = gross realized PnL + gross unrealized PnL
net total PnL        = gross total PnL − accumulated fees
equity               = cash + market value
```

With no deposits or withdrawals after creation, this invariant must always hold:

```text
equity − starting cash = net total PnL
```

The caller must identify the mark-price policy, such as best bid, midpoint, or last trade. This accounting layer only evaluates the supplied mark.

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

## Explicit exclusions

- Execution-price selection
- Order validation and order state transitions
- Market-data ingestion
- Risk limits beyond cash and owned quantity
- Multiple instruments in one account
- Short positions, margin, leverage, borrowing, interest, and liquidation
- Deposits, withdrawals, dividends, splits, funding, and corporate actions
- Tax-lot accounting methods such as FIFO and LIFO
- Persistence, APIs, authentication, and user interfaces
