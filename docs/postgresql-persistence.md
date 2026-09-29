# Chronos PostgreSQL paper-trading persistence

## Scope

This component makes the in-memory paper-order processor durable. It stores a
validated recovery snapshot, normalized into an account row and a row per
instrument, and an append-only order/event journal in one PostgreSQL
transaction. A new process can reconstruct the account, every position, risk
limits, processed client-order IDs, applied fill IDs, each instrument's
active market snapshot, and consumed shadow liquidity without relying on the
previous process.

The adapter is optional. The accounting, market-data, replay, risk, and
execution libraries still build without PostgreSQL or Docker.

## Transaction boundary

For one submitted paper order, a successful database transaction contains:

1. An optimistic account-version update of the account row (cash, health,
   and stream binding).
2. Only the position and execution-state rows the order changed: at most the
   order instrument's ledger and execution state. Rows for other instruments
   are not rewritten.
3. The complete original order intent and market observation.
4. Its structured risk, execution, or processing outcome.
5. Any simulated fill, the cash, and the order instrument's ledger afterward.
6. Ordered immutable audit events.

The session passes the state it loaded at the expected version and the state
after the order; the repository writes the difference. Because the version
matched, the stored rows equal the loaded state, so the difference is exact.
A ledger or execution state is created or changed, never removed.

PostgreSQL commits all of these changes or none of them. A uniqueness failure,
version conflict, connection error, integrity failure at commit, or other
database failure rolls back the transaction, including position and
execution rows already written in it. `PersistentPaperOrderSession` also restores its in-memory state
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
receives a structured version conflict without overwriting the winner. Cash
is shared by every instrument, so orders for different instruments on one
account serialize on the same version: a retry re-runs risk against the cash
the winner left, and cash cannot be spent twice.

## Recovery snapshot

The authoritative snapshot is three tables:

- `paper_accounts`, one row per account: starting and current cash, all
  configured pre-trade risk limits, processor health, the version, and
  `market_stream_id`, the stream the account executes on (null before its
  first execution).
- `paper_positions`, one row per `(account_id, instrument)` the account has
  filled: quantity, open cost basis, gross realized PnL, fees paid, and the
  latest fill timestamp. A closed position keeps its row with zero quantity
  and zero cost basis.
- `paper_execution_states`, one row per `(account_id, instrument)` the
  account has executed against: the active top-of-book observation with its
  full lineage (`market_*` columns, the same shape as the journal's) and the
  consumed bid and ask shadow liquidity.

The database enforces the invariants that span them:

- A position references its instrument's execution state, because every fill
  executed against one.
- Every execution state references `(account_id, market_stream_id)` on the
  account, so an account's instruments cannot use different streams.
- Deferred constraint triggers on accounts, positions, and execution states
  check, at commit, that
  `cash + Σ open cost basis = starting cash + Σ gross realized PnL − Σ fees`
  and that an account is bound to a stream exactly when it has an execution
  state. They run after every row an order writes, so a partial write never
  commits.
- Per-row checks keep quantities, cost, and fees non-negative, zero quantity
  exactly when cost basis is zero, and consumed liquidity within the
  displayed book.

Applied fill IDs and processed client order IDs are reconstructed from the
append-only order journal. All recovery queries use one repeatable-read,
read-only database snapshot, so a concurrent commit cannot mix versions.
Recovery passes through the same C++ validation boundaries used by direct
state restoration. Negative balances, inconsistent cost basis, totals out of
range, invalid market observations, duplicate IDs, mismatched account
identity, execution states on different streams, a position without an
execution state, and consumed liquidity beyond the visible book fail closed
instead of creating a partially valid processor.

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
optional fill, and a snapshot of state after processing: the account's cash
(`account_cash_after_raw`) and one instrument's ledger
(`position_after_instrument` and the `position_*_after_*` columns). For
records written since migration 003 the instrument is the order's; for older
records it is the account's single instrument, which differs from the
order's only for the retired `instrument_mismatch` rejection. An instrument
the account never filled has a zero ledger and a null fill time. These
columns are audit data: recovery and every read use the snapshot tables. Malformed input is retained
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
  serves fill pages. With an instrument filter,
  `paper_order_account_instrument_record_idx` and the partial
  `paper_order_account_instrument_booked_fill_idx` on
  `(account_id, instrument, order_record_id)` serve the same pages. Without
  them a filtered page reads every journal row of the account: on a synthetic
  journal of 249,000 rows whose largest account holds 50,000 AAPL orders and
  40 SPY orders, a 40-row SPY page read 1,983 buffers (1,884 for fills) and
  discarded 49,960 rows; with them, 5 buffers each, scanning the index
  backward with no filter step.
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

The portfolio endpoint adds no table or column. It loads the account through
the same recovery path, in one repeatable-read, read-only snapshot, and
values it against the current market in memory. The persisted stream binding
and each instrument's active top-of-book observation are what bind the
valuation to the account's market timeline, and the persisted latest fill
timestamps bound market time.
Nothing about a valuation is written; it is recomputed on every read.

## Migrations

Migrations are ordered files in `migrations/`. Each owns its transaction,
records its version in `chronos_schema_migrations`, and is idempotent, so
applying a file again is safe. An applied migration is never edited; schema
changes add a new file.

- `001_paper_trading.sql`: account snapshots and the order and event journal.
- `002_paper_order_read_indexes.sql`: keyset indexes for the history API.
- `003_multi_instrument_accounts.sql`: multi-instrument accounts. Moves each
  account's position into `paper_positions` and its execution state into
  `paper_execution_states`, adds the stream binding and commit-time
  integrity triggers, records the instrument of the journal's "after"
  snapshot, adds the filtered-history indexes, and drops the moved columns
  so no value has two writable homes.

PostgreSQL's image runs the mounted files only when it initializes an empty
volume, so a fresh volume gets 001, 002, and 003 in order. An existing local
volume needs each newer file applied once, in order.

Sending a file on standard input avoids depending on the container's
`/docker-entrypoint-initdb.d` bind mount, which Docker Desktop on macOS can
present as empty when it lacks permission to read the repository's folder.
Integration tests apply every migration file in order before running, so
running them against a database upgrades it.

`002` and `003` use plain `CREATE INDEX`, which blocks writes while it builds.
That is fine locally and in CI; a large production table would need
`CREATE INDEX CONCURRENTLY` outside a transaction, under a deliberate
migration policy that Chronos does not yet define.

### Upgrading an existing volume

Migration 003 rewrites the account tables, so stop the paper-trading server
and take a backup first. The backup is a single file outside the volume:

```sh
docker compose exec -T postgres pg_dump -U chronos -d chronos -Fc \
  > chronos-before-003.dump
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/003_multi_instrument_accounts.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -c 'SELECT version, description FROM chronos_schema_migrations ORDER BY version'
```

What 003 guarantees:

- **All or nothing.** The file is one transaction. It takes account, order,
  and event table locks in the same order as an order writer, so concurrent
  appliers wait rather than deadlock, and a second applier finds version 3
  recorded and does nothing.
- **Validated before anything changes.** An account without a fill must hold
  no position state, and an account with fills must have an active market; a
  violation raises an exception naming how many accounts are affected, and
  the transaction rolls back with the old schema and data intact. After the
  copy, every account's identity is re-checked with the new tables.
- **No data loss.** Account IDs, cash, risk limits, health, and versions are
  unchanged. Each account with a fill gets exactly one position row, and each
  account with an active market one execution-state row, with the same raw
  values. Journal rows and events are not changed apart from the recorded
  instrument and the renamed "after" columns. The integration tests migrate
  old-format data and compare every value; this repository's development
  database was checked the same way with fingerprints of every account,
  position, execution state, order, and event before and after.
- **Idempotent.** Once version 3 is recorded the file does nothing.

To restore the backup instead, recreate the volume and load the dump with
`pg_restore -U chronos -d chronos --clean --if-exists`. There are no down
migrations.

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
