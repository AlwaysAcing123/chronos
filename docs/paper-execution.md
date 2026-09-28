# Chronos simple paper-execution contract

## Scope

This document defines the first order-to-fill boundary in Chronos. Manual and
model-generated decisions produce the same `OrderIntent`, pass through the
same `PreTradeRiskEngine`, and enter the same
`TopOfBookExecutionSimulator`. The output is a simulated execution report or
a structured error.

This is an in-memory, single-account, single-instrument, long-only paper
simulation. It cannot send an order to a broker, exchange, or real-money
account.

## Flow

```text
manual UI or model policy
    -> OrderIntent
    -> pre-trade validation and risk
    -> ApprovedOrderIntent
    -> top-of-book execution
    -> simulated report and optional Fill
```

Only the risk engine can construct an `ApprovedOrderIntent`. This makes the
approved value a typed capability: the execution simulator cannot be called
with an arbitrary, unchecked `OrderIntent`.

The next milestone connects the optional `Fill` to `PaperAccount` and verifies
end-to-end accounting reconciliation. Until that coordinator exists, risk,
execution, and fill application are separate in-memory operations rather than
one atomic transaction.

## Order intent

Every order contains:

- A caller-supplied client order ID used for idempotency
- The target paper account and instrument
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
It then enforces:

- Exact account and instrument identity
- A structurally valid top-of-book event for the order's instrument
- No potentially invalid book or questionable receive timestamp
- No market observation received after order submission
- A configurable maximum quote age
- Displayed liquidity on the opposite side of the book
- Maximum order quantity and notional
- Maximum long position after a buy
- Sufficient cash for a buy and its flat fill fee
- Sufficient owned position for a sell
- Sufficient cash to pay the fee for a possible partial sell fill
- Checked fixed-point arithmetic with structured overflow rejection

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

Displayed bid and ask liquidity are tracked independently. Multiple orders
against the same market observation cannot reuse quantity that an earlier
simulated order consumed. A strictly newer ingest sequence establishes a new
snapshot and resets those counters.

This per-account shadow-liquidity rule is deliberately conservative inside one
snapshot, but it is not a real matching-engine or queue-position model. One
paper account never changes another user's market or simulated liquidity.

## Identity, ordering, and lineage

- A client order ID may be processed only once, including when the first
  result was a cancellation.
- A generated fill ID is deterministic: `<client-order-id>:fill:0`.
- An execution simulator is permanently bound to one account and instrument.
- After the first execution, market observations must retain the same stream
  identity and cannot move to a lower ingest sequence.
- The same stream and ingest sequence cannot identify two different snapshots.
- Rejected execution attempts do not consume liquidity or reserve their client
  order ID, so a caller may correct the routing error and retry.

## Fees and time

This slice uses one configurable flat fee per non-empty simulated fill. A
cancelled order has no fill and no fee. More realistic maker/taker schedules
belong in a later versioned fee model.

No latency model exists yet. The fill timestamp is the order submission
timestamp. Explicit decision, acknowledgement, arrival, and execution times
will be added before Chronos claims realistic trading evaluation.

## Important limitations

- Top-of-book only; no Level-2 sweep, queue, or market impact
- Immediate-or-cancel only; no resting order lifecycle
- One fill at most per order
- Long-only paper accounting
- Flat fee per fill
- No latency or slippage beyond crossing the visible spread
- In-memory state and an unbounded processed-ID set
- No durable audit log or restart recovery
- No atomic risk/execution/account update yet
- No reservation of cash, position, or risk capacity between approval and fill
- No API, authentication, authorization, or user interface

Because account state is not versioned in an approval, callers must not cache
an approval while applying other fills to the same account. The next
order-processing coordinator will serialize and atomically persist risk,
execution, and accounting events so stale approvals cannot overspend cash or
position.
