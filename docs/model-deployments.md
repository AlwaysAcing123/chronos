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

- **One model.** A verified `logistic` or `majority` model from one research
  experiment, for one instrument, identified by the SHA-256 of its parameters
  as the inference worker re-derived them.
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

The database must have migration `005_model_deployments.sql` applied. The
server never applies migrations itself.

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

In the web client, Research has a **Deploy to paper** form: choose a model,
review the frozen request, create it stopped. It also has a **Paper
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

The policy lost money, as the historical evaluations did. This is evidence
that the machinery works, not that the model has skill.

## Limitations

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
- **No real-broker anything.** An Alpaca deployment uses IEX market data
  only.
