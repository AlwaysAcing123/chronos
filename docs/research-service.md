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

The same service also keeps the **model library**: immutable model versions
registered from verified experiments or imported as constrained ONNX
classifiers, and **comparisons** of several versions under one frozen
specification. Their decisions are in
[ADR 0021](decisions/0021-model-registry-imports-and-comparisons.md).

It also runs **walk-forward evaluation**: collections of independent
recordings, frozen plans, and studies that fit fresh models per fold and run
them through the engine under fee and latency scenarios
([walk-forward-evaluation.md](walk-forward-evaluation.md),
[ADR 0022](decisions/0022-walk-forward-evaluation.md)), and **strategy
diagnostics** that explain those studies' runs exactly without running
anything again ([strategy-diagnostics.md](strategy-diagnostics.md),
[ADR 0024](decisions/0024-strategy-diagnostics.md)).

## Running it

```sh
cd python
.venv/bin/python -m chronos_research serve --tools ../build --artifacts ../data --state ../data/research \
  [--host 127.0.0.1] [--port 8090] [--max-running 1] [--max-queued 8] [--job-timeout-s 1800] [--retain-jobs 200] \
  [--docker docker] [--docker-host <DOCKER_HOST>]
cd ../apps/web
npm run dev        # proxies /api/v1/research to CHRONOS_RESEARCH_ORIGIN, default http://127.0.0.1:8090
```

- **Address.** It listens on a loopback address only; any other host is
  refused at startup. `data/research/`, the job records, is ignored by Git.
- **Dependencies.** Only the standard library and the package's own code:
  no web framework, uploads, pickle, or joblib.
- **Proxy.** The Vite proxy lists `/api/v1/research` before `/api`, so the
  trading API keeps every other path.
- **The ONNX sandbox.** Imports run only in a Docker container built from
  this source (`python -m chronos_research onnx-sandbox build`; see
  [tools/onnx-sandbox/README.md](../tools/onnx-sandbox/README.md)).
  `--docker` names the Docker CLI and `--docker-host` an optional
  `DOCKER_HOST`. Without Docker or the image, everything else works and
  imports are refused with the reason.

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
| `GET /api/v1/research/models?state=active\|archived\|all` | Model versions (default `active`); versions that do not verify are under `invalid_entries`, on the first page only |
| `POST /api/v1/research/models` | Register a fitted experiment model as a version (a `registration` job) |
| `GET /api/v1/research/models/{model_version_id}` | One version, with its compatibility, limitations, and a check of its source experiment |
| `POST /api/v1/research/models/{model_version_id}/lifecycle` | Archive or restore it: `{"state", "reason"}` |
| `POST /api/v1/research/imports` | Stage an ONNX import's `chronos.model_import/1` manifest |
| `GET /api/v1/research/imports/{import_id}` | The staged import and its latest `import` job |
| `PUT /api/v1/research/imports/{import_id}/model` | Its model bytes, `application/octet-stream`, at most 1 MiB |
| `POST /api/v1/research/imports/{import_id}/validation` | Validate the reviewed bytes in the sandbox and import them (an `import` job) |
| `GET /api/v1/research/comparisons` | Published comparisons; ones that do not verify are under `invalid_entries` |
| `POST /api/v1/research/comparisons` | Start a comparison (a `comparison` job) |
| `GET /api/v1/research/comparisons/{comparison_id}` | One comparison, with each member's schedule and run verified again |
| `GET /api/v1/research/collections` | Verified collections of recordings; ones that do not verify are under `invalid_entries` |
| `GET /api/v1/research/collections/{collection_id}` | One collection: members, coverage, evidence, declared sessions, instrument counts |
| `POST /api/v1/research/walk-forward/plans` | Preview: build and publish the plan of a request (a `walk_forward_plan` job) |
| `GET /api/v1/research/walk-forward/plans` | Published plans |
| `GET /api/v1/research/walk-forward/plans/{plan_id}` | One plan |
| `POST /api/v1/research/walk-forward/studies` | Start a reviewed plan's study: `{"plan_id"}` (a `walk_forward` job) |
| `GET /api/v1/research/walk-forward/studies` | Published studies |
| `GET /api/v1/research/walk-forward/studies/{study_id}` | One study, with each fold model, schedule, and run verified again |
| `POST /api/v1/research/strategy-diagnostics` | Explain studies' runs: `{"study_ids"}` (a `strategy_diagnostics` job) |
| `GET /api/v1/research/strategy-diagnostics` | Verified diagnostics, optionally `?study=` |
| `GET /api/v1/research/strategy-diagnostics/{diagnostic_id}` | One artifact, with each verification level reported apart, every source checked again, and each figure's trust |
| `GET /api/v1/research/strategy-diagnostics/{diagnostic_id}/fills` and `/episodes` | One run's rows: `?run` required, `account`, `limit`, `cursor` |
| `GET /api/v1/research/jobs?kind=evaluation\|registration\|import\|comparison\|walk_forward_plan\|walk_forward\|strategy_diagnostics` | Every job, newest first, optionally of one kind |
| `GET /api/v1/research/jobs/{job_id}` | One job of any kind |

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
  `unexpected_field`, `invalid_type`, or `invalid_decimal`. The one other
  body is an import's model on `PUT /imports/{id}/model`:
  `application/octet-stream` with no parameters, refused with `413` on its
  announced `Content-Length` above 1 MiB before any byte is read, and
  `400 incomplete_body` if fewer bytes arrive. A request that stops sending
  is dropped after 30 seconds.
- **Decimals and integers.** Both are strings. Decimals follow the trading
  API's grammar and scale; trailing zeros are accepted and canonicalized.
- **Domain errors.** A well-formed value outside its domain is
  `422 invalid_configuration` with the field and reason `out_of_domain`.
- **Queries.** Query strings are strict: only `limit` (1 to 200, default
  50), `cursor`, and, for trades, `account`, for models, `state`, and for
  jobs, `kind`. Cursors are opaque and bound to
  their list and run. Invalid percent encoding or UTF-8 is a
  `400 malformed_request` with reason `invalid_query`, never a server error.
- **Paging growing lists.** Jobs (newest first), experiments, results,
  model versions, and comparisons (in ID order) can grow while a client
  pages them, so their cursors
  are keysets: a cursor names the last entry returned, and the next page
  continues strictly after it.
  - Following a chain never repeats an entry or skips one that existed
    throughout it.
  - An entry added behind the cursor appears when the list is read again
    from the first page.
  - These cursors are version `v2`. A `v1` offset cursor for these lists is
    refused with `invalid_cursor`; restart the chain.
  - One result's decisions, trades, and equity points never change, so they
    keep offset cursors.
- **Responses.** Timestamps are RFC 3339 with nine fractional digits; a
  `*_ns` artifact field becomes the field without the suffix. Durations
  stay integer nanoseconds.
- **Errors.** `{"error": {"code", "message", "field", "reason"}}`. The codes
  include:
  - `route_not_found` and `method_not_allowed` (with `Allow`);
  - `experiment_not_found`, `run_not_found`, `result_not_found`,
    `job_not_found`, `model_not_found`, `import_not_found`,
    `comparison_not_found`, and `split_not_found`;
  - `unknown_instrument` and `unevaluable_model` (with the model's reason);
  - `artifact_invalid` (an artifact whose bytes do not verify);
  - for imports, `invalid_manifest` and `incompatible_contract` (422),
    `model_already_uploaded`, `model_not_uploaded`, and `model_mismatch`
    (409), and `sandbox_unavailable` (503, with the sandbox's `reason`);
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
- **Kinds.** `evaluation` (`evaluate-strategy`), `registration`
  (`register-model`), `import` (`import-model --staged`), `comparison`
  (`compare-models --spec`), `walk_forward_plan` (`plan-walk-forward
  --request`), `walk_forward` (`run-walk-forward --plan --progress`), and
  `strategy_diagnostics` (`diagnose-strategies --study … --progress`),
  each one fixed child command. `GET /runs` lists evaluations only, as
  before; `GET /jobs` lists every kind.
- **Progress.** A `walk_forward` job's child writes `{"progress": {...}}`
  lines to stderr. The worker reads complete lines as they arrive: a line
  that is a progress report (at most six known keys, each a short lowercase
  token) replaces the job's `progress` and is saved; every other line stays
  the bounded log, so an error still parses. Only walk-forward and
  diagnostics jobs show `progress`, and an interrupted one keeps its last
  report.
- **Sandbox containers.** An import or comparison job's child may start
  ONNX sandbox containers.
  - Each such job gets a supervisor token, recorded in its job record
    before the child starts and passed as `--sandbox-supervisor`.
  - When the child ends, however it ends (including timeout, an output
    flood, or shutdown), the service removes every container under the
    token and checks that Docker lists none. Only then is the job's slot
    free.
  - If Docker cannot be reached at that moment, the record stays
    unreleased. It is reclaimed at the next start and before every later
    sandbox job. If Docker answers but such a container stays, a new
    sandbox job fails with `sandbox_cleanup_pending`.
  - Unreleased records are never pruned. See ADR 0021.
- **States.** `queued`, `running`, `succeeded`, `failed` (with a code such
  as `evaluation_failed` or `timed_out`), and `interrupted`. A refused
  registration, import, or comparison fails with the refusal's code and,
  where the child reported one, its `reason`:
  - `name_version_taken` and `registration_refused`;
  - `import_refused` (the manifest, the profile, the smoke test, or the
    staged bytes, with a reason such as `unsupported_operator`,
    `external_data`, `graph_too_large`, `malformed_model`, or
    `non_finite_output`), `sandbox_unavailable`, and `sandbox_failed` (a
    timeout, an exit, or a reply outside the protocol);
  - `comparison_refused`;
  - `plan_refused` (such as `insufficient_coverage`, `invalid_request`, or
    `workload_exceeded`), `collection_refused`, and `study_refused` (such
    as `source_invalid` when a source changed after review);
  - otherwise `registration_failed`, `import_failed`,
    `comparison_failed`, `plan_failed`, or `study_failed`.
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
- **Results.** `GET /results` lists version 1 strategy runs, the runs of
  experiment models. A version 2 run, of a registered model version, is
  listed by the comparison it belongs to; `GET /results/{run_id}` and its
  decisions, trades, and equity read either version.

## The model library

- **Registration.** `POST /models` with `{"experiment_id", "instrument",
  "model", "name", "version"}` checks that the experiment verifies and the
  model is fitted, then queues a `registration` job. The child verifies the
  sources again, with reproduction, and publishes the version and binds its
  name. A name and version bound to another version fails the job with
  `name_version_taken`, and nothing is published.
- **Listing and detail.** A listing entry gives the family, instrument,
  origin (`experiment` or `import`), names, lifecycle, payload, and the
  information cutoff with its basis: `verified`, `declared`, or
  `unavailable` with a reason. The detail adds:
  - the inputs, outputs, target, runtime, and provenance;
  - what Chronos verified and, for an import, what it did not;
  - compatibility with evaluation and with paper deployment, each
    `compatible` with its requirement or `unavailable` with a reason (an
    import's evaluation needs the sandbox; a deployment needs an active
    version with a known cutoff);
  - the limitations;
  - for an experiment model, whether its source experiment still holds
    exactly this payload (`matches`, `missing`, `invalid`, or `differs`);
    an import has none (`not_applicable`).
- **Lifecycle.** `POST /models/{id}/lifecycle` with `{"state": "archived" |
  "active", "reason"}`. Archiving changes only what new comparisons and
  deployments may choose.
- **Status.** `GET /status` also reports `imports`: the
  `chronos.onnx_classifier/1` profile's limits, and the sandbox's
  capability (`available` with its image, or `unavailable` with a reason
  and detail).

## Imports

An import is staged in two requests and validated as a job, so the bytes
reviewed are the bytes validated.

1. `POST /imports` with the `chronos.model_import/1` manifest checks it at
   once (`422 invalid_manifest` or `incompatible_contract`) and returns
   `201` with the staged import.
2. `PUT /imports/{id}/model` stores the bytes and their SHA-256 (`201`; the
   same bytes again are `200`, other bytes `409 model_already_uploaded`).
3. `POST /imports/{id}/validation` with `{"model_sha256"}` must name the
   uploaded digest (`409 model_mismatch`, or `model_not_uploaded`). It is
   refused with `503 sandbox_unavailable` unless the sandbox is available,
   and otherwise queues an `import` job, deduplicated by manifest and
   model digests.

The job checks the profile and runs the smoke corpus in the sandbox, then
publishes exactly those bytes. Staged imports live in `--state/imports/`
and survive a restart; at most 64 are kept, the oldest pruned unless a job
still needs them.

## Comparisons

`POST /comparisons` takes `{"model_version_ids", "split_artifact_id",
"instrument", "window", "configuration"}`:
- 2 to 6 distinct versions, else `422 invalid_configuration`;
- versions that exist, else `422 model_not_found`;
- a split that exists, else `422 split_not_found`;
- a configuration checked as an evaluation's.

The service adds the spec's schema, `chronos.model_comparison_spec/1`, and
queues a `comparison` job deduplicated by the spec's digest. The job
predicts, runs, and replays each member in its own accounts, keeps refused
and failed members with their reasons, and publishes one immutable
`chronos.model_comparison/1` artifact.

Only after every member is predicted does the job read the window's
realized labels. It scores each completed member's stored predictions with
the experiment's metric definitions (`predictive`):
- accuracy, balanced accuracy, log loss, Brier score, and per-class
  precision, recall, and F1;
- the rows scored, and the rows not scored by the split's reason.

A member that cannot be scored keeps its trading results and says why. The
detail route re-verifies each
member's schedule and run (`verified`, or `invalid:` with why); the CLI's
`inspect-comparison` also replays every run. See
[strategy-evaluation.md](strategy-evaluation.md#registered-model-versions)
and ADR 0021.

## Walk-forward evaluation

`POST /walk-forward/plans` takes `{"collection_id", "instrument",
"minimum_training_sources", "configuration", "scenarios": [{"name",
"fee_per_fill", "latency_ns"}]}`:
- every field checked as an evaluation's: decimals and integers as
  strings, canonicalized before deduplication;
- a collection that exists, else `422 collection_not_found`;
- the plan's rules (a base scenario with the configuration's own fee and
  latency, a named stress scenario no cheaper and no faster, at most four
  cells, values the engine holds exactly), else `422 invalid_configuration`
  with the rule's reason.

The service adds the request's schema, `chronos.walk_forward_request/1`, and
queues a `walk_forward_plan` job: verifying the members runs the C++ tools,
so even Preview never runs on a request thread. The job publishes the plan
of exactly this request, content-addressed, or fails with why (too few
recordings is `plan_refused` / `insufficient_coverage`, with the counts).

`POST /walk-forward/studies` takes only `{"plan_id"}`, so a study is always
the reviewed plan; an unknown plan is `422 plan_not_found`. Its
`walk_forward` job verifies every source again and rebuilds the plan before
anything runs, then runs every fold, model, and scenario, and publishes one
`chronos.walk_forward_study/1`, complete or partial. A study's fold runs
(`chronos.strategy_run/3`) open through `/results/{run_id}` and its
decisions, trades, and equity like any run, and are listed by their study
rather than under `/results`. See
[walk-forward-evaluation.md](walk-forward-evaluation.md#http-contract).

## The Research view

- **Navigation.** The header has four views: Trading, Models,
  Comparisons, and Research. Research lives under `#/research` and
  `#/research/results/<run-id>`, so a reload or a link reopens the same
  view; Models and Comparisons are described in
  [web-client.md](web-client.md#models-and-comparisons). The workspace's section links
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

  `test_model_routes.py`, `test_import_routes.py`, and
  `test_comparison_routes.py` cover the library, imports (with the real
  sandbox when it is available), and comparisons the same way: the request
  conventions, jobs and their refusals, persistence across a restart, and
  corrupt artifacts. `test_walk_forward_routes.py` covers collections,
  Preview and Start jobs, deduplication, progress apart from the log, a
  source changed after review, fold runs as results, and keyset paging. `test_registry.py`, `test_model_import.py`,
  `test_onnx_profile.py`, `test_onnx_sandbox.py`, `test_comparison.py`, and
  `test_registered_worker.py` cover the modules beneath them. The ONNX
  tests build their models at test time with the optional `onnx` package
  and are skipped without it or without the sandbox, unless
  `CHRONOS_REQUIRE_ONNX_SANDBOX=1`.
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

## The inference worker

`python -m chronos_research inference-worker --artifacts <dir> --experiment
<id> --instrument <symbol> --model logistic|majority` serves one verified
model to a model deployment, which the paper-trading server runs
([model-deployments.md](model-deployments.md)). It is not part of this
service. With `--model-version <id>` (and `--docker <path>` for an import)
instead of `--experiment` and `--model`, it serves a registered model
version over `chronos.inference_worker/2`; see
[model-deployments.md](model-deployments.md#registered-model-versions).

- **Protocol.** It speaks `chronos.inference_worker/1`, one JSON object
  per line. A ready line gives the model's identity: the experiment, the
  SHA-256 of the verified parameters, the feature set, and the training
  cutoff, the experiment's validation start.
- **Requests.** Each request is a row's five exact model inputs, and each
  reply is the same row-pure prediction a historical inference schedule
  makes. The tests prove the worker's predictions and parameter checksum
  bit-identical to the schedules', on synthetic data and on the local
  recording's experiment.
- **Verification.** The experiment is verified once, at start; a model that
  does not verify, or is unevaluable, fails closed before the ready line.
- **Malformed requests.** A malformed request gets an error line and the
  worker continues. A line over 8 KiB ends it.
- **Describe.** `--describe` prints the ready line and exits; the server
  uses it to verify a model before creating a deployment.

## Limitations

- **Loopback only.** There is no authentication; the service is for the
  machine it runs on.
- **No cancellation.** A job runs to completion or its time limit.
- **No retraining.** The service evaluates prebuilt experiments and
  registered or imported versions as they are. Building datasets, features,
  labels, splits, and experiments is still the `demo` and `evaluate`
  commands' work.
- **Imports need Docker.** Without Docker and the sandbox image an import
  cannot be validated, evaluated, or deployed, and an import's declared
  cutoff cannot be verified at all.
- **Simple listings.** Results are listed in ID order, not by recency; the
  runs list gives recency. The Research view loads one page of each list
  and an older page only when asked ("Load more"); a refresh re-reads the
  loaded pages from the first.
