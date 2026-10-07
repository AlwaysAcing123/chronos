# ADR 0020: Restart-safe live-stream identity

Status: proposed, revised 2026-10-06 after Codex's review; not yet accepted.

It changes how a live source numbers its events. It also adds a
database-side writer fence at every commit that records a live sequence. It
changes no replay, dataset, or recording identity, and no lineage,
freshness, watermark, risk, or accounting rule.

**The first version (2026-10-05) claimed "at most one process publishes on a
live stream at a time".** It defended that with a local publication
deadline checked before each event was applied. That claim was wrong:
- a local check followed by a local apply is not a distributed fence;
- a writer can pause between the two for any length of time;
- observers and order transactions were not fenced at all;
- reacquiring the lease revived permission for frames numbered under the
  old lease;
- idle reads never checked the lease.

This revision replaces the claim and its argument. See "Review findings"
for each finding and its correction.

## Context

Every market event carries its stream ID and an ingest sequence. Durable
state records that identity:

| Table | Columns |
|---|---|
| `paper_execution_states` | `market_ingest_sequence` (each instrument's latest execution quote) |
| `paper_order_records` | `market_ingest_sequence`, `execution_market_ingest_sequence` |
| `paper_fills` | `market_ingest_sequence` |
| `paper_orders` (working orders) | `accepted_market_ingest_sequence` |
| `model_deployments` | `watermark_ingest_sequence` |
| `model_deployment_decisions` | `observation_ingest_sequence`, `execution_ingest_sequence` |

The guards that read these assume that a stream's sequences only increase:
- `market_lineage_violation` refuses an older sequence, and a changed quote
  under an equal one, for orders and valuation alike;
- a working order matches only quotes after the one it was accepted on;
- a deployment's watermark breaks receive-time ties by sequence.

A replay satisfies that assumption by construction: its sequences come from
the recording, so sequence N always names the same event. The Alpaca live
source did not. Its counter started from the process's empty market state,
so every process began at zero.

The 2026-10-05 acceptance showed the result:
- An account's last execution quote was sequence 1087 on
  `alpaca-iex-validation`.
- After a restart, a newer quote on the same stream was sequence 70.
- Valuation answered 503 `incompatible_market_timeline`, correctly; orders
  were refused as `stale_market_approval`.

Waiting for the counter to catch up is not a fix: every identity on the way
is reused, with different content.

## Requirements

1. **No reuse, no regression.** Each acquisition reserves sequences after
   every block reserved by earlier acquisitions on that live stream.
   Sequences are issued in increasing order within each acquisition and
   never reused, across clean and crash restarts, from the stream's first
   durable reservation on. Requirement 3 separately fences late effects
   from an earlier generation.
2. **Contiguity within a run.** While one connection is live, sequences stay
   contiguous. An undeclared gap, duplicate, or regression still interrupts
   the market state.
3. **One authoritative writer.** At any instant at most one writer
   generation can affect durable or authoritative state through the stream:
   orders, fills, deployment decisions and watermarks, and valuations. No
   effect of an earlier generation can take hold after a later generation
   begins. That must hold however long a deposed writer is paused, however
   its clocks behave, and whatever it still believes.
4. **Fail closed.** Nothing is issued without a durably reserved sequence.
   Exhaustion, unavailable storage, and an ownership conflict stop
   publication with a short, credential-free diagnostic.
5. **Replays unchanged.** Recording, dataset, and replay identities stay
   deterministic. A replay never reads or seeds live allocation state.
6. **Guards unchanged.** No lineage, timestamp, freshness, watermark,
   latency, ownership, risk, or accounting check is weakened. No history,
   execution state, reservation, or watermark is cleared.

A first fresh quote after a restart must be usable by a recovered account
even if it is the only quote that arrives.

**Not claimed:** that only one process ever publishes into its own market
state. A deposed writer that has not yet noticed it was deposed can keep
numbering and applying events in its own process. Requirement 3 makes those
events powerless; nothing below depends on how quickly the deposed writer
notices.

## Options

### Allocation

1. **Seed from the largest durable reference at startup.** Insufficient. A
   process publishes far more than it records: quotes it showed, matched
   without a fill, or skipped as candidates. It is used exactly once per
   stream, as an explicit, recorded legacy transition (below).
2. **Durable allocation per event.** A database round trip on every quote,
   and the feed stalls whenever PostgreSQL does. Rejected.
3. **Epoch prefix.** A durable epoch in the high bits. It still needs the
   same ownership and fencing, and a fixed bit split caps events and
   restarts. Rejected; option 4 generalizes it.
4. **Durable block reservation.** **Chosen.** A writer reserves blocks of
   2^20 sequences under its lease, off the hot path, and numbers events from
   them in memory. A crash leaves at most the rest of one block unused.

### Ownership and fencing

| Option | Guarantee | Cost | Verdict |
|---|---|---|---|
| **Expiring lease, local deadline only** (the first version) | none: a pause between check and apply, or in an observer, lets a deposed writer's events and transactions take effect after a takeover | none | **Rejected** (Codex findings 1–3) |
| **Larger margin or another local check** | none: it moves the race, since a pause is unbounded | none | Rejected |
| **Database check before each event** | still a gap between that check and the in-process apply | a round trip per quote | Rejected |
| **Acknowledged-shutdown, single-host ownership.** No expiry takeover; a successor on the same host takes over only once a lock the kernel releases at process exit is free, and another host needs an operator's confirmation | true single-writer publication on a host, with no time assumption; immediate same-host crash recovery | gives up automatic takeover by another host; the product guarantee changes | **Not adopted.** It needs Aaron's decision, and can be layered on top of the next option later |
| **Fenced expiring lease.** The database checks, at each authoritative commit, that the sequences it records belong to the stream's current writer generation | requirement 3 for every durable and authoritative effect; automatic takeover after expiry is kept | one indexed lookup with a share lock per fenced transaction; takeovers wait for in-flight fenced transactions | **Chosen** |

Also considered and rejected:
- **A PostgreSQL advisory lock for ownership.** It is released when the
  owning session ends, even while that process is alive behind a broken
  connection; fencing would still be needed.
- **Wall-clock time as the sequence.** Two events can share a nanosecond,
  instruments interleave, and clocks step backward.

## Design

### Vocabulary

- **Reserve (durable).** A confirmed transaction moves
  `live_stream_sequences.next_sequence` from N to N + k for the current
  writer. An unanswered commit counts as not reserved; if it did commit, its
  block is a gap.
- **Generation.** Each acquisition starts a writer generation. The stream's
  row records its first sequence, `writer_first_sequence`. The generation
  owns every sequence from that first sequence up to `next_sequence`. Every
  earlier sequence belongs to an earlier generation.
- **Run.** One live connection's numbering, inside one generation, with a
  process-local run number. A run ends when:
  - the connection ends;
  - the lease is lost or lapses;
  - the lease is re-acquired;
  - the next block does not continue the current one.
- **Issue.** In memory, the source takes n contiguous sequences of the
  current run for one frame. The frame stays bound to that run.
- **Publish.** `MarketStateFeed::publish(event, permit, run)` applies the
  event, under the market-event gate, only if that run is still current.
  This is in-process hygiene, not a fence.
- **Fence.** The database check, at commit, that a change's live sequences
  belong to the stream's current generation.

### Storage

**Migration 006** adds `live_stream_sequences`, one row per live stream.

| Column | Meaning |
|---|---|
| `stream_id` | the logical stream |
| `next_sequence` | first sequence never reserved, 0 to 2^64; 2^64 means exhausted |
| `writer_id`, `writer_label`, `lease_expires_at` | the current writer and its lease; all null when released |
| `origin`, `adopted_maximum` | `new`, or `adopted` after legacy history (below) |
| `acquisitions`, `created_at`, `updated_at` | for operators |

**Migration 007** adds `writer_first_sequence`: the first sequence of the
current generation, null when there is no writer, and never above
`next_sequence`.
- A row that has a writer when 007 is applied gets `next_sequence`: that
  pre-007 writer's sequences can no longer pass the fence, which fails
  closed until it re-acquires.
- A pre-007 binary cannot acquire or release a stream after 007, because it
  does not maintain the column; it fails closed. Apply 007 with every live
  server on the database stopped.
- A trigger requires every change of writer to begin its generation at the
  old `next_sequence`. So no writer, of any version, can take over a stream
  and leave the deposed generation's sequences current.

Both migrations rewrite no other data and are idempotent.

### The writer lease

The writer ID is 128 random bits per process, plus a label of process ID
and host. Expiry uses the database's `clock_timestamp()` only. Defaults:
lease L = 15 s, renewal every 5 s, margin M = 3 s.

- **Acquire.** One transaction takes the row `FOR UPDATE`. It refuses while
  another writer's lease is unexpired. It re-checks durable history
  (below), reserves a block, starts a generation at the block's first
  sequence, and sets the lease.
- **Renew.** Only while the row is still this writer's. It extends the lease
  and, below half a block, reserves the next block.
- **Release.** Only `WHERE writer_id` is this process's, and only after
  shutdown has drained what can publish (below). It clears the writer and
  `writer_first_sequence`.
- **Local deadline.** Request start (steady clock) + L − M. Issuance,
  publication, and quote reads stop at it locally. It bounds how long a
  deposed writer that is running normally keeps serving quotes in its own
  process. It is not a safety bound: requirement 3 rests on the fence alone.

### The fence (requirement 3)

Every authoritative change that records a live sequence checks it in the
same transaction:
- **Account changes.** Manual orders, model orders, GTC fills, and every
  journal record, fill, and execution state in
  `write_account_change`. The sequences checked are the ones the change
  records anew: the quote a submission executes against, and the
  observation a match fills on. Rows written earlier keep the sequences they
  were recorded with, such as a working order accepted in an earlier
  generation, and are not re-checked.
- **Model decisions and watermarks.** In `record_decision`, the
  observation's sequence.

The check is a `SELECT … FOR SHARE` of the stream's row. It requires a
current writer and that every sequence the change records lies from
`writer_first_sequence` up to `next_sequence`.
- **Lock order.** It runs after the account row lock (or the deployment row
  lock), so the order is always account (or deployment), then stream. The
  keeper locks only the stream's row, so no cycle is possible.
- **Refusal.** The change rolls back.
  - Orders answer 503 `market_data_unavailable` (`source_unavailable`). The
    client order ID is not consumed.
  - Decisions are refused (`decision_refused`).
  - The GTC matcher fails its source, which suspends the market state.

**Why it holds.** A takeover, a release, and every reservation update the
stream's row, and an update must wait for each share lock to end. So a
fenced transaction that read the current generation commits before the
next generation begins. Any later fenced transaction sees the new row and
refuses an earlier generation's sequences. No clock or pause bound is
involved.
- **Sequences above it.** The deposed writer's sequences are all below the
  new generation's first sequence, because blocks are reserved in order.
- **The deposed writer's own sequences.** If the same process later
  re-acquires the stream, its old run's sequences stay below its own new
  generation, so they are refused too.

**Valuations** check the same condition as a read (no lock) for the
sequences of their marks. A valuation answers for a generation that was
current when it was read.

**Not fenced, because they record no live identity:** cancellations,
abandoning a decision, and lifecycle transitions. A recovering server stops
deployments the earlier process left running, as before.

**Databases without 007:** the repository skips the fence. There, no
current server can have acquired a live stream, because acquisition needs
the column. It looks for the column again until it appears.

### In-process runs and quote eligibility

These keep a deposed or lapsed writer from serving stale quotes in its own
process. They are not the safety mechanism.

- **Issued frames stay bound to their run.**
  - `issue(run, n)` and `publish(…, run)` refuse any run that is not
    current.
  - Losing, lapsing, or re-acquiring the lease ends the run.
  - A new run needs the source to suspend, declare its continuation, resume,
    and receive fresh quotes. An obsolete frame, or the rest of one, is
    never applied.
- **A lapse is sticky.** If a renewal finishes at or after the deadline that
  was in force, the run ends even though the lease was renewed. Cached
  quotes never become eligible again without a new run.
- **Quote reads check the run.** `MarketState` asks its quote authority on
  every execution, valuation, and display read. Without a current run, or
  past the local deadline, no quote is executable, without waiting for new
  traffic.
- **Idle reads check the run.** Before every read, after a frame, an empty
  frame, or a read timeout alike, the source checks that its run is still
  current, and ends the session if not.

### Contiguity and declared discontinuity

- **Declared starts.** A process's market state starts at zero. Before going
  live, each run declares its first sequence with
  `MarketState::continue_at(next)`.
- **The jump stays unusable.** `continue_at` is allowed only while the state
  is suspended, and only forward. `resume()` then requires a new top of book
  for every instrument, so no quote from before a jump is executable after
  it.
- **Gaps within a run.** An undeclared gap, duplicate, or regression still
  interrupts the market state.

### Shutdown

The server stops in this order:
1. HTTP, which finishes requests in flight;
2. deployments, with their runners joined;
3. the matcher;
4. the source, whose thread is joined; observers run on that thread, so
   they are drained;
5. the lease, which is released last.

Release leaves the stream with no writer, so any straggler would fail the
fence anyway.

### Scope

- **Instruments.** One namespace per stream, as before.
- **Streams.** Each stream ID has its own row, generation, and fence. An
  account bound to another stream is refused as before.
- **Replays.** They never construct the issuer and never touch the table.
  The fence finds no row for them.

### Failure handling

| Case | Behavior |
|---|---|
| Crash | The lease remains until expiry (≤ 15 s). The next process waits, names the holder, and opens no Alpaca connection. On takeover, a new generation starts after the dead writer's block |
| Pause or partition of a writer | Its lease may expire and a successor take over. Whatever it publishes in its own process afterwards cannot commit, be decided, or be valued (the fence). Locally its quotes go ineligible at its deadline |
| Clean shutdown | Drain, then release; a restart acquires at once |
| Storage unavailable | No lease means no connection. While live, issuance stops at the deadline and the run ends. A later renewal does not revive cached quotes |
| Blocked I/O | Bounded by statement, lock, idle-transaction, and connect timeouts. The deadline is local, and requirement 3 does not depend on it |
| Uncertain commit | Treated as not done; at worst a gap |
| Ownership conflict | Wait; never steal an unexpired lease |
| Ownership loss | The run ends, its blocks are discarded for good, and the writer acquires again with a new generation |
| Exhaustion | A block ending at 2^64 − 1 marks the stream exhausted at once. A frame that cannot fit fails the source with "use a new stream ID" |
| UINT64_MAX | All arithmetic is checked; sequences travel as text, never JSON numbers |

### Legacy streams

A stream with no row and durable references is refused unless the server
starts with `--adopt-live-stream-history`. That continues after the largest
reference, `max_ref`.
- **What adoption proves.** No durable guard can see reuse.
- **What it does not.** That identities a pre-006 process published but
  never recorded are not reused.

Every acquisition re-checks that `max_ref < next_sequence`.

### Readiness

HTTP, `/health`, accounts, and history work without a lease. No quote is
executable without a current run and a fresh quote. Deployments never
start on their own.

`/api/v1/market-data` reports the stream's `first_ingest_sequence`: the
first sequence this process accepted. Sequences only increase within a
process, so this is verifiable evidence that every quote since the start
follows a durable baseline (`validate.py verify-continuity`).

### Build boundaries

| Component | Location |
|---|---|
| Interfaces and the generic keeper (`LiveIngestSequences`, `PublicationPermit`, `LeasedIngestSequences`, the store interface) | `services/market_data`, no PostgreSQL |
| `MarketQuoteAuthority` | `core`, an interface only |
| Store and fence | `adapters/postgresql` |
| Alpaca adapter | the interfaces only, so an Alpaca-only build links no libpq |

### Costs

- **Hot path.** Quotes never touch the database.
- **Fenced transactions.** Each order, fill, and decision commit, and each
  valuation, costs one more primary-key lookup on `live_stream_sequences`.
  Commits also take a share lock, which shares with other fenced
  transactions.
- **Takeovers, releases, renewals, and reservations.** Each waits for
  fenced transactions in flight, which are short and bounded by their
  timeouts.
- **Databases without 007.** One catalog lookup per commit, until the column
  appears.
- **The trigger.** One comparison on each update of a stream's row: a
  renewal every 5 s, a reservation, an acquisition, or a release.

## Review findings (2026-10-06) and corrections

| Finding | Correction |
|---|---|
| P1: a frame numbered under an old lease was published after loss and reacquisition | Frames are bound to their run. Loss and reacquisition end the run, and a new run needs a session restart and fresh quotes. Even if such a frame were applied, the fence refuses its sequences |
| P1: an idle source kept an executable quote after its lease lapsed and a successor took over | Quote reads check the run and the deadline, the source checks them on idle reads, and a lapse is sticky. The successor's takeover makes the old quote unusable for orders, fills, decisions, and valuation (the fence) |
| P1: a check before apply is not a fence | Requirement 3 now rests on a database fence at every authoritative commit and valuation, held under a share lock that serializes with takeovers. The single-writer publication claim is withdrawn |
| P1: the continuity check passed on a later snapshot than the one it checked | One snapshot is checked and judged; the first fresh violation fails at once; disappearance is reported. The server's `first_ingest_sequence` gives verifiable first-since-start evidence. Without it, the result says it was sampled |

## Consequences

- **Live sequences are identities, not counts.** They jump by up to one
  block (2^20) per restart.
- **A second server on the same stream waits.** It opens no second Alpaca
  connection.
- **A deposed writer may still show quotes in its own process** until its
  local deadline. It cannot trade, fill, decide, or value on them.
- **A crash makes the next start wait up to 15 s** for the lease, plus the
  source's reconnect backoff.
- **Clearing a lease by hand needs care.** Clear `writer_id`,
  `writer_label`, `lease_expires_at`, and `writer_first_sequence` together.
  Thanks to the fence it is safe even if the old process is alive.
- **Existing live databases need one explicit adoption per stream**, or new
  stream IDs.
- **The durable-history check** scans six tables at each acquisition,
  bounded by the 5 s statement timeout.

## Implementation notes (2026-10-05)

- **Real Alpaca IEX, market closed.** The run used a copy of the acceptance
  database, upgraded to 006.
  - **Adoption.** It adopted history up to 8453 (decisions and a watermark),
    not 1087 (fills).
  - **Restarts.** The clean restart released the lease and re-acquired it
    at once. After a SIGKILL, the next start waited for the dead lease and
    opened no connection.
  - **Not proven.** No quote arrived, so valuation on a first fresh real
    quote is unproven.

## Implementation notes (2026-10-06)

- **Reproduced first.** Codex's four reproductions became permanent tests,
  and each failed on the uncorrected tree before any fix:
  - `tests/leased_ingest_sequences_tests.cpp`: an obsolete frame after
    takeover and reacquisition;
  - `tests/alpaca_live_source_tests.cpp`: a frame held at the real
    market-event gate, an idle cached quote after a lapse, and a late
    renewal;
  - `tests/live_stream_restart_tests.cpp`: a deposed writer's quote traded,
    valued, decided on, and filled a GTC order; a lapsed writer's cached
    quote kept serving;
  - `tools/deployment_validation/test_validate.py`: continuity judged on a
    later snapshot than the one checked.
- **Exact boundaries.** Publication is allowed at deadline − 1 ns and
  refused at the deadline. A renewal confirmed 1 ns before the deadline
  keeps its run; one confirmed at it ends the run.
- **Serialization.** A takeover's row lock waits for a fenced statement held
  open in a transaction, and the deposed generation is refused once the
  takeover commits (`tests/postgresql_live_stream_sequences_tests.cpp`).
- **Live market.** Not exercised for this revision: the corrections were
  finished after the close, and a closed market proves nothing.

## Market-hours verification (2026-10-07)

The revised code passed a fresh-account IEX restart session on a new
persistent isolated database with migrations 001–007. After a clean
restart, first server sequence 2097152 followed durable history 1048623;
after SIGKILL, first server sequence 3145728 followed durable history
2097246. Fresh observed quotes valued the recovered one-share AAPL position,
and a sell filled after each restart. Four booked paper fills independently
reconciled to flat inventory, cash/equity 9999.82, and PnL −0.18 with zero fees.

A verified majority-model control predicted Hold twice, never ordered,
stayed inactive after explicit Stop, and recovered from the crash as
`stopped/server_restarted`. A duplicate POST was refused and its original
order remained retrievable without changes. The development-data fingerprint
was unchanged; the owned source stopped and released its lease. A saved
database backup was successfully restored to another new isolated database.

The earlier evidence databases were RAM-only Docker tmpfs and were lost
during Docker recovery. Thus this session does **not** rerun the exact
legacy-adopted October 5 account. Its saved logs remain; a new persistent
account proves fresh-account restart continuity, not legacy migration on
fresh quotes. Real network disconnect and pending-live-order crash coverage
remain deterministic, not real-live proof. See `docs/model-deployments.md`
for the first suspect-quote rejection, successful retry, and evidence path.

Code review and this bounded live verification do not change the ADR's
proposed status; Aaron's milestone acceptance is still required.
