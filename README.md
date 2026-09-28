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
- Unit and edge-case tests for arithmetic, accounting, market data, replay,
  risk, and execution

The core now accepts, simulates, and books paper orders in memory through one
controlled path. It does not yet persist order and account state or provide an
API or web application. See [plan.md](plan.md) for the complete roadmap.

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

## Repository map

```text
adapters/   Provider-specific integrations isolated from the core
core/       C++ domain types and paper-trading core
docs/       Contracts, architecture decisions, and accounting rules
tests/      C++ unit and edge-case tests
plan.md     Product requirements and delivery roadmap
```

## Continuous integration

GitHub Actions builds and tests every pull request and every push to `main`
with GCC 14 and Clang 18 on Ubuntu 24.04. A separate job runs the test suite
with AddressSanitizer and UndefinedBehaviorSanitizer enabled. The workflow
fails if no tests are discovered and receives read-only repository access.
Pushes to `main` and manual runs also receive an AppleClang check on macOS 15.

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
The record/replay data-job direction is captured in
[docs/decisions/0003-record-replay-data-jobs.md](docs/decisions/0003-record-replay-data-jobs.md).
