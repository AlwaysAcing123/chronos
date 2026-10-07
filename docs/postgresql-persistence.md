# Chronos PostgreSQL paper-trading persistence

## Scope

This component makes the in-memory paper-order processor durable. It stores a
validated recovery snapshot, normalized into an account row and a row per
instrument, each executed order's current state and fills, and an
append-only order/event journal, one PostgreSQL transaction per change. A
new process can reconstruct the account, every position, risk limits,
processed client-order IDs, applied fill IDs, each instrument's active market
snapshot and consumed shadow liquidity, and every working order with its
fills, fee state, priority, and reservation, without relying on the previous
process.

The adapter is optional. The accounting, market-data, replay, risk, and
execution libraries still build without PostgreSQL or Docker.

## Transaction boundary

An account changes in one of three ways: a submission, a match of its
working orders against one observation, or a cancellation. Each is one
transaction that contains, in this order:

1. The account row, locked first and version-checked (cash, health, and
   stream binding).
2. Only the position and execution-state rows the change touched. Rows for
   other instruments are not rewritten.
3. For a match: each filled working order's `paper_orders` row moved from
   exactly its recovered state, its new `paper_fills` row, and its events.
4. For a submission: the complete original order intent and market
   observation, its structured outcome, and a snapshot of cash and the
   instrument's ledger afterward (`paper_order_records`); for an executed
   order, its `paper_orders` row and any fill.
5. For a cancellation: the order's terminal `paper_orders` row with its
   cancel request ID.
6. Ordered immutable audit events.

The service and the working-order matcher take the account row lock with
`SELECT … FOR UPDATE` and recover the account inside the same transaction,
so a change always starts from the committed state and two changes of one
account never interleave, across connections and processes. No transaction
locks more than one account, and the account row is always its first lock.
Repository connections of the server set `lock_timeout` (5 s for the
service, 1 s for the matcher); a wait longer than that is reported as
`PaperPersistenceUnavailable` and retried or returned as `503`.

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
the winner left, and cash cannot be spent twice. The server's own writers
lock the row before recovering the account, so they wait for one another
rather than conflict; the version check remains the defense against any
writer that does not.

A cancel request ID is unique per account (`paper_orders_cancel_request_key`):
it cancels at most one order. Matching is idempotent through the execution
state: an account's working orders are offered an observation only while it
is newer than the instrument's latest execution observation, and taking any
liquidity from it records it there, so a retried or repeated match of the
same observation books nothing. Fill IDs are unique per account
(`paper_fills_fill_id_key`), and fill indexes per order.

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
  consumed bid and ask shadow liquidity. It is also the "last processed
  observation" of each instrument for matching.
- `paper_orders`, one row per executed order (every order that reached the
  simulator): status, original, filled, and remaining quantities, filled
  notional, fill count, the fee its first fill pays, cancellation reason and
  cancel request ID, the stream and observation it was accepted on, and its
  submission and latest-change market times. `fee_charged`,
  `cumulative_fee_raw`, `reserved_cash_raw`, and `reserved_quantity_raw` are
  generated columns computed from the row itself (the buy reservation rounds
  `limit × remaining × 1e-9`, which is exact, half away from zero), so they
  can never disagree with it. Its primary key is the journal record ID,
  which is also the order's priority.
- `paper_fills`, append-only: every booked fill with its order, index from 0,
  price, quantity, fee, exact notional, time, and the observation it
  executed against.

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
- The same deferred triggers, now also on `paper_orders`, check that every
  account's working orders reserve no more cash than it holds and, per
  instrument, no more shares than its position.
- A deferred trigger on `paper_orders` and `paper_fills` checks that every
  order's fills add up to its filled quantity, filled notional, cumulative
  fee, and fill count, numbered without gaps.
- `paper_orders` checks tie status to quantities, cancellation reason, cancel
  request ID, and time in force: only a good-til-cancelled order works or is
  cancelled by a client, and a terminal order reserves nothing.
  `paper_fills` checks that only fill 0 pays a fee, that a fill ID names its
  order and index, and that each notional is exactly the rounded product; its
  composite foreign key makes a fill agree with its order's account, client
  order ID, instrument, and side.

Applied fill IDs come from `paper_fills`, processed client order IDs from
the journal, and working orders, in priority order, from the partial
`paper_orders_account_working_idx`. Recovery recomputes each working order's
reservation in C++ and refuses a row whose generated reservation differs.
Recovery for a read runs in one repeatable-read, read-only snapshot; for a
change, inside the transaction that holds the account lock, where no other
writer of the account can commit.
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
valid. Within one order, `event_order` numbers its events from 1 in the
order they happened, across every transaction that changed the order, and
`occurred_at_ns` is each event's market time. Events recorded before
migration 004 are dated with their submission's time, when all of them were
written. Depending on the outcome, an order records a subset of:

- `order_submitted`
- `risk_rejected`
- `execution_rejected`
- `order_completed` (an immediate-or-cancel order finished executing)
- `order_accepted` (a good-til-cancelled order was accepted)
- `fill_applied`, with the `fill_id` it booked
- `order_filled` (a good-til-cancelled order has nothing left)
- `order_cancelled`
- `account_state_updated`
- `processor_faulted`

The current snapshot is optimized for recovery; the journal is optimized for
audit and future event-driven consumers. The repository exposes no mutation or
deletion operation for journal records. Both are written in the same
transaction.

## Journal read model

`list_orders`, `find_order`, `list_fills`, `list_working_orders`,
`list_order_fills`, and `list_order_events` read the journal and the order
state for the paper-trading API. They map persisted rows only; risk,
execution, and accounting are never re-run to reconstruct a result. Every
order record carries its current state from `paper_orders` when it reached
execution, checked against the journaled submission: an immediate-or-cancel
order never changed after it, and a good-til-cancelled order's state can
follow from it.

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
- **Booked fills.** Fill history reads `paper_fills`, newest first by
  `fill_record_id` through `paper_fills_account_fill_idx` and, filtered,
  `paper_fills_account_instrument_fill_idx`. A processing fault can journal
  the simulated fill that accounting rejected; it never reaches
  `paper_fills`. A fill recorded before migration 004 has the fill record ID
  of its journal record, and later fills are numbered after every copied
  one, so a cursor issued before the migration pages the same fills after
  it.
- **Forward pages.** Working orders page in priority order after an order
  record ID through `paper_orders_account_working_idx`; one order's fills
  after a fill index through `paper_fills_fill_index_key`; one order's events
  after an event number through `paper_events_order_record_id_event_order_key`.
- **Notional.** The read side checks each fill's stored notional against
  `calculate_notional` of its price and quantity, the function the account
  used.
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
- `004_working_orders.sql`: good-til-cancelled working orders. Adds
  `paper_orders` and `paper_fills` with their constraints, triggers, and
  indexes, dates events and links fill events to their fills, widens
  `paper_events.event_order` to `integer`, allows the working execution
  statuses in the journal, and extends the reconcile trigger to
  reservations. See [ADR 0011](decisions/0011-durable-gtc-working-orders.md).
- `005_model_deployments.sql`: model deployments. Adds:
  - `model_deployments`, one per deployment, owning exactly one account
    (`account_id` is unique);
  - `model_deployment_decisions`, with named consistency checks, at most
    one pending decision per deployment, and one decision per client order
    ID;
  - a `BEFORE INSERT` trigger on `paper_order_records` that refuses any
    order journaled into an owned account unless it is the owner's model
    frozen market-IOC order for a pending decision;
  - a deferred constraint trigger that requires the decision to be
    `recorded`, with matching execution identifiers and times, at the
    order's commit. Guessing a pending ID through an ordinary writer rolls
    back the whole transaction, including its account and fill changes.

  It adds tables and journal triggers; existing accounts stay manual and
  untouched. See
  [ADR 0018](decisions/0018-model-paper-deployments.md).
- `006_live_stream_sequences.sql`: durable live-stream sequences. Adds
  `live_stream_sequences`, one row per live stream:
  - the first ingest sequence never reserved, as `numeric(20, 0)`, up to
    2^64 for an exhausted stream;
  - the one server process's writer lease, with expiry in the database's
    clock;
  - and, for a stream continued after older history, the largest sequence
    that history references.

  It adds one table and rewrites no data; no row is created until a live
  server first acquires a stream. Replays never use it. See
  [ADR 0020](decisions/0020-restart-safe-live-stream-identity.md).
- `007_live_stream_writer_fence.sql`: the live-stream writer fence. Adds to
  `live_stream_sequences`:
  - `writer_first_sequence`, the first sequence of the current writer's
    generation, set when a server takes the stream and cleared when it
    releases it;
  - a constraint that a writer has a generation, only a writer has one, and
    it never begins beyond what was reserved;
  - a trigger that makes every change of writer begin its generation where
    every earlier reservation ended.

  Each transaction that records a live sequence (an order, fill, execution
  state, model decision, or watermark) reads the stream's row `FOR SHARE`
  and is refused unless its sequences belong to the current generation; a
  valuation reads the same without a lock. A takeover, release, or
  reservation updates the row, so it waits for those transactions. A refused
  order answers 503 `market_data_unavailable` (`source_unavailable`) and
  consumes no client order ID.

  A stream that has a writer when 007 is applied gets a generation starting
  at its `next_sequence`, so that writer records nothing more until it takes
  the stream again. Apply 007 with every live server on the database
  stopped: a server built before it cannot take or release a stream
  afterwards. It rewrites no other data. See
  [ADR 0020](decisions/0020-restart-safe-live-stream-identity.md).

PostgreSQL's image runs the mounted files only when it initializes an empty
volume, so a fresh volume gets 001 to 007 in order. An existing local
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

Migration 004 is applied the same way, with the server stopped and a fresh
backup:

```sh
docker compose exec -T postgres pg_dump -U chronos -d chronos -Fc \
  > chronos-before-004.dump
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/004_working_orders.sql
```

What 004 guarantees:

- **All or nothing, and idempotent,** as for 003: one transaction, and
  nothing to do once version 4 is recorded. It locks `paper_accounts` first,
  which every writer and reader touches first, then `paper_events` and
  `paper_order_records` in the order migration 001 takes its index locks, so
  a concurrent rerun of 001 cannot deadlock with it. (003 locks the journal
  before the events and can deadlock with such a rerun; PostgreSQL then
  aborts one of the two, which can simply be run again.)
- **Validated before anything changes.** Every executed record must be a
  declared immediate-or-cancel order whose outcome, quantities, and fill have
  one of the three shapes the simulator produces, on the order's own
  observation, and every `fill_applied` event must belong to a booked fill.
  These are the rules the journal reader enforces, so the migration refuses
  exactly the rows the server would refuse to read, naming the rule and the
  count, and rolls back with the version 3 schema and data intact.
- **No data loss.** No existing column value changes. Every executed order
  gets one `paper_orders` row that repeats its journaled outcome and every
  booked fill one `paper_fills` row with its ID; the copy is recounted and
  re-reconciled before version 4 is recorded.

Migration 005 is applied the same way. Like the others, it is one
transaction and does nothing once version 5 is recorded. It needs version 4,
and refuses to run without it. It locks `paper_accounts` first, then the
migration table, so concurrent servers apply it once. It rewrites no
existing row: a test checksums every version 4 row before and after. A
failure part-way leaves no table, function, trigger, or version row behind.

A database that the repository's integration tests have run against before
this release contains accounts named `corrupt-*` whose journal rows those
tests deliberately corrupted to prove that reads fail closed. Migration 004
refuses them, as it should. The tests now repair what they corrupt, but
older rows remain; they belong only to test accounts and can be removed from
a test database after a backup, or the tests can use a fresh database.

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
- Working orders, their reservations, fee state, and priority survive a new
  process; several fills of one order add up and pay one fee; matching the
  same observation twice, from two connections, or after a lost connection
  books once; cancellation is durable and idempotent and releases the
  reservation; concurrent matchers and a canceller serialize; and every
  working-order query can use an index.
- Migration 004 copies a populated version 3 database exactly, reruns as a
  no-op, rolls back on each kind of corrupt version 3 row, cannot deadlock
  with a concurrent rerun of migration 001, and its constraints refuse
  inconsistent order and fill rows.
- Migration 005 has its own tests:
  - it changes no version 4 row;
  - it requires 004;
  - it leaves nothing behind on failure;
  - concurrent applications apply it once;
  - its constraints and the ownership trigger refuse what they should.

  `chronos_postgresql_model_deployment_tests` covers the deployment
  repository: creation (including concurrent identical creates), the
  lifecycle, the watermark across restarts, the atomic order-and-decision
  commit, a failpoint between the order's rows and the decision's
  resolution, a lost connection, abandonment, and the lock order between a
  submission and a new decision.

The integration executable returns the standard CTest skip code when
`CHRONOS_TEST_POSTGRES_URL` is absent, so ordinary core builds do not require a
database.

## Current limitations

- One PostgreSQL connection per repository; no connection pool yet
- No statement timeout; the server's connections have a lock timeout
- Recovery for every change reads all of the account's fill IDs and
  processed client order IDs, so its cost grows with the account's history
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
