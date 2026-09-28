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

## Connection loss and text parameters

`PaperPersistenceUnavailable` reports that PostgreSQL could not be reached or
that the connection was lost during an operation. It derives from
`std::runtime_error`; every other database failure remains a plain
`std::runtime_error` because it indicates a defect or corrupt state rather
than an outage. When a lost connection interrupts a commit, whether the
transaction committed is unknown. Resubmitting the same client order ID is
safe because the database accepts it at most once.

The next repository operation after a loss resets the connection once. The
repository keeps no session state on the connection, so reconnection is
transparent. Rolling back an abandoned transaction never throws; a lost
connection has no transaction left to roll back.

libpq text parameters end at the first NUL byte. The repository rejects any
text parameter containing one with `std::invalid_argument` before sending the
statement. A silently truncated identifier would otherwise make the journal
disagree with the in-memory idempotency state.

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

## Journal read model

`list_orders`, `find_order`, and `list_fills` read the journal for the
paper-trading history API. They map persisted rows only; risk, execution,
and accounting are never re-run to reconstruct a result.

- **Consistency.** Each read runs in one repeatable-read, read-only
  transaction that first checks the account, so a missing account is always
  distinguished from an empty history.
- **Bounded work.** Pages are newest first by `order_record_id` and fetch at
  most `limit + 1` rows; the extra row only signals an older page. There is no
  `OFFSET`. `paper_order_account_record_idx` serves order pages and lookups
  scanned by account, and the partial `paper_order_account_booked_fill_idx`
  serves fill pages.
- **Keyset order.** Within one account, `order_record_id` increases in commit
  order: the identity sequence allocates in call order (cache 1), and a
  submission inserts its record only after locking the account row with the
  optimistic version update. A page chain that continues from
  `order_record_id < last` therefore never repeats a record while new orders
  are committed. Allocating record identities outside that lock, or giving
  the sequence a cache, would break this property.
- **Booked fills.** Only `receipt` records book fills. A processing fault can
  journal the simulated fill that accounting rejected, so fill reads filter on
  `outcome_kind = 'receipt'` and never report it.
- **Notional.** The journal stores fill price and quantity, not notional. The
  read side derives notional with the same `calculate_notional` function the
  account used, so it is exact under the current accounting contract.
- **Fail closed.** A row whose fields could not have been written together
  raises `std::runtime_error`. Examples include an execution status that
  disagrees with its fill and quantities, an execution that names a different
  market observation, a rejection without a code, a declared enum value
  spelled as `unknown:<n>`, or a code outside the vocabulary. Malformed
  intents that the journal preserves on purpose round-trip exactly.

## Portfolio valuation

The portfolio endpoint adds no table, column, or migration. It loads the
account through the same recovery path, in one repeatable-read, read-only
snapshot, and values it against the current market in memory. The persisted
active top-of-book observation is what binds the valuation to the account's
market timeline, and the persisted latest fill timestamp bounds market time.
Nothing about a valuation is written; it is recomputed on every read.

## Migrations

Migrations are ordered files in `migrations/`. Each owns its transaction,
records its version in `chronos_schema_migrations`, and is idempotent, so
applying a file again is safe. An applied migration is never edited; schema
changes add a new file.

- `001_paper_trading.sql`: account snapshots and the order and event journal.
- `002_paper_order_read_indexes.sql`: keyset indexes for the history API.

PostgreSQL's image runs the mounted files only when it initializes an empty
volume. For an existing local volume, apply a new file once:

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
```

Sending the file on standard input avoids depending on the container's
`/docker-entrypoint-initdb.d` bind mount, which Docker Desktop on macOS can
present as empty when it lacks permission to read the repository's folder.
Integration tests apply every migration file in order before running.

`002` uses a plain `CREATE INDEX`, which blocks writes while it builds. That
is fine locally and in CI; a large production table would need
`CREATE INDEX CONCURRENTLY` outside a transaction, under a deliberate
migration policy that Chronos does not yet define.

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
- History reads reconstruct filled, partial, cancelled, rejected, and faulted
  orders exactly, page stably while orders arrive, list only booked fills,
  reject inconsistent rows, and survive connection loss.
- Processed IDs remain effective after recovery.
- Malformed rejected inputs remain persistable and auditable.
- Text containing NUL bytes is rejected without persisting anything.
- A terminated connection is reported as unavailable and then reconnected.
- A connection lost while a commit waits on a row lock records nothing and
  leaves the client order ID reusable.

The integration executable returns the standard CTest skip code when
`CHRONOS_TEST_POSTGRES_URL` is absent, so ordinary core builds do not require a
database.

## Current limitations

- One PostgreSQL connection per repository; no connection pool yet
- No statement or lock timeout; a blocked row lock blocks the caller
- No authentication or user-to-account authorization layer
- No TLS policy or production secret manager configuration
- No migration runner CLI, rollback migrations, or schema-version check at
  server startup
- Snapshot recovery is validated against database constraints and domain
  invariants but is not yet independently recomputed from the complete journal
- No retention, archival, backup, replication, or disaster-recovery policy
- The paper-trading API uses this component through request-scoped
  sessions and the journal read model; see
  [paper-trading-api.md](paper-trading-api.md)

These belong to the backend and production-hardening milestones. They do not
weaken the current guarantees for local, atomic, restart-safe paper trading.
