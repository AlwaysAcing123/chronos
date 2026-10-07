# ADR 0019: What a deployment is doing, and how long it took

Status: accepted (2026-10-04). It extends ADR 0018 and changes no trading
behavior.

## Context

A running deployment answers "is it working?" badly today:

- **Counters are cumulative.** The run counters (`skipped`, `waits`) count
  outcomes since Start. Nothing in them says what is happening now; the
  largest counter is usually the oldest condition, not the current one.
- **Saved decisions never cover skips.** They hold every prediction, policy
  outcome, and intended order. A candidate skipped before inference has no
  decision, and a pending order's waits leave no trace.
- **Source conditions are invisible.** The source's condition, such as a
  paused replay or a reconnecting live feed, appears only on the
  market-data resource, and nothing ties it to a deployment.
- **Timing is easy to misread.** The docs warn that execution-quote offsets
  are not order latency, but the API exposes only raw timestamps and two
  wall-clock maxima.

## Options

1. **Derive everything from existing data on read.** Durable decisions plus
   the market-data resource cover lifecycle, saved predictions, and source
   state. They cannot explain a skipped candidate or a pending order's wait:
   that information exists only for an instant inside the runner.
2. **Persist telemetry.** An event table or durable activity log would
   survive restarts, but every candidate would cost a database write. The
   observer path must stay free of database work, and growing histories are
   out of scope.
3. **A bounded, process-local snapshot plus derivation on read.** The runner
   keeps the latest outcome of each of its two activities, deciding and
   submitting, under the mutex it already holds for its counters. Everything
   else is derived on read from durable rows and in-memory market state.

**Chosen: option 3.** It adds two fixed-size records per runner and no
database work on the trading path. It also adds one bounded, indexed join to
the existing decision page.

## The three facts, kept separate

| Fact | Source | Survives restart |
|---|---|---|
| Lifecycle: `stopped`, `running`, `stopping`, `faulted`, with reason | `model_deployments` | yes |
| Latest processed observation and its outcome; latest submission check | runner snapshot (`activity`) | no: `null` after restart |
| Current source and quote condition | `MarketDataSource::status()` and `MarketState`, read at request time | n/a: always current |

The response joins them as `condition`, the one explanation clients show. It
is computed on read by this precedence, first match wins:

1. **Lifecycle.** A deployment that is not `running`: `stopped`, `stopping`,
   or `faulted`, with its durable reason. This beats any runtime or market
   fact.
2. **No runner here.** Durably `running`, but this process has no runner
   (`not_running_here`). This happens only if another process shares the
   database.
3. **Source.** The source is not delivering: `source_not_started`,
   `source_paused`, `source_completed`, `source_connecting`,
   `source_reconnecting`, `source_failed`, `source_stopped`, or
   `no_market_data_source`. A live source that is `live` is delivering, and
   so is a replay that is `running`.
4. **Quote, for the deployment's instrument.**
   - `instrument_not_trading`: the latest status is halted or closed.
   - `quote_unavailable`, with the market state's reason: `source_unavailable`,
     `awaiting_fresh_quote`, `stream_interrupted`, or `no_top_of_book`.
   - `quote_suspect`.
   - `quote_stale`: the quote's age exceeds the account's maximum quote age.

   Silence alone never reads as "market closed": only a trading-status
   event says that.
5. **Pending order.** `order_pending`, with the latest submission check's
   reason, such as `awaiting_latency` or `order_matching_busy`, plus the
   decision's not-before and expiry times.
6. **Latest candidate of this run.** For example `before_training_cutoff`
   with the cutoff and the observation time, `waiting_for_decision_interval`
   with the next eligible receive time, `not_after_watermark`,
   `inputs_unavailable:<reason>`, `stale_signal`, `account_unavailable`,
   `persistence_unavailable`, or `decision_refused`. A decided candidate is
   `watching`, waiting for the next one.
7. **No candidate yet in this run:** `waiting_for_candidate`.

Each condition carries its provenance, `observed_at`: the market and server
wall times of the fact behind it. For a market condition that is now; for a
runtime outcome it is when the runner recorded it.

A skipped candidate is described by its outcome only. No prediction is
invented for it. The latest prediction is always the latest saved decision,
labelled with its run.

## Clocks

| Clock | Meaning | Used for |
|---|---|---|
| Market | The server's market clock: the receive time of the latest event, or, for a live source, the receive clock advancing in silence. Accelerated in a fast replay | Every timestamp of observations, decisions, orders, and fills, and every derived duration below |
| Server wall | `system_clock`, UTC | Only to stamp when the runner recorded a runtime outcome |
| Steady | `steady_clock` | Inference and trusted-order-path durations, which are wall time and include waiting for the gate |

All decision durations are derived on the backend from saved timestamps,
with checked 64-bit subtraction, as exact nanosecond strings. Any of them
can be `unavailable` with a reason:

- **`signal_to_decision`**: `decided_at` minus the observation's receive
  time. The market time from the signal to its saved decision.
- **`execution_quote_offset`**: the execution quote's receive time minus the
  observation's. It says which quote the order used. It is not the order's
  latency.
- **`signal_to_resolution`**: `resolved_at` minus the observation's receive
  time.
  - For a `recorded` decision this is when the order was accepted and
    journaled, in market time; the deferred database guard requires the
    order's `submitted_at` to equal it.
  - For an `abandoned` decision it is when it was abandoned.
  - It does not mean a fill.
- **`signal_to_fill`**: the last booked fill's `filled_at` minus the
  observation's receive time. It exists only when a fill was booked.

The configured `latency_ms` is the minimum from signal receive to
eligibility. It is not a promised latency.

When a duration is unavailable, its reason says why:

| Reason | When |
|---|---|
| `no_order` | A held decision |
| `not_resolved` | A pending decision |
| `no_execution` | Abandoned |
| `no_fill` | Nothing booked |
| `inconsistent_clock` | The difference would be negative |
| `overflow` | The subtraction would overflow |

A duration is never clamped or zeroed.

## Fills

A decision's `fill` aggregates the booked fills of its one order: count,
quantity, notional, fees, first and last `filled_at`, and the price when
there is exactly one fill. A decision that is `recorded` but `cancelled`
(nothing executable), or rejected, has `fill: null`. Abandoned and pending
decisions never have one.

The join goes from decision to order (the unique partial index on account
and client order ID) to fills (the unique index on order and fill index). It
is bounded by the page size, which is at most 200.

## Locks and cost

- **Runner snapshot.** Written under the runner's existing leaf `mutex_` by
  plain assignment, at the points that already increment counters. Status
  reads copy it under `runners_mutex_` then the runner's mutex, the order
  the counters already use.
- **Market reads.** Market state, hub trading status, and source status are
  read after `runners_mutex_` is released. Each takes only that component's
  own leaf mutex. None takes the gate, the service mutex, or a row lock.
- **The observer path is unchanged.** It does no database work and no
  inference.
- **Reads are read-only.** `GET` routes run only `SELECT`s, so reading a
  deployment never changes its version, watermark, decisions, orders, or
  liquidity. A test asserts this.
- **Cost.** One status read costs O(1) per deployment, plus a market-state
  read per deployment on list pages of at most 200. A decision page costs
  one bounded query.

## Restart

`activity` and `run` live in memory. After a restart they are `null`, and
`condition` falls back to lifecycle: a deployment that was running is
`stopped (server_restarted)`, and any other keeps its reason. Saved decisions, orders, fills, and the watermark
remain, and still show the latest prediction and timing.

## Consequences

- Clients get one explanation instead of inferring one from counters.
- **Additive API changes.** The deployment resource gains `activity`,
  `market`, and `condition`; decisions gain `timing` and `fill`.
- The manager now reads the market-data source's status, through an
  optional function in its options, so tests and servers without a source
  still work: `no_market_data_source`.
