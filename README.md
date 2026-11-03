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
- Future-midpoint-return label artifacts: exact fixed-point returns to the
  first observation at least one horizon later, typed reasons for every
  unavailable label, and a label availability time separate from the
  prediction time
- Chronological, purged split artifacts whose no-leakage inequalities are
  checked when written and when read
- A Python research package that evaluates a no-skill majority baseline and a
  logistic-regression baseline per instrument on verified artifacts, into an
  immutable evaluation artifact with plain-number model parameters
- A deterministic C++ strategy runner that trades a versioned long/flat
  policy over a verified dataset through the same execution and accounting
  code, beside cash and buy-and-hold benchmarks, and a local research service
  with a Research view that starts evaluations and shows their results
- A model library of immutable versions, constrained ONNX imports in a
  sandbox, and fair comparisons of several versions under one frozen spec
- Controlled model deployments to dedicated paper accounts, each started
  explicitly, with durable decisions and model-attributed orders
- Walk-forward studies over independent recordings with cost and latency
  scenarios, and strategy diagnostics that explain each run's PnL exactly,
  each checked by an independent standard-library auditor
- The paper-trading server replays a verified normalized dataset directly
  (`--replay-dataset`), labelling a synthetic one as invented prices
- A one-command, isolated demo workspace with a guide through three flows on
  real services over a versioned synthetic scenario
  ([docs/demo-workspace.md](docs/demo-workspace.md))
- Unit and edge-case tests for arithmetic, accounting, market data, replay,
  datasets, features, labels, splits, risk, and execution, plus Python tests,
  real-PostgreSQL service and API tests, and a real-stack browser suite

The core accepts, simulates, books, persists, and recovers paper orders
through controlled paths, the optional API exposes that path to local
clients, and the web workspace in `apps/web/` is the first of them. A
replayed market moves while the server runs, and an optional live IEX feed
can replace it. Recordings become normalized datasets, feature, label, and
split artifacts, baseline models, strategy evaluations, comparisons,
walk-forward studies, diagnostics, and paper deployments. Every result is a
paper simulation on minutes of data: none is evidence of skill or
profitability. Authentication, richer models and policies, and historical
charts are not built yet. See [plan.md](plan.md) for the complete roadmap.

## Try the demo workspace

After the prerequisites in
[docs/demo-workspace.md](docs/demo-workspace.md#prerequisites) (Python 3.12,
CMake and a C++20 compiler, libpq, Node.js 22, and Docker with Compose), one
command builds, seeds, and starts an isolated local stack, then prints the
guide's address:

```sh
python3.12 tools/demo/chronos_demo.py start
```

Every price in it is invented. Ctrl-C stops it, and nothing is deleted.
`status` reports what is there, and `new-session` creates a fresh trading
database. It uses its own ports (18510 to 18513), database, and Compose
volume, never the development database, and needs no credential.

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
Several recordings for a walk-forward evaluation are planned, quoted, and
acquired only under a recorded human authorization with per-recording caps
and a durable attempt ledger that never retries a possibly charged download;
see [docs/real-market-validation.md](docs/real-market-validation.md).

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

## Labels, splits, and baseline evaluation

Feature artifacts are labelled with future midpoint returns, and labels are
split chronologically with purging at each boundary:

```sh
./build/chronos_dataset labels extract --features data/features \
  --feature-artifact <FEATURE-ID> --output data/labels --horizon-ns 1000000000
./build/chronos_dataset labels inspect --output data/labels --artifact <LABEL-ID> \
  --features data/features
./build/chronos_dataset splits create --labels data/labels --label-artifact <LABEL-ID> \
  --output data/splits --train-percent 60 --validation-percent 20 --test-percent 20
./build/chronos_dataset splits inspect --output data/splits --artifact <SPLIT-ID> \
  --labels data/labels
```

The Python research package in `python/` trains and evaluates the baselines
after `chronos_dataset` has verified and reproduced every source artifact:

```sh
cd python
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python --require-hashes -r requirements.lock
.venv/bin/python -m chronos_research evaluate --chronos-dataset ../build/chronos_dataset \
  --features ../data/features --labels ../data/labels --splits ../data/splits \
  --split-artifact <SPLIT-ID> --output ../data/experiments
.venv/bin/python -m chronos_research inspect --output ../data/experiments --experiment <ID>
.venv/bin/python -m ruff format --check . && .venv/bin/python -m ruff check . && .venv/bin/python -m mypy
CHRONOS_DATASET=../build/chronos_dataset \
CHRONOS_SYNTHETIC_DATASET=../build/chronos_synthetic_dataset \
  .venv/bin/python -m pytest
```

Label, split, and experiment directories are generated and ignored by Git. See
[docs/labels-and-splits.md](docs/labels-and-splits.md),
[docs/baseline-evaluation.md](docs/baseline-evaluation.md), and ADRs
[0013](docs/decisions/0013-future-midpoint-return-labels.md),
[0014](docs/decisions/0014-chronological-purged-splits.md), and
[0015](docs/decisions/0015-python-research-boundary-and-baseline-artifacts.md).

## Evaluate a strategy and use Research

One command takes the local recording through every research artifact,
verifies it, and runs a long/flat policy over each fitted model's
validation and test windows beside cash and buy-and-hold benchmarks:

```sh
cd python
.venv/bin/python -m chronos_research demo --tools ../build --artifacts ../data \
  --recording ../data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340
.venv/bin/python -m chronos_research serve --tools ../build --artifacts ../data --state ../data/research
```

With the research service running, the web workspace's **Research** view
starts evaluations and shows their trades, equity, drawdown, costs,
benchmarks, and provenance. Every result is a paper simulation on minutes
of data, not evidence of profitability. See
[docs/research-walkthrough.md](docs/research-walkthrough.md),
[docs/strategy-evaluation.md](docs/strategy-evaluation.md),
[docs/research-service.md](docs/research-service.md), and ADRs
[0016](docs/decisions/0016-inference-schedules-and-strategy-runs.md) and
[0017](docs/decisions/0017-local-research-service.md).

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

Every API build, with or without the Databento adapter, can replay a
normalized dataset instead, with `--replay-dataset <catalog>/<dataset-id>`
in place of `--replay-dbn`. The server verifies every event of it before it
listens. Its stream must be `--stream-id`, and its instruments exactly the
`--instrument` list. A synthetic dataset is labelled as invented prices,
not market data, and a recording keeps its provider's labels
([docs/market-data-sources.md](docs/market-data-sources.md#normalized-dataset-replay)).

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
accounted by Chronos.

A live stream's ingest sequences continue across restarts, so the same
stream ID and accounts carry on after one
([ADR 0020](docs/decisions/0020-restart-safe-live-stream-identity.md)):
- **One writer per stream.** One server at a time holds a live stream's
  lease in `live_stream_sequences` (migration 006). A second server on the
  same stream and database waits, and opens no Alpaca connection.
- **A deposed writer changes nothing.** Every order, fill, model decision,
  and valuation checks in the database that its quote belongs to the
  stream's current writer (migration 007). A server that was paused or cut
  off while another took over may still show quotes for a few seconds, but
  cannot trade, fill, decide, or value on them.
- **Crash restarts wait.** After a crash, the next start waits up to 15 s
  for the old lease to expire.
- **Older history needs one adoption.** A stream whose accounts or
  deployments predate migration 006 continues only after one start with
  `--adopt-live-stream-history`.

See [docs/alpaca-adapter.md](docs/alpaca-adapter.md) for the lifecycle,
exact numeric mapping, freshness rules, and IEX limitations.

The server binds to `127.0.0.1:8080` by default. It has no authentication
and is not production-secure. See
[docs/paper-trading-api.md](docs/paper-trading-api.md) for the request and
response contract, history pagination, the portfolio's best-bid mark policy,
decimal and timestamp rules, error codes, market-state ownership, and
limitations.

A local database volume created before a migration existed needs it applied
once, in order; each file is idempotent. Migration 003 turns single-instrument
accounts into multi-instrument ones, 004 adds working orders and fills,
005 adds model deployments and the guards on the accounts they own, 006
adds durable live-stream sequences, 007 adds the fence that refuses a
deposed live writer, and 008 adds deployments of registered model versions.
Stop every server on the database and back up the volume first, as
described in
[docs/postgresql-persistence.md](docs/postgresql-persistence.md#upgrading-an-existing-volume):

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/003_multi_instrument_accounts.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/004_working_orders.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/005_model_deployments.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/006_live_stream_sequences.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/007_live_stream_writer_fence.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/008_registered_model_deployments.sql
```

Migration 006 adds one table and changes no existing row. Migration 007
adds one column, a constraint, and a trigger to that table. A stream that
has a writer when 007 is applied can record nothing more until a server
takes it again, which a restart does. A server built before 007 cannot take
or release a stream on a database that has it.

Before you trust a database with deployments, check what is actually
installed, not just its version row. An older, unmerged copy of 005 could
record version 5 without the deferred `paper_order_records_deployment_resolution`
trigger and its `paper_deployment_order_is_resolved` function. Rerunning the
released file then does nothing, because it is idempotent. This check is
read-only:

```sh
CHRONOS_VALIDATION_DATABASE_URL=postgresql://chronos:<password>@127.0.0.1:55432/chronos \
  python/.venv/bin/python tools/deployment_validation/validate.py preflight
```

It lists the migrations (1 to 8 are required), both deployment triggers,
and their functions. It also lists each live stream's reservations, writer
lease, and the first sequence of its writer's generation, and any stream
with durable history but no live-stream record. Such a stream is a replay, or a live stream that needs
`--adopt-live-stream-history` once. It then reports whether the database
is safe. If it is not, use a fresh, correctly migrated database for
deployments: the released migrations are never edited, and nothing repairs
user data automatically.

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
datasets/   Normalized market datasets and the shared derived-artifact layer
features/   Point-in-time feature engine and feature artifacts
labels/     Future-return labels and label artifacts
splits/     Chronological, purged splits and split artifacts
python/     Research package: models, evaluations, studies, and the research service
strategy/   The C++ strategy runner's policy and engine
tools/      Command-line tools, independent auditors, and the demo workspace
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
A Python job builds the C++ verifier and synthetic-dataset tool, installs the
hash-pinned research dependencies, and runs formatting, lint, strict type
checking, and the research tests on synthetic data only.

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
[docs/normalized-market-datasets.md](docs/normalized-market-datasets.md),
point-in-time features in [docs/feature-engine.md](docs/feature-engine.md),
labels and splits in [docs/labels-and-splits.md](docs/labels-and-splits.md),
and baseline evaluation in [docs/baseline-evaluation.md](docs/baseline-evaluation.md).
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
