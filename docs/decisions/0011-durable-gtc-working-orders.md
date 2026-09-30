# ADR 0011: Durable good-til-cancelled working orders

- Status: Proposed
- Date: 2026-09-29

## Context

Every paper order was immediate-or-cancel (IOC): it executed once against
the server's current quote and any remainder was cancelled. A trader could
not leave a limit order working and have later quotes fill it, which is the
most common way limit orders are used. A resting order needs things IOC
never did: a durable current state that changes after submission, more than
one fill per order, resources held back from later orders while it works,
a way to cancel it, and a component that offers it every later market
observation, including across restarts. Everything stays a Level-1 paper
simulation: no broker, exchange, or real money is involved.

## Decision

### Vocabulary and state machine

`TimeInForce` gains `GoodTilCancelled`, spelled `gtc`. Order statuses:

| Status | Meaning | Working |
|---|---|---|
| `working` | accepted, nothing filled | yes |
| `partially_filled` | some filled, remainder still working | yes |
| `filled` | nothing remains | no |
| `partially_filled_and_cancelled` | some filled, remainder cancelled | no |
| `cancelled` | nothing filled, cancelled | no |
| `rejected` | refused by risk or execution at submission | no |
| `faulted` | accounting refused a fill; the account is halted | no |

```text
submit ─┬─ rejected
        ├─ faulted
        ├─ filled                                    (IOC or GTC, all at once)
        ├─ partially_filled_and_cancelled | cancelled (IOC)
        └─ working ──┬─ partially_filled ─┬─ filled
                     │                    ├─ partially_filled_and_cancelled (client cancel)
                     │                    └─ faulted
                     ├─ filled
                     ├─ cancelled (client cancel)
                     └─ faulted
```

Every order has `quantity = filled_quantity + remaining_quantity`. The IOC
statuses and cancellation reasons keep their names and meanings, and
existing IOC history is not reinterpreted. A client cancellation has its own
reason, `user_requested`; the IOC reasons `limit_not_marketable` and
`displayed_liquidity_exhausted` are produced only by the IOC simulator.

### Market orders cannot be good-til-cancelled

A resting market order has no price at which it would stop filling, so its
reservation would be unbounded and later quotes could fill it at any price.
`market` + `gtc` is a recorded risk rejection, `market_order_requires_ioc`
(field `time_in_force`). It consumes its client order ID like every recorded
rejection. An undeclared time in force stays `unsupported_time_in_force`.

### Acceptance

A GTC limit order passes the same parsing, validation, risk, persistence, and
idempotency path as an IOC order. It needs a usable current quote at
acceptance, including the opposite side. What is marketable immediately
fills from the opposite best, up to the displayed quantity the account has
not already taken from that observation; the remainder rests. An order that
fills nothing takes no liquidity. The client chooses neither the time, the
price, nor the observation.

### Reservations

While an order works it holds back:

| Side | Cash | Shares |
|---|---|---|
| buy | limit price × remaining, rounded like every notional, plus the fee until the first fill | none |
| sell | the fee until the first fill | the remaining quantity |

Reservations never change cash, positions, cost basis, realized PnL, fees,
or equity; they limit what later orders may use. Risk:

- a new buy needs `cash − reserved cash ≥ notional + fee`, and a new sell's
  fee must fit in that same available cash; a new sell needs
  `held − reserved shares ≥ quantity`. This applies to IOC orders too, so an
  IOC order cannot spend what working orders reserve;
- a GTC order's notional, for the notional limit and for its reservation,
  is quantity × limit, the most it can pay; an IOC order is still sized at
  the opposite best;
- a buy's position limit counts the unfilled shares of working buys;
- overflow anywhere is the recorded rejection `arithmetic_overflow`, with no
  state changed.

A fill reduces the reservation to the new remainder at the limit, so price
improvement releases the difference at once; cancellation and a final fill
release everything. The server computes `reserved_cash`, `available_cash`,
and each position's `reserved_quantity` and `available_quantity`; clients
display them and compute nothing.

Each fill's notional is rounded on its own, so a partial buy fill can cost up
to one money unit (1e-8 USD) more than the reservation it releases. When the
account has less than that unreserved, taking the fill would leave its other
reservations uncovered, so that observation is skipped for that order
(`ReservationShortfall`) and a later one can fill it. The invariant
`reserved cash ≤ cash` therefore holds after every transition exactly,
rather than approximately. A fill whose notional rounds to zero, or that
would overflow an account total, is skipped the same way. A skipped order
keeps its priority: no newer order on the same side can consume that
observation. The side's remaining displayed quantity is recorded as consumed
in the account's shadow-liquidity state, making the decision durable across
later submissions and restarts; the opposite side remains independent.

### One fee per order

The flat fee is charged once, by the first non-zero fill; later fills carry
zero fee. A cancellation with no fill pays nothing, a partial fill followed
by a cancellation keeps its one fee, and an IOC order with a fill pays
exactly one fee as before.

### Matching later observations

The matcher (`WorkingOrderMatcher`) observes every event the market state
accepts, through `MarketEventObserver` on `MarketStateFeed::apply`, on the
source's own thread, after the event is applied and before subscribers are
told or the next event is applied. It never uses `MarketDataNotifier`, which
coalesces. For a top of book it asks PostgreSQL which healthy accounts on
the event's stream have a working order on the event's instrument that was
accepted before it and is marketable at it (buy: ask ≤ limit; sell: bid ≥
limit), through a prepared statement on a partial index. It then matches
each such account in its own transaction:

1. lock the account row and recover the account under the lock;
2. `PaperOrderProcessor::match` offers the observation to that instrument's
   working orders in priority order;
3. commit what changed, or release the lock if nothing did.

Eligibility: an observation is skipped for an account, without cancelling
anything, when it is invalid, suspect, stale at its market time, from
another stream, older than or equal to the account's latest execution
observation of that instrument, or lacks the side an order needs. An order
is offered only observations strictly newer than its acceptance observation.
Each order fills at the observed opposite best price, up to the displayed
quantity this account has not taken from that observation, with no deeper
sweep, no queue position, and nothing from trades. Fills are timestamped
with the observation's receive time, so matching an observation always has
the same result whichever transaction does it.

### Shadow liquidity

Liquidity accounting is the existing per-account, per-instrument execution
state: the latest execution observation and the bid and ask quantity the
account's orders took from it. Working orders and new orders share it, so
the account never reuses liquidity it already consumed on one snapshot, and
a strictly newer snapshot resets the counters. Accounts remain independent
worlds: two accounts matched against the same observation each see its full
size.

### Priority

Priority is first in, first out per account and instrument: the order's
journal record ID, a durable identity assigned at commit. Submissions of one
account are serialized by its row lock and read their quote under it, so
record-ID order is also acceptance-observation order, with the record ID
breaking ties between orders accepted on the same observation. It survives
restarts because it is stored. Clients see it as an opaque `priority`.
Price does not change priority: an account's working orders are not an order
book, and a higher-limit order accepted later does not take liquidity ahead
of an earlier one.

### Serialization and the market-event gate

Every mutation of an account (a submission, a match, a cancellation) takes
the account's row lock first and recovers the account under it, so writes
of one account are serialized by PostgreSQL across connections and
processes, and each sees every change committed before it. No transaction
locks two accounts, and the account row is always its first lock, so there
is no lock-order cycle.

The row lock alone leaves one race: a submission that reads quote *d* before
the source applies event *e* but commits after the matcher asked PostgreSQL
for *e*'s candidates would never be offered *e*. `MarketEventGate` closes it
in the process: the feed holds it exclusively while applying and matching an
event, and a submission holds it shared from before the account lock until
its commit. So every event is offered to exactly the working orders
committed before it was applied, and an order accepted on one observation is
offered every later one. Because the gate is always taken before any account
lock, gate and row locks never wait on each other in a cycle. A submission
that cannot get the gate within `order_matching_wait` (2 s) is refused with
`503 order_matching_busy`; nothing is recorded and the same request may be
sent again. Submissions first queue among themselves, under the same
deadline, and wait for the gate before they take the service's connection
lock, so the lock order is submission queue, gate, connection lock, account
row. At most one submission holds the gate at a time, so a burst of
submissions delays the feed by one submission, not by the whole queue (and
a reader-preferring `shared_timed_mutex`, as libstdc++'s is, cannot starve
it), and reads and cancellations never wait for the gate. A submission
reads its order back before it releases the gate, so its response pairs the
order and the account at the same instant.

Consequences of this design:

- Races are decided by account serialization. A cancellation that commits
  first leaves nothing to fill; a fill that commits first is kept, and the
  cancellation cancels only what remains or, if nothing does, answers
  `409 order_not_cancellable` with the order.
- The guarantee "every later observation is offered" holds for writers in
  this server process. A second server on the same database and stream
  cannot double-book, because each account's execution state records the
  observations it used, but it can make an order miss one observation.
  One server per database is the supported deployment.

### Separate connection, synchronous matching

The matcher owns a PostgreSQL connection of its own rather than sharing the
service's. The service serializes every request on its connection, so a
shared one would make every API request wait for matching and every match
wait for the slowest request. With separate connections, matching holds only
the account it matches, and API reads never wait.

Matching is synchronous on the source's thread rather than queued to another
thread. The market state then never runs ahead of matching by more than the
event being matched, which is what lets the gate order submissions against
it with one event of lookahead, and there is no queue to bound, drain on
shutdown, or lose. The cost is backpressure: the source waits for matching.
Measured with the service tests on macOS (Docker Desktop, Debug build), an
observation with no marketable working order costs one prepared-statement
round trip, about 0.5 to 0.7 ms, and an observation that books a fill about
15 ms (about 20 round trips, dominated by the Docker network). The licensed
10-minute recording (119,282 top-of-book observations) replays through the
server at `max` in about 60 to 66 s, of which matching accounts for about
60 s; decoding and applying the same events with no matching
(`chronos_databento_replay`) takes about 1.7 s. At `1x`, matching uses about
a tenth of the recording's 10 minutes. Replay slows down rather than
skipping an event. For a live feed, a long stall makes the provider
disconnect, which the live source already treats as a gap (see ADR 0009).

### Persistence

Migration 004 keeps the journal (`paper_order_records`) as the immutable
record of each submission and adds, in normalized tables:

- `paper_orders`: one row per executed order, its current state. Its
  reservation, cumulative fee, and fee-charged flag are generated from its
  own columns, so they cannot disagree with it, and a CHECK matrix ties
  status, quantities, reason, cancel request ID, and time in force together.
- `paper_fills`: every booked fill, append-only, numbered per order from 0,
  with a foreign key that makes a fill agree with its order's account,
  client order ID, instrument, and side, and CHECKs that only fill 0 pays a
  fee and that each notional is the exact rounded product.
- `paper_events`, still append-only, gains each event's market time and the
  fill a `fill_applied` event booked.

Deferred constraint triggers check at commit that every account's working
orders' reservations are covered by its cash and each instrument's shares,
and that every order's fills add up to its filled quantity, notional, fee,
and fill count. Recovery restores intent, quantities, fee state, priority,
and reservations from these rows, and cross-checks the stored reservation
against the one C++ computes; the "last processed observation" is the
existing per-instrument execution state. Inconsistent rows fail closed.

Existing data is copied exactly: every executed IOC order gets its
`paper_orders` row from its journaled outcome, every booked fill keeps its
ID and gets a fill record ID equal to its journal record ID (so fill-history
cursors issued before the migration still page correctly), and every event
is dated with its submission's time. The migration validates first and
refuses, rolling back entirely, any executed record the journal reader would
itself refuse. It locks `paper_accounts` first, which every writer and
reader touches first, and then `paper_events` before `paper_order_records`,
the order in which a rerun of migration 001 takes its index locks, so the
two cannot deadlock. (Migration 003 locks them the other way round and can;
it cannot be changed, and PostgreSQL resolves such a deadlock by aborting
one of the two, which is then run again.)

### Cancellation

`POST /api/v1/accounts/{account_id}/orders/{client_order_id}/cancel` with
exactly `{"cancel_request_id": "<identifier>"}`. It needs no market data and
works on a halted account. Under the account lock it:

- returns the order with `already_cancelled: true` when this request ID
  already cancelled it (a retry);
- refuses with `409 duplicate_cancel_request_id` when the ID cancelled
  another order of the account;
- refuses with `409 order_not_cancellable`, including the order, when the
  order is terminal or never worked;
- otherwise makes the order terminal, keeps its fills, releases its
  reservation, appends `order_cancelled` and `account_state_updated`, and
  returns the order and account.

Unknown accounts and orders stay `404 account_not_found` and
`404 order_not_found`. The cancellation time is the market clock, or the
order's latest change if the clock is behind it or not started. After an
uncertain result (`503`, `500`, lost response) a client reads the order: if
its `cancel_request_id` is this request's, it was cancelled; if it still
works, sending the same request again is safe; a client never creates a new
ID for the same cancellation.

### Crash recovery and outages

- Every transition is one transaction. A lost connection before commit
  records nothing; a lost commit acknowledgement is resolved by the retry,
  which finds the observation already used or the cancellation already
  recorded.
- When PostgreSQL is unavailable while matching, the matcher retries the
  same observation with exponential backoff (50 ms to 2 s) until it
  succeeds or the server stops, holding the source meanwhile, so no later
  observation overtakes it. A lock held longer than one second by another
  session is retried the same way, so it cannot block shutdown. `/health`
  reports `order_matching.state` as `retrying`.
- On shutdown the matcher is stopped before the source is joined; an
  observation abandoned mid-retry is simply not matched. After a restart a
  replay is played from its start and each account's execution state skips
  what it already used, so matching resumes exactly; a live feed continues
  with new observations.
- An account whose stored state cannot be recovered is logged once and
  skipped for that observation; other accounts are still matched. An
  accounting rejection of a matched fill faults that order and halts the
  account, like the IOC path, and publishes account activity like a fill.
- Any other matcher failure, such as a query PostgreSQL rejects, fails the
  source. The feed suspends the market state before it releases the gate,
  so a submission that was waiting finds no usable quote instead of trading
  ahead of working orders that were never offered the event.
- If startup fails after the source has started, a guard stops the matcher
  before the source is destroyed, so its thread can be joined even while a
  match is retrying a lost connection.

### Level-1 limits: not an exchange or queue model

The simulation sees only the best bid and ask and their displayed sizes. It
does not model deeper levels, queue position, hidden or reserve liquidity,
other participants, latency, or market impact, and it never fills from
trades. A marketable working order fills against displayed size as soon as a
later observation shows it; a real resting order would join a queue and
might not fill at all.

### Cancel and replace later

Cancel/replace can build on this without changing it: a replace is a
cancellation of the remainder (with its own request ID) and a new order that
inherits nothing, recorded in one account transaction. Whether it keeps
priority when only the quantity decreases is the one decision it adds;
`paper_orders.order_record_id` already carries priority, so keeping it means
updating the row in place rather than inserting a new one.

## Alternatives considered

- **A queue between the feed and a matcher thread.** It decouples the source
  from PostgreSQL, but the market state would then run ahead of matching by
  the queue's depth, and a submission reading the latest quote could not be
  ordered against the events still queued without a per-event history.
- **Matching from the change notifier.** It coalesces events, so it would
  skip observations.
- **An in-memory index of working orders to skip the per-event query.** It
  would make memory the authority for which orders are offered, and a writer
  outside the process would be invisible to it.
- **Reserving by recomputation alone.** Recomputing `limit × remaining` after
  each fill is exact but, because fills round separately, can undercover by
  one money unit; skipping the rare fill that would do so keeps the
  invariant exact without over-reserving.
- **Price priority.** An account's own working orders are not a market;
  acceptance order is what a client can predict.

## Consequences

- Orders, fills, and events now have a current state separate from the
  submission journal; APIs return the current state everywhere.
- Every mutation takes the account row lock first, so the optimistic version
  conflict no longer occurs between this server's own requests; a request
  waits up to five seconds for a lock another session holds.
- Replay speed with working orders is bounded by PostgreSQL round trips.
