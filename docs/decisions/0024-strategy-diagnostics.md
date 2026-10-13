# ADR 0024: Strategy diagnostics: exact PnL attribution and decision cohorts

- Status: Proposed
- Date: 2026-10-12

## Context

A walk-forward study (ADR 0022) reports each run's exact net PnL. It does not
say why: whether a loss came from crossing the spread, from the midpoint
moving between a signal and its fill, from fees, from marking an open
position at the bid, or from the fixed-point rounding of booked notionals.
It also scores every prediction, but not the predictions the policy acted on.
The real pilot (two studies, 48 runs) lost money with the logistic model
and did nothing with the majority model. The aim is an explanation that can
be checked, not a better strategy.

Constraints:

- **Nothing existing changes.** Recordings, datasets, features, labels,
  plans, fold models, schedules, runs, studies, the engine and its
  accounting, and the walk-forward auditor keep their bytes and meanings.
- **No refitting and no new runs.** `inspect_study` refits fold models and
  replays runs, so diagnostics never call it. They read the study, plan,
  runs, and schedules through their non-refitting verified readers and
  verify the recordings through the C++ verifiers, which recompute in memory
  and write nothing.
- **Exact arithmetic.** Prices are 10^-9, quantities and money 10^-8. A
  midpoint can be half a price unit, so a product of quantity and midpoint
  needs 18 decimal places, more than the Money API carries.

## Decision

### Artifact: `chronos.strategy_diagnostics/2`

`<artifacts>/strategy_diagnostics/<diagnostic_id>/`: `manifest.json`,
`fills.ndjson` (one row per fill), and `episodes.ndjson` (one row per
holding episode). The request is `chronos.strategy_diagnostics_request/1`,
`{"schema", "study_ids"}`, one to four distinct study IDs in ascending order.
The ID is the SHA-256 of the canonical manifest without `diagnostic_id`,
with its newline. The manifest records no build environment: nothing in it
depends on floating point, so the same sources give the same ID on any
platform. Publication is atomic and never replaces an entry; an identical
rerun is `already_present`. Version 1 was the first reviewed draft, never
merged; its artifacts are refused as another schema, never reinterpreted.

The manifest names every source by ID and checksum: each study, plan, and
collection; each fold's dataset (events checksum), feature artifact (rows
checksum), label artifact (rows checksum), and stream; and each run's ID,
journal checksum, schedule ID, and schedule rows checksum. A cell that did not complete, or a model that has no schedule, is
kept with its state and reason, and the study's diagnostics are then
`partial`.

### Joins: complete identities only

- An event is (dataset, stream, instrument, ingest sequence) in the run's
  own dataset; ingest sequences repeat across recordings, so a sequence is
  never looked up outside its dataset.
- A decision's signal is its schedule row (same stream, instrument, ingest
  sequence, availability time, class, and confidence) and that row's
  top-of-book event, whose receive time must be the order's signal time.
- An order is (run, account, decision); its one resolution is the execution
  or expiry with the same (run, account, client order ID). A duplicate,
  missing, or second resolution, or a reference to another account, run,
  stream, or instrument, refuses the diagnostics (`join_failed`); nothing
  is guessed.
- An execution's observation must be the event with its ingest sequence,
  with identical bid, ask, receive time, and suspect flag; the final mark
  must be the event with the mark's ingest sequence, with identical bid and
  receive time.
- Labels are the evaluation recording's verified labels, joined to schedule
  rows by identity, under the plan's scoring cutoff. They are read only to
  describe outcomes after the fact: nothing here feeds a model, a policy, an
  order, or a choice of what to run.

### Attribution (per run and account)

For fill *i*: signed quantity *q_i* (+ buy, − sell), price *p_i*, booked
notional *n_i* signed the same way, fee *f_i*, execution midpoint *m_i* =
(bid + ask) / 2 of its execution observation, and signal midpoint *s_i* of
its decision's signal event. *Q* is the final position, *B* the final mark's
bid, *M* that event's midpoint, and *V* the booked final market value.

```text
midpoint_inventory_pnl     = Q·M − Σ q_i·m_i
execution_spread_cost      = Σ q_i·(p_i − m_i)
fees                       = Σ f_i
terminal_bid_markdown      = Q·(M − B)
rounding_adjustment        = (V − Q·B) − Σ (n_i − q_i·p_i)
booked_net_pnl             = midpoint_inventory_pnl − execution_spread_cost − fees
                             − terminal_bid_markdown + rounding_adjustment
observed_midpoint_movement = Σ q_i·(m_i − s_i)          (positive = adverse)
signal_reference_pnl       = Q·M − Σ q_i·s_i
midpoint_inventory_pnl     = signal_reference_pnl − observed_midpoint_movement
```

- **Independence.** `booked_net_pnl` is the engine's final `net_total_pnl`;
  `rounding_adjustment` has its own definition (booked minus exact
  notionals and market value), never the residual. Both identities must
  hold exactly, or generation refuses (`reconciliation_failed`). The engine's
  ledger is also checked: cash = starting cash − Σ n_i − Σ f_i, fees, and
  net PnL = equity − starting cash.
- **Execution rules.** A balanced ledger is necessary, not sufficient: a
  ledger booked with another rounding or fee balances just as well. Every
  booked notional must be the engine's own rounding of the price times the
  quantity, *n_i* = round(*p_i*·|*q_i*|) to 10^-8 half away from zero, and
  the final market value *V* = round(*B*·*Q*) the same way; every fill's fee
  must be the run's frozen `fee_per_fill`. Each is an exact comparison of
  integers, with no tolerance, or generation refuses (`ledger_invalid`).
- **Missing inputs.** A component that needs a value that does not exist is
  `{"unavailable": "<reason>"}`. Nothing is substituted: not the ask, a
  trade, the cost basis, an earlier quote, or zero. *M* and *B* are
  required only when *Q* ≠ 0. An unavailable final valuation makes
  `booked_net_pnl`, `rounding_adjustment`, and every term needing *Q·B* or
  *Q·M* unavailable, with the engine's reason; the execution-side
  components stay. When only *M* is missing (no final ask), the
  reconciliation is checked at the bid instead, through
  `inventory_pnl_at_bid = Q·B − Σ q_i·m_i` (= `midpoint_inventory_pnl −
  terminal_bid_markdown`).
- **Serialization.** Every attribution figure is an exact decimal string
  with at most 18 fractional digits (every value lies on the 5 × 10^-18
  lattice), canonical: no exponent, trailing zeros, or negative zero. It is
  its own `diagnostic_decimal` type, never the Money schema; booked engine
  figures stay Money.
- **Timing.** The configured latency is reported apart from what was
  observed: signal to arrival, arrival to execution, and signal to
  execution, per execution, with count, minimum, maximum, and sum per
  account. Expired orders have no execution time.

### Decisions (per run, strategy account)

Four cohorts, each a subset of the one before:

1. **Scorable predictions**: schedule rows with a prediction. Its scored
   rows and reasons must equal the study's own scores exactly.
2. **Sampled decisions**: journaled strategy decisions (the decision
   interval samples rows; skipped rows are counted from the manifest).
3. **Order-scheduling decisions**: decisions whose reason is
   `order_scheduled`.
4. **Orders with fills**: those decisions' orders that filled at least in
   part.

Each cohort reports its size, label status (`scored`,
`label_unavailable:<reason>`, `label_after_scoring_cutoff`), predicted and
realized classes, confusion counts, and accuracy and balanced accuracy (mean
recall over the classes that occur) as exact reduced fractions with a
6-place decimal rounded half away from zero, or undefined with a reason when
the denominator is zero. Confidence uses ten fixed bins, chosen before any
result was examined: [0, 0.1), [0.1, 0.2), …, [0.9, 1.0] of the predicted
class's `confidence_ppb`. Decisions are also counted by reason, and orders by
outcome, cancellation reason, and rejection code.

Each cohort also summarizes its scored items' future returns, in the label
artifact's own unit, `return_raw`: (future midpoint − current midpoint) /
current midpoint in parts per billion, as the label generator rounded it.
It reports rows, the exact sum, minimum, maximum, and an exact mean, for the
cohort, for each predicted class, and for each fixed confidence bin, and
counts the items without a scored label. Sums and counts pool, so a pooled
mean is a pooled sum over pooled rows. These are descriptions after the
fact: no threshold, ranking, model, policy, or choice of what to run uses
them.

Holding episodes run from a fill that leaves a flat position to the fill that
makes it flat again (`completed`), or to the window's end (`censored`, its
marked PnL needing the final valuation). Completed episodes report booked
PnL: −Σ n_i − Σ f_i over their fills.

### Comparisons and pooling

Per study: every (fold, model, scenario, account), and pooled over folds per
(model, scenario, account), as exact sums of each component over
independent reset accounts; a component missing in any fold makes the
complete pool unavailable, with a partial pool naming its included folds.
A pool with no completed run evaluated nothing: every financial figure and
difference is `{"unavailable": "no_completed_runs"}`, never a zero, while
its counts are genuinely zero. An account that was evaluated and never
traded keeps its genuine zeros.
Strategy minus buy and hold and strategy minus cash are given per component.
Decision cohorts pool counts over folds and then compute their ratios, never
averaging fold ratios. With more than one study, the same pooling runs over
studies, for scenarios with identical names, fees, and latencies in all of
them. A study with no completed fold of a cell is listed as missing from
that pool, not summed in as a zero. Nothing is spliced, annualized, tested
for significance, ranked, or declared a winner.

### Generation, inspection, and audit

- `diagnose-strategies --study <id>...` verifies the sources and publishes
  the artifact; `list-strategy-diagnostics`; `inspect-strategy-diagnostics`
  verifies it and rebuilds it from its sources, requiring identical bytes.
  It publishes nothing.
- The research service queues `strategy_diagnostics` jobs on the existing
  bounded workers (deduplication, timeout, restart as interrupted, process
  groups killed on stop). `POST /strategy-diagnostics` is the only way to
  generate. GETs read verified artifacts and never generate anything. IDs
  are the only names, never paths.
- `tools/strategy_diagnostics_audit/audit.py`, run with `python -I -S`, uses
  the standard library alone. It authenticates every source before using it
  (each manifest's identity and schema, the size, checksum, and row count of
  each consumed file, and every binding between study, plan, collection
  member, dataset, features, labels, schedule, and run), then recomputes
  every figure from the raw journal, events, schedule, feature, and label
  files and compares them with the artifact. It must catch planted errors.
- A service read checks every source again: each study's, plan's, and
  collection's identity and what it names, and each dataset's, feature
  artifact's, label artifact's, schedule's, and run's manifest identity,
  bindings, and consumed file's size and SHA-256 against what the
  diagnostics read. Hashing about 1 GB of real-pilot sources on every read
  is avoided by reusing the service's own earlier SHA-256 of a file only
  while its device, inode, size, modification time, and change time are all
  unchanged; the read says how many it hashed and how many it reused. Each
  run, fold prediction cohort, and pool then carries its own trust: verified
  only if every source it rests on is, otherwise naming the sources that
  are not. The browser labels exactly those figures as recorded and
  unverified, and says of a recorded identity only that it held when
  generated.
- Verification levels are kept apart everywhere: artifact checksums (every
  read), every source (at generation, and again on every service read),
  recomputed inspection (CLI), the independent audit (CLI), and the engine
  replay (recorded by the study, never repeated here).

## Consequences

- A loss can be decomposed into spread, midpoint movement, fees, terminal
  markdown, and rounding, to the last 10^-18, and checked independently.
- The UI shows the server's exact strings and performs no calculation.
- Generating diagnostics adds a new artifact directory beside the sources.
  The sources themselves are never written.

## Alternatives rejected

- **Floating point or Money-scale rounding.** It loses the half-unit
  midpoint and makes reconciliation approximate.
- **A residual "other" term.** It reconciles by construction and so proves
  nothing.
- **Checking rounding within half a unit.** At an exact tie both directions
  pass, so a wrong rounding rule goes unseen; the exact rounded integer is
  required instead.
- **Rehashing every source on every read, or calling unread sources
  verified.** The first costs about 1 GB of reading per real-pilot page
  load; the second claims what was not checked. Reuse while unchanged keeps
  every read a check, and says which digests it reused.
- **Replaying runs to get midpoints.** The journal, the dataset, and the
  schedule already hold them. Replay is the study inspector's job, and
  refitting is out of scope.
- **Extending the study manifest.** It would change study bytes and IDs.
- **Joining by timestamp or sequence alone.** Timestamps tie and sequences
  repeat across recordings.

## Limitations

- Top-of-book only: the spread cost is against the displayed midpoint, not
  a measure of impact.
- A few short recordings support description, not inference: no confidence
  interval, significance, or generalization.
- Diagnostics of a run that failed before it started do not exist; the cell
  is listed as missing.
