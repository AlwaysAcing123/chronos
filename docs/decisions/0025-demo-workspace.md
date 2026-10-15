# ADR 0025: An isolated, guided demo workspace

- Status: Proposed
- Date: 2026-10-13

## Context

Every part of Chronos exists, but trying it takes a dozen commands, a
licensed recording or test-only tools, and care not to touch a development
database. Three gaps stand between a newcomer and the platform:

- **No provider-independent replay.** Research writes normalized datasets,
  but the paper-trading server replays only a Databento DBN file (in a
  Databento build), Alpaca IEX, or nothing.
- **No production synthetic data.** The only synthetic producers are
  test tools (`tests/tools/synthetic_dataset.cpp`, `python/tests`), and the
  saved demonstration (`python -m chronos_research demo --synthetic-events`)
  runs the test tool.
- **No safe one-command stack.** The documented setup shares the
  development database on 55432, stops at migration 007, and leaves each
  service to be started, ordered, and stopped by hand.

Constraints: every engine, builder, accounting rule, cutoff, watermark,
ownership rule, risk check, and sandbox stays as it is. Nothing contacts a
market-data provider, reads a credential, or touches the development
databases, accounts, services, licensed recordings, or `data/real-pilot`.

## Decision

### A. Dataset replay: `--replay-dataset <DIR>`

- **One more replay source, in every API build.** The server already links
  `chronos_datasets` (through features and deployments). The opener lives
  in `chronos_market_data_sources`
  (`services/market_data/src/dataset_replay.cpp`), which gains a dependency
  on `chronos_datasets`. Neither depends on the other's dependents, so
  there is no cycle, and no provider adapter enters the core.
- **Exclusive sources.** `--replay-dbn`, `--replay-dataset`, and
  `--alpaca-iex` are mutually exclusive; each may appear once.
  `--replay-speed` and `--replay-autostart` apply to either replay and are
  refused without one. DBN and Alpaca behavior is unchanged.
- **Verified before exposure.** Before the HTTP server binds, the server
  runs `verify_dataset`, which reads every event to EOF and checks the
  count, checksum, summary, and quality report. `open_dataset` checks only
  metadata and sizes, so it is never enough. The playback reader is then
  opened separately and must name the same dataset ID, and it verifies the
  checksum again at EOF.
- **Stream and instruments must match.** `--stream-id` must equal the
  manifest's stream. The `--instrument` set must equal the manifest's
  instrument set exactly. Subsets are not supported, because every event of
  the dataset enters the market state; a subset would trade some
  instruments while others moved unseen. Nothing is relabelled or
  rewritten.
- **Honest labels from provenance.** The description comes from the
  manifest alone. A dataset whose provider is `synthetic` (the rule the
  research collection already uses) is described as synthetic: its label
  says so, and a coverage notice explains that its prices are invented. The
  market-data resource gains `"synthetic": true|false`. Any other dataset
  is a recorded replay labelled with its own provider, dataset, schema,
  normalizer, and query window. Providers stay descriptive data; no
  component branches on a provider name.
- **Ready, never trading on its own.** The source is the existing
  `PacedReplaySource`. Start, Pause, Resume, Completed, Failed, freshness,
  SSE, liquidity, and accounting are unchanged.
- **Trust boundary.** Verification followed by playback assumes the
  dataset directory is immutable and private to the operator (published
  datasets are read-only). A file changed between verification and EOF
  fails the replay at the first invalid event or at EOF. It cannot stop an
  owner who rewrites files concurrently from having had a valid-looking
  prefix played, and this is not claimed.

### B. A production seed over the real research machinery

- **Producer.** The event-table conversion moves from the test tool into a
  production helper in `chronos_datasets` (`event_table.hpp`). The test
  tool keeps its provenance byte for byte, so every test dataset keeps its
  identity. `chronos_dataset synthetic write` is the production producer:
  provider `synthetic`, normalizer `chronos.synthetic.event_table/1`, a
  description naming the scenario, and the real SHA-256 of the event table
  as its recording checksum.
- **Scenario `chronos.demo_scenario/1`.** A fixed, versioned generator
  (`chronos_research/demo_scenario.py`). It uses a SplitMix64 generator in
  integer arithmetic, so its bytes depend on no library or platform, and
  its invented prices start at AAPL 100.00 and SPY 500.00. Each instrument
  has a persistent buy/sell/neutral pressure regime. The regime drives the
  displayed sizes and the next tick, so book imbalance carries information
  about the next second. The data is designed that way to give the machinery
  coverage; it says nothing about any market.

  | Session | Stream | Receive window (UTC) | Use |
  | --- | --- | --- | --- |
  | h1 | `chronos-demo-v1-h1` | 2026-03-02 14:30–14:40 | history |
  | h2 | `chronos-demo-v1-h2` | 2026-03-03 14:30–14:40 | history |
  | h3 | `chronos-demo-v1-h3` | 2026-03-04 14:30–14:40 | history; experiment |
  | playback | `chronos-demo-v1-playback` | 2026-03-05 14:30–15:10 | interactive replay only |

- **Seed (`python -m chronos_research seed-demo`).** It uses only the
  existing builders:
  - `pipeline` for datasets, 1 s labels, the h3 split, and both baselines;
  - `registry` for AAPL logistic and majority versions;
  - `comparison` for those two on h3's test window;
  - `collection`, `walk_forward`, and `walk_forward_study` for h1–h3, with
    AAPL and SPY studies and one minimum training recording (two folds);
  - `strategy_diagnostics` for both studies.

  The walk-forward auditor and the strategy-diagnostics auditor then check
  each study and the diagnostics independently.
- **Fixed in advance, never tuned on outcomes.** These settings are fixed
  now, before any data exists:
  - **Configuration.** The research service's defaults: threshold
    500000000 ppb, target 1, latency 1 ms, decision interval 1 s, starting
    cash 10000, fee 0.01, quote age 5 s, maximum notional 1000000.
  - **Scenarios.** `base` (0.01, 1 ms) and `stress` (0.05, 200 ms).
  - **Horizon.** 1 s.

  Losses and unavailable outcomes are reported as the artifacts state them.
- **Chronology is asserted.** Each historical session ends before the next
  begins. Every historical artifact ends before playback begins. Each
  registered version's information cutoff precedes the playback window, so
  a deployment may decide on it.
- **Navigation manifest.** `chronos.demo_seed/1` is content-addressed and
  published with the existing `publish` only after everything verifies and
  both audits pass. It holds the scenario, the chronology, the artifact IDs,
  and safe hash routes. It holds no paths, metrics, or money. A second run
  finds every artifact, and the manifest, already present. A corrupt
  existing artifact is refused, never repaired.

### C. One command, one owned stack

`python3.12 tools/demo/chronos_demo.py start | status | new-session`, a
foreground supervisor using the standard library only:

- **Owned roots.** The state root (default `.chronos-demo/`, ignored by Git)
  holds the seed, the sessions, and the run record. The toolchain (by
  default `<root>/toolchain`) holds the C++ build (PostgreSQL and API, no
  provider adapter, no tests), a hash-pinned Python environment, and the
  web build with the guide enabled. Both are real, owner-only directories,
  and a symlink in their chains is refused. No file is written through a
  link:
  - records, captured output, and build stamps are written to a new file
    that is renamed over the name, so a link there is replaced, never
    followed or truncated;
  - logs are appended only to this user's own plain file with one name; a
    link, a hard link, or a special file is refused before anything runs;
  - reads never follow a link;
  - each build destination (the CMake build, the Python environment, the
    web output that a build empties, `node_modules`) must be a real
    directory before a tool writes into it.

  The directories' owner is trusted not to change them while a supervisor
  runs.
- **Isolated names.** The Compose project is `chronos-demo-<root hash>`
  with its own volume; it is never `chronos-postgres-data` or the
  development database. Each session has its own database,
  `chronos_demo_session_NNNN`. Default ports are web 18510, trading 18511,
  research 18512, and PostgreSQL 18513, all on 127.0.0.1. A new root can
  move them together with `--port-base`. An occupied port is refused, never
  reclaimed.
- **Migrations.** 001 to 008 are applied in order, each with
  `ON_ERROR_STOP`, through the container's `psql`, never through
  `/docker-entrypoint-initdb.d`. The rows and both deployment guards are
  then verified before trading starts. The files must match recorded
  SHA-256 digests, so an edited migration is refused before it is applied.
- **Lifecycle.**
  1. A first `start` creates session 1, fresh and ready to play.
  2. Before a trading process with a replay is started, the session durably
     records `playback_launched`. Every later start of that session opens it
     in history mode: no replay source, deployments recovered stopped by the
     server's own `recover()`, and valuation unavailable without quotes.
     Nothing replays from byte zero into existing accounts, watermarks are
     never cleared, and nothing resumes on its own.
  3. `new-session` creates the next database and makes it current. The old
     session is kept. It is refused while the stack runs.
- **Readiness is ownership.** A child counts as ready only when all of these
  hold:
  - its own output announces the address it bound;
  - the child is still alive;
  - the service answers with its identity header and expected content. The
    web server must return the exact `index.html` that the toolchain built.

  Every stage has a deadline.
- **Serialized.** An exclusive `flock` on the root is held for the
  supervisor's lifetime. It is taken before the root's record is created or
  changed, so of simultaneous first starts only the holder creates it. A
  second `start` only reads: it reports the running stack (exit 0 when
  ready) or the stage in progress (exit 3), and starts nothing. A root
  whose filesystem does not enforce the lock (a Docker Desktop bind mount,
  for one) is refused: the supervisor checks that a second open of the lock
  file is excluded.
- **Children.** Each child, and each build or seed step, runs in its own
  process group, with an allow-listed environment that carries no provider
  key. Stopping one stops its whole group: SIGTERM, then SIGKILL to
  whatever is left, the child or anything it started in the group. The
  leader's exit is observed without reaping it (`waitid` with `WNOWAIT` on
  Linux, a kqueue exit event on macOS), and it is reaped only once its group
  is gone. Until then the group's ID cannot name another group. A process
  that outlives SIGKILL is reported, recorded for `status`
  (`cleanup.json`), and makes the command exit 6 instead of 0; it is never
  passed over. On Ctrl-C or SIGTERM, the web server stops first. The trading
  server then stops, which stops deployments with `server_stopped`, and the
  research service follows. The demo PostgreSQL container stops last; its
  volume is kept. A child that exits early fails the stack in the same
  order, keeps data and logs, and exits 1.
- **Database rollback.** A start is responsible for the demo PostgreSQL
  from before `compose up`. Compose can create or start the container and
  still fail (a health wait, a deadline, an interrupt), so a failed start
  stops it, keeping its volume. `new-session` stops a database it started,
  and never one that was already running. A stop that fails is reported
  and recorded for `status` until a later stop of the database succeeds.
  The command then exits 6 instead of 0, and an earlier error keeps its
  own code.
- **Hard termination.** A SIGKILL of the supervisor cannot run cleanup, and
  its children may survive. The next `start` finds the run record or
  occupied ports and refuses, naming the recorded processes. It never kills
  a stored PID, and never uses `pkill` or `killall`. The person stops those
  processes. This boundary is documented and tested, not claimed away.

### D. The guide

The guide is a build-time flag, `VITE_CHRONOS_DEMO=1`, set only by the
supervisor's web build:

- **Placement.** It adds a **Guide** view (`#/demo`). Ordinary builds contain
  no guide and make no extra request.
- **Data.** It reads `GET /api/v1/research/demo`, which the research service
  serves only when started with `--demo-seed`. The route returns the
  verified navigation manifest and the session's number and mode.
- **No side effects.** It links to the real views and starts, creates, or
  simulates nothing. It reuses the existing panels, alerts, badges, facts,
  and buttons.

### E. Acceptance

The real-stack suite, `tools/demo/acceptance.py` with
`apps/web/e2e/demo-acceptance.spec.ts`, starts the actual supervisor on
private roots and ports. It drives all three flows through the browser with
no route mocks, then checks every lifecycle edge: stop, restart, a hard
kill, a new session, and refusals. A native Linux CI job runs it with
bounded timeouts and uploads its evidence on failure.

## Consequences

- **A safe way in.** A newcomer runs one command, gets an isolated stack of
  real services over clearly synthetic data, and cannot reach a provider,
  the development database, or real money.
- **The market-data contract grows.** The resource gains `synthetic`.
  Clients that ignore unknown fields are unaffected, and the web client
  requires it.
- **Bounded cleanup.** Cleanup after a hard kill is the person's to do, by
  design.
- **A duplicate list.** Migration digests are recorded twice, in the
  migrations and in the supervisor. A new migration must update both.
