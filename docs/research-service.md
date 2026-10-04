# Research service and Research view

The web workspace's **Research** view lets a user:
1. choose a verified experiment, instrument, and fitted model;
2. read its training window, prediction metrics, and limitations;
3. review the policy, capital, cost, latency, and risk assumptions;
4. start a strategy evaluation and follow its real status;
5. open, reload, and rediscover the immutable result: trades, equity,
   drawdown, costs, benchmarks, and provenance.

Review freezes the exact request shown. Changing the catalog selection
does not change that request; choose **Edit** and review again to change it.

Research talks to a local research service, never to the trading API. It
cannot touch a trading account, the replay, or a live source, and switching
views keeps the open paper account. The decisions are in
[ADR 0017](decisions/0017-local-research-service.md).

## Running it

```sh
cd python
.venv/bin/python -m chronos_research serve --tools ../build --artifacts ../data --state ../data/research \
  [--host 127.0.0.1] [--port 8090] [--max-running 1] [--max-queued 8] [--job-timeout-s 1800] [--retain-jobs 200]
cd ../apps/web
npm run dev        # proxies /api/v1/research to CHRONOS_RESEARCH_ORIGIN, default http://127.0.0.1:8090
```

- **Address.** It listens on a loopback address only; any other host is
  refused at startup. `data/research/`, the job records, is ignored by Git.
- **Dependencies.** Only the standard library and the package's own code:
  no web framework, uploads, pickle, or joblib.
- **Proxy.** The Vite proxy lists `/api/v1/research` before `/api`, so the
  trading API keeps every other path.

## Contract

| Method and path | Purpose |
|---|---|
| `GET /api/v1/research/status` | Version, defaults, limits, job counts, and whether `chronos_dataset` and `chronos_strategy` are built |
| `GET /api/v1/research/experiments` | Verified experiments with their dataset, windows, per-instrument models (fitted, or unevaluable with the reason), prediction metrics, and limitations; unreadable ones under `invalid_entries` |
| `GET /api/v1/research/experiments/{id}` | One of those |
| `POST /api/v1/research/runs` | Start an evaluation |
| `GET /api/v1/research/runs` | Jobs, newest first |
| `GET /api/v1/research/runs/{job_id}` | One job |
| `GET /api/v1/research/results` | Published strategy runs; unreadable ones under `invalid_entries` |
| `GET /api/v1/research/results/{run_id}` | A run's results, configuration, assumptions, definitions, provenance, and environment |
| `GET /api/v1/research/results/{run_id}/decisions` | Its decisions, oldest first |
| `GET /api/v1/research/results/{run_id}/trades?account=strategy\|buy_and_hold\|cash` | Its orders and their outcomes, oldest first |
| `GET /api/v1/research/results/{run_id}/equity` | Every valuation point of every account |

A run request names everything; nothing is defaulted silently:

```json
{"experiment_id": "05be6dd4…", "instrument": "AAPL", "model": "logistic", "window": "test",
 "configuration": {"threshold_ppb": "500000000", "target_quantity": "1", "latency_ns": "1000000",
   "decision_interval_ns": "1000000000", "starting_cash": "10000", "fee_per_fill": "0.01",
   "maximum_quote_age_ns": "5000000000", "maximum_order_notional": "1000000"}}
```

### Conventions

The conventions are the paper-trading API's
([paper-trading-api.md](paper-trading-api.md#representation-rules)).

- **Bodies.** `POST` bodies must be `application/json` (else `415`) and at
  most 16 KiB (else `413`). Unknown, duplicate, missing, or null fields and
  JSON numbers are `400 malformed_request`, with a `reason` such as
  `unexpected_field`, `invalid_type`, or `invalid_decimal`.
- **Decimals and integers.** Both are strings. Decimals follow the trading
  API's grammar and scale; trailing zeros are accepted and canonicalized.
- **Domain errors.** A well-formed value outside its domain is
  `422 invalid_configuration` with the field and reason `out_of_domain`.
- **Queries.** Query strings are strict: only `limit` (1 to 200, default
  50), `cursor`, and, for trades, `account`. Cursors are opaque and bound to
  their list and run. Invalid percent encoding or UTF-8 is a
  `400 malformed_request` with reason `invalid_query`, never a server error.
- **Responses.** Timestamps are RFC 3339 with nine fractional digits; a
  `*_ns` artifact field becomes the field without the suffix. Durations
  stay integer nanoseconds.
- **Errors.** `{"error": {"code", "message", "field", "reason"}}`. The codes
  include:
  - `route_not_found` and `method_not_allowed` (with `Allow`);
  - `experiment_not_found`, `run_not_found`, and `result_not_found`;
  - `unknown_instrument` and `unevaluable_model` (with the model's reason);
  - `artifact_invalid` (an artifact whose bytes do not verify);
  - `queue_full` (503);
  - `misdirected_request` (421, a non-loopback `Host`);
  - `internal_error`.
- **Headers.** Every response carries `X-Chronos-Service: research`; the
  web client trusts no response without it, as it requires
  `X-Chronos-Trading-Mode: paper` from the trading API.

## Jobs

- **No work on a request thread.** A `POST` validates the request, checks
  that the experiment verifies and the model is fitted, and queues a job.
- **Workers.** A bounded pool of workers (`--max-running`) runs each job as
  one fixed child command with an explicit argument vector and a time
  limit: `python -m chronos_research evaluate-strategy …`. The child runs in
  its own process group, and the whole group is killed at the time limit
  and terminated at shutdown. After a one-second shutdown grace period,
  any remaining group is killed, including descendants whose leader has
  exited; worker launch and registration are synchronized with shutdown.
  The child:
  1. verifies the sources again, with reproduction;
  2. publishes the schedule;
  3. runs `chronos_strategy run`;
  4. runs the result again with `inspect --catalog --schedules`.
- **Bounds.** The queue holds at most `--max-queued` jobs (beyond that,
  `503 queue_full`). Combined stdout and stderr are capped at 1 MiB while
  the child is running; exceeding the cap kills its process group and fails
  the job. Only the last 4 KiB of a job's log is kept.
- **States.** `queued`, `running`, `succeeded`, `failed` (with a code such
  as `evaluation_failed` or `timed_out`), and `interrupted`.
- **Deduplication.** An identical request, after canonicalization, returns
  the equal job when it is queued, running, or succeeded (`200`,
  `"deduplicated": true`). After a failure or an interruption it starts new
  work.
- **Durability.** Each job record is replaced atomically (written, synced,
  renamed) in `--state/jobs/`. Completed results are immutable artifacts
  and survive any restart.
- **Recovery.** A job that was queued or running when the service stopped
  is marked `interrupted` on the next start, never resumed unnoticed. On
  shutdown, running children are terminated and queued jobs interrupted.
- **Retention.** At most `--retain-jobs` job records are kept, oldest
  finished first; the artifacts are kept regardless.

## Verification of what is served

- **IDs, never paths.** Artifacts are named only by validated IDs: 64
  lowercase hex digits, or 32 for jobs. `root/<id>` must be a real
  directory and every file a regular file. Manifest filenames must be
  single leaf names, not paths. Catalog and artifact directories are
  pinned by descriptors and opened with `O_NOFOLLOW`, as is the file,
  so symlinks and parent-path substitutions cannot escape the catalog.
- **Bytes, not directories.** The service checks the very bytes it serves:
  - a manifest's identity against its directory name (C++ artifacts hash
    without the trailing newline, Python ones with it);
  - each consumed file against the manifest's size and SHA-256.

  An artifact that fails is `422 artifact_invalid`, or an `invalid_entries`
  item in a listing.
- **Full verification in the job.** Every evaluation verifies and
  reproduces the whole ancestry in its child process before publishing.

## The Research view

- **Navigation.** The header has two views, Trading and Research. Research
  lives under `#/research` and `#/research/results/<run-id>`, so a reload
  or a link reopens the same view. The workspace's section links
  (`#portfolio`, …) keep working. The account selection is separate browser
  state, so "Back to trading" finds the same account open.
- **Model.** The model panel shows:
  - the experiment, dataset, and windows: training, the time by which
    training labels were known, validation, and test;
  - instruments, with unevaluable models and their reasons;
  - prediction metrics, labelled as classification, and kept apart from
    trading results ("Accuracy is not profit");
  - the limitations.
- **Evaluation.** The form is prefilled with the service's defaults.
  Unevaluable models cannot be chosen. Nothing starts until "Review
  evaluation" lists every value and the fixed assumptions. A refused field
  is marked invalid.
- **Runs and results.** Runs poll once a second only while a job is queued
  or running. Saved results list every published run.
- **Result.** The result page has:
  - a comparison of strategy, buy and hold, and cash: equity, PnL, return,
    realized and unrealized PnL, fees, fills, orders, turnover, drawdown,
    and position;
  - the equity chart;
  - trades by account, and decisions with their counts, both paged;
  - configuration, assumptions, definitions, window, and provenance.
- **Unavailable values.** An unavailable value is shown as "Unavailable"
  with its reason, never as zero.
- **States.** Loading, an unreachable service (with how to start it),
  missing tools, an empty catalog, failed and interrupted jobs, and corrupt
  artifacts each have their own message.

### The equity chart's boundary

The chart draws the three equity curves over market time.

- **Approximate geometry only.** Times and equity strings are converted to
  floating point only to place lines on the screen. Every value a reader
  sees comes from the server's exact strings: the axis labels, the readout,
  the tooltip text (`aria-valuetext`), and the table.
- **Gaps.** An unavailable valuation breaks a line rather than inventing a
  point.
- **Telling lines apart.** The lines differ by dash pattern as well as
  colour.
- **Keyboard.** The chart is a slider: arrow keys, Home, End, PageUp, and
  PageDown move between valuation points, and the readout gives the exact
  values.
- **Table.** "Equity at every valuation point (table)" is the full
  accessible alternative.

## Tests

- **Python.** `python/tests/test_service.py` runs the service on an
  ephemeral port over artifacts the real tools built from synthetic events,
  with real child jobs. It covers:
  - the catalog;
  - a full job, deduplication, and every paged resource;
  - invalid requests, bodies, queries, methods, and hosts;
  - unevaluable models;
  - symlinked and corrupt artifacts;
  - restart recovery, failed jobs, timed-out jobs, and a full queue.
- **Web unit tests.** `apps/web` covers the client contract, routing,
  formatting, the components (including the chart's keyboard and readout),
  and keeping the account across views.
- **Browser, mocked.** `e2e/research.spec.ts` runs the whole flow on
  desktop and mobile: axe, overflow, reload, keyboard operation, and the
  failure states.
- **Browser, real service.** `e2e/research-live.spec.ts` is an opt-in run
  against a real service:

  ```sh
  CHRONOS_WEB_LIVE_RESEARCH=1 CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:8090 \
    CHRONOS_API_ORIGIN=http://127.0.0.1:8099 npx playwright test research-live.spec.ts
  ```

  (`8099` keeps it away from any running trading server.)

## Limitations

- **Loopback only.** There is no authentication; the service is for the
  machine it runs on.
- **No cancellation.** A job runs to completion or its time limit.
- **No retraining.** The service evaluates prebuilt experiments. Building
  datasets, features, labels, splits, and experiments is still the `demo`
  and `evaluate` commands' work.
- **Simple listings.** Results are listed in ID order, not by recency; the
  runs list gives recency.
