# Chronos PostgreSQL paper-trading persistence

## Scope

This component makes the in-memory paper-order processor durable. It stores a
validated recovery snapshot and an append-only order/event journal in one
PostgreSQL transaction. A new process can reconstruct the account, risk
limits, processed client-order IDs, applied fill IDs, active market snapshot,
and consumed shadow liquidity without relying on the previous process.

The adapter is optional. The accounting, market-data, replay, risk, and
execution libraries still build without PostgreSQL or Docker.

## Transaction boundary

For one submitted paper order, a successful database transaction contains:

1. An optimistic account-version update.
2. The complete original order intent and market observation.
3. Its structured risk, execution, or processing outcome.
4. Any simulated fill and the account state afterward.
5. Ordered immutable audit events.
6. The latest validated recovery snapshot.

PostgreSQL commits all of these changes or none of them. A uniqueness failure,
version conflict, connection error, or other database failure rolls back the
transaction. `PersistentPaperOrderSession` also restores its in-memory state
to the pre-submission snapshot when persistence does not commit.

## Idempotency and concurrency

Every non-empty `(account_id, client_order_id)` pair is unique in the database.
The durable path consumes a supplied client order ID for every recorded
submission, including risk rejections. A corrected retry must use a new client
order ID. A missing ID can be recorded repeatedly as a malformed rejection but
provides no idempotency guarantee. This is stricter than the low-level
in-memory risk engine, which does not reserve IDs for rejections.

Every account row has a monotonically increasing version. A session may
commit only when its expected version matches the database. If two sessions
load version 4, the first successful commit creates version 5 and the second
receives a structured version conflict without overwriting the winner.

## Recovery snapshot

The authoritative snapshot stores:

- Starting and current cash
- Position and open cost basis
- Realized PnL, paid fees, and latest fill timestamp
- All configured pre-trade risk limits
- Processor health state
- Active top-of-book observation and its full lineage
- Consumed bid and ask shadow liquidity
- Account version

Applied fill IDs and processed client order IDs are reconstructed from the
append-only order journal. All recovery queries use one repeatable-read,
read-only database snapshot, so a concurrent commit cannot mix versions.
Recovery passes through the same C++ validation
boundaries used by direct state restoration. Negative balances, inconsistent
cost basis, invalid market observations, duplicate IDs, mismatched account or
instrument identity, and consumed liquidity beyond the visible book fail
closed instead of creating a partially valid processor.

## Audit journal

`paper_order_records` preserves the routed account, requested account,
instrument, exact order fields, exact market observation, structured outcome,
optional fill, and account state after processing. Malformed input is retained
faithfully so a rejected order with an unknown enum value, zero quantity, or
negative timestamp remains auditable.

`paper_events` adds a database-assigned sequence for deterministic retrieval.
Sequence allocation is not a promise of transaction commit order and gaps are
valid. Depending on the outcome, one submission records a subset of:

- `order_submitted`
- `risk_rejected`
- `execution_rejected`
- `order_completed`
- `fill_applied`
- `account_state_updated`
- `processor_faulted`

The current snapshot is optimized for recovery; the journal is optimized for
audit and future event-driven consumers. The repository exposes no mutation or
deletion operation for journal records. Both are written in the same
transaction.

## Local development

Chronos pins the official PostgreSQL 18.6 Debian image by digest and binds it
only to localhost on port `55432`, avoiding the conventional local port
`5432`.

Install the C client library once on macOS:

```sh
brew install libpq
```

Start PostgreSQL and wait for its health check:

```sh
docker compose up -d --wait postgres
```

Build the optional adapter:

```sh
cmake -S . -B build-postgres \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON \
  -DPostgreSQL_ROOT=/opt/homebrew/opt/libpq
cmake --build build-postgres --parallel
```

Run the complete suite against the local database:

```sh
CHRONOS_TEST_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ctest --test-dir build-postgres --output-on-failure --no-tests=error
```

Stop the container while retaining its named volume:

```sh
docker compose down
```

`docker compose down --volumes` permanently deletes the local Chronos
database and should be used only for an intentional development reset.

## CI

The PostgreSQL persistence job starts a real PostgreSQL service, installs
`libpq`, applies the migration, and runs the complete test suite. Integration
tests prove:

- Filled orders and shadow liquidity survive a new process/session.
- Duplicate client order IDs roll back the snapshot version and all events.
- Risk rejections are durable and idempotency-protected.
- Stale concurrent sessions cannot overwrite committed state.
- Processed IDs remain effective after recovery.
- Malformed rejected inputs remain persistable and auditable.

The integration executable returns the standard CTest skip code when
`CHRONOS_TEST_POSTGRES_URL` is absent, so ordinary core builds do not require a
database.

## Current limitations

- One PostgreSQL connection per repository; no connection pool yet
- No authentication or user-to-account authorization layer
- No TLS policy or production secret manager configuration
- No migration runner CLI or rollback migrations
- Snapshot recovery is validated against database constraints and domain
  invariants but is not yet independently recomputed from the complete journal
- No retention, archival, backup, replication, or disaster-recovery policy
- No API endpoint exposes this component yet

These belong to the backend and production-hardening milestones. They do not
weaken the current guarantees for local, atomic, restart-safe paper trading.
