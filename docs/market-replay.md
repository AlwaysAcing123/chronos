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

## Current boundary

This increment supports event-step replay of synthetic and local real DBN
recordings. It does not yet pace events at real time, schedule timers, persist
normalized events, calculate features, or simulate execution. Those
responsibilities build on the replay clock and market view rather than
entering the provider adapter.
