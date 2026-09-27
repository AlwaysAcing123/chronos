# Chronos

Chronos is a real-market research, model-evaluation, and paper-trading
platform. The project is being built as a sequence of small, tested vertical
slices, beginning with exact financial arithmetic, portfolio accounting, and
normalized market-data contracts.

Chronos simulates trading only. It does not place real-money orders.

## Current implementation

- Fixed-point price, quantity, and money types
- Provider-independent market-event types
- Fill and trading-domain types
- Single-instrument, long-only paper-account accounting
- Exact cash, position, cost-basis, fee, and PnL calculations
- Unit and edge-case tests for arithmetic and accounting

The project does not yet ingest market data, replay markets, accept orders, or
provide an API or web application. See [plan.md](plan.md) for the complete
roadmap.

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

## Repository map

```text
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
outputs, and generated reports must not be committed. Provider credentials
will be read from local environment variables when data ingestion is added.

The normalized event contract is documented in
[docs/market-data-contract.md](docs/market-data-contract.md). The provider
decision and licensing boundaries are documented in
[docs/decisions/0001-market-data-provider.md](docs/decisions/0001-market-data-provider.md).
