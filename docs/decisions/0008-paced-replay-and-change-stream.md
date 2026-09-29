# ADR 0008: Paced replay and browser change notifications

- Status: Proposed
- Date: 2026-09-28

## Context

The server replayed its whole recording before listening, so every user saw
the recording's final, frozen quote. A realistic paper-trading session needs
the market to move while the server runs, under the user's control, and the
browser needs to see it move without polling every resource many times a
second. The deterministic replay core must stay independent of wall time, and
no event may be dropped or reordered on its way to the market state.

## Decision

### A provider-independent source boundary

The server owns exactly one `MarketDataSource`, selected at startup. A replay
source pulls validated events through the core `MarketEventReader`
interface, which the Databento adapter implements; the pacing driver never
sees provider types. Every source applies events through `MarketStateFeed`,
which counts them exactly and fails, rather than skips, on rejection. One
lifecycle vocabulary (`ready`, `running`, `paused`, `completed`, `failed`,
`stopped`, and the live states) describes every source to the API.

### Pace by receive time against an absolute steady-clock schedule

`ReplaySchedule` maps each receive timestamp to an absolute steady-clock
deadline, anchored at the first event:

```text
due = anchor_wall + (receive_time - anchor_receive) / multiple
```

- Receive time, not exchange time, because replay time is receive time and a
  strategy cannot act before the feed delivered data.
- Absolute deadlines, because sleeping for relative gaps accumulates
  processing and timer latency; late events simply become due now.
- Speeds are an enum of exact integer multiples (`1x`, `10x`, `100x`) plus
  `max`, which never waits. No floating-point rate exists.
- All arithmetic is checked `__int128` narrowed to `int64`; overflow fails
  the replay.
- The schedule reads no clock. The driver is given a `MonotonicClock`, so
  tests control time exactly and never sleep.

### Pause shifts the anchor; there is no rewind

Pausing holds the next event and stops market time. On resume the paused
wall time is added to the anchor, so playback continues where it stopped
instead of bursting. Rewind and in-place restart are not implemented: durable
accounts record each execution's market lineage and fill time, and moving the
shared market backward would put every account behind its own history.
Replaying again means restarting the server with new accounts, until a
replay-session design scopes accounts to a replay epoch.

Playback starts in `ready` so a user can load the page first;
`--replay-autostart` starts it at once.

### Threads without `std::jthread`

AppleClang's libc++ still gates `std::jthread` and `std::stop_token` behind
an experimental flag, and AppleClang is a supported compiler. Each source
therefore owns one `std::thread`, started in its constructor and joined by
an idempotent `stop()` (guarded by `std::call_once`) and its destructor. Every
wait is a condition-variable wait whose predicate includes stop.

### Server-Sent Events carry invalidations, not data

`GET /api/v1/market-data/events` streams small notices with a revision
number. The browser invalidates its market-dependent queries and refetches
the existing typed resources.

- One-way updates suit SSE; WebSockets would add a protocol and state for no
  benefit here.
- Carrying no market data keeps provider data inside the server (no raw feed
  redistribution) and keeps all arithmetic on the server.
- `MarketDataNotifier` stores one revision counter. Each subscriber
  remembers what it last sent and sends at most one notice per 250 ms, so
  changes coalesce without any per-subscriber queue; `publish()` never waits
  for a subscriber. Every event still reaches the market state.
- Streams are capped at 8, each on a reserved HTTP worker thread, with a
  10-second heartbeat. Closing the notifier ends them at shutdown.
- Polling remains the fallback when the stream is unavailable.

A separate `GET /api/v1/market-data` resource reports the source, stream,
and per-instrument quote availability without touching the database, so it
is cheap to refetch on every notice.

## Consequences

- The HTTP server becomes available immediately with a replay source, and a
  recording is streamed from disk rather than loaded.
- A completed replay stays at its final state until the server restarts.
- A disconnected browser holds a stream slot until the next heartbeat fails
  to write, at most 10 seconds.
- Existing durable accounts from an earlier session are behind a restarted
  replay; new accounts are needed for each session.

## Alternatives considered

- **Sleep for each inter-event gap.** Simple, but drift grows with every
  event and pause handling becomes ad hoc.
- **A floating-point speed multiplier.** Allows arbitrary speeds, but
  introduces rounding into timing without a product need.
- **Push quotes over the stream.** Fewer requests, but it duplicates the
  resource contracts in a second format and redistributes provider data to
  the browser.
- **WebSockets.** Bidirectional capability is unnecessary for invalidation.
- **Per-subscriber event queues.** They need bounds and drop policies; a
  revision counter needs neither.
