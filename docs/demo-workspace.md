# The demo workspace

One command starts an isolated, local Chronos with a guide through three
flows: manual paper trading, evaluating and paper-deploying a model, and
explaining a strategy result. Everything runs on your machine against real
services: PostgreSQL, the C++ paper-trading server, the Python research
service and its inference worker, and the web client. The data is
synthetic, and every screen says so. The decisions are in
[ADR 0025](decisions/0025-demo-workspace.md).

**What it is not.** Every price is invented by the scenario
`chronos.demo_scenario` version 1. AAPL and SPY appear as names only: their
prices here, starting at 100.00 and 500.00, are neither security's real
price, and nothing was observed on an exchange. The scenario is built so
that book imbalance says something about the next second, which gives the
machinery something to find. Nothing a model or strategy does on it says
anything about a market. Trading is simulated: no order reaches a broker or
exchange, and no real money is involved.

## Prerequisites

| Need | macOS | Ubuntu |
| --- | --- | --- |
| Python 3.12, run as `python3.12` | python.org, or `uv python install 3.12` | `apt-get install python3.12 python3.12-venv` |
| CMake 3.24 or newer and a C++20 compiler | `xcode-select --install`, `brew install cmake` | `apt-get install cmake clang` |
| PostgreSQL's client library, libpq | `brew install libpq` | `apt-get install libpq-dev` |
| Node.js 22.22.2 or a later 22.x, with npm | `nvm install 22.22.2` | `nvm install 22.22.2` |
| Docker with Compose v2, running | Docker Desktop | Docker Engine and the Compose plugin |

About 2 GB of free disk is enough. The first start downloads pinned
dependencies only:

- CMake's hash-pinned cpp-httplib and nlohmann/json, from GitHub;
- the hash-pinned Python wheels in `python/requirements.lock`, from PyPI;
- the npm packages in `apps/web/package-lock.json`, from the npm registry;
- the digest-pinned `postgres:18.6-trixie` image.

It contacts no market-data provider and needs no credential. Nothing is
installed outside the workspace, and no global setting is changed.

## Start it

```sh
python3.12 tools/demo/chronos_demo.py start
```

It runs in the foreground and prints each stage:

1. **Checking prerequisites.** Each missing one is named with how to
   install it, and the start exits 2 having changed nothing.
2. **Building the C++ server and tools.** A Release build with PostgreSQL
   and the API, no provider adapter, and no tests, into the toolchain. The
   first build takes a few minutes; later starts rebuild only what changed.
   `CC` and `CXX`, if set, choose the compiler for that first build.
3. **Preparing the Python environment.** A venv with
   `python/requirements.lock` installed by hash.
4. **Building the web client with the guide.** A private copy of
   `apps/web` with its own `npm ci`, built with `VITE_CHRONOS_DEMO=1`. The
   repository's `apps/web/node_modules` is never touched.
5. **Seeding and verifying the synthetic research artifacts.** The first
   start runs `python -m chronos_research seed-demo`, which takes about two
   minutes:
   - three historical sessions and a 40-minute playback session;
   - their datasets, features, labels, a split, and both baselines;
   - two registered AAPL model versions and their comparison;
   - a collection, AAPL and SPY walk-forward studies, and strategy
     diagnostics, each checked by its independent auditor;
   - the `chronos.demo_seed/1` navigation manifest.

   Later starts run `check-demo-seed`, which verifies that manifest and
   every event of the playback dataset in a few seconds.
6. **Starting the demo PostgreSQL** and creating or checking the session's
   database (migrations below).
7. **Starting the research service**, with the guide's data.
8. **Starting the paper-trading server**, replaying the playback dataset at
   1x. It verifies every event first, then waits in Ready.
9. **Starting the web client.**

It then prints the guide's address, `http://127.0.0.1:18510/#/demo`. Open
it, and follow the guide. Nothing starts on its own: the replay waits for
**Start replay**, and a deployment for **Start**.

A service is taken as started only when its own output names the address
it bound and it answers there as itself. The web server, for instance,
must serve the exact `index.html` this build made. Every stage has a
deadline. If a stage fails, the start stops what it started, keeps every
log, and exits 1.

### Ports and names

| What | Default |
| --- | --- |
| Web client and guide | `127.0.0.1:18510` |
| Trading API | `127.0.0.1:18511` |
| Research service | `127.0.0.1:18512` |
| Demo PostgreSQL | `127.0.0.1:18513` |
| State root | `.chronos-demo/` in the repository, ignored by Git |
| Compose project and volume | `chronos-demo-<root hash>` and `chronos-demo-<root hash>_postgres-data` |
| Session databases | `chronos_demo_session_0001`, `…0002`, … |

Every address is on 127.0.0.1. An occupied port is refused with exit 5 and
is never taken over. Use another root for other ports:
`--root <dir> --port-base <port>`. The ports are web, then trading,
research, and PostgreSQL, one apart. The workspace never uses the
development `chronos-postgres-data` volume or the development database on
55432.

### The root

```text
.chronos-demo/
  workspace.json     ports, the Compose project, the verified seed's ID
  run.json           the supervisor's state, stage, URLs, and children
  cleanup.json       what a stop could not end, until it is resolved
  supervisor.lock    held while a supervisor runs
  secrets/           the demo database's random password (owner only)
  artifacts/         the seed and every research artifact you make
  research/          the research service's job records
  sessions/NNNN/     session.json and each launch's logs
  logs/              build, seed, and database logs
  toolchain/         build/, python/, web/ (or --toolchain <dir>)
```

The root and everything in it are owner-only real directories. A link is
never followed or written through: records and captured output replace a
link with a new file, and a log that is a link, a hard link, or not a plain
file is refused before anything runs. Each build destination must be a
real directory.

## Stop, reopen, and start a new session

- **Ctrl-C** stops the stack in order. The web client goes first. The
  trading server is next, and it stops any running deployment with reason
  `server_stopped`. The research service follows, then the demo PostgreSQL
  container. Every database, log, and artifact is kept, and the start
  exits 0, or 6 if something could not be stopped (see Troubleshooting).
- **A second start** while one runs reports the running stack and exits 0.
  During another start's startup it reports the stage and exits 3.
  Neither starts anything.
- **The first start of a session plays its replay.** Before the trading
  server is launched with the replay, the session records
  `playback_launched_at` durably. Every later start of that session opens it
  in **history mode**: there is no replay source, so there are no quotes.
  Orders cannot fill, valuation is unavailable rather than zero, and the
  guide says so. Deployments that were running recover stopped, by the
  server's own recovery: `server_restarted` after a crash, or
  `server_stopped` after a clean stop. Pending decisions are abandoned.
  Nothing replays from the beginning into accounts that already traded,
  watermarks are never cleared, and nothing resumes on its own.
- **`python3.12 tools/demo/chronos_demo.py new-session`** creates the next
  session's own database and makes it current. Earlier sessions are kept.
  It is refused while the stack runs (exit 3); stop it first.
- **`python3.12 tools/demo/chronos_demo.py status`** reports the
  supervisor, the last run's state, the current session and its mode, and,
  while running, each service's answer. It never prints a password or
  connection string.

Nothing deletes a session, a volume, or the root. To start over, stop the
stack and run `start` with a new `--root`, or remove a root yourself, along
with its Compose volume:
`docker volume rm chronos-demo-<root hash>_postgres-data`.

### After a hard kill

A Ctrl-C or SIGTERM lets the supervisor stop everything it started. A
SIGKILL, a power cut, or a crash does not, and its children may still be
running. The next start finds the run record of a supervisor that never
finished. If any recorded child is still running, it lists them, kills
nothing, and exits 4. Stop them yourself, for example with
`kill <pid>` after `ps -p <pid> -o command`, then start again. If a
child holds a port, the start is also refused for that port.

## Migrations

A new session's database gets migrations 001 to 008 applied in order, each
with `ON_ERROR_STOP`, through the container's `psql`. The supervisor then
verifies that exactly versions 1 to 8 are recorded and that both deployment
guards and their functions are installed
(`paper_order_records_deployment_ownership` and
`paper_order_records_deployment_resolution`). Every later start verifies
the same again.

- **No init script.** Nothing relies on Compose's
  `/docker-entrypoint-initdb.d`, which does not run on an existing volume.
- **Released files only.** Each migration file must have the SHA-256 the
  supervisor records. An edited migration is refused before anything is
  applied, because released migrations are never edited.
- **No reuse.** An existing database is never reused for a new session.

## The three flows

The guide (`#/demo`) explains each step and links to the real views. It
creates, submits, and starts nothing itself.

1. **Manual paper trading.**
   - Start the replay, create an account, and buy 1 AAPL at market.
   - Read its order, fill, cash, position, and the 0.01 fee.
   - Rest a good-til-cancelled limit buy below the market and see the cash
     it reserves, then cancel it and see the reservation released.
   - The guide explains displayed liquidity, and why a paper order removes
     no real shares from any book.
2. **Evaluate, then paper-deploy a model.**
   - Open the registered logistic version and its comparison with the
     majority baseline on h3's test window. You can also start your own
     evaluation as a real job.
   - Deploy the version to a new account that only it owns, and press
     **Start**. Watch its saved predictions, decisions, and model-source
     orders.
   - Press **Stop**. Stop never sells.
   - The guide explains why an evaluation (the first recorded quote at or
     after each scheduled order) and a deployment (the quote current when
     its order is accepted) do not have identical outcomes, timing, or
     profit.
3. **Explain a strategy result.**
   - Open the AAPL walk-forward study, its diagnostics, and the first fold's
     logistic base run.
   - Read the exact PnL attribution: spread, fees, and the final mark at the
     best bid. Each verification level is reported apart.
   - Return to trading: the selected manual account is unchanged.

The guide also explains synthetic, recorded, and live data, market time
versus wall time, and configured versus observed latency.

## Troubleshooting

| Message | What to do |
| --- | --- |
| `the Docker daemon is not reachable` | Start Docker Desktop or the Docker service, then `start` again. |
| `Port 18510 (web) is already in use` | Stop what holds it, or use another `--root` and `--port-base`. |
| `A previous demo supervisor was terminated…` | Stop the listed processes yourself; see above. |
| `… failed (exit N); see <log>` | The named log under the root or toolchain has the tool's own output. |
| `verifying the seed failed` | An artifact under `artifacts/` no longer verifies. Nothing repairs it. Move the root aside and start a new one. |
| `migrations/00N_….sql differs from the released migration` | Restore the released file; migrations are never edited. |
| `migration 00N_….sql failed on chronos_demo_session_NNNN: PostgreSQL refused: …` | PostgreSQL's own error follows. The current session is unchanged, and the half-made database is kept, never reused. Fix the cause, then run `new-session` again; it takes the next number. |
| The guide says history | The session has played once already: run `new-session`, then `start`. |
| `… is on a filesystem that does not enforce file locks …` | The root is on a network share or a container's bind mount, where two supervisors could share it. Use a root on a local disk, with `--root`. |
| `… is not this user's own plain file …` or `… is a link; it was not followed` | Something other than the workspace's own file is at that name. Nothing was written to it. Move it aside, then start again. |
| Exit 6, or `status` shows `not cleaned up (…): …` | The command did its work, but a stop could not end something: a process that outlived SIGKILL, or the demo PostgreSQL. Check it yourself (`ps -p <pid>`, `docker ps`). `status` keeps showing a process while it runs, and the database until a later stop of it succeeds. |

## Real-stack acceptance

```sh
(cd apps/web && npm ci && npx playwright install --only-shell chromium)
python3.12 tools/demo/acceptance.py --evidence <dir> [--repeat 3]
```

The first line installs the web client's pinned Playwright and its headless
Chromium (about 200 MB, into Playwright's own cache), which only the
acceptance suite needs. It starts the real supervisor on its own root and ports (18710 by default)
and drives the three flows in Chromium, desktop and phone, with no mocked
response. It then checks:

- exact accounting, model ownership, and that Stop keeps the position;
- a real evaluation job;
- the independent auditors;
- a repeat and a concurrent start, simultaneous first starts on a fresh
  root, and a graceful stop;
- a hard-killed trading server reopened as history;
- a new session, an occupied port, a failed child, a database that cannot
  start or is interrupted while starting, and a corrupt dataset;
- that nothing outside the workspace changed.

It writes `acceptance.json`, logs, and screenshots into the evidence
directory. CI runs it natively on Linux (`demo-workspace`).

## Limitations

- **Synthetic only.** The guide and its checks use synthetic data. The
  server can replay a real normalized recording with `--replay-dataset`,
  with its own provider labels, but the workspace does not offer one. The
  seeded models were fitted on synthetic sessions, so no real-data
  deployment is offered.
- **Fixed playback.** It runs at 1x, once per session. A completed replay
  stays at its final quotes.
- **Not hardened.** There is no authentication. Everything listens on
  localhost and is not production-secure.
- **Cleanup after a hard kill** is yours to do, by design.
- **Process groups.** Stopping a child ends its process group. A process
  that leaves the group on purpose (with `setsid`, as a daemon does) is no
  longer the child's, and is not stopped.
- **Trusted files.** Verification and playback read the dataset twice and
  assume the root's files are not rewritten in between. Published
  artifacts are read-only, but an owner rewriting them concurrently is out
  of scope.
- **Platforms.** It runs on macOS and Linux only. Atomic publication needs
  `renamex_np` or `renameat2`. The root must be on a local disk: the
  supervisor refuses one whose filesystem does not enforce `flock`, as
  Docker Desktop's bind mounts do not.
- **IDs and environments.** Seed IDs are deterministic for a given pinned
  environment. An experiment records its library and interpreter versions,
  so a different Python patch release gives different, equally verified,
  IDs.
