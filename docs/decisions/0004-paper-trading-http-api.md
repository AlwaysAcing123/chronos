# ADR 0004: In-process C++ paper-trading HTTP API

- Status: Proposed
- Date: 2026-09-28

## Context

The durable paper-trading path (`PersistentPaperOrderSession`) now accepts,
simulates, books, journals, and recovers orders. The manual order ticket and
future model runtimes need one typed, network-accessible entry point to it.

The boundary must not create a second execution truth. Risk, execution,
accounting, and persistence already live in C++ with exact fixed-point types,
and clients must never choose the quote at which they trade.

## Decision

### Keep the API in the C++ process

The HTTP transport runs in the same process as the core and calls a typed
`PaperTradingService` directly. The service is transport-independent and is
tested without an HTTP server; the HTTP layer only parses, routes, and maps
typed results to status codes and JSON.

A service written in another language would need either Python/TypeScript
bindings for the domain or an RPC boundary into C++. Both duplicate the order,
fixed-point, and result types at a trust boundary and add a second runtime,
for no product benefit in this slice. The plan's Python layer remains for
research, and the web application stays a client of this API.

### Libraries

| Role | Choice | Pin |
|---|---|---|
| HTTP/1.1 server | cpp-httplib 0.58.0 | commit `4f3f9ef19be83ae97a5d9a059432dc00e445b7ab`, SHA-256 `415e8d638f6e2390f51565d4fdc86577dc0ef09f56da7ea74d7731d9da8349a6` |
| JSON | nlohmann/json 3.12.0 | release `json.tar.xz`, SHA-256 `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa` |

Both are MIT-licensed, header-only, and already present in the dependency
graph of the pinned Databento C++ SDK, which fetches them by tag without a
hash. Chronos declares them first, so a build that enables both the API and
the Databento adapter uses one hash-verified copy of each. The nlohmann/json
archive was also checked against the maintainer's release signature
(`7971 67AE 41C0 A6D9 232E 4845 7F3C EA63 AE25 1B69`); cpp-httplib does not
sign releases, so it is pinned by commit archive, like the Databento SDK.

cpp-httplib is chosen for a small footprint, first-class CMake integration,
straightforward synchronous handlers, and built-in WebSocket support for a
later live-update slice. Its security record is the main cost: more than
twenty advisories were published between 2025 and 2026, mostly in header
parsing, chunked encoding, and client TLS. All are fixed before 0.51, and
0.57.0 contains further server-side request-smuggling fixes, so the pin is the
latest release rather than the SDK's 0.54.1. Chronos contains that risk by:

- binding to `127.0.0.1` by default and documenting that the API is not
  production-secure;
- binding exclusively: the library's default `SO_REUSEPORT` would let a second
  server process share the port and silently receive part of the traffic;
- limiting request bodies, read time, and write time;
- disabling compression, TLS, and other optional features the server does not
  need, except TLS when the Databento historical client requires it;
- confining `httplib.h` to one translation unit behind a transport-independent
  request/response interface, so the library can be replaced without touching
  the service, routing, or JSON contract.

cpp-httplib configures itself with preprocessor macros. Two translation units
compiled with different macros in one binary violate the one-definition rule;
the Databento SDK documents a past segfault from exactly this. Chronos
therefore configures httplib once per build tree: OpenSSL is required when the
Databento adapter is enabled and disabled otherwise, and optional compression
support is always disabled. Every consumer receives the same macros through
the `httplib::httplib` target.

nlohmann/json is chosen for maturity, UTF-8 validation, a non-throwing parse
entry point, and a parser callback that can reject duplicate object keys. The
API never uses its number conversion for financial input: decimal values are
JSON strings parsed by Chronos into fixed-point types.

### Alternatives considered

- **Boost.Beast**: mature, asynchronous, and WebSocket-capable, but requires
  Boost and a hand-written connection loop, parser limits, timeouts, and
  router. That is more code on the trust boundary than this slice warrants.
- **Drogon**: a complete framework with its own database layer, which would
  compete with the libpq repository.
- **Crow**: ergonomic, but depends on Asio and has a smaller review history.
- **Pistache**: Linux-only; macOS is a supported development platform.
- **oat++, POCO, uWebSockets, CivetWeb**: heavier frameworks or C-style APIs
  that add integration work without improving the first slice.
- **Boost.JSON, RapidJSON, simdjson, glaze**: faster, but speed is irrelevant
  here. RapidJSON is effectively unmaintained, simdjson is parse-only, and
  current glaze requires C++23.

### Request processing and connections

Handlers are synchronous. `PaperTradingService` owns exactly one
`PostgresPaperOrderRepository` connection and serializes every operation with
one mutex, because libpq connections and sessions are not thread-safe.

Sessions are request-scoped: each submission loads the account inside a
repeatable-read snapshot, submits, and discards the session. The database is
the only source of account truth between requests, so several server
processes can share one database; optimistic concurrency resolves races, and
the losing request receives a conflict without writing anything. Loading per
request costs time proportional to the account's journal. That is acceptable
until profiling shows otherwise; a session cache or incremental recovery can
be added behind the service without changing the API.

### Market state and time

The server owns market data through `MarketState`, fed by a replay driver now
and by a live feed later. Order submission reads the account instrument's
current execution quote; the request contains no price other than an optional
limit, and no timestamp. The order's `submitted_at` is the market clock, the
receive time of the latest accepted event, so replays are reproducible and
freshness is judged by the existing risk rule in the stream's own time.

### JSON representation

- The API never emits a JSON number. Money, prices, and quantities are
  canonical decimal strings; versions, sequences, and nanosecond durations are
  base-10 integer strings; timestamps are RFC 3339 UTC strings with exactly
  nine fractional digits.
- Requests are strict: unknown fields, duplicate keys, `null`, non-string
  decimals, exponents, signs other than a leading minus, leading zeros, and
  excess fractional digits are rejected before the domain is invoked.

## Consequences

- Manual and model orders share one in-process path from request to journal.
- The default core build still has no external dependencies; the API is an
  optional target that requires the PostgreSQL adapter.
- One slow database operation delays every request in a process. A connection
  pool with per-account serialization is the planned scaling step.
- Enabling the API with the Databento adapter compiles the SDK's historical
  client against cpp-httplib 0.58.0 instead of 0.54.1, and without optional
  HTTP compression. The SDK's offline tests pass in that configuration; its
  paid download path is not exercised by automated tests.
- Authentication, TLS, and authorization are not implemented. The API must not
  be exposed beyond localhost.
