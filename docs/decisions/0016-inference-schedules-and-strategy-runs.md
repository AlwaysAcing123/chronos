# ADR 0016: Inference schedules and deterministic strategy runs

- Status: Proposed
- Date: 2026-10-04

## Context

The baseline experiment (ADR 0015) scores predictions; it does not trade.
Evaluating a model as a trading policy needs two things that live on
different sides of the Python/C++ boundary:
- the fitted models, which are Python's;
- the event-driven execution, risk, accounting, and valuation, which are
  C++'s and already serve paper trading.

The experiment's `predictions.ndjson` holds only rows whose labels became
known in time. That selection uses the future, so trading on it would leak.

## Decision

### A label-free, immutable inference schedule (Python)

Python publishes an inference schedule: one fitted model's predictions on
every feature row of one instrument in one evaluation window.
- **Contents.** Every row's identity, time, exact model inputs, class,
  probabilities, and integer `confidence_ppb`, or a contemporaneous
  `feature_unavailable:<reason>`.
- **What it reads.** Only the verified feature rows and the verified
  experiment's parameters; never the label or split rows. The split's
  manifest contributes only the window boundaries and the training labels'
  availability bound, which must precede the window.
- **Arithmetic.** Each row is inferred alone, in a fixed order of
  elementwise operations, so its prediction does not depend on its batch.
- **Identity.** The schedule is content-addressed like an experiment.

**Rejected:**
- *Trading `predictions.ndjson`*: future-selected rows.
- *Models in C++*: a second implementation of every model, for no gain.
- *Python bindings, or C++ calling Python*: a native or embedded runtime in
  the deterministic runner.
- *Batch inference*: a row's last bit could change with its window.

### A deterministic C++ strategy runner

`chronos_strategy` reads the verified normalized dataset in ingest order.
- **Features.** It recomputes each row's features with the same engine, and
  refuses a schedule whose identity, time, or inputs differ.
- **Accounts.** It runs one versioned policy, `long_flat_threshold` v1, in
  a fresh `PaperOrderProcessor`, with cash and buy-and-hold benchmarks in
  their own fresh processors.
- **Shared code.** Risk, fills, fees, liquidity, accounting, and valuation
  are paper trading's own code. Only the policy and the event loop are new.

The ordering per event is: apply the event; execute eligible earlier
orders; check the schedule row against the recomputed features; decide;
schedule a delayed order.
- **Execution observation.** The first top-of-book event of the instrument
  received at or after an order's arrival executes it, whatever its
  quality. Waiting for a better quote would be selection.
- **The window's end.** Events at or after the end are never applied, and
  pending orders then expire.
- **Valuation.** It runs after each decision and execution, and at the
  end, marking at the best bid. An unmarkable position is reported as
  unavailable, never as zero.

**Rejected:**
- *Waiting for a "good" execution quote*: optimistic.
- *Mid-price or full fills*: they fabricate liquidity.
- *Liquidating at the end*: an invented sale.
- *Sharing one account's liquidity across the strategy and the benchmarks*:
  they are counterfactuals, not concurrent traders.

### An immutable result artifact

The result is a manifest plus a `journal.ndjson` of decisions, orders,
executions, expiries, and valuations. Its ID is content-addressed and
includes the build environment.
- **Inspection.** `inspect` checks structure, identity, and checksums, and
  can run the schedule again to require the same journal and results.
- **Publication.** It shares the derived-artifact layer: atomic,
  never-replacing, read-only.

## Consequences

- **One result format.** The UI and any later model share it, and the
  policy and fee assumptions are recorded with every number.
- **A cheap trust boundary.** A schedule is a declared input; C++ trusts it
  only as far as it can recompute it. `inspect-schedule` rebuilds a
  schedule from its sources in Python.
- **Build-specific IDs.** A different compiler gives a different run ID for
  identical economics. That is acceptable: the economics and the journal
  checksum are what reproduction compares.
- **Extension.** More policies, instruments, or order types are new
  versions of the policy or of the runner.
