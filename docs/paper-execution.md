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
[decisions/0010-multi-instrument-accounts.md](decisions/0010-multi-instrument-accounts.md);
good-til-cancelled working orders in
[decisions/0011-durable-gtc-working-orders.md](decisions/0011-durable-gtc-working-orders.md).

## Flow

```text
manual UI or model policy
    -> OrderIntent
    -> PaperOrderProcessor::submit
        -> pre-trade validation and risk, against unreserved cash and shares
        -> ApprovedOrderIntent
        -> top-of-book execution
        -> simulated report and optional Fill
        -> paper-account accounting
        -> a good-til-cancelled remainder rests as a WorkingOrder
    -> PaperOrderReceipt

later market observation of the instrument
    -> PaperOrderProcessor::match
        -> each eligible, marketable working order in priority order
        -> Fill at the opposite best, up to untaken displayed size
        -> paper-account accounting
    -> WorkingOrderMatch

client cancel request
    -> PaperOrderProcessor::cancel
    -> WorkingOrderCancellation
```

Only the risk engine can construct an `ApprovedOrderIntent`. This makes the
approved value a typed capability: the execution simulator cannot be called
with an arbitrary, unchecked `OrderIntent`.

`PaperOrderProcessor` is the normal end-to-end entry point. It owns one fresh
paper account, its risk engine, its execution simulator, and its working
orders. Approval never escapes `submit`: the processor evaluates the current
account and reservations, executes the order immediately, and applies any
resulting fill before returning. This prevents a caller from caching an
approval while other fills change cash or position.

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

Time in force is immediate-or-cancel (`ioc`) or good-til-cancelled (`gtc`).
A market order must be immediate-or-cancel: `market` + `gtc` is the risk
rejection `market_order_requires_ioc`. An immediate-or-cancel order executes
once and cancels any remainder. A good-til-cancelled limit order executes
what is marketable now and rests the remainder as a working order, which
later observations fill until nothing remains or a client cancels it.
Replace requests are deferred (see ADR 0011).

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
- Sufficient unreserved shared cash for a buy and its flat fee: cash minus
  what working orders reserve
- Sufficient unreserved position in the order's instrument for a sell: the
  position minus what working sells reserve
- Sufficient unreserved cash to pay the fee for a sell
- For a buy, the position limit counts the unfilled shares of the
  instrument's working buys
- Checked fixed-point arithmetic with structured overflow rejection,
  including the account totals: risk previews the complete fill against the
  account, so a fill that would make total cost basis, realized PnL, or fees
  unrepresentable is rejected before execution rather than faulting the
  processor
- An order timestamp that does not precede the account's latest fill in any
  instrument, because the server's market clock is shared by every
  instrument

Notional and buying power are checked conservatively using the complete
requested quantity: at the current opposite best price for an
immediate-or-cancel order, even if displayed quantity is smaller or its
limit is not marketable, and at the limit price for a good-til-cancelled
order, the most it can ever pay. This avoids approving an order on the
assumption that it will receive only a partial fill.

## Working orders and reservations

A working order holds back from later orders:

- a buy: `limit price × remaining`, rounded like every notional, plus the
  fee until its first fill;
- a sell: its remaining shares, plus the fee from cash until its first fill.

Reservations change no cash, position, cost basis, realized PnL, fee, or
equity. A fill reduces a buy's reservation to the new remainder at the limit,
so a fill below the limit releases the difference immediately; the final
fill and a cancellation release everything. `reserved cash ≤ cash` and, per
instrument, `reserved shares ≤ position` hold after every transition and on
recovery.

Each fill's notional is rounded separately, so a partial buy fill can cost
up to one money unit (1e-8 USD) more than the reservation it releases. When
the account has less than that unreserved, the fill is not taken at that
observation (`WorkingOrderSkipReason::ReservationShortfall`) instead of
leaving the other reservations uncovered; the order keeps working. A skip
also prevents newer orders on the same side from taking that observation's
liquidity: the rest of that side is marked consumed in this account's shadow
liquidity state, so FIFO priority still holds across later submissions and
restarts. The opposite book side remains independent. The same applies to
an immediate fill at acceptance, and to a fill whose notional rounds to zero
or would overflow an account total.

`match(observation, market_time)` offers one observation to the working
orders of its instrument, in priority order (acceptance order). An order is
eligible only for an observation strictly newer than the one it was
accepted on, and newer than the account's latest execution observation of
the instrument; suspect, stale, invalid, other-stream, and missing-side
observations are skipped without cancelling anything. A buy is marketable
when the ask is at or below its limit, a sell when the bid is at or above
it, and each fills at that opposite best price up to the displayed size the
account has not already taken from the observation. A halted account's
orders never fill, but can be cancelled.

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
- Some liquidity: the available quantity is filled; an immediate-or-cancel
  remainder is cancelled and a good-til-cancelled remainder rests.
- No liquidity: an immediate-or-cancel order is cancelled; a
  good-til-cancelled order rests unfilled and takes nothing.
- Non-marketable limit: likewise cancelled or resting.

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
- A generated fill ID is deterministic: `<client-order-id>:fill:<n>`, where
  `n` counts the order's fills from 0. A client order ID never contains
  `:`, so fill IDs of different orders cannot collide.
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

This slice uses one configurable flat fee per order, charged by its first
non-zero fill; later fills of a good-til-cancelled order carry zero fee. An
order that never fills pays no fee, and a partially filled order that is
cancelled keeps its one fee. More realistic maker/taker schedules belong in
a later versioned fee model.

No latency model exists yet. A fill at submission is timestamped with the
order's submission time; a fill of a working order with the receive time of
the observation that filled it. Explicit decision, acknowledgement,
arrival, and execution times will be added before Chronos claims realistic
trading evaluation.

Orders submitted through the paper-trading API are timestamped with the
server's market clock, the receive time of the latest accepted market event,
and execute against the server's own quote. See
[paper-trading-api.md](paper-trading-api.md).

## Important limitations

- Top-of-book only; no Level-2 sweep, queue position, or market impact; a
  working order fills from displayed size as soon as a later quote shows it
- No shared exchange: accounts never see each other's orders or liquidity
- No replace; cancellation cancels the whole remainder
- Long-only paper accounting
- One flat fee per order
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
