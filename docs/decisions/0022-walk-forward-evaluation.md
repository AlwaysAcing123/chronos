# ADR 0022: Walk-forward evaluation over independent recordings

- Status: Proposed
- Date: 2026-10-08

## Context

Every evaluation so far comes from one recording and its 60/20/20 split
(ADRs 0013–0016): a model is fitted on the first 60 % of one session and
measured on the last 20 % of the same session. A credible evaluation needs
models fitted on earlier recordings and measured on later, independent ones,
again and again as history grows, through the same exact trading engine.

Constraints that shape the design:

- **Existing meanings stay.** `VerifiedSources` is one dataset and its
  split. Schedule versions 1 and 2 accept only that split's validation and
  test windows. Strategy-run versions 1 and 2 name an experiment model or a
  registered version. Registry version 1 knows only experiment and import
  origins. Comparisons evaluate frozen models, and deployments serve
  registered versions. None of these changes.
- **One real recording.** The only real data on hand is ten minutes of one
  session. It proves the pipeline, not generalization, and it must never be
  cut, cloned, or relabelled into pretend sessions.
- **The engine is the C++ runner.** Every financial figure is its exact
  arithmetic, and it recomputes every scheduled row from its dataset.

## Decision

### Collections: `chronos.research_collection/1`

`<artifacts>/collections/<collection_id>/manifest.json`.

- **What it holds.** It references member recordings by their existing
  label artifact, that artifact's feature artifact, and its dataset, with
  IDs and checksums. Nothing is concatenated, renumbered, or rewritten, and
  rows keep their identity (stream, instrument, ingest sequence). Equal
  sequence numbers in two recordings therefore never meet, and no row is
  merged because its sequence matches another.
- **Verification.** Every member is verified by the C++ tools:
  - the labels, reproduced from their features;
  - the features, reproduced from their dataset;
  - the dataset, every event of it.

  The ancestry is then cross-checked through the label/feature verification
  that `verify_sources` uses, factored out unchanged. Counts and coverage
  come from rows read while hashing, and are used only if they are the
  verified bytes.
- **Provenance is recorded, never upgraded.**
  - The evidence is `synthetic` (the test generator) or `observed` (a
    provider recording through a known converter), and one collection holds
    one kind.
  - A session or date label is the creator's declaration.
  - With no exchange calendar, every member is `partial`, with its actual
    duration.
  - Missing provenance, such as a null provider coverage window, is
    recorded as unavailable.
- **Refusals:** a duplicate member, dataset, or stream; an aliased provider
  recording; a different horizon or scale; overlapping members (strictly
  disjoint, so a tie at a boundary is refused rather than divided); a
  synthetic/observed mixture; and bounds of 12 members and 2,000,000 feature
  rows per member.

### Plans: `chronos.walk_forward_plan/1`

`<artifacts>/walk_forward_plans/<plan_id>/manifest.json`, built from a strict
`chronos.walk_forward_request/1`: collection, instrument, minimum training
recordings, strategy configuration, and scenarios.

- **Folds expand.** The members holding the instrument form the timeline.
  Fold *k* evaluates timeline member *k*, for every *k* from the minimum,
  and trains on every earlier member. A later fold learns from earlier
  evaluation recordings once their labels are history: predeclared
  chronological retraining, not tuning. Rolling windows are not offered.
- **Windows.** Each evaluation window is its whole recording, `[first
  receive, last receive + 1)`, so timestamps are never divided.
- **The information boundary** is the window's start, and it is exclusive. A
  training row needs available inputs, a usable label, and *both* its
  prediction time and its label's availability time strictly before the
  boundary. Equality is excluded. Selecting by prediction time alone would
  leak, because a label is known only when its future observation arrives.
  Every excluded row is counted by member and reason. With strictly disjoint
  recordings the rule is always met by earlier members, and the code still
  enforces and counts it.
- **Fixed before any result:**
  - membership, intervals, boundaries, and eligible-row and class-support
    counts;
  - the model configurations and the strategy configuration;
  - one to four fee × positive-latency scenarios: a `base` equal to the
    configuration, and a `stress` no cheaper and no faster on either axis
    and costlier on at least one; a zero fee only where a scenario names it;
  - the scoring rule.
- **Values.** Every option and scenario value is checked, at the plan
  boundary for the CLI and the service alike, against what the engine holds
  exactly (`strategy_configuration.canonical_configuration`), and must be
  written canonically; the folds' total starting capital must also fit the
  engine's money.
- **Workload.** At most 12 sources, 10 folds, 4 scenarios, 80 strategy runs,
  and 4,000,000 eligible training rows per fold, all checked before
  anything runs. The planned counts are recorded.
- **Too few recordings** is refused with `insufficient_coverage`, naming what
  is present and what is needed.
- **Inspection** re-verifies the sources and rebuilds the plan from its
  request, so membership, cutoffs, and counts are recomputed, not hashed.

### Fold models: `chronos.walk_forward_fold_model/1`

`<artifacts>/walk_forward_models/<fold_model_id>/{manifest.json,model.json}`.
They are historical only, outside registry version 1, never deployable, and
never disguised as experiment or import models.

- **Fitting.** Each fold fits a fresh majority model and a fresh logistic
  model, scaler included, on that fold's training rows only, with the
  existing fitting, parity check, class order, and convergence rules.
- **What it records:**
  - the plan and fold, and the ordered training sources with their IDs and
    checksums;
  - a digest of the selected rows (identity, class, and exact inputs, in
    order);
  - class support, exclusions, and the maximum prediction and label times;
  - the information boundary, the configurations, the environment, the
    exact parameters, and each model's parameter hash.
- **Unevaluable models.** No training rows, too little class support, a
  non-converged fit, or a fit warning gives an unevaluable model with its
  reason. It is never substituted.

### Schedule version 3 and strategy-run version 3

- **`chronos.inference_schedule/3`** has version 1's rows and row-pure
  inference, with a different provenance:
  - `source`: the collection, and the evaluation dataset and features. There
    are no labels and no split.
  - `model`: the plan, fold, fold model, model name, and parameter hash.
  - `training`: the information boundary and the latest training prediction
    and label times, all strictly before the window.
  - `window`: the explicit half-open interval.

  The Python and C++ readers of versions 1 and 2 are unchanged. Version 3 is
  a separate, strict branch of each, and no reader accepts arbitrary fields
  or a split reference it does not have.
- **`chronos.strategy_run/3`** names the same provenance. The runner reads
  version 3 exactly like the others: it recomputes every row's identity,
  time, inputs, coverage, and order from the dataset. A training time at or
  after the window is `LabelLeakage`.

### Studies: `chronos.walk_forward_study/1`

`<artifacts>/walk_forward_studies/<study_id>/manifest.json` links the
collection, plan, fold models, schedules, and runs.

- **Execution.**
  - Each fold model's schedule is built from features and frozen parameters
    alone, every feature row of the window, scoreable or not.
  - It runs once per scenario through `chronos_strategy`, with only the fee
    and latency changed, and each run is replayed. A run counts for its cell
    only if it is of that cell's schedule, source, evaluation events, and
    window, under all eight frozen options with the scenario's fee and
    latency; any other run makes the cell failed (`run_mismatch`), and
    inspection applies the same rule to every recorded run. The auditor
    derives the same fields itself: it hashes the events and schedule files
    and recomputes the boundary and training cutoffs.
  - Every run is its own strategy, cash, and buy-and-hold accounts with the
    configured capital. Holdings, orders, liquidity, and market state are
    reset per recording, with no trading before the window, positive
    latency, no fill on the signal's quote, and expiry at the window's end.
    Open positions are marked, never liquidated.
  - No stress result is derived from another.
- **Scoring.** The schedule builder takes the evaluation features and the
  frozen parameters, and has no label argument. Only after a fold's
  schedules exist does scoring read its evaluation labels again from the
  verified artifact and join them by identity. (Planning reads every
  member's labels, because a later fold trains on an earlier evaluation
  recording; that they cannot reach a fold's fit, predictions, or execution
  is shown by construction and by mutation tests.) A scheduled row is scored
  when its inputs were available, its label is usable, and the label was
  known before the window's end. Every other row is counted by reason. The
  metrics are the experiment's definitions.
- **Outcomes.** Every planned fold, model, and cell is kept: completed,
  unevaluable, failed, or unavailable, each with its reason. A study missing
  anything is `partial`, never complete. Inspection re-derives everything
  except a run that never started; a stored cell without a run is accepted
  only in exactly that failed shape, never with figures or another state.
- **Aggregation.**
  - Classification is pooled over the scored rows of every fold of a model,
    so it is sample-weighted, and per-fold results are shown beside it.
    Fold accuracies are never averaged.
  - Each completed cell records its strategy's exact differences from its
    own benchmarks: the PnL difference, and that as a percentage of its
    starting cash, rounded once. The browser does no arithmetic.
  - Money is summed exactly over independent reset accounts, in the
    engine's fixed point with its 64-bit range checked: PnL, fees, turnover,
    and the total starting capital. The return is total PnL over total
    capital, rounded once, half away from zero, to 8 places, as the engine
    rounds.
  - A missing terminal valuation makes the complete aggregate unavailable.
    A partial aggregate states its included and planned folds and is never
    the headline.
  - There are no spliced equity curves, portfolio drawdowns, annualized
    ratios, row-IID confidence intervals, automatic winners, promotion, or
    deployment.

### Service and web (Milestone D)

The existing research service runs both steps as jobs, with its validation,
deduplication, caps, process-group deadlines, output bounds, durable status,
and interruption on restart; no new job framework.
- **Preview** is a `walk_forward_plan` job, because verifying the members
  runs the C++ tools. It publishes the plan of exactly the request; a plan is
  content-addressed, so publishing it is harmless.
- **Start** is a `walk_forward` job named by the plan's ID alone, so later
  catalog or form changes cannot alter the reviewed intent. It verifies every
  source again before anything runs.
- **Progress.** The child writes bounded `{"progress": {...}}` lines to
  stderr; the worker keeps the latest as the job's `progress`, and the rest
  stays the log.
- **Reading.** Collections, plans, and studies are read verified; a study's
  detail checks each fold model, schedule, and run again. Fold runs open as
  results and are listed by their study.

A Walk-forward workflow inside Research (`#/research/walk-forward`) previews,
reviews, and starts a plan, follows its folds, and shows a study: trading per
fold first, totals over reset accounts, and prediction quality apart, with
synthetic and partial results labelled. It shows the service's verification,
read again on every load, beside every run, schedule, and fold model; a
study with any artifact that does not verify says so first, and every
figure and total resting on one is labelled unverified.

## Consequences

- Multi-recording evidence is now possible and honest about what it is. With
  one real recording, Chronos says the coverage is insufficient rather than
  manufacturing folds.
- Three new artifact kinds, and a third version of schedules and runs, all
  additive. Earlier versions keep their bytes, IDs, and meanings.
- Training, prediction, and execution for a fold depend only on that fold's
  earlier recordings and its evaluation features. Tests mutate evaluation
  labels and later recordings to show it.

## Alternatives rejected

- **Concatenating recordings into one dataset and splitting it.** That
  renumbers sequences, invents a continuous market, and cuts by row count.
- **Calling a later recording a split's test window.** It is not that
  split's window.
- **Registering fold models as experiment or import versions.** That claims
  provenance they do not have, and invites deploying them.
- **Deriving stress results from the base PnL.** Latency changes fills,
  quantities, risk outcomes, and later decisions; only the engine can say.
- **Averaging fold metrics, compounding reset capital, or splicing
  equity.** Each describes a portfolio that never existed.
- **Rolling-window or tuned membership.** It is not needed for correctness
  and invites selection.

## Limitations

- One instrument per study, the existing feature set, horizon, two
  baselines, and the long/flat policy.
- Whole-recording windows only.
- Same-environment reproducibility only: fitted parameters depend on the
  numerical environment, which every artifact records.
- A few recordings cannot establish skill. Chronos reports what happened,
  per fold, and makes no claim of significance.
