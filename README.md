# Chronos

Chronos is a real-market research, model-evaluation, and paper-trading
platform. The project is being built as a sequence of small, tested vertical
slices, beginning with exact financial arithmetic, portfolio accounting, and
normalized market-data contracts.

Chronos simulates trading only. It does not place real-money orders.

## Current implementation

- Fixed-point price, quantity, and money types
- Provider-independent market-event types
- Structured single-event market-data validation
- A validated-event type that cannot be created without passing validation
- Stateful stream, duplicate, regression, and sequence-gap detection
- Optional Databento `EQUS.MINI` MBP-1 record normalizer
- Streaming DBN file ingestion with timestamp-aware symbol resolution
- Checksum-backed, write-once recording manifests
- Quote-first, cost-bounded historical DBN acquisition
- Offline deterministic DBN replay with event counts and final market views
- Fill and trading-domain types
- Multi-instrument, long-only paper-account accounting: one shared cash
  balance and a durable ledger per instrument, kept when a position closes
- Exact cash, position, cost-basis, fee, and PnL calculations, with account
  totals and the accounting identity checked across every position
- Shared manual/model paper-order intents with explicit source attribution
- Structured pre-trade validation, quote-freshness checks, and risk limits
- Deterministic top-of-book execution of immediate-or-cancel market and
  limit orders and of durable good-til-cancelled limit orders, which work
  across restarts and fill from later quotes in FIFO priority
- Partial fills, one fee per order, cancellation reasons, idempotent
  client-requested cancellation, account-wide idempotency, and
  per-account, per-instrument shadow liquidity
- Server-computed cash and share reservations for working orders, enforced by
  risk and by commit-time database checks
- End-to-end order processing that immediately books simulated fills into the
  owned paper account
- Exact reconciliation from order and fill through cash, position, fees, and
  PnL
- Optional PostgreSQL persistence with atomic, normalized account, position,
  and execution-state snapshots, commit-time integrity checks, and an
  append-only order, rejection, fill, and portfolio event journal
- Validated restart recovery, database idempotency, account-row locking with
  lock timeouts, and optimistic versions as a second defense
- Reconnection after a lost database connection, reported as a typed
  unavailability error
- A thread-safe, server-owned market state whose clock timestamps orders, so
  clients can never choose the quote or time they trade at
- A transport-independent paper-trading application service shared by manual
  and model orders
- An optional local HTTP API for health, account creation and retrieval,
  IOC and GTC order submission, working orders, cancellation, per-order
  fills and lifecycle events, history, and portfolio valuation, with exact
  decimal strings and stable machine-readable errors
- Durable order and fill history read from the journal, with stable
  newest-first keyset pagination, an optional per-instrument filter, and
  order lookup for reconciling uncertain submissions
- Read-only portfolio valuation of every open position at its instrument's
  current best bid, all or nothing, with exact per-position and account market
  value, unrealized and total PnL, equity, and a fixed-point return percentage
  that reconcile before they are returned
- A local React and TypeScript paper-trading workspace: account creation, a
  watchlist of the configured instruments with the selected symbol's quote,
  portfolio positions and closed ledgers, a manual order ticket with a symbol
  selector and time in force that reconciles uncertain submissions, working
  orders with reservations, reconciled cancellation, and each order's
  lifecycle, and durable order and fill history filterable by symbol
- Paced historical replay inside the running server: a recording plays by
  receive time at `1x`, `10x`, `100x`, or `max` against an absolute
  steady-clock schedule, with start, pause, and resume controls
- A provider-independent market-data source boundary with one lifecycle
  vocabulary, and a market-data resource reporting the source, stream, and
  per-instrument quote availability and current quotes
- Coalesced Server-Sent Events change notices that let the web workspace
  refresh market-dependent views as the market moves, and account views when
  a working order fills, with polling as the fallback
- Optional live market data from Alpaca's free IEX stream: one exact,
  fail-closed WebSocket adapter with an explicit connect, authenticate, and
  subscribe lifecycle, reconnection with backoff, and quotes that are never
  executable while the feed is down or before a fresh quote arrives. IEX is
  one exchange, not SIP or the NBBO, and is labelled so everywhere
- Provider-independent normalized market datasets: immutable,
  content-addressed directories of canonical NDJSON events with exact raw
  fixed-point strings, a strict manifest, and a deterministic quality report,
  published atomically and verified end to end when read; converted offline
  from Databento recordings and replayable by the existing replay code
- A point-in-time `top_of_book` feature engine (best bid and ask, sizes,
  spread, midpoint, imbalance, microprice) with checked integer arithmetic,
  typed unavailability, and reproducible, content-addressed feature artifacts
- Unit and edge-case tests for arithmetic, accounting, market data, replay,
  datasets, features, risk, and execution, plus real-PostgreSQL service and
  API tests

The core accepts, simulates, books, persists, and recovers paper orders
through controlled paths, the optional API exposes that path to local
clients, and the web workspace in `apps/web/` is the first of them. A
replayed market now moves while the server runs, and an optional live IEX
feed can replace it. Recordings become normalized datasets and feature
artifacts for research. Labels, model evaluation, authentication, model
deployment, and historical charts are not built yet. See [plan.md](plan.md)
for the complete roadmap.

## Requirements

- CMake 3.24 or newer
- A C++20 compiler with a signed 128-bit integer extension

The supported development compilers are AppleClang, Clang, and GCC.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --no-tests=error
```

Compiler warnings are treated as errors.

## Run with sanitizers

AddressSanitizer detects memory-safety errors. UndefinedBehaviorSanitizer
detects operations whose behavior is not defined by C++.

```sh
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ctest --test-dir build-sanitized --output-on-failure --no-tests=error
```

Sanitizers require AppleClang, Clang, or GCC. On macOS, use AppleClang or
Clang; the Homebrew GCC toolchain does not provide the required Darwin
sanitizer runtime.

## Build the Databento adapter

The provider adapter is optional so the core can build without downloading a
vendor SDK. Its first configuration downloads a checksum-pinned Databento C++
release and requires OpenSSL 3 and Zstandard.

```sh
cmake -S . -B build-databento \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON
cmake --build build-databento --parallel
ctest --test-dir build-databento --output-on-failure --no-tests=error
```

Provider tests generate synthetic DBN recordings with the official SDK. They
exercise metadata, decoding, symbol mapping, normalization, source-integrity,
and manifest edge cases without an API key or licensed market-data download.

The optional build also creates `chronos_databento_historical`, a JSON-output
command-line tool that quotes and downloads small real recordings. It refuses
to download without an explicit maximum quoted cost and never overwrites an
existing recording. See
[docs/historical-data-acquisition.md](docs/historical-data-acquisition.md).

`chronos_databento_replay` reads a local recording without network access,
validates and replays every event, recomputes its checksum-backed manifest, and
prints deterministic JSON containing event counts and each instrument's final
trade and top of book. See [docs/market-replay.md](docs/market-replay.md).

`chronos_databento_normalize` converts a local recording, offline, into an
immutable normalized dataset in a catalog directory such as the ignored
`data/normalized/`:

```sh
./build-databento/chronos_databento_normalize \
  --input data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
  --catalog data/normalized
```

## Normalized datasets and features

The default build includes `chronos_dataset`, which lists, verifies, and
inspects normalized datasets and extracts point-in-time feature artifacts:

```sh
./build/chronos_dataset list --catalog data/normalized
./build/chronos_dataset inspect --catalog data/normalized --dataset <ID>
./build/chronos_dataset features extract --catalog data/normalized \
  --dataset <ID> --output data/features
./build/chronos_dataset features inspect --output data/features \
  --artifact <ARTIFACT-ID> --catalog data/normalized
```

Datasets and feature artifacts are content-addressed: the same recording
always gives the same IDs and bytes, and nothing is ever overwritten. They are
generated, often from licensed data, and stay out of Git. See
[docs/normalized-market-datasets.md](docs/normalized-market-datasets.md),
[docs/feature-engine.md](docs/feature-engine.md), and
[docs/decisions/0012-normalized-market-dataset-and-features.md](docs/decisions/0012-normalized-market-dataset-and-features.md).

## Build the PostgreSQL persistence adapter

The persistence adapter is optional. It requires PostgreSQL's C client library
and a reachable PostgreSQL server. The supplied Docker Compose service uses
localhost port `55432` and retains data in a named volume.

```sh
brew install libpq
docker compose up -d --wait postgres
cmake -S . -B build-postgres \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON \
  -DPostgreSQL_ROOT=/opt/homebrew/opt/libpq
cmake --build build-postgres --parallel
CHRONOS_TEST_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ctest --test-dir build-postgres --output-on-failure --no-tests=error
```

See
[docs/postgresql-persistence.md](docs/postgresql-persistence.md) for the
transaction contract, recovery model, idempotency semantics, and reset
instructions.

## Build and run the paper-trading API

The HTTP API is optional and requires the PostgreSQL adapter. Its first
configuration downloads hash-pinned cpp-httplib and nlohmann/json releases.
Adding the Databento adapter lets the server play a local recording into its
market state, paced by the recording's receive time, while it serves requests.

```sh
docker compose up -d --wait postgres
cmake -S . -B build-api \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON \
  -DCHRONOS_ENABLE_PAPER_TRADING_API=ON \
  -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON \
  -DPostgreSQL_ROOT=/opt/homebrew/opt/libpq
cmake --build build-api --parallel
CHRONOS_TEST_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ctest --test-dir build-api --output-on-failure --no-tests=error

CHRONOS_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ./build-api/chronos_paper_trading_server \
    --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
    --instrument AAPL --instrument SPY \
    --replay-dbn data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
    --replay-speed 10x

# Playback waits in `ready`; start, pause, and resume it with empty commands
curl -s -X POST localhost:8080/api/v1/market-data/replay/start \
  -H 'Content-Type: application/json' -d '{}'
```

`--replay-speed` accepts `1x` (the default), `10x`, `100x`, or `max`, and
`--replay-autostart` starts playback without waiting for Start. The web
workspace offers the same controls. A replay cannot be rewound; restart the
server, and use a new account, to play it again. See
[docs/market-replay.md](docs/market-replay.md#paced-replay).

To trade against live IEX quotes instead, build with
`-DCHRONOS_ENABLE_ALPACA_ADAPTER=ON` and start the server with `--alpaca-iex`
in place of the replay options. Credentials are read only from
`APCA_API_KEY_ID` and `APCA_API_SECRET_KEY`, never from the command line:

```sh
APCA_API_KEY_ID=... APCA_API_SECRET_KEY=... \
CHRONOS_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ./build-api/chronos_paper_trading_server \
    --stream-id alpaca-iex-2026-09-29 --instrument AAPL --instrument SPY \
    --alpaca-iex
```

Chronos consumes Alpaca market data only; orders are still simulated and
accounted by Chronos. Use a new stream ID and new accounts for each live
session. See [docs/alpaca-adapter.md](docs/alpaca-adapter.md) for the
lifecycle, exact numeric mapping, freshness rules, and IEX limitations.

The server binds to `127.0.0.1:8080` by default. It has no authentication
and is not production-secure. See
[docs/paper-trading-api.md](docs/paper-trading-api.md) for the request and
response contract, history pagination, the portfolio's best-bid mark policy,
decimal and timestamp rules, error codes, market-state ownership, and
limitations.

A local database volume created before a migration existed needs it applied
once, in order; each file is idempotent. Migration 003 turns single-instrument
accounts into multi-instrument ones, and 004 adds working orders and fills:
stop the server and back up the volume first, as described in
[docs/postgresql-persistence.md](docs/postgresql-persistence.md#upgrading-an-existing-volume):

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/003_multi_instrument_accounts.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/004_working_orders.sql
```

## Run the web workspace

The browser client needs Node.js 22.22.2 (`apps/web/.nvmrc`) and a running
API server as above. It is simulated trading only: every screen says so, and
nothing can reach a broker or real money.

```sh
cd apps/web
npm ci
npm run dev        # http://127.0.0.1:5173, proxying /api and /health to :8080
npm run check      # type check, lint, unit and component tests, build
npx playwright install chromium
npm run test:e2e   # browser tests against the build, with a mocked API
```

The development proxy keeps the browser same-origin with the API, so the C++
server needs no CORS support. Market-dependent views refresh on the server's
change notices (Server-Sent Events), and everything falls back to polling
every 2 seconds when the stream is unavailable. See [docs/web-client.md](docs/web-client.md) for the
architecture, exact-decimal rules, uncertain-order reconciliation, and tests.

## Repository map

```text
adapters/   Provider-specific integrations isolated from the core
apps/web/   React and TypeScript paper-trading workspace
core/       C++ domain types and paper-trading core
datasets/   Normalized market datasets: format, writer, reader, catalog
features/   Point-in-time feature engine and feature artifacts
docs/       Contracts, architecture decisions, and accounting rules
services/   Application service and HTTP API over the core
tests/      C++ unit, edge-case, and integration tests
plan.md     Product requirements and delivery roadmap
```

## Continuous integration

GitHub Actions builds and tests every pull request and every push to `main`
with GCC 14 and Clang 18 on Ubuntu 24.04. A separate job runs the test suite
with AddressSanitizer and UndefinedBehaviorSanitizer enabled. The workflow
also tests the optional Databento and Alpaca adapters, and tests PostgreSQL
persistence and the paper-trading API against a real PostgreSQL service,
including a sanitizer build that combines the API with both adapters. The
Alpaca tests use fake transports and a loopback WebSocket server; CI needs no
credentials and never connects to Alpaca. It fails if
no tests are discovered and receives read-only repository access. Pushes to
`main` and manual runs also receive an AppleClang check on macOS 15. A web
job installs `apps/web` from its lockfile on a pinned Node.js and runs type
checking, lint, unit and component tests, the production build, and the
Playwright suite; it needs no server, database, credentials, or market data.

## Data and credentials

Raw or normalized licensed market data, credentials, model artifacts, build
outputs, and generated reports must not be committed. Provider credentials are
read from local environment variables (`DATABENTO_API_KEY`, `APCA_API_KEY_ID`,
`APCA_API_SECRET_KEY`) and never passed on the command line;
[.env.example](.env.example) lists the variable names only.

The normalized event contract is documented in
[docs/market-data-contract.md](docs/market-data-contract.md). The provider
decision and licensing boundaries are documented in
[docs/decisions/0001-market-data-provider.md](docs/decisions/0001-market-data-provider.md).
The first provider mapping is documented in
[docs/databento-adapter.md](docs/databento-adapter.md).
Recording integrity and manifest semantics are documented in
[docs/recording-manifest.md](docs/recording-manifest.md).
Real-data quoting and guarded downloads are documented in
[docs/historical-data-acquisition.md](docs/historical-data-acquisition.md).
Replay time, ordering, reset, and market-view semantics are documented in
[docs/market-replay.md](docs/market-replay.md).
Normalized datasets and their quality reports are documented in
[docs/normalized-market-datasets.md](docs/normalized-market-datasets.md), and
point-in-time features in [docs/feature-engine.md](docs/feature-engine.md).
Paper-order, risk, and simple top-of-book execution semantics are documented in
[docs/paper-execution.md](docs/paper-execution.md).
PostgreSQL transaction, journal, idempotency, and recovery semantics are
documented in
[docs/postgresql-persistence.md](docs/postgresql-persistence.md).
The paper-trading HTTP contract is documented in
[docs/paper-trading-api.md](docs/paper-trading-api.md), and its transport and
dependency decision in
[docs/decisions/0004-paper-trading-http-api.md](docs/decisions/0004-paper-trading-http-api.md).
The web client's architecture and tests are documented in
[docs/web-client.md](docs/web-client.md), and its foundation decision in
[docs/decisions/0007-web-client-foundation.md](docs/decisions/0007-web-client-foundation.md).
The journal read model and pagination decision is recorded in
[docs/decisions/0005-paper-journal-read-model.md](docs/decisions/0005-paper-journal-read-model.md).
The best-bid mark policy and portfolio-valuation rules are recorded in
[docs/decisions/0006-portfolio-valuation.md](docs/decisions/0006-portfolio-valuation.md).
The market-data source boundary, live freshness, and browser change
notifications are documented in
[docs/market-data-sources.md](docs/market-data-sources.md), and the pacing and
notification decision in
[docs/decisions/0008-paced-replay-and-change-stream.md](docs/decisions/0008-paced-replay-and-change-stream.md).
The Alpaca IEX live adapter is documented in
[docs/alpaca-adapter.md](docs/alpaca-adapter.md), and its decision in
[docs/decisions/0009-alpaca-iex-live-adapter.md](docs/decisions/0009-alpaca-iex-live-adapter.md).
Multi-instrument accounts — the scopes of cash, positions, liquidity, and
timelines, all-or-nothing valuation, and the normalized schema and its
migration — are recorded in
[docs/decisions/0010-multi-instrument-accounts.md](docs/decisions/0010-multi-instrument-accounts.md).
Durable good-til-cancelled working orders — their lifecycle, matching,
reservations, fees, cancellation, serialization with the market stream, and
migration 004 — are recorded in
[docs/decisions/0011-durable-gtc-working-orders.md](docs/decisions/0011-durable-gtc-working-orders.md).
The record/replay data-job direction is captured in
[docs/decisions/0003-record-replay-data-jobs.md](docs/decisions/0003-record-replay-data-jobs.md).
