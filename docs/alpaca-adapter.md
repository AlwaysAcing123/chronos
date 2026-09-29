# Alpaca IEX live adapter

The optional Alpaca adapter (`adapters/alpaca`) streams live US-equity trades
and quotes from Alpaca's free IEX feed into the paper-trading server's market
state. Databento remains the primary research and historical provider
([ADR 0001](decisions/0001-market-data-provider.md)); Alpaca IEX is an
inexpensive secondary live Level-1 feed
([ADR 0009](decisions/0009-alpaca-iex-live-adapter.md)).

Chronos only consumes Alpaca market data. It never calls Alpaca's trading,
order, position, account, or brokerage APIs, and it does not use Alpaca's
paper broker: Chronos remains the execution and accounting system. The only
endpoint in the code is `wss://stream.data.alpaca.markets/v2/iex`.

## IEX is one exchange

The free feed carries quotes and trades from the Investors Exchange (IEX)
alone. It is **not** consolidated SIP data, **not** the NBBO, and does **not**
cover every US exchange; IEX's share of volume is small, and its best bid and
offer can be worse than the national best. Every Chronos surface says so:
the source is labelled `Alpaca IEX`, its `coverage_notice` states the
limitation and its `coverage_label` ("IEX only, not SIP or NBBO") summarizes
it, the web client shows both persistently, and every event carries the
`SourceSpecific` quality flag and venue `IEX`. Simulated fills against these
quotes are fills against one venue's displayed liquidity.

## Build and run

```sh
cmake -S . -B build-api \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON \
  -DCHRONOS_ENABLE_PAPER_TRADING_API=ON \
  -DCHRONOS_ENABLE_ALPACA_ADAPTER=ON \
  -DPostgreSQL_ROOT=/opt/homebrew/opt/libpq
cmake --build build-api --parallel

# Credentials only from the environment, never on the command line.
export APCA_API_KEY_ID=...        # your Alpaca key ID
export APCA_API_SECRET_KEY=...    # your Alpaca secret key
CHRONOS_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ./build-api/chronos_paper_trading_server \
    --stream-id alpaca-iex-2026-09-29 \
    --instrument AAPL --instrument SPY \
    --alpaca-iex
```

On macOS, credentials kept in the Keychain can be loaded into the
environment of the server process alone, without printing them:

```sh
APCA_API_KEY_ID="$(security find-generic-password -a chronos -s com.chronos.alpaca.api-key-id -w)" \
APCA_API_SECRET_KEY="$(security find-generic-password -a chronos -s com.chronos.alpaca.api-secret -w)" \
CHRONOS_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ./build-api/chronos_paper_trading_server --stream-id alpaca-iex-2026-09-29 \
    --instrument AAPL --instrument SPY --alpaca-iex
```

Production code never calls the Keychain; it reads the two variables.
`--alpaca-iex` cannot be combined with `--replay-dbn` or its options, allows at
most 30 `--instrument` values, and requires a build with
`CHRONOS_ENABLE_ALPACA_ADAPTER=ON`. The HTTP server listens at once; `/health`
and `/api/v1/market-data` report `connecting`, `authenticating`,
`subscribing`, `live`, or `reconnecting` while the feed establishes itself.

Use a new `--stream-id` and new accounts for each live session: ingest
sequences start at zero in every process, so an account that traded in an
earlier session is behind the new stream and fails closed
(`submission_timestamp_moved_backward` or `incompatible_market_timeline`),
as when switching sources.

## Documentation consulted

Verified on 2026-09-29 against Alpaca's current documentation:

- [Real-time Stock Data](https://docs.alpaca.markets/docs/real-time-stock-pricing-data)
  (page updated 2026-05-18): stream URL, the `v2/iex` feed, trade and quote
  schemas, RFC-3339 nanosecond timestamps, corrections and cancels.
- [WebSocket Stream](https://docs.alpaca.markets/docs/streaming-market-data)
  (page updated 2026-08-27): the `connected` greeting, message
  authentication within 10 seconds, subscription acknowledgements, framing,
  error codes, and the connection limit.
- [CTA/UTP bid and ask size display change](https://docs.alpaca.markets/us/changelog/marketdata-bid-and-ask-size-display-change)
  (changelog): quote sizes are shown in shares from 2025-11-03.

### Protocol facts relied on

- One URL per feed: `wss://stream.data.alpaca.markets/v2/iex`.
- After the WebSocket opens, the server sends `[{"T":"success","msg":"connected"}]`.
- Authentication by message: `{"action":"auth","key":…,"secret":…}`,
  answered by `[{"T":"success","msg":"authenticated"}]`, within 10 seconds.
  (Header authentication also exists; Chronos uses the message so each
  handshake step is explicit.)
- `{"action":"subscribe","trades":[…],"quotes":[…]}` is answered with the
  complete current subscription, `[{"T":"subscription","trades":[…],"quotes":[…],…}]`.
  A trade subscription automatically adds `corrections` and `cancelErrors`.
- Every frame is a JSON array. Control messages (`success`, `error`,
  `subscription`) arrive alone; data messages may be batched.
- Errors are `[{"T":"error","code":N,"msg":…}]` with codes 400 invalid syntax,
  401 not authenticated, 402 auth failed, 403 already authenticated, 404 auth
  timeout, 405 symbol limit exceeded, 406 connection limit exceeded, 407 slow
  client, 409 insufficient subscription, 410 invalid subscribe action, and 500
  internal error.
- Many subscriptions allow one connection per endpoint.
- Quote fields: `T`="q", `S`, `bx`, `bp`, `bs`, `ax`, `ap`, `as`, `c`, `t`, `z`.
  Trade fields: `T`="t", `S`, `i`, `x`, `p`, `s`, `c`, `t`, `z`.

### Assumptions

- **Sizes are treated as shares.** Alpaca's current stock quote schema still
  describes `bs` and `as` as "in round lots", while its 2025-11-03 change
  notice says CTA/UTP quote sizes are shown in shares. That notice describes
  the consolidated SIP feeds and does not explicitly settle the IEX mapping.
  IEX's direct feed represents size in shares, so Chronos uses Alpaca's raw
  IEX size without multiplying. This remains a documented provider
  assumption until Alpaca's IEX-specific schema is unambiguous. If Alpaca
  were actually sending lots, Chronos would understate displayed liquidity,
  producing smaller simulated fills rather than inventing liquidity. Trade
  size `s` is shares.
- **IEX exchange code `V`.** Every non-empty quote side and every trade on the
  IEX feed must carry Alpaca's IEX code `V`; anything else fails the source.
- **No data before the acknowledgement.** Market data before the subscription
  is acknowledged is treated as a protocol violation.
- **Integers for sizes.** The schema types sizes as integers; a fractional or
  exponent size is rejected.

## Lifecycle

```text
Connecting --(WebSocket open, "connected")--> Authenticating
Authenticating --("authenticated")--> Subscribing
Subscribing --(acknowledgement matches)--> Live
any state --(transient failure)--> Reconnecting --(backoff)--> Connecting
any state --(fatal failure)--> Failed (until restart)
any state --(server stop)--> Stopped
```

| Step | Timeout | Timeout outcome |
|---|---|---|
| TCP connect, TLS and WebSocket handshakes | 5 s per operation | reconnect |
| `connected` greeting | 10 s | reconnect |
| `authenticated` | 10 s | reconnect |
| subscription acknowledgement | 10 s | reconnect |
| idle poll while live | 1 s | advance market time, keep reading |
| rest of a frame after its first bytes | 1 s (cpp-httplib uses the poll timeout) | close; reconnect |
| WebSocket ping | every 10 s; 2 missed pongs close the connection | reconnect |

Failures are classified deliberately:

| Transient: reconnect after backoff | Fatal: `failed` until restart |
|---|---|
| connection refused, reset, closed, or TLS failure | 402 auth failed (credentials rejected) |
| handshake step timeout | 409 insufficient subscription |
| 404 auth timeout, 406 connection limit, 407 slow client, 500 | 405 symbol limit, 410 invalid subscribe action |
| | 400, 401, 403 (a request Alpaca did not accept) |
| | an undocumented error code |
| | a frame that is not valid JSON, an unknown message type, a missing or mistyped required field |
| | market data before the acknowledgement, or a control message mixed with data |
| | an acknowledgement that differs from the request |
| | a quote or trade that violates the market-data contract |
| | a binary frame, a market-state rejection, ingest-sequence exhaustion |

Malformed data is never hidden by reconnecting: the frame's events are not
applied, the market is suspended, and `diagnostic` explains the failure
without echoing the frame.

Reconnect delays are bounded exponential with equal jitter
(`ReconnectBackoff`): attempt *n* waits between half and all of
`min(30 s, 1 s × 2ⁿ)`. Reaching `live` resets the backoff. The wait is on the
injected `MonotonicClock` and ends at once on shutdown; tests inject the
jitter and never sleep.

At most one upstream WebSocket exists: the previous connection is closed
before the next attempt begins.

## Normalization

`AlpacaNormalizer` maps a frame's data messages to validated Chronos events.
Alpaca types never leave the adapter.

| Alpaca | Chronos |
|---|---|
| quote (`T`="q") | `TopOfBookEvent` |
| trade (`T`="t") | `TradeEvent`, aggressor `Unknown` (not reported) |
| `S` | `InstrumentId`; it must be a subscribed symbol |
| `t` | `event_timestamp` (RFC 3339, nanoseconds) |
| local receipt, from the receive clock | `receive_timestamp` |
| adapter counter | `ingest_sequence`, contiguous across reconnects |
| none | `venue_sequence = null` |
| feed | `venue = IEX` |
| always | quality `SourceSpecific` (IEX-only data; not suspect) |
| `bp` > `ap` | quality `PotentiallyInvalidBook` (suspect, never executable) |
| receipt before `t` | quality `QuestionableReceiveTimestamp` (suspect) |
| `bp`/`bs` = 0/0 (or `ap`/`as`) | that side is empty |
| `c`, `z`, `i` | validated for type, not stored |
| `c` (correction), `x` (cancel) | not applied (see Limitations) |

### Exact numbers

- The frame is parsed with a SAX handler that keeps each JSON number's exact
  text. nlohmann/json computes a `double` for a decimal token internally;
  Chronos discards it unread. No price or size ever passes through binary
  floating point.
- Prices are parsed with `Price::parse` (nine decimal places, the Databento
  scale). Exponent notation is rejected, and more than nine decimal places is
  rejected as inexact rather than rounded; values beyond the `int64`
  fixed-point range are rejected.
- Sizes must be whole, non-negative integers in shares, parsed with
  `Quantity::parse` and range-checked.
- A side is empty only when price and size are both zero. A zero price with a
  positive size, the reverse, or a negative value is inconsistent and fails
  closed.
- A locked book (bid equal to ask) is valid; a crossed book is flagged as
  potentially invalid, as in the Databento normalizer and the market-data
  contract.

### Timestamps

`parse_rfc3339_timestamp` accepts `YYYY-MM-DDTHH:MM:SS[.1–9 digits](Z|±HH:MM)`,
applies offsets exactly, and rejects impossible dates and times, leap seconds,
more than nine fractional digits, times before 1970, and times beyond the
signed-nanosecond range (2262). Receipt time is read from a
`MonotonicUtcClock` as soon as a frame is read, before parsing: the system
clock is read once at startup and advanced by the monotonic clock, so a
wall-clock step never reorders receive times. When local receipt precedes the
provider's event time (clock skew), the event keeps both timestamps as
observed and is flagged `QuestionableReceiveTimestamp`, which the risk engine
rejects; no timestamp is adjusted.

### Ordering and sequences

Events keep the provider's message order within and across frames. A frame
is normalized atomically: every data message becomes an event with the next
contiguous ingest sequence, or, if any message is rejected, none does and the
sequence does not advance. One sequence runs for the life of the process,
across reconnects. Alpaca publishes no venue sequence, so Chronos cannot
detect a gap in what IEX or Alpaca delivered; the ingest sequence only proves
that Chronos applied what it received in order.

## Freshness and availability

A live feed can stop without any event saying so. Chronos therefore keeps two
separate protections (see [market-data-sources.md](market-data-sources.md#live-freshness)):

1. **Source availability.** The source suspends `MarketState` in every state
   but `live`. A suspended market serves no quote: orders are refused, not
   recorded, with `503 market_data_unavailable` / `source_unavailable`, and
   the portfolio is not valued. When the subscription is acknowledged the
   market resumes, but each instrument stays `awaiting_fresh_quote` until a
   quote from the new connection is applied. `live` with instruments still
   awaiting quotes is "connected, no quote yet"; `live` with every instrument
   available is "live and ready".
2. **Quote age.** While live, every read, including each 1-second idle poll,
   advances market time to the local receive clock
   (`MarketState::advance_time`). Order submission time is this market time,
   and the account's `maximum_quote_age_ns` (5 s by default) rejects a quote
   whose receive time is older. A quiet or dead-but-open connection therefore
   makes quotes stale within the quote-age limit even before the ping check
   closes it. No event is fabricated to advance time, and replay never calls
   `advance_time`.

Market time trails real receipt time by at most one poll interval (1 s)
plus processing while the feed is idle, so a quote may be up to that much
older than the limit at submission; an active feed advances time with every
frame.

## Credentials

- Read only from `APCA_API_KEY_ID` and `APCA_API_SECRET_KEY`; never from the
  command line, a file, or the Keychain in code.
- A missing, empty, or whitespace-containing variable stops startup with exit
  code 2 and a message naming the variable, never its value.
- `AlpacaCredentials` is move-only, has no accessor or stream operator, and
  overwrites its strings when destroyed. The authentication frame is built
  only to be sent and is overwritten after sending; it is never logged or
  stored.
- Every diagnostic and log line from the source passes through
  `AlpacaCredentials::redact`, so even an Alpaca error that echoed a
  credential would appear as `[REDACTED]`. Diagnostics include Alpaca's short
  error text, made printable and bounded, but never frame content.
- The browser never receives or requests credentials; the API has no route
  that exposes them.

## Transport

The adapter uses `httplib::ws::WebSocketClient` from the cpp-httplib commit the
API already pins by SHA-256, built with OpenSSL. Server certificate and host
name verification are always enabled, against the system trust store (the
macOS Keychain roots or OpenSSL's default paths). Each blocking call has a
timeout, so stop is noticed within one poll (1 s) or, while a handshake is
in progress, within the connection timeouts. A clean close waits up to
cpp-httplib's 5-second close handshake. cpp-httplib applies the read
timeout to every read of a frame, and only a timeout before a frame's first
byte is reported as an idle poll; a stall of more than one poll in the middle
of a frame is a failure that closes the connection. The source then
reconnects (quotes suspended, fresh quotes required), so such a stall is
safe but costs a reconnect. The 1-second poll makes this rare on real
networks while keeping stop and market-time lag within a second. See ADR
0009 for the alternatives considered.

## Cost model

```text
one Alpaca WebSocket (IEX)
        |
one Chronos live adapter (AlpacaLiveSource thread)
        |
one server-owned MarketState
        |
coalesced local SSE change notices (at most 4/s per browser)
        |
one or more local browser views, refetching Chronos resources
```

Browsers never talk to Alpaca and never cause an Alpaca request: UI polling
and SSE refetches read Chronos's own resources. Provider usage is one
connection and one subscription per server process, independent of how many
browsers are open.

## Tests

No test uses credentials, the network, or real time:

- `chronos_alpaca_adapter_tests`: wire decoding, exact numbers, precision and
  range limits, timestamps, empty and crossed books, lineage, frame
  atomicity, sequence exhaustion, symbol deduplication and the 30-symbol
  limit, acknowledgement matching, credential errors and redaction, and the
  endpoint allowlist.
- `chronos_alpaca_live_source_tests`: the state machine over a scripted fake
  transport with a manual clock: handshake, rejected credentials, handshake
  timeouts, exponential backoff, disconnects that block stale quotes until
  fresh ones arrive, market-time advance during silence, frame ordering,
  fail-closed protocol violations, transient errors, shutdown during connect,
  backoff, and live streaming, and secret-free logs.
- `chronos_alpaca_websocket_transport_tests`: the real cpp-httplib transport
  against a WebSocket server on 127.0.0.1 emulating Alpaca's handshake.

## Limitations

- IEX only; not SIP, not NBBO, not every exchange.
- No venue sequence, so no gap detection.
- Trade corrections and cancels are received but not applied, so the last
  trade view can show a trade Alpaca later corrected. Execution and
  valuation use quotes only.
- An account binds to one stream for all its instruments, so it can trade
  both AAPL and SPY within one live session, but each session needs new
  accounts.
- No hot swap between sources; restart the server, with a new stream ID and
  accounts, to switch.
- Alpaca's market-data terms govern display and redistribution; the local,
  single-user server does not redistribute provider data beyond its own
  browser views.
