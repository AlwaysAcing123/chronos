# Deterministic market replay

## Responsibility

The replay core consumes only `ValidatedMarketEvent` values. It has no
Databento dependency and does not read files, sleep, or use wall-clock time.
Each accepted event advances a simulated clock, revalidates the stream
identity and contiguous ingest sequence, and updates a point-in-time market
view.

The DBN reader and replay engine are deliberately separate:

```text
DBN bytes -> provider adapter -> ValidatedMarketEvent -> replay -> market view
```

That boundary lets historical files, live capture, synthetic tests, and a
future OCaml implementation share the same replay contract.

## Time policy

Replay time is the event's receive timestamp, not its exchange event
timestamp. A strategy cannot act on information before the feed received it.
Exchange timestamps may move backward between events and remain available for
latency analysis, but receive time may never move backward.

Multiple events may have the same receive timestamp. Their contiguous ingest
sequence is the stable tie-breaker, so a trade and top-of-book update emitted
from one MBP-1 record are replayed in their original normalized order without
inventing a time difference.

The engine rejects a receive-time regression instead of sorting, buffering,
or silently clamping it. Reordering would hide a source-quality problem and
could introduce future information. A rejected event changes neither the
clock, sequence boundary, nor market view.

## Current market view

`CurrentMarketView` stores the latest independently observed state for each
instrument:

- last trade;
- top of book;
- trading status; and
- instrument definition.

Trades do not mutate the quote, and quote updates do not erase the last trade.
Every stored event retains its exchange timestamp, receive timestamp, ingest
sequence, stream identity, venue information, and quality flags. Instruments
are stored in deterministic symbol order so snapshots compare identically
across repeated runs.

## Determinism and reset

For a fixed validated event sequence and initial ingest sequence, replay
produces the same steps, final simulated time, and market view. `reset`
clears the clock and view and establishes an explicit new sequence boundary.
Tests replay the same compressed synthetic DBN recording twice through the
complete provider-to-view path and compare the results exactly.

The optional Databento build also provides an offline executable for real
recordings:

```sh
./build-databento/chronos_databento_replay \
  --input data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340
```

It streams the DBN file through the provider adapter and replay engine without
loading the recording into memory. Its deterministic JSON report contains the
recomputed manifest, aggregate and per-instrument event counts, replay time
bounds, the final ingest sequence, and final trade and top-of-book state. It
does not contact Databento.

## Server market state

`MarketState` wraps one `MarketReplay` for the paper-trading server. It is
safe to call from a feed thread and request threads at once, serves the latest
top of book for an instrument together with the replay clock, and stops
serving execution quotes permanently after any rejected event. The API uses
the replay clock as each order's submission time, as described in
[paper-trading-api.md](paper-trading-api.md).

A live source can also suspend the state, advance its clock without an
event, and require fresh quotes after a reconnection
([market-data-sources.md](market-data-sources.md#live-freshness)). A replay
uses none of these, so replay time is still exactly the receive time of the
latest accepted event and replays remain reproducible.

## Current boundary

The replay core supports event-step replay of synthetic and local real DBN
recordings; the server paces it in real time as described below. The core
does not persist normalized events, calculate features, or simulate
execution. Those
responsibilities build on the replay clock and market view rather than
entering the provider adapter.

## Paced replay

The deterministic core above never sleeps. The paper-trading server adds a
paced driver around it, `PacedReplaySource`, which plays a recording into the
server's `MarketState` on its own thread while the HTTP API serves
([market-data-sources.md](market-data-sources.md)).

### Timing contract

- **Receive time drives playback.** Replay time is the receive timestamp, so
  pacing uses the same clock: an event is played when the recording's receive
  clock reaches it, never earlier than the feed delivered it.
- **The first event is delivered at once** when playback starts. It anchors
  the schedule: its receive time plays at that steady-clock instant.
- **Every later event is due at an absolute deadline**:

  ```text
  due = anchor_wall + (receive_time - anchor_receive) / multiple
  ```

  `multiple` is 1, 10, or 100; `max` has no deadline and plays as fast as
  validation allows. The division is exact integer arithmetic on
  nanoseconds, and every intermediate value is overflow-checked; an overflow
  fails the replay rather than wrapping.
- **Absolute deadlines prevent drift.** Sleeping for each relative gap would
  add processing time and timer latency to every step, so a 10-minute
  recording would run long. With absolute deadlines, a late event is simply
  due now: the driver catches up without waiting until it is back on
  schedule.
- **Equal receive times need no wait**, and events keep the reader's order
  and ingest sequence exactly. Nothing is reordered, skipped, or coalesced
  before the market state.
- **Receive-time regressions still fail closed.** An earlier receive time is
  due at once, and the market state then rejects it and interrupts
  permanently, exactly as without pacing.

At `1x` the 10-minute AAPL/SPY recording takes about 10 minutes of unpaused
playback, at `10x` about 1 minute, at `100x` about 6 seconds, and at `max`
as long as validation takes (about a second).

### Pause, resume, and stop

- Pausing freezes playback: the next event is held and market time stops.
- The paused wall time is added to the schedule's anchor when playback
  resumes, so the remaining events keep their spacing instead of arriving in
  a burst to make up for the pause.
- Stop, used at server shutdown, interrupts any wait immediately; every wait
  is a condition-variable wait woken by pause and stop.
- The recording is read one event at a time, so memory stays bounded
  regardless of recording size.

### End states

- The end of the recording is `completed`; the reader's verified SHA-256 is
  logged and reported as `recording_sha256`. The market stays at the final
  state, and orders can still trade against it.
- A reader error, a market-state rejection, or a timing overflow is `failed`,
  with a safe diagnostic. The market state keeps whatever it last accepted;
  after a rejection it is interrupted and serves no quote.

### Why there is no rewind

Durable accounts record every execution's market observation and fill time,
and accounting requires both to move forward. Rewinding the shared market
state would put every existing account behind the market it traded in, so
their orders would be rejected rather than silently mixing two timelines.
Replaying again therefore means restarting the server and using new
accounts. A later replay-session design can scope accounts to a replay epoch
and make restart safe.
