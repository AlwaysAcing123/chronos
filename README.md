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
- Single-instrument, long-only paper-account accounting
- Exact cash, position, cost-basis, fee, and PnL calculations
- Shared manual/model paper-order intents with explicit source attribution
- Structured pre-trade validation, quote-freshness checks, and risk limits
- Deterministic top-of-book IOC market and limit execution
- Partial fills, cancellation reasons, idempotency, and per-account shadow
  liquidity
- End-to-end order processing that immediately books simulated fills into the
  owned paper account
- Exact reconciliation from order and fill through cash, position, fees, and
  PnL
- Optional PostgreSQL persistence with atomic account snapshots and an
  append-only order, rejection, fill, and portfolio event journal
- Validated restart recovery, database idempotency, and optimistic concurrency
- Reconnection after a lost database connection, reported as a typed
  unavailability error
- A thread-safe, server-owned market state whose clock timestamps orders, so
  clients can never choose the quote or time they trade at
- A transport-independent paper-trading application service shared by manual
  and model orders
- An optional local HTTP API for health, account creation and retrieval,
  immediate-or-cancel order submission, history, and portfolio valuation, with
  exact decimal strings and stable machine-readable errors
- Durable order and fill history read from the journal, with stable
  newest-first keyset pagination and order lookup for reconciling uncertain
  submissions
- Read-only portfolio valuation at the server's current best bid, with exact
  market value, unrealized and total PnL, equity, and a fixed-point return
  percentage that reconcile before they are returned
- A local React and TypeScript paper-trading workspace: account creation,
  market and portfolio views, a manual order ticket that reconciles uncertain
  submissions, and durable order and fill history, refreshed by polling
- Unit and edge-case tests for arithmetic, accounting, market data, replay,
  risk, and execution, plus real-PostgreSQL service and API tests

The core accepts, simulates, books, persists, and recovers paper orders
through controlled paths, the optional API exposes that path to local
clients, and the web workspace in `apps/web/` is the first of them. Live
streaming, authentication, model deployment, and historical charts are not
built yet. See [plan.md](plan.md) for the complete roadmap.

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
Adding the Databento adapter lets the server replay a local recording into its
market state before it starts listening.

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
    --replay-dbn data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst
```

The server binds to `127.0.0.1:8080` by default. It has no authentication
and is not production-secure. See
[docs/paper-trading-api.md](docs/paper-trading-api.md) for the request and
response contract, history pagination, the portfolio's best-bid mark policy,
decimal and timestamp rules, error codes, market-state ownership, and
limitations.

A local database volume created before
`migrations/002_paper_order_read_indexes.sql` existed needs that file applied
once; it is idempotent:

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
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
server needs no CORS support. Reads are polled every 2 seconds; there is no
streaming yet. See [docs/web-client.md](docs/web-client.md) for the
architecture, exact-decimal rules, uncertain-order reconciliation, and tests.

## Repository map

```text
adapters/   Provider-specific integrations isolated from the core
apps/web/   React and TypeScript paper-trading workspace
core/       C++ domain types and paper-trading core
docs/       Contracts, architecture decisions, and accounting rules
services/   Application service and HTTP API over the core
tests/      C++ unit, edge-case, and integration tests
plan.md     Product requirements and delivery roadmap
```

## Continuous integration

GitHub Actions builds and tests every pull request and every push to `main`
with GCC 14 and Clang 18 on Ubuntu 24.04. A separate job runs the test suite
with AddressSanitizer and UndefinedBehaviorSanitizer enabled. The workflow
also tests the optional Databento adapter, and tests PostgreSQL persistence
and the paper-trading API against a real PostgreSQL service, including a
sanitizer build that combines the API with the Databento adapter. It fails if
no tests are discovered and receives read-only repository access. Pushes to
`main` and manual runs also receive an AppleClang check on macOS 15. A web
job installs `apps/web` from its lockfile on a pinned Node.js and runs type
checking, lint, unit and component tests, the production build, and the
Playwright suite; it needs no server, database, credentials, or market data.

## Data and credentials

Raw or normalized licensed market data, credentials, model artifacts, build
outputs, and generated reports must not be committed. Provider credentials are
read from local environment variables and never passed on the command line.

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
The record/replay data-job direction is captured in
[docs/decisions/0003-record-replay-data-jobs.md](docs/decisions/0003-record-replay-data-jobs.md).
