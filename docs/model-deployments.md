# Model deployments

A model deployment trades one verified research model in one dedicated
paper account, on the paper-trading server's own market data: a paced
replay of a local recording or the live Alpaca IEX stream. It is simulated
trading only. No order ever reaches a broker or exchange. The design and
its reasoning are in
[ADR 0018](decisions/0018-model-paper-deployments.md).

The chain is the same as the historical strategy runner's, except that it
runs live:

```text
market event -> point-in-time features -> model inputs -> prediction
  -> long_flat_threshold/1 -> durable decision -> trusted order path
  -> journaled paper order and fill -> positions and PnL
```

## What a deployment is

- **One model.** Either a verified `logistic` or `majority` model from one
  research experiment, identified by the SHA-256 of its parameters as the
  inference worker re-derived them; or one registered model version from
  the model library, identified by its immutable version ID and payload
  checksum ([below](#registered-model-versions)). Either serves one
  instrument.
- **One account.** A new paper account, created with the deployment. The
  account belongs to the deployment for good. Manual orders and cancellations
  on it are refused with `409 account_owned_by_deployment`, whatever source
  the request claims. A database trigger refuses any order journaled into it
  except the deployment's own frozen order for a decision still pending.
  A deferred constraint also requires that decision's matching execution
  to be resolved as `recorded` in the same transaction. An ordinary writer
  cannot commit an order by guessing a pending decision's ID.
- **One policy.** `long_flat_threshold` version 1, the same code as the
  historical runner (`strategy/src/long_flat_policy.cpp`). An up prediction
  at or above the threshold targets the target position. A confident down
  prediction targets flat. Anything else holds. It never goes short, and never
  holds more than the target, which is at most one share. The server also
  enforces this: the account's order and position limits equal the target.
- **One stream.** The server's stream, fixed at creation.

## Lifecycle

| State      | Meaning                                                            |
|------------|--------------------------------------------------------------------|
| `stopped`  | Not trading. A new deployment starts here, and so does every deployment after a restart. |
| `running`  | Its runner is deciding, and may submit orders.                     |
| `stopping` | Stop was requested; its runner is ending.                          |
| `faulted`  | It stopped itself: a limit, the model, or the account failed.      |

- **Start** is explicit. It loads the model, re-verifies its identity
  against the one recorded at creation, and begins a new run. A run has its
  own order and rejection counts. A deployment never starts on its own: at
  startup the server records every `running` or `stopping` deployment as
  `stopped (server_restarted)` and abandons every pending decision.
- **Stop** ends the run and keeps the position. Nothing is liquidated.
- **The kill switch** stops every deployment at once with reason
  `kill_switch`. It is idempotent.

Starting twice, stopping twice, and creating twice from the identical
request are all idempotent.

At most eight deployments can run in one server process. A ninth Start is
refused with `409 invalid_deployment_state`, reason
`deployment_capacity_reached`; Stop frees a slot. The ID `kill-switch` is
reserved for the control route and cannot identify a deployment.

## When it decides, and when it orders

A runner decides on the newest top-of-book row of its instrument. Rows
arriving while it is busy replace each other in a one-slot hand-off and are
counted as `superseded`. A row is decided on only when all of these hold:

- it was received at or after the model's training cutoff, the
  experiment's validation start;
- it is strictly after the last decided observation, the durable watermark,
  which survives restarts and replays;
- at least the decision interval has passed since the last decision;
- its five model inputs are available;
- it is no older than the maximum signal age.

Every other row is counted by reason.

A decision is written durably before anything acts on it, with its
inputs, prediction, position, policy reason, and the order it intends. An
order decision is then `pending` until the trusted order path records its
one order, or abandons it.

The trusted path re-checks everything under the market-event gate and the
account, deployment, and decision locks. The deployment must still be
running in the same run, and its account healthy. The instrument must be
trading, and the current execution quote must be:

- available and not suspect;
- of the deployment's stream;
- later in ingest order than the signal's own observation;
- received at least the deployment's latency after it.

The order is a market immediate-or-cancel order on that current quote. A
signal older than its maximum age is abandoned (`signal_expired`), never
filled late.

This is a current-market contract: the order trades whatever the market is
when it is accepted. The historical runner's contract is "the first quote at
or after the arrival time". They differ on purpose, and live results are
not bit-reproducible: which rows are superseded depends on timing.

## Durability

| Moment                                  | What survives                         |
|-----------------------------------------|---------------------------------------|
| Before a decision is recorded           | Nothing; the next row decides again.  |
| A held decision                         | The decision and the advanced watermark. |
| A pending decision, before its order    | The decision, `pending`. After a crash it is abandoned (`server_restarted`) and never resubmitted. |
| The order's commit                      | The order, its fill, the account, and the decision `recorded`, all in one transaction, or none of them. |
| An uncertain commit (connection lost)   | The decision says which: `pending` means no order exists. The runner reads it back and continues. |

Invariant: a decision has at most one order, and an order exists only for a
`recorded` decision.

## Running it

Deployments are off unless the server is started with `--model-deployments`
and the research environment:

```sh
CHRONOS_POSTGRES_URL=postgresql://chronos:<password>@127.0.0.1:55432/chronos \
build/chronos_paper_trading_server \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
  --instrument AAPL --instrument SPY --fee-per-fill 0.01 \
  --replay-dbn data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst --replay-speed 10x \
  --model-deployments \
  --research-python python/.venv/bin/python \
  --research-python-root python \
  --research-artifacts data
```

The database must have migrations `005_model_deployments.sql` and
`008_registered_model_deployments.sql` applied (with 006 and 007 between
them). The server never applies migrations itself; a database without 008
fails deployment reads and creates with a message naming the file. To deploy
imported ONNX models, also give the Docker CLI's absolute path:
`--onnx-docker /usr/local/bin/docker` (on macOS with Homebrew,
`/opt/homebrew/bin/docker`).

Without the flag, the deployment routes still list deployments and still
stop them, so a server cannot be left unable to stop what another started.
Creating or starting one is refused with `409 deployments_disabled`.

Each running deployment has one inference worker, `python -m
chronos_research inference-worker`, in its own process group. Its
environment holds only `PATH` and `PYTHONPATH`, so no credential reaches it.
It is ended with its run. A worker that times out, exits, or answers outside
the protocol faults the deployment. A prediction's write and read share
one wall-clock deadline, so a worker that stops reading cannot block Stop
indefinitely. Requests are limited to 8 KiB and responses to 64 KiB.
Probability checks use binary64 only for model output, never financial
arithmetic: the probabilities must be finite, in range, sum to one, and
agree with the winning class and exact parts-per-billion confidence.

In the web client, Research has a **Deploy to paper** form for an
experiment's model, and a registered version's page under Models has
**Deploy this version to paper**: review the frozen request, create it
stopped. It also has a **Paper
deployments** panel with each deployment's state and reason, its run's
counts, its latest decisions, Start behind an explicit confirmation, Stop,
and the kill switch. **Open in trading** shows the account's portfolio,
orders, and fills in the trading workspace. The workspace offers no order
ticket for an owned account.

The deployment list refreshes every second while running or stopping, and
every five seconds while idle. A lost control response is an **unknown
outcome**, not proof that the action failed. After an uncertain Start or
Stop, another Start is blocked until Stop is acknowledged. An uncertain
creation can be retried with the same reviewed request and IDs.

The API is in [paper-trading-api.md](paper-trading-api.md#model-deployments).

## Registered model versions

A deployment can serve a registered model version
([ADR 0021](decisions/0021-model-registry-imports-and-comparisons.md)):
a version taken from an experiment, or a constrained ONNX import.

- **The request.** It names the version by `model_version_id` alone, in
  place of `experiment_id` and `model`; a body with both shapes is
  `400 malformed_request`. Its canonical form is
  `chronos.model_deployment_request/2`. An experiment deployment's request
  is `chronos.model_deployment_request/1`, byte for byte as before, so every
  existing deployment's `request_sha256` still holds.
- **The worker.** The server runs `inference-worker --model-version <id>
  --instrument <symbol>`. With `--onnx-docker` it adds `--docker <path>`
  and `--sandbox-supervisor <token>`, a fresh random token for this worker.
  It speaks `chronos.inference_worker/2`, whose ready line is:

  ```json
  {"cutoff_basis":"declared","family":"onnx","instrument":"AAPL",
   "model_feature_set":{"name":"top_of_book_baseline","version":"1"},
   "model_version_id":"5e05c484…","payload_sha256":"2bce7582…",
   "protocol":"chronos.inference_worker/2","training_cutoff_ns":"1790343319090266460","type":"ready"}
  ```

  Requests and predictions are protocol 1's. A baseline version predicts
  with the same row-pure arithmetic as its experiment model, bit for bit.
  An import's every prediction runs in the ONNX sandbox container, never in
  the server or the worker.
- **Creation.** The worker's `--describe` verifies the version, requires
  the requested instrument and a known information cutoff, and refuses an
  archived version. For an import it also loads the model in the sandbox.
  Anything else refuses the create (`422 model_unavailable`,
  `model_rejected`), and nothing is written. The deployment records the
  version ID, its family as the model name, the payload checksum, the
  cutoff, and the cutoff's basis. It never records a name and version, so
  nothing can retarget it.
- **Start.** Start re-verifies the version and requires the identical
  identity: version, instrument, family, payload checksum, feature set,
  cutoff, and basis. A difference faults it with `model_identity_mismatch`.
- **Declared cutoffs.** An import's cutoff is its uploader's declaration.
  The deployment decides only on market data received at or after it, as
  for any cutoff, and its status and the UI mark it "Declared by uploader,
  not verified".
- **Archive.** An archived version cannot be chosen for a new deployment. A
  deployment that already serves it still starts, because archiving never
  changes what uses a version.
- **Failures.** A prediction that is not a valid distribution (wrong shape,
  not finite, out of range, or not summing to one within 1e-12) is an error
  line, which faults the deployment with `inference_request_refused`. A
  sandbox that times out or exits makes the worker report the failure and
  exit, which faults it with `inference_protocol_error`, or with
  `inference_timeout` if the server's own deadline passes first. None of
  these records an order. Without `--onnx-docker`, an import cannot be
  created or started.
- **Sandbox containers.** The server's deadlines (30 s to load, 2 s per
  prediction) can kill a worker before it removes its own sandbox
  container. So whenever a worker ends, by Stop, a deadline, an exit, or a
  describe, the server removes every container under the worker's token
  and checks that Docker lists none. It writes a diagnostic line if it
  cannot confirm that. Each container's own watchdog also removes it when
  its worker dies, including when the server dies too (ADR 0021).
- **Storage.** Migration 008 adds `model_reference` (`experiment` or
  `registered`), `model_version_id`, and `training_cutoff_basis`. One
  constraint, `model_deployments_one_model_reference`, keeps an experiment
  row to migration 005's rules (an experiment ID, no version ID, a logistic
  or majority model, a verified cutoff), and a registered row to a version
  ID and no experiment ID. Existing rows become experiment rows, unchanged.
- **Status.** The deployment resource's `model` has `reference`, and either
  `experiment_id` or `model_version_id`, with `name` (the family) and
  `training_cutoff_basis`
  ([paper-trading-api.md](paper-trading-api.md#the-deployment-resource)).

## What it is doing, and how long it took

Each deployment card answers what the deployment is doing now, separately
from what it last decided ([ADR 0019](decisions/0019-deployment-observability.md)).

**Now** is the server's `condition`, the first of these that applies:

1. **Its lifecycle.** A stopped, stopping, or faulted deployment is shown as
   such, with its reason, whatever its last run saw.
2. **The source.** The source is not delivering: the replay has not
   started, is paused, or has finished, or the live feed is connecting,
   reconnecting, failed, or stopped.
3. **The quote.** The instrument is halted or closed (only a trading-status
   event says so), or its quote is unavailable, suspect, or older than the
   account's maximum quote age. Silence makes a quote stale. It never says
   the market is closed.
4. **A pending order.** An order is waiting to be placed, with the latest
   check's reason (such as the minimum latency, or the market being busy
   matching), its not-before time, and its expiry.
5. **The latest candidate of this run.** It was received before the
   training cutoff (the cutoff is shown), arrived within the decision
   interval (the next eligible receive time is shown), was already decided
   on, had no model inputs, or was too old. Otherwise the deployment is
   watching for the next quote.

Each condition shows when it was observed, in the market clock and the
server's wall clock. A skipped candidate has no prediction, and none is
invented. **Latest saved prediction** is always the newest saved decision,
labelled with its run when that is an earlier one. Confidence is the model's
own: it is not a probability of profit.

**Timing** keeps clocks apart:

- **Configured timing.** The minimum latency, decision interval, and signal
  age, all in the market clock. The latency is a minimum, not a promise.
- **Latest order timing.** Market time from the signal to its decision, to
  the order's acceptance, and to its fill, and how long after the signal the
  execution quote was received. They are derived exactly from saved times.

  The execution quote's offset is not the order's latency. In a 10x replay
  on 2026-10-04, one order's quote was received 116 ms after its signal,
  while the order was accepted 262 ms after it.

  A recorded order that booked nothing has no fill time; neither does a
  rejected one. A duration that cannot be derived says why. It is never
  zero.
- **Work time.** Inference and the trusted order path, in wall-clock time
  for this server process, including the order path's waits for the market.

**Counts by reason** sit in a collapsed detail: cumulative for the run in
this process, not a current state. After a restart, only durable facts
remain: lifecycle, saved decisions, orders, fills, and the watermark.

## Authority

The server has no authentication. Ownership is a local application rule
for a single-user server on localhost: it keeps a manual click or script from
trading a model's account by mistake, and keeps the model from trading
anyone else's. It is not access control. The trusted order path has no
HTTP route.

## The demonstration

A paced 10x replay of the local AAPL/SPY recording, on a fresh database
(2026-10-04), with three deployments of the AAPL and SPY logistic models
of experiment `05be6dd4…`. It used the research service's default policy:
threshold 0.5, target 1 share, latency 1 ms, one decision a second, and a
5 s maximum signal age.

| Deployment | Decisions | Orders | Abandoned | Cash | Position | Fees | Realized |
|---|---|---|---|---|---|---|---|
| demo-aapl | 206 | 64 | 2 | 9977.84 | 0 | 0.64 | −21.52 |
| demo-spy (stopped at 13:39:01) | 208 | 17 | 0 | 9229.86 | 1 SPY | 0.17 | −0.36 |
| demo-aapl-2 (after a crash and restart) | 237 | 76 | 1 | 9978.10 | 0 | 0.76 | −21.14 |

The run covered the full lifecycle:

- **Before the training cutoff** (13:35:19.090), all candidates were skipped:
  16,959 for AAPL and 54,610 for SPY.
- **Pause** froze everything.
- **Stop** kept the SPY share and traded nothing more.
- **A SIGKILL and restart** recorded demo-aapl as `stopped
  (server_restarted)`.
- **Starting it again over the same recording** decided nothing: 10,764
  candidates were not after its watermark.
- **Terminating the server's PostgreSQL connections during the run** made
  one demo-aapl-2 order uncertain. It was reconciled from its decision, and
  trading continued.

An independent check of the database against the normalized recording
found every invariant held:

- every decision's five inputs re-derived exactly from the raw quote it
  observed;
- one order per recorded decision, and no other;
- every execution after the signal's quote and within its maximum age;
- every fill at the recorded opposite best price;
- cash, fees, realized PnL, and position equal to the API's, to the cent.

A second replay on 2026-10-04 checked the observability added by ADR 0019,
on a fresh database. Its conditions appeared in order:
- `source_not_started`
- `before_training_cutoff`
- trading
- `source_paused`
- `waiting_for_decision_interval` and `order_pending`
- `stopped`

`validate.py audit --recording` independently confirmed both of its
deployments:
- 332 decisions, all inputs re-derived from the recording;
- 106 orders, each with exactly one fill at the opposite best price;
- cash, fees, realized PnL, and position equal to the API's. The original
  auditor reported the API's equity but did not independently verify it;
  the review corrected this limitation.

Six more replays on another fresh database, with the final build, passed
the same audit: six deployments, 953 decisions, and 301 orders, each with
one fill. The audit also failed, as it should, on each of five deliberate
corruptions of a copy of the database. Killing the server (`SIGKILL`) during
a run left that deployment recorded as running. The restarted server
stopped it as `server_restarted`, and nothing changed afterwards. That run
had made no decisions: a replay of data it had already decided on never
passes its durable watermark.

The isolated Codex browser review independently checked 17 decisions and
two fills after Stop: all 17 inputs matched the normalized recording,
cash and equity were both 9999.91, fees were 0.02, gross realized PnL was
−0.07, and the remaining position and open cost were zero. The corrected
auditor passed with no problems. Regression fixtures additionally cover
fractional weighted-average cost removal, half-away notional rounding,
incorrect equity and unrealized PnL, and incomplete recording coverage.

The policy lost money, as the historical evaluations did. This is evidence
that the machinery works, not that the model has skill.

## Validating on live data

Automated tests cover the live source with a fake transport: connecting, a
first and a fresh quote, a disconnect while an order is pending, silence, a
crossed book, Stop, the kill switch, and restart. Real Alpaca IEX data is
checked by an operator, with `tools/deployment_validation/`:

1. **Preflight, read-only.** Use a fresh database with migrations 001 to
   007, and confirm both deployment triggers and their functions are
   installed:
   `CHRONOS_VALIDATION_DATABASE_URL=... validate.py preflight`.
   - Keep evidence databases in persistent storage, not Docker tmpfs, and
     save a database backup before restarting shared infrastructure. A
     Docker/VM restart discards tmpfs databases.
   - It also lists each live stream's reservations, writer lease, and the
     first sequence of its writer's generation.
   - A copy of a database from before 006, with history on the stream,
     needs `CHRONOS_VALIDATION_ADOPT_LIVE_STREAM_HISTORY=1` for its first
     live start (ADR 0020).
2. **Start an isolated live server.** Run `start_live_server.sh` with
   `CHRONOS_VALIDATION_DATABASE_URL` and `CHRONOS_VALIDATION_EVIDENCE` set.
   - It loads the Keychain credentials into the server's environment only.
   - It refuses a busy port.
   - It refuses if another local server already holds the one Alpaca
     connection: coordinate with its owner rather than stop it.
3. **Run.**
   `validate.py run --server http://127.0.0.1:8083 --deployment-id ... --account-id ... --experiment ... --instrument AAPL --minutes 10 --i-understand-this-uses-live-iex-market-data --evidence run.json`
   - It refuses unless the source is live and a quote is fresh.
   - It creates the deployment stopped (one share, five orders, two
     rejections at most), then starts it explicitly.
   - It records every condition change for a bounded time, stops it, and
     checks that nothing is decided after Stop.
   - It uses new deployment/account IDs only. Once Start is attempted,
     cleanup tries Stop even if a reply is lost or the operator interrupts
     the tool. If Stop cannot be confirmed after three attempts, it reports
     an unknown outcome: stop the isolated server before proceeding.
   - Its verdict is one of: `fill observed`, `orders observed, no fill`,
     `inference observed, no order`, or `inconclusive`. A closed market or an
     idle feed is inconclusive, never a pass.
4. **Restart.** Stop the server with SIGTERM, start it again, and check:
   - `validate.py verify-stopped --server ... --deployment-id ...`;
   - with an open position,
     `validate.py verify-continuity --server ... --account-id ... --instrument AAPL`.

   The second waits for the first quote received after the account's last
   fill. That quote must follow every durable sequence of the stream, and
   it must value the open position. A ledger that reconciles is not this:
   the audit reports `valuation_verified` separately. No fresh quote is
   inconclusive, never a pass.
   - The quote judged is the one in the snapshot that was found fresh, and
     the first one decides: a violation fails at once, and a quote gone
     before valuation fails.
   - `proof` says what was shown. `server_first_sequence`: the server's
     `first_ingest_sequence` also follows the durable history, which covers
     every quote since it started. `first_observed_quote`: the server did
     not report it, so earlier quotes were sampled, not proven
     (`proof_limitation`).
   - Durable history is read once, when the check begins. Run it before the
     restarted server trades: rows it already wrote make the check fail,
     never pass.
5. **Audit.** Run `validate.py audit --server ... --deployment-id ...`. It
   rebuilds cash, fees, weighted-average realized PnL, open cost, and
   position from the fills using independent integer arithmetic and the
   account's half-away rounding contract. It checks:
   - every decision's linkage to its one order;
   - later-quote and latency rules, and expiry;
   - opposite-side prices, and the position limit.
   - when valuation is available, best-bid marks, market value, unrealized
     PnL, equity, and total PnL against the independently rebuilt account.

   With `--recording`, a replay's inputs are also re-derived from the
   normalized recording. Live triggering quotes are not retained, so for
   live data the tool says so instead.
6. **Scan.** `scan_evidence.sh <dir>` counts the evidence files that contain
   a credential, without printing one. It must print 0 and exit successfully.
   Missing Keychain credentials, an unreadable directory, or a scan error
   fail rather than claiming a clean scan. A match exits with status 1;
   an incomplete scan exits with status 2.

A real disconnect is not forced: nothing scoped to the test's own
connection can drop it safely, so real reconnect recovery stays covered by
the fake transport only. Results on 2026-10-04, a Sunday evening:

| Check | Result |
|---|---|
| Handshake (connect, authenticate, subscribe, live) | passed |
| Fresh quote | none (market closed): **inconclusive** |
| Inference, orders, and fills on live data | not observed |
| Stop and restart on live data | not exercised, since nothing was created |
| Credential scan of the evidence | 0 files |

### Market-hours acceptance, 2026-10-05

An isolated server and a fresh migrated database consumed real AAPL IEX
quotes. No broker order was sent. A verified logistic model recorded 23
decisions, five orders, and five booked paper fills, then became inactive
at its five-order limit. An independent audit reproduced cash 9665.89,
gross realized PnL -0.09, one remaining share, and open cost 334.02. This
particular acceptance account had zero configured fees; these figures are
not evidence of profitability or realistic total execution costs.

A separate majority-model deployment verified explicit Stop, reload, and
quietness. Killing the isolated server while that deployment was running
and restarting it restored it as inactive, `stopped/server_restarted`.
Its decisions and account remained unchanged during a ten-second check
while new live quotes arrived. Browser checks covered the live feed label,
booked fills, account ownership, controls, and widths 1440, 412, and 360 px.

**Restart continuity failed on the October 5 tree.** The fix was subsequently
reviewed (ADR 0020) and validated with a fresh position on October 7, below. The
Alpaca source initializes its ingest counter from process-local market
state, which is empty after a process restart. The durable account had
already used quote sequence 1087 on `alpaca-iex-validation`. After another
restart, a newer, executable live quote on the same stream had sequence 70;
portfolio valuation correctly refused it with `503` and
`incompatible_market_timeline`. Waiting until the counter catches up is
not a continuity guarantee and reuses stream/sequence identities. The
lineage checks must remain intact; this failure required the durable
restart-safe identity and writer fence described below.

Live triggering inputs were not independently re-derived because their
quote history is not retained. A real network disconnect and a crash with
a pending live order were not exercised in this session. They remain
covered by automated/fake-transport tests, not this live evidence.

The local evidence is in
`/private/tmp/chronos-live-acceptance-20261005.0Z3tJO/`, including the run,
independent audits, Stop/restart checks, the exact failed-continuity
responses, and browser screenshots. The isolated live server and web copy
were stopped after the test; the usual development app and accounts were
not changed. This path is local evidence, not a distributable fixture.

### Restart continuity (ADR 0020)

The failure above is now reproduced deterministically by
`chronos_live_stream_restart_tests`, with:
- an account that bought on sequence 1087;
- one fresh quote after a restart;
- the real service, repository, and Alpaca source over the fake transport.

On the tree that the acceptance tested, the quote was sequence 0,
valuation reported "the quote does not continue the instrument's market-data
timeline", and the sell was refused. After the fix, the quote follows every
sequence the earlier process reserved, the position is valued at its bid,
and the sell fills. That holds after a clean stop and after a crash.

On real Alpaca IEX data with the market closed (2026-10-05, 23:22 ET), a
copy of the acceptance database was upgraded to 006 and checked:

| Check | Result |
|---|---|
| Adoption | took the lease after durable history up to **8453** (the control deployment's decisions; fills stopped at 1087), and continued at 8454 |
| Handshake | connected, authenticated, subscribed, and went live |
| Clean restart | lease released after the source stopped; taken again at once, from the next block |
| SIGKILL restart | reported the dead writer and its expiry, opened no Alpaca connection, and took the lease after expiry (about 13 s) |
| Fresh quote, valuation of the open position | none (market closed): `verify-continuity` **inconclusive** |
| Credential scan of the evidence | 0 files |

### Market-hours restart validation, 2026-10-07

The October 5 scratch databases used Docker tmpfs and did not survive a
Docker recovery on October 7. Their saved reports and logs remain, but no
database backup was found in the scoped evidence directories. The rerun
therefore used a **new persistent isolated database**, with migrations
001–007, and a fresh one-share AAPL paper position. It does not prove
fresh-quote continuity of the lost legacy-adopted account.

| Check | Observed result |
|---|---|
| Clean restart | First server sequence 2097152 exceeded durable history 1048623; that quote valued the open position at bid 336.95 |
| Crash restart | Waited for the old writer lease; first server sequence 3145728 exceeded durable history 2097246; observed fresh quote 3145735 valued the position at bid 336.96 |
| Trading after each restart | Both one-share sells filled; four paper fills in total, including the initial buy and a new buy before the crash |
| Duplicate after clean restart | POST returned 409 `duplicate_client_order_id`; GET recovered the original fill, and account/history were unchanged |
| Majority-model control | Two verified live predictions produced Hold, no orders; explicit Stop stayed quiet, and crash recovery stayed `stopped/server_restarted` |
| Independent accounting | Flat inventory, cash/equity 9999.82, gross realized PnL −0.18, and fees 0 matched the four fills |
| Preservation | Development data-only dump fingerprints matched before/after; test writer lease released, owned server stopped |
| Backup | Isolated database dump restored successfully into a second new isolated database |

The crash check valued the first fresh quote the validator observed, not
necessarily the process's first quote. Its `server_first_sequence` proof
also checks that the first accepted sequence follows the durable baseline;
neither check waits for a restarted counter to catch up. Live decisions
did not independently re-derive their inputs from retained triggering
quotes. No real network disconnect or pending-live-order crash was forced.

An initial attempt recorded a safe `suspect_market_state` rejection because
local receipt preceded the provider timestamp by 136,961 ns. Its account
and failure evidence were retained. The successful attempt used another
new account and a quality-aware test helper; no product safety rule changed.

Local evidence and the restored-backup result are documented in
`/private/tmp/chronos-persistent-live-acceptance-20261007.yukLKf/`.
No broker order was sent. This is operational evidence, not model skill or
a public distributable fixture; the ADR remains proposed pending acceptance.

## Limitations

- **Real-live proof is bounded.** October 7 verified clean/crash restart
  continuity and trading for a fresh AAPL paper position on IEX. Legacy
  adoption followed by fresh real quotes, a real network disconnect, and
  a pending-live-order crash were not exercised in that rerun. Since ADR
  0020, a live stream's sequences come from durable
  reservations under an expiring writer lease and continue across restarts,
  and the database refuses a deposed writer's orders, fills, decisions, and
  valuations. `chronos_live_stream_restart_tests` reproduces the 2026-10-05
  failure and Codex's deposed and lapsed writers on the tree before the fix
  and passes after it. Deterministic and fake-transport coverage is broader
  than the live session; neither should be presented as the other.
- **A deposed writer can still display quotes** in its own process until
  its local lease deadline (at most 12 s after its last renewal). It cannot
  act on them.
- **A crash makes the next live start wait** up to 15 s for the old lease,
  plus the source's reconnect backoff.
  A stream with history from before migration 006 needs one explicit
  `--adopt-live-stream-history`. That adoption proves nothing about
  identities the old process published but never recorded.
- **The realized latency is much longer than the configured one.** A
  submission waits for the market-event gate. Under a fast replay the feed
  holds the gate for most of the time, applying each event and matching
  working orders with a database query, so a submission can wait up to the
  order-matching wait (2 s wall clock).

  In the demonstration below (10x replay, configured latency 1 ms), every
  order still executed after its signal's quote and at least 1 ms after it,
  but the realized delays were much longer:

  | Deployment | Median | 90th percentile | Max |
  |---|---|---|---|
  | demo-aapl-2 | 0.44 s | 1.3 s | 2.1 s |
  | demo-aapl | 0.91 s | 3.4 s | 4.9 s |

  These are market time, roughly a tenth of that in wall time. Three
  signals in 160 order decisions expired and were abandoned, never filled
  late. Live results are therefore not comparable to a historical run's
  "first quote at or after the latency" execution. Each run reports the wall
  time of its order path (`max_submission_us`); a fairer gate, or matching
  outside it, is the fix.
- **Run counters are in memory.** They describe the current server process
  only. Decisions, orders, and the watermark are durable.
- **One instrument, at most one share, market IOC orders only.** One
  deployment per account.
- **Imported models need Docker and declare their cutoff.** An import runs
  only in the ONNX sandbox, so the server needs `--onnx-docker` and the
  sandbox image. Chronos cannot verify an import's declared cutoff.
- **No real-broker anything.** An Alpaca deployment uses IEX market data
  only.
