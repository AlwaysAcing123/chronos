# ADR 0009: Alpaca IEX as the first live market-data adapter

- Status: Proposed
- Date: 2026-09-29

## Context

Chronos needs a live market so a paper trader can act on current prices.
Databento remains the primary research and historical provider
([ADR 0001](0001-market-data-provider.md)), but a live Databento subscription
is deliberately deferred. Alpaca offers a free live stream of IEX trades and
quotes that is enough for a local, single-user Level-1 demonstration. A live
feed also introduces problems replay never had: connections drop, an idle
feed produces no events, credentials must be handled, and provider data can
be malformed.

## Decision

### Alpaca IEX, market data only

Add an optional `CHRONOS_ENABLE_ALPACA_ADAPTER` build and a `--alpaca-iex`
server source that consumes `wss://stream.data.alpaca.markets/v2/iex` only.
Chronos never calls Alpaca's trading or brokerage APIs and never uses its
paper broker; Chronos keeps execution and accounting. The feed is labelled
`Alpaca IEX` everywhere with a notice that IEX is one exchange, not SIP, not
the NBBO, and not every US exchange. The free plan's 30-symbol limit is
enforced before connecting.

### Transport: cpp-httplib's WebSocket client

The adapter uses `httplib::ws::WebSocketClient` from the cpp-httplib commit
already pinned by SHA-256 for the API server (ADR 0004), built with OpenSSL
when the Databento or Alpaca adapter is enabled. Certificate and host-name
verification stay on. No new dependency is added, and cpp-httplib's
headers stay outside Chronos's `-Werror` as system includes.

Alternatives considered:

- **Boost.Beast.** Mature and flexible, but it brings Boost and Asio's
  asynchronous model into a codebase that uses one thread per source.
- **IXWebSocket, websocketpp, libwebsockets.** Capable, but each is another
  pinned dependency with its own TLS integration, for features Chronos does
  not need.
- **Shelling out to `wscat` or `websocat`.** Rejected: an external process
  would own the credentials and the connection, and could not be shut down
  or tested deterministically.

cpp-httplib's WebSocket support is newer than its HTTP client. Every call is
bounded by a timeout, and the source handles every failure as a transport
closure, so a transport defect can reconnect but cannot corrupt data.

### An explicit lifecycle with deliberate failure classes

The source moves through `connecting`, `authenticating`, `subscribing`,
`live`, and `reconnecting`, each handshake step with its own timeout.
Network loss, timeouts, and Alpaca's transient errors reconnect after a
bounded exponential backoff with injectable jitter. Rejected credentials, an
insufficient plan, a mismatched subscription, unknown message types, and any
message that violates the market-data contract fail the source until
restart, so malformed data is never hidden by reconnecting forever.

### Live freshness: suspension, fresh quotes, and a watermark

Two mechanisms keep an old quote from looking current:

- The source suspends the server's `MarketState` whenever it is not live. A
  suspended market serves no execution quote (`source_unavailable`). On
  resubscription each instrument is `awaiting_fresh_quote` until a quote from
  the new connection arrives.
- While live, the source advances market time to its local receive clock on
  every read, including idle 1-second polls (`MarketState::advance_time`).
  Order time is market time, so the existing quote-age limit rejects a quote
  that stopped updating. No event is fabricated.

Transient unavailability is a source state; permanent corruption still
interrupts `MarketState`. Replay never suspends or advances time, so its
determinism is unchanged. The receive clock reads the system clock once at
startup and advances with the monotonic clock, so receive times never move
backward.

### Exact numbers from the JSON text

Frames are parsed with a SAX handler that keeps every number's lexeme.
Prices and sizes are parsed from that text into fixed-point values; nothing
is converted through `double`, and unsupported precision is rejected rather
than rounded. Sizes are shares, per Alpaca's 2025-11-03 changelog; if they
were ever lots, liquidity would be understated, not overstated.

### Credentials only from the environment

`APCA_API_KEY_ID` and `APCA_API_SECRET_KEY` are read at startup. They are
never command-line options, never sent to the browser, and never logged:
the credentials type exposes only the authentication message and a
redaction function that every diagnostic passes through. Keeping them in the
environment keeps production code portable; retrieving them from the macOS
Keychain is a launch-time concern of the operator.

### Message authentication

Chronos authenticates with the `auth` message rather than WebSocket upgrade
headers, so authentication is an explicit, individually timed step whose
acknowledgement and rejection are ordinary messages.

## Consequences

- One upstream connection per server feeds every browser; UI activity never
  reaches Alpaca.
- Quotes become unavailable during any disconnect and stay so per instrument
  until fresh data arrives, so a live market can be less available than a
  replay.
- Market time trails receipt by up to one poll interval (1 s) while idle.
- cpp-httplib applies the poll timeout to the rest of a frame as well, so a
  mid-frame stall longer than a second closes the connection and forces a
  (safe) reconnect. A shorter poll would make that likelier; a separate
  ticker thread for market time was rejected because it could advance time
  past an event whose receive time was already taken.
- Alpaca publishes no venue sequence, so gaps upstream cannot be detected.
- Corrections and cancels are not applied; execution and valuation use quotes.
- Accounts remained single-instrument at the time (multi-instrument since
  [ADR 0010](0010-multi-instrument-accounts.md)), and each live session needs
  a new stream ID and new accounts.

## Revisit when

- Consolidated or deeper live data is needed (a Databento live
  subscription).
- Alpaca changes its protocol, size units, or plan limits.
- Multiple simultaneous sources or hot swapping are required.
