# Market-data sources and browser notifications

## Boundary

The paper-trading server has exactly one market-data source, chosen when it
starts. A source turns provider data into validated Chronos events and applies
them to the server-owned `MarketState`; nothing after that boundary depends on
the provider.

```text
Databento DBN reader --> PacedReplaySource --+
                                             +--> MarketStateFeed --> MarketState
Alpaca IEX WebSocket --> AlpacaLiveSource ---+          |                  |
                                                        v                  v
                                               MarketDataNotifier   execution, risk,
                                                        |           valuation, API
                                                        v
                                               SSE change notices --> browsers
```

The Alpaca adapter is documented in [alpaca-adapter.md](alpaca-adapter.md).

- `MarketEventReader` (core) is the pull interface a recording implements.
  The Databento adapter provides `EquitiesMiniMbp1EventReader`; the replay
  driver never sees Databento types.
- `MarketDataSource` (`services/market_data`) is the lifecycle interface the
  API uses: a status snapshot, replay commands, and `stop()`. It is safe to
  call from any thread.
- `MarketStateFeed` applies events for every source identically: it counts
  them exactly (failing, not wrapping, at 2⁶⁴ − 1), applies them to
  `MarketState`, and publishes a change notice. A rejected event fails the
  source; it is never skipped.
- Provider names appear only as descriptive data in the source description.
  Execution, accounting, valuation, and the web client have no
  provider-specific conditionals.

Switching sources requires restarting the server; there is no hot swap.

## Lifecycle

`MarketDataSourceState` is one vocabulary for every source:

| State | Meaning |
|---|---|
| `none` | The server has no source; the market stays empty |
| `ready` | A replay is open and waiting for Start |
| `running` | A replay is delivering events |
| `paused` | A replay is holding its next event |
| `completed` | A replay reached the end of its recording |
| `connecting`, `authenticating`, `subscribing` | A live source is establishing its feed |
| `live` | A live source is receiving its subscription |
| `reconnecting` | A live source lost its feed and is retrying |
| `failed` | The source stopped for good; `diagnostic` explains why |
| `stopped` | The server is shutting down |

Replay commands (`start`, `pause`, `resume`) are typed: an invalid
transition reports the current state, and a non-replay source reports that
replay control does not apply.

### Transient unavailability and permanent corruption

These are deliberately different:

- A **permanent** failure is anything that means the source can no longer
  establish a complete current view: malformed normalized data, a sequence
  gap or duplicate, a stream mismatch, a receive-time regression, a corrupt
  recording, or a replay timing/count overflow. A rejected event interrupts
  `MarketState` permanently. Failures outside an event suspend it. In either
  case the source becomes `failed`, every quote is unavailable, and only a
  restart recovers. A replay that reaches EOF normally is `completed`, not
  suspended, and its final state remains usable.
- A **transient** unavailability, such as a network disconnection of a live
  feed, is a source state (`reconnecting`), not a market-state interruption.
  It still makes quotes unavailable for execution, because an old quote must
  never look current (see [Live freshness](#live-freshness)).

## Live freshness

Replay time advances only with events, which is exactly right for a
recording. A live feed can go quiet or lose its connection without any event
saying so, so a live source adds two protections, both kept out of replay:

- **Suspension.** `MarketState::suspend()` makes every execution quote
  unavailable (`source_unavailable`) without interrupting the stream. A live
  source suspends whenever it is not `live`: while connecting,
  authenticating, subscribing, reconnecting, after failing, and when
  stopped. A failed or externally stopped replay also suspends so a valid
  prefix of a corrupt/incomplete recording cannot remain tradable. `resume()`
  starts a freshness epoch in which each instrument is
  `awaiting_fresh_quote` until a top of book received after the resume is
  applied. A quote from before a gap is therefore never served as current,
  and `live` with instruments awaiting quotes means "connected, no quote
  yet".
- **Watermark.** `MarketState::advance_time()` moves market time forward
  without an event. The live source calls it on every read, including each
  1-second idle poll, with its local receive clock. Orders are timestamped with
  market time, so the account's maximum quote age rejects a quote that stopped
  updating, and the portfolio refuses to mark it. Time may stay equal but
  never move backward; nothing is fabricated as an event.

The receive clock (`MonotonicUtcClock`) reads the system clock once at
startup and adds monotonic elapsed time, so receive timestamps and market
time never regress when the wall clock is corrected. A replay never suspends
and never advances time, so its behavior and determinism are unchanged.

## Cost model

```text
one upstream connection (Alpaca IEX) or one local recording
        |
one source thread
        |
one server-owned MarketState
        |
coalesced SSE change notices (at most 4/s per browser)
        |
any number of local browser views
```

Browsers read only Chronos resources. No browser action, poll, or refetch
reaches a provider, so provider usage does not grow with the number of open
views.

### Threads and shutdown

Each source owns one thread. It is started in the constructor, stopped by
`stop()` (idempotent and safe from several threads), and joined in the
destructor; no thread is detached. Every wait is a condition-variable wait
whose predicate includes stop, so shutdown is prompt. The server declares the
market state, notifier, feed, and source before the service and HTTP server,
so it stops HTTP first and joins the source before destroying what it feeds.

`std::jthread` and `std::stop_token` are not used because AppleClang's
standard library still gates them behind an experimental flag.

## Browser notifications

A browser learns about market changes from a Server-Sent Events stream
(`GET /api/v1/market-data/events`), not by polling fast.

- **No market data.** A notice carries only a revision number. The browser
  refetches its typed resources (market data and portfolio valuation), so no
  quote, book, or provider payload is redistributed and no arithmetic moves
  to the browser.
- **Coalesced, never queued.** `MarketDataNotifier` keeps one revision
  counter. `publish()` increments it and wakes idle subscribers; it never
  blocks the source. Each subscriber remembers the last revision it sent and
  sends at most one notice per 250 ms, so any number of changes in that
  window become one notice with the latest revision. There is no
  per-subscriber queue, so a slow or disconnected browser cannot grow memory
  or slow the source.
- **Bounded.** At most 8 streams are open; each holds one reserved HTTP
  worker thread, so streams never starve ordinary requests. A heartbeat
  comment every 10 s keeps idle connections open and finds closed ones.
- **Prompt shutdown.** Stopping the HTTP server first closes the notifier,
  which ends every stream so the server can join its workers.
- **Never behind.** A notice does not restart a read already in flight, so a
  burst of notices cannot starve it. A query that was fetching when a notice
  arrived is read once more when that fetch settles, so the latest change is
  shown without waiting for the safety poll.
- **Fallback.** Without an open stream, market-dependent resources are polled
  every 2 s. While the stream is open they are refetched on each notice, and
  polled only every 15 s as a safety net. Durable account and history
  resources are always polled every 2 s, because other clients can record
  orders.

Every underlying event still reaches `MarketState`; only the browser
invalidations are coalesced.
