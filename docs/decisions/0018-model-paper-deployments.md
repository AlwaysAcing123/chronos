# ADR 0018: Controlled model-driven paper deployments

- Status: Proposed
- Date: 2026-10-04

## Context

Historical evaluation (ADR 0016) trades a verified model only in throwaway
accounts over a recorded window. A **deployment** runs the same verified
model and the same `long_flat_threshold` policy against the paper-trading
server's current market. Its orders go into a durable paper account,
through the same risk, execution, accounting, and journal as manual orders.

Constraints that shape the design:
- **The source thread holds the gate.** The market-data source applies each
  event and offers it to the `MarketEventObserver` while holding the
  `MarketEventGate` exclusively. `PaperTradingService::submit_order` takes
  the same gate shared, so calling it from `observe()` would deadlock or
  time out.
- **Inference is Python's.** The verified plain-parameter models and their
  row-pure inference (`schedule.py`) live in Python. C++ owns market
  processing, the policy, risk, orders, accounting, and every authoritative
  financial value.
- **No authentication.** Ownership of an account can only be local
  application policy. Ordinary clients must still not be able to write into
  a model's account by editing request fields such as `source.kind`.
- **Crashes and uncertainty.** A crash, a lost database connection, or an
  uncertain response must never produce a second economic action for one
  decision.

## Decision

### Where it runs: inside the paper-trading server, with a Python worker

Deployments run inside the C++ paper-trading server, the only process that
can write orders. The trusted path is
`PaperTradingService::submit_deployment_order`. It is called in-process by
the deployment runtime and is reachable from no HTTP route. HTTP clients can
only create, start, stop, and read deployments.

Each running deployment has one long-lived Python inference worker:
`python -m chronos_research inference-worker`. The server spawns it, with a
fixed argument vector, in its own process group. Server flags opt in to
deployments: the Python executable, the research package directory, and the
artifact roots. Without them the deployment routes report the capability as
unavailable.

The server caps active deployments at eight. The worker's write and read
share one deadline; its input pipe is nonblocking. Request and response
lines are bounded to 8 KiB and 64 KiB respectively. Binary64 probabilities
are validated separately from fixed-point financial values, including the
winning class and the exact `floor(1e9 × p)` confidence.

Protocol `chronos.inference_worker/1`, one JSON object per line:
- **Ready.** The worker loads the experiment once, with
  `verified_experiment` (identity, checksums, every stored prediction
  re-derived). It reports the experiment ID, instrument, model, the
  SHA-256 of the model's canonical stored parameters, the model feature
  set, and the training cutoff, then never reads the files again.
- **Requests.** Each request carries an ID, the observation's identity
  (stream, instrument, ingest sequence), and the five exact model inputs as
  integer strings.
- **Responses.** Each response echoes both IDs and gives the class, the
  probabilities as shortest round-trip strings, and `confidence_ppb`
  (`floor(1e9 × p)`). Inference uses `logistic_predictions`, the row-pure
  arithmetic schedules use, so live and historical predictions of the same
  row are identical.
- **Checks.** The server checks every field: identities, a known class,
  probabilities that are finite and in [0, 1] with the class at their
  maximum, and a confidence consistent with its probability. A malformed,
  late, mismatched, or non-finite answer faults the deployment and produces
  no order.

**Rejected:**
- *C++ inference*: a second model implementation, with its own parity
  burden.
- *Spawning Python per quote*: too slow, and with unbounded process churn.
- *Running deployments in the research service, submitting over HTTP*:
  orders would need a client-asserted identity that any client could
  spoof, and the engine would be driven by coalesced browser notices or
  snapshot polling.
- *Sharing the historical evaluation job queue*: a live loop must not wait
  behind a historical run.

### Account ownership

A deployment is created together with a new, dedicated paper account, in
one transaction. `model_deployments.account_id` is unique and references
`paper_accounts`, so an account belongs to at most one deployment, ever,
and an existing manual account can never be taken over.
- **Manual paths refuse it.** Manual submission and cancellation check
  ownership under the account row lock and refuse a model-owned account with
  `409 account_owned_by_deployment`. That holds while the deployment is
  stopped too; there is no ownership release.
- **The trusted path requires it.** It requires the account to belong to the
  submitting deployment.

Without authentication this isolates accounts by local application policy.
It does not protect against a hostile process with database access.

### Lifecycle

| State | Meaning |
|---|---|
| `stopped` | Configured, or stopped. Never trades. Created deployments start here |
| `running` | May decide and trade. It may still be waiting for the training cutoff or usable market data; skip and wait counters describe that in the API |
| `stopping` | Stop accepted. No new decision; an unresolved decision is being reconciled |
| `faulted` | Stopped by a safety check, with its reason. Never resumes by itself |

**Transitions:**
- **Start.** `stopped`/`faulted` → `running`. It is explicit. It requires:
  - a worker whose verified identity equals the recorded one;
  - the server's stream equal to the deployment's;
  - a healthy account;
  - no unresolved decision.

  Each start increments `run_generation`.
- **Stop.** `running`/`faulted` → `stopping` → `stopped`. It is idempotent on
  `stopped`.
- **Fault.** `running` → `faulted`.
- **Restart.** On server start, every `running` or `stopping` deployment
  becomes `stopped` with reason `server_restarted`, after its unresolved
  decisions are reconciled. Nothing resumes trading automatically.
- **Configuration.** A deployment's model, instrument, policy, timing, and
  limits are fixed at creation. Changing anything means a new deployment
  and account.

### Sampling, timing, and the lock order

**Observing (source thread).** The observer chain is the working-order
matcher first, unchanged, then the deployment hub.
- The hub runs the shared `TopOfBookFeatureEngine` on every event, so trading
  status is current.
- For each top of book it computes the feature row and the five model inputs
  (`model_inputs`, the function the historical runner uses).
- It then offers a candidate to each running deployment of that instrument:
  a one-slot mailbox, where the latest candidate wins.
- It takes only leaf mutexes and never blocks.

**Deciding (one runner thread per running deployment).** A runner takes the
newest candidate, and decides only if all of these hold:
- the candidate is at or after the training cutoff;
- it is strictly after the persisted decision watermark (receive time and
  ingest sequence);
- it is at least `decision_interval_ns` of receive time after the previous
  decision;
- its inputs are available;
- it is still current: the market clock is at most `max_signal_age_ns` past
  it.

Every candidate that is skipped is counted by reason: `coalesced`,
`before_training_cutoff`, `within_interval`, `feature_unavailable`, or
`stale`. No backlog is ever processed as though its quotes were current.

**Inference.** It runs on the runner thread, holding no lock, under a
wall-clock timeout. The timeout is separate from market time; a paused
replay does not age a request.

**The global lock order:**
1. `submission_mutex_`;
2. the `MarketEventGate` (shared);
3. the service connection mutex;
4. the PostgreSQL account row;
5. the deployment row;
6. the decision row.

Hub and mailbox mutexes are leaves, never held while taking another lock.
- **No deadlock with the source.** The source thread holds the gate only
  while applying and observing, and the hub never waits for a runner. A
  runner never holds a leaf while taking the gate, so no cycle exists.
- **Matching is unchanged.** The matcher still sees every event first and
  matches as before.

### Execution contract

A deployment order executes like a manual one: at the server's current
execution quote, when it is accepted. That is a current-market contract, not
the historical runner's first-eligible-observation rule, so economic
results of the two need not agree.

**Acceptance.** It happens under the gate and the account lock, and
revalidates:
- the deployment is `running`, in the same generation;
- the decision is still unresolved;
- the account is owned and healthy;
- the source is available and the quote is current (the `MarketState`
  rules), unsuspect, and no older than the account's maximum quote age;
- the instrument is not halted or closed;
- the quote is strictly later than the signal (a greater ingest sequence)
  and was received at least `latency_ns` after it, so an order never fills
  on its own or an earlier quote;
- the market clock is at most `max_signal_age_ns` past the signal.

If the quote is not yet late enough, the runner waits for the next
observation. If the signal expires first, the decision is abandoned and no
order is made. Signal, acceptance, and execution times are recorded as they
happened; the configured latency is a minimum, not a guarantee.

### Training chronology

The worker reports the training cutoff: the experiment's validation start.
Training observations end before it, and every training label was known
before it.
- **Waiting.** A deployment does not decide on observations before the
  cutoff; its `before_training_cutoff` skip counter increases.
  Replaying the recording from its start therefore cannot trade a model
  inside its own training period.
- **Watermark.** The persisted watermark makes a replay restart or a
  reconnect unable to reuse old observations, and the account's own
  timeline rule refuses them too.

### Durable decisions and the recovery invariant

Every decision is written before anything acts on it.
- **Recording.** A transaction on the deployment row checks that the
  deployment is running in this generation, that no decision is pending,
  and that the observation is past the watermark. It then inserts the
  decision, advances the watermark, and allocates the next sequence.
- **What it holds.** Observation identity, exact inputs, prediction, reason,
  and, for an order, the intended side, quantity, and deterministic client
  order ID (`<deployment>-<sequence>`). A hold is resolved at once; an order
  decision stays `pending`.
- **Submission.** One transaction takes the account row, then the
  deployment row, then the decision row. It revalidates as above, runs the
  existing processor, and commits the order journal, fills, and account
  change together with the decision's resolution (`recorded`, with the
  outcome).
- **Abandonment.** Abandoning a decision (expired, stopped, refused,
  recovered after a crash, or after an uncertain failure) takes the same
  three locks and resolves the pending decision as `abandoned`, with the
  reason.

**Invariant: one decision produces at most one recorded order.**
- **Orders.** The only writer of a deployment order is the submission
  transaction. It moves its decision out of `pending` in the same
  transaction (`UPDATE … WHERE state = 'pending'`, which must change exactly
  one row). The journal's ownership trigger matches the frozen order
  payload; a deferred constraint checks at commit that the decision is
  `recorded` with the order's execution sequence and times. An ordinary
  writer cannot commit by guessing a pending decision ID.
- **Pending.** A decision leaves `pending` at most once. A partial unique
  index allows one pending decision per deployment. The order journal's
  unique `(account, client_order_id)` is a second guard.
- **Recovery is exact.** The decision and its order commit together, so a
  pending decision never has an order. After a process restart, every
  pending decision is abandoned and never resubmitted. Within an ongoing
  run, an uncertain failure is read back: a `recorded` decision is complete;
  a still-`pending` decision may be retried only through the same trusted
  path, with the same ID and all eligibility checks. Account locking waits
  for an earlier in-flight transaction to commit or roll back before a
  retry can decide what happened.

A crash leaves either the decision unrecorded, or the decision recorded
and pending, or decision and order committed together. Each case recovers
without a second action.

### Stop and the kill switch

- **Stop.** It moves the deployment to `stopping`, durably, which stops new
  decisions and invalidates queued candidates. It waits, bounded, for the
  runner, which abandons any unsubmitted pending decision. An in-flight
  submission simply completes or rolls back; the acceptance check runs under
  the deployment row lock, so it cannot commit after `stopping`. Only then
  is the deployment `stopped`. Positions are kept and shown; nothing is sold.
  The policy is IOC-only, so there is no working remainder to cancel.
- **Kill switch.** It applies the same durable stop to every deployment on
  the server, with reason `kill_switch`. It is idempotent, and it is not a
  liquidation.

### Policy and limits

The policy is `long_flat_threshold` v1, shared with the historical runner
through one function. A deployment is one instrument, one share at most,
market IOC.
- **Account limits.** At most `target_quantity` per order and per position;
  the server's notional limit, fee, and maximum quote age.
- **Deployment limits.** A maximum number of orders per run and of
  consecutive rejections. Reaching either faults the deployment.
- **No order** is made while:
  - the deployment is not running, or a decision is unresolved;
  - the feed is unavailable, stale, suspect, or halted;
  - inference or features are invalid, or the account is unhealthy;
  - the decision cannot be recorded.

## Consequences

- **Real paper orders.** Model orders are journaled, filled, valued, and
  listed like manual ones, attributed to their deployment (source `model`,
  ID `<deployment>`) and immutable model version.
- **Bounded sampling.** Decisions sample at most one observation per
  interval. Live results depend on worker timing, so they are not
  reproducible like historical runs.
- **Different contracts.** The live execution contract differs from the
  historical one; the documentation says so.
- **Setup.** Running deployments needs the Python environment next to the
  server, and an explicit server opt-in.

## Implementation notes (2026-10-04)

- **Realized latency.** Under a 10x replay, orders waited for the
  market-event gate far longer than the configured latency: median
  0.4–1.1 s of market time, with a 1 ms setting. Three signals in 160 order
  decisions expired and were abandoned. The positive-latency guarantee held
  for every order. A fairer gate, or matching outside it, would shorten the
  wait. Each run now reports its order path's wall time.
- **Uncertain decisions.** A decision whose recording is uncertain may still
  have committed, leaving it pending in the database while the runner holds
  nothing. The runner resynchronizes from the deployment row whenever a
  decision is refused or its recording fails, adopting the pending decision
  and its watermark, so it never stalls behind a decision it did not know it
  made.
- **Concurrent creates.** Two identical concurrent creates both see no
  deployment. The second's account insert waits for the first's, then
  conflicts. It re-reads the deployment in a fresh snapshot, and returns the
  existing deployment rather than `account_exists`.
- **Visibility.** What a deployment is doing, and how long its orders took,
  is described in [ADR 0019](0019-deployment-observability.md).
