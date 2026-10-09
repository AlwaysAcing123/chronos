# Walk-forward evaluation

A walk-forward study fits fresh models on earlier, independent recordings and
measures them on each later one, through the same exact C++ trading engine as
every other evaluation, once per fee and latency scenario. Its decisions are in
[ADR 0022](decisions/0022-walk-forward-evaluation.md).

The chain is four immutable, content-addressed artifacts:

| Artifact | Schema | Root under the artifacts directory |
| --- | --- | --- |
| Collection of recordings | `chronos.research_collection/1` | `collections/<collection_id>/manifest.json` |
| Plan | `chronos.walk_forward_plan/1` | `walk_forward_plans/<plan_id>/manifest.json` |
| Fold models | `chronos.walk_forward_fold_model/1` | `walk_forward_models/<fold_model_id>/{manifest,model}.json` |
| Study | `chronos.walk_forward_study/1` | `walk_forward_studies/<study_id>/manifest.json` |

Each fold model's predictions are a `chronos.inference_schedule/3` in
`schedules/`, and each run of them a `chronos.strategy_run/3` in `strategies/`
([strategy-evaluation.md](strategy-evaluation.md#walk-forward-fold-models)).

**What it is not.** One real recording exists: ten minutes of one session. A
study of it is correctly refused (`insufficient_coverage`), and Chronos never
splits, clones, shifts, or renames a recording to make more. Every study shown
here is of synthetic recordings, which exercise the machinery and say nothing
about a market. A few recordings cannot establish skill: there is no
significance test, annualized ratio, combined equity curve, winner,
promotion, or deployment of fold models.

## Walkthrough (synthetic, isolated)

Everything below uses a fresh artifacts root of its own, ignored by Git, and
never the development databases, accounts, or other artifact roots.

```sh
# 1. The C++ verifier, synthetic-dataset tool, and strategy runner.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel --target chronos_dataset chronos_synthetic_dataset chronos_strategy_cli
cd python
ROOT=../data/walk-forward-demo
mkdir -p "$ROOT"

# 2. Three independent synthetic trading days (the tests' generator): each its
#    own dataset and stream, with features and 1 ms labels. Prints their label
#    artifact IDs. Synthetic only: nothing is downloaded.
PYTHONPATH=. .venv/bin/python - "$ROOT" <<'PY'
import sys
from pathlib import Path
from chronos_research.pipeline import Tools
from tests.recordings import WEEK, Recordings, prepare
recordings = Recordings(artifacts=Path(sys.argv[1]), tools=Tools(Path("../build")))
for recording in WEEK:
    print(recording.stream, prepare(recordings, recording).label_id)
PY

# 3. Collect them: every member re-verified by the C++ tools. A session label is
#    recorded as a declaration.
.venv/bin/python -m chronos_research create-collection --tools ../build --artifacts "$ROOT" \
  --label-artifact <day-1-labels> --label-artifact <day-2-labels> --label-artifact <day-3-labels> \
  [--session-label <labels-id>=<declared text>]

# 4. Preview the plan without publishing it, then publish it.
REQUEST='{"schema": {"name": "chronos.walk_forward_request", "version": "1"},
  "collection_id": "<collection>", "instrument": "AAPL", "minimum_training_sources": "1",
  "configuration": {"decision_interval_ns": "1000000", "fee_per_fill": "0.01", "latency_ns": "1000",
    "maximum_order_notional": "1000000", "maximum_quote_age_ns": "5000000000", "starting_cash": "10000",
    "target_quantity": "1", "threshold_ppb": "500000000"},
  "scenarios": [{"name": "base", "fee_per_fill": "0.01", "latency_ns": "1000"},
                {"name": "stress", "fee_per_fill": "0.05", "latency_ns": "200000"}]}'
.venv/bin/python -m chronos_research plan-walk-forward --tools ../build --artifacts "$ROOT" --request "$REQUEST" --preview
.venv/bin/python -m chronos_research plan-walk-forward --tools ../build --artifacts "$ROOT" --request "$REQUEST"

# 5. Run the study (every fold, model, and scenario through chronos_strategy),
#    then reproduce it: sources verified again, fold models and schedules rebuilt
#    to the published bytes, every run replayed, every score and total recomputed.
.venv/bin/python -m chronos_research run-walk-forward --tools ../build --artifacts "$ROOT" --plan <plan> --progress
.venv/bin/python -m chronos_research inspect-walk-forward-study --tools ../build --artifacts "$ROOT" --study <study>

# 6. Audit it independently: the standard library alone, nothing from Chronos.
python3.12 -I -S ../tools/walk_forward_audit/audit.py "$ROOT" <study>
```

`list-collections`, `inspect-collection`, `list-walk-forward-plans`,
`inspect-walk-forward-plan` (rebuilds the plan from its request over sources
verified again), and `list-walk-forward-studies` complete the CLI. Success
prints one JSON document; a refusal prints `{"error": {"kind", "message",
"reason"}}` on stderr and exits 1.

### In the browser

```sh
cd python
.venv/bin/python -m chronos_research serve --tools ../build --artifacts ../data/walk-forward-demo \
  --state ../data/walk-forward-demo/research --port 18190
cd ../apps/web
CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:18190 npm run dev
```

Open **Research → Walk-forward evaluation** (`#/research/walk-forward`):

1. **Recording collections** lists every verified collection, its evidence
   (synthetic or observed provider data), and each member's stream, declared
   session label, receive time, partial coverage, and rows per instrument.
2. **Plan a walk-forward study** chooses the collection, instrument, how many
   recordings the earliest fold trains on, the configuration, and the
   scenarios: the base (the configuration's own fee and latency), an optional
   labelled stress scenario, and an optional, explicitly selected zero-fee
   reference. With too few recordings, Preview is unavailable and the page
   says how many there are and how many are needed.
3. **Preview plan** sends exactly the request shown; a job verifies every
   member and publishes its plan. **Review the plan** opens
   `#/research/walk-forward/plans/<plan_id>`: every fold's training
   recordings, eligible and excluded rows, class support, latest training
   label, information boundary, and evaluation recording; the frozen
   scenarios, configuration, rules, and limits. A reload shows the same plan.
4. **Start study** sends only the plan's ID. The job verifies every source
   again before anything runs and reports its stage and fold until it ends;
   **Open the study** opens `#/research/walk-forward/studies/<study_id>`.
5. The study shows, in order: alerts for synthetic and partial results;
   **Trading results by fold**, one row per engine run with its exact PnL,
   return, fees, turnover, fills, orders, drawdown, differences from its own
   benchmarks, and a link to the run's journal; **Totals over reset
   accounts**; **Prediction quality**, per fold and pooled, apart from
   trading: rows predicted, scored, and not scored by reason, class support,
   predicted classes, confusion counts, per-class recall, accuracy, balanced
   accuracy, log loss, and Brier score; the fold models and whether every artifact still verifies; and
   how to reproduce it. On a phone every figure is labelled with its fold,
   model, and scenario. Each run, schedule, and fold model shows the service's
   check, read again on every load; if anything does not verify or was not
   checked, the page says so first and labels every figure and total that
   rests on it unverified.

## Definitions

- **Collection.** References existing label artifacts, their feature
  artifacts, and their datasets by ID and checksum; rows keep their identity
  (stream, instrument, ingest sequence) and are never renumbered or merged.
  Members are ordered by first receive time and must be strictly disjoint.
  Refused: duplicate members, datasets, or streams; aliased provider
  recordings; different horizons, feature or label sets, or scales;
  overlaps; synthetic/observed mixtures; more than 12 members or 2,000,000
  feature rows in one. Every member is `partial` (no exchange calendar), and
  missing provenance is recorded as unavailable.
- **Folds.** The members holding the instrument form a timeline. Fold *k*
  evaluates timeline member *m + k* (for *m* = the minimum training
  recordings) and trains on every earlier member: expanding, predeclared
  chronological retraining. A later fold learns from earlier evaluation
  recordings once their labels are history.
- **Window.** The whole evaluation recording, `[first receive, last receive
  + 1)`.
- **Information boundary.** The window's start, exclusive. A training row
  needs available inputs, a usable label, and both its prediction time and
  its label's availability time strictly before the boundary; every other row
  is counted by member and reason.
- **Fold models.** A fresh majority model and a fresh logistic model, scaler
  included, fitted on that fold's rows alone with the experiment's fitting,
  parity check, and convergence rules. No training rows, too little class
  support, non-convergence, or a fit warning makes a model unevaluable with
  its reason; nothing takes its place. Fold models are historical only:
  outside the model registry and never deployable.
- **Schedules.** Every feature row of the instrument in the window is
  predicted from verified features and frozen parameters, labelled or not.
  The builder has no label argument.
- **Scoring.** Afterwards, the evaluation labels are read again and joined by
  identity. A row is scored when its inputs were available, its label is
  usable, and the label was known before the window's end; every other row
  is counted by reason (`feature_unavailable:<reason>`,
  `label_unavailable:<reason>`, `label_after_scoring_cutoff`). The metrics
  are the experiment's: accuracy, balanced accuracy, log loss, Brier score,
  per-class precision, recall, and F1, confusion counts, and class support.
- **Scenarios.** One to four fee-per-fill × positive-latency cells, frozen in
  the plan: `base` is the configuration's own fee and latency; with more than
  one, `stress` is no cheaper and no faster on either axis and costlier or
  slower on at least one; a zero fee appears only where a scenario names it.
  Values are checked against what the engine holds exactly, by the same rules
  for the CLI and the service (money and quantities at 8 decimal places in a
  signed 64-bit integer, durations from 1 to 2^63 − 1, latency below 10^18, the
  threshold from 0 to 1,000,000,000 ppb), and must be written canonically
  (the service canonicalizes its requests first); the folds' total starting
  capital must fit the engine's money too (`workload_exceeded`). Spreads and recorded prices never
  change; the engine already pays the spread by filling at the quote.
- **Runs.** Every fold × fitted model × scenario is its own
  `chronos_strategy` run in new strategy, cash, and buy-and-hold accounts with
  the configured starting cash, replayed by `chronos_strategy inspect`. A run
  counts for its cell only if it is of that cell's schedule, with the
  schedule's source and window and the evaluation recording's events, under
  all eight frozen options with the scenario's fee and latency; otherwise the
  cell fails `run_mismatch`. Each
  completed cell records its strategy's exact differences from its own
  benchmarks (PnL, and that as a percentage of its starting cash, rounded
  once).
- **Outcomes.** Every planned fold, model, and cell is kept: `completed`,
  `unevaluable`, `failed`, or `unavailable` (not run, because its model or
  fold has no schedule), with its reason. Anything not completed makes the
  study `partial` and is listed in `missing`. A source or plan that no longer
  verifies stops the study before anything runs (`source_invalid`), and
  nothing is published. Inspection re-derives every cell; the one exception,
  a run that never started, is accepted only as exactly `{detail,
  fee_per_fill, latency_ns, reason: run_failed, scenario, state: failed}`
  under its own scenario, and anything else without a run is refused
  (`artifact_invalid`).
- **Classification totals.** Pooled over the scored rows of every included
  fold, so every row weighs the same; fold accuracies are never averaged.
- **Money totals.** Summed over independent reset accounts in the engine's
  fixed point (10^-8, 64-bit, checked): PnL, fees, turnover, fills, orders,
  and the starting capital given to each fold's account. The return is total
  PnL over total capital, rounded once, half away from zero, to 8 places. A
  fold whose cell did not complete or lacks a final valuation makes the
  complete total unavailable, with what is missing; a partial total then
  names its included and planned folds and is never the complete result.

## Workload limits

Checked before anything runs: at most 12 sources, 10 folds, 4 scenarios, 80
strategy runs (10 folds × 2 models × 4 scenarios), and 4,000,000 eligible
training rows per fold. The plan records its planned counts. The service's
job timeout (`--job-timeout-s`, default 1800 s) bounds a study's wall time.

## HTTP contract

All under `/api/v1/research`, with the service's conventions
([research-service.md](research-service.md#walk-forward-evaluation)).

| Route | Purpose |
| --- | --- |
| `GET /collections?limit&cursor` | Verified collections: members, coverage, evidence, instrument counts |
| `GET /collections/{collection_id}` | One collection |
| `POST /walk-forward/plans` | Preview: queue a `walk_forward_plan` job for a request |
| `GET /walk-forward/plans?limit&cursor` | Published plans |
| `GET /walk-forward/plans/{plan_id}` | One plan |
| `POST /walk-forward/studies` | Start: queue a `walk_forward` job for `{"plan_id"}` |
| `GET /walk-forward/studies?limit&cursor` | Published studies |
| `GET /walk-forward/studies/{study_id}` | One study, and whether each fold model, schedule, and run still verifies |
| `GET /jobs?kind=walk_forward` | Study jobs, with their latest `progress` |
| `GET /results/{run_id}` (and its decisions, trades, equity) | A fold run, like any result; fold runs are listed by their study, not among results |

A preview body is `{"collection_id", "instrument", "minimum_training_sources",
"configuration", "scenarios": [{"name", "fee_per_fill", "latency_ns"}]}`:
strict fields, decimals and integers as strings, canonicalized before
deduplication. An unknown collection or plan is `422` before any job; a plan
rule (base, stress, bounds) is `422 invalid_configuration` with its reason;
too few recordings fails the plan job with `plan_refused` /
`insufficient_coverage` and the counts.

A study job's `progress` is its child's latest report: `stage`
(`verify_sources`, `fold`, `aggregate`, `publish`) and, within a fold,
`fold`, `folds`, `step` (`fit`, `schedule`, `run`, `score`), `model`, and
`scenario`. An interrupted job keeps its last report and never resumes.

## Tests and evidence

```sh
cd python
CHRONOS_REQUIRE_TOOLS=1 CHRONOS_DATASET=../build/chronos_dataset \
  CHRONOS_SYNTHETIC_DATASET=../build/chronos_synthetic_dataset \
  .venv/bin/python -m pytest tests/test_collection.py tests/test_walk_forward_plan.py \
    tests/test_fold_models.py tests/test_walk_forward_study.py tests/test_walk_forward_routes.py \
    tests/test_walk_forward_end_to_end.py
cd ../apps/web
npm test && npx playwright test walkForward.spec.ts
# Against a real, isolated service (see "In the browser"):
CHRONOS_WEB_LIVE_RESEARCH=1 CHRONOS_RESEARCH_ORIGIN=http://127.0.0.1:18190 \
  npx playwright test walkForward-live.spec.ts --workers=1
```

- **Synthetic recordings** (`python/tests/recordings.py`): independent days
  with their own streams, overlapping ingest sequences, different sizes and
  regimes, irregular gaps, long pauses, and timestamp ties.
- **Leakage.** Flipping a fold's evaluation labels after verification leaves
  its fit, schedule bytes, and engine results identical and changes only its
  scores; the next fold's fit legitimately changes. A changed training label
  changes its fold's fit; a later recording changes no earlier fold's numbers,
  only identities.
- **End to end in CI.** `test_walk_forward_end_to_end.py` runs the CLI path
  and the independent auditor (`python -I -S tools/walk_forward_audit/audit.py`),
  which derives every expectation from the plan, not from the runs: each
  run's whole configuration, source, and window; every planned cell exactly
  once; and every strategy decision re-derived from the plan's threshold,
  target, and decision interval. It recomputes membership, cutoffs, training
  support, every schedule row, confusion counts, every decision's signal,
  every order's single
  resolution, each execution on the first quote received at or after its
  arrival (and each expiry only with none left in the window), every fill,
  fee, cash, position, mark, PnL, and every total from the raw files, and
  must catch a planted error. A scenario whose latency outlasts the
  recordings shows every order expiring at the window's end.
- **Rows the dataset does not give.** A fold schedule rewritten with
  consistent checksums but a changed input, time, identity, order, or a
  missing row is refused by the C++ runner; the same rewrite unchanged runs.

## Limitations

- One instrument per study; the `top_of_book_baseline` inputs, the horizon,
  the two baselines, and the long/flat policy as they are.
- Whole-recording windows only; no rolling windows, hyperparameter search,
  random cross-validation, or calibration.
- Reproducibility is same-environment: fitted parameters depend on the
  numerical environment, which every artifact records.
- Inspection does not execute again a cell that failed before any run
  existed; it accepts that recorded failure only in its exact shape.
- Planning reads every member's labels (a later fold trains on an earlier
  evaluation recording); that a fold's own evaluation labels cannot reach its
  fit, predictions, or execution is shown by construction and by tests.
- Real multi-session evidence does not exist yet. More recordings need a
  separate, quote-first, cost-capped acquisition step
  ([historical-data-acquisition.md](historical-data-acquisition.md)).
