# Chronos simple paper-execution contract

## Scope

This document defines the first order-to-fill boundary in Chronos. Manual and
model-generated decisions produce the same `OrderIntent`, pass through the
same `PreTradeRiskEngine`, and enter the same
`TopOfBookExecutionSimulator`. The output is a simulated execution report or
a structured error.

This is an in-memory, single-account, long-only paper simulation. One account
trades any number of instruments from one cash balance; every order names
exactly one of them. It cannot send an order to a broker, exchange, or
real-money account. The multi-instrument rules are recorded in
[decisions/0010-multi-instrument-accounts.md](decisions/0010-multi-instrument-accounts.md).

## Flow

```text
manual UI or model policy
    -> OrderIntent
    -> PaperOrderProcessor
        -> pre-trade validation and risk
        -> ApprovedOrderIntent
        -> top-of-book execution
        -> simulated report and optional Fill
        -> paper-account accounting
    -> PaperOrderReceipt
```

Only the risk engine can construct an `ApprovedOrderIntent`. This makes the
approved value a typed capability: the execution simulator cannot be called
with an arbitrary, unchecked `OrderIntent`.

`PaperOrderProcessor` is the normal end-to-end entry point. It owns one fresh
paper account, its risk engine, and its execution simulator. Approval never
escapes `submit`: the processor evaluates the current account, executes the
order immediately, and applies any resulting fill before returning. This
prevents a caller from caching an approval while other fills change cash or
position.

A successful `PaperOrderReceipt` contains the execution report, an accounting
receipt when a fill occurred, and the complete account state afterward.
Cancelled orders have no accounting receipt and leave the account unchanged.
Risk and execution rejections also leave accounting state unchanged.

## Order intent

Every order contains:

- A caller-supplied client order ID used for idempotency, unique across the
  whole account whatever the instrument
- The target paper account and the one instrument it trades
- Side, quantity, order type, optional limit price, and time in force
- A submission timestamp
- Explicit source attribution consisting of a source kind and source ID

Manual and model orders differ only in source attribution. Source information
does not alter risk or fill behavior.

The first slice supports market orders and limit orders with
immediate-or-cancel time in force. Resting orders, replace requests, and
cancel requests require an order state machine and are intentionally deferred.

## Pre-trade validation and risk

The risk engine rejects malformed intents before examining account exposure.
Which instruments are tradeable is the application's rule: the paper-trading
service refuses an unconfigured instrument before anything is recorded. The
engine then enforces:

- Exact account identity
- A structurally valid top-of-book event for the order's instrument; a quote
  for another instrument is a `market_instrument_mismatch` rejection, so an
  AAPL quote can never fill an SPY order
- No potentially invalid book or questionable receive timestamp
- No market observation received after order submission
- A configurable maximum quote age
- Displayed liquidity on the opposite side of the book
- Maximum order quantity and notional, per order
- Maximum long position after a buy, per instrument: the order instrument's
  position only
- Sufficient shared cash for a buy and its flat fill fee
- Sufficient owned position in the order's instrument for a sell
- Sufficient cash to pay the fee for a possible partial sell fill
- Checked fixed-point arithmetic with structured overflow rejection,
  including the account totals: risk previews the complete fill against the
  account, so a fill that would make total cost basis, realized PnL, or fees
  unrepresentable is rejected before execution rather than faulting the
  processor
- An order timestamp that does not precede the account's latest fill in any
  instrument, because the server's market clock is shared by every
  instrument

Notional and buying power are checked conservatively using the complete
requested quantity at the current opposite best price, even if displayed
quantity is smaller or an IOC limit is not marketable. This avoids approving
an order on the assumption that it will receive only a partial fill.

Risk approval captures the exact order, market observation, candidate
execution price, maximum notional, and configured fee. Later mutations to the
caller's original objects cannot change the approval.

## Execution model

A market buy executes against the best displayed ask. A market sell executes
against the best displayed bid. A limit order executes only when its limit is
marketable at that same price. There is no price improvement and no sweep into
deeper levels.

The simulator fills at most the remaining displayed quantity:

- Enough liquidity: the order is filled.
- Some liquidity: the available quantity is filled and the IOC remainder is
  cancelled.
- No liquidity: the complete order is cancelled.
- Non-marketable limit: the complete order is cancelled.

Displayed bid and ask liquidity are tracked independently, per account and
per instrument. Multiple orders against the same market observation cannot
reuse quantity that an earlier simulated order consumed. A strictly newer
ingest sequence for an instrument establishes a new snapshot for that
instrument and resets its counters. Consuming AAPL liquidity never reduces
SPY's, and a fill changes only its own instrument's position.

This per-account shadow-liquidity rule is deliberately conservative inside one
snapshot, but it is not a real matching-engine or queue-position model. Each
paper account is an independent simulated world: one account never changes
another's market or simulated liquidity, even for the same instrument.

## Identity, ordering, and lineage

- A client order ID may be processed only once per account, including when
  the first result was a cancellation, and including for another instrument.
- A generated fill ID is deterministic: `<client-order-id>:fill:0`.
- An execution simulator is permanently bound to one account. It keeps an
  execution state per instrument: the latest executed observation and the
  liquidity consumed from it.
- After the first execution in any instrument, every instrument's market
  observations must come from the same stream: one account uses one
  compatible market stream or replay session.
- Per instrument, observations cannot move to a lower ingest sequence, and the
  same sequence cannot identify two different snapshots. Instruments are
  independent: an AAPL observation at sequence 8 is valid after SPY executed
  at sequence 12.
- Rejected execution attempts do not consume liquidity or reserve their client
  order ID, so a caller may correct the routing error and retry.

The durable PostgreSQL path applies a stricter API boundary: every recorded
submission consumes its client order ID, including a risk rejection. A
corrected durable submission therefore requires a new ID.

## Accounting and reconciliation

Every non-empty simulated fill is applied exactly once to the processor's
owned `PaperAccount`. The fill in the accounting receipt is the same value as
the fill in the execution report. Cash and the fill instrument's position,
cost basis, realized PnL, and fees therefore change through the existing
accounting contract rather than a second execution-specific calculation.

The processor exposes only a const account view. Callers can request an
explicit mark-to-market snapshot and verify the accounting identity:

```text
equity - starting cash = net total PnL
```

The portfolio API marks each open position at its instrument's current best
bid through `value_portfolio`. It shares two rules with this pipeline so that
valuation and execution cannot drift apart: `MarketDataQuality::is_suspect`,
which the risk engine uses to reject suspect books, and
`market_lineage_violation`, which the simulator uses to bind an account to
one stream and each instrument to its own timeline. The processor's
`executions()` exposes each instrument's latest executed observation for that
check.

If accounting ever rejects a risk-approved simulated fill, the processor
returns an invariant-violation error and permanently enters a faulted state.
It will reject subsequent submissions instead of continuing with execution
liquidity and account state out of sync. This is a defensive failure mode;
ordinary input errors are returned earlier as risk or execution rejections.

## Fees and time

This slice uses one configurable flat fee per non-empty simulated fill. A
cancelled order has no fill and no fee. More realistic maker/taker schedules
belong in a later versioned fee model.

No latency model exists yet. The fill timestamp is the order submission
timestamp. Explicit decision, acknowledgement, arrival, and execution times
will be added before Chronos claims realistic trading evaluation.

Orders submitted through the paper-trading API are timestamped with the
server's market clock, the receive time of the latest accepted market event,
and execute against the server's own quote. See
[paper-trading-api.md](paper-trading-api.md).

## Important limitations

- Top-of-book only; no Level-2 sweep, queue, or market impact
- No shared exchange: accounts never see each other's orders or liquidity
- Immediate-or-cancel only; no resting order lifecycle
- One fill at most per order
- Long-only paper accounting
- Flat fee per fill
- No latency or slippage beyond crossing the visible spread
- An unbounded processed-ID set inside each reconstructed processor
- One processor instance is not safe for concurrent calls
- No authentication, authorization, or user interface; the optional local
  HTTP API is described in [paper-trading-api.md](paper-trading-api.md)

The low-level risk and execution classes remain independently testable, but
application code should use `PaperOrderProcessor` so approvals cannot outlive
the account state against which they were checked. Durable application code
should use `PersistentPaperOrderSession`, which adds atomic journaling,
validated restart recovery, database idempotency, and optimistic concurrency.
