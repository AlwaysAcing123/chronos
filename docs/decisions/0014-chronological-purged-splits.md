# ADR 0014: Chronological, purged splits

- Status: Proposed
- Date: 2026-09-30

## Context

A model is trained on one part of the data, compared with others on a
second, and measured once on a third. With time-series market data, random
shuffling makes the measurement meaningless. Adjacent rows share nearly the
same features and overlapping future horizons, so a shuffled test set is
largely a copy of the training set, and the model also learns from the
future. Even a chronological split leaks when a training row's label is only
known after the next period has begun. Chronos needs a split that is
chronological, leakage-safe by construction, deterministic, and verifiable
from its own artifact.

## Decision

### Three contiguous periods by row order

- **Order.** Label rows are taken in ingest order, whose prediction times
  must never decrease. This is checked.
- **Shares.** Whole percentages for train, validation, and test, default
  60/20/20, each positive and together exactly 100.
- **Boundaries.** ⌊*n* · cumulative percentage / 100⌋.
- **No shuffling.** Rows are never shuffled, and train precedes validation,
  which precedes test.

### Equal prediction times are never divided

A boundary that falls between rows with the same prediction time moves later
until the time changes. Rows that share a time belong to the period of the
first of them. "Later" was chosen over "earlier" so that the first period
keeps at least its nominal share. Either is deterministic.

If a period ends up empty, because there are too few rows or one time covers
a whole share, creation fails with `empty_split`. It is never silently
rebalanced.

### Purging at both boundaries, with strict comparisons

| Row | Usable only if |
|---|---|
| train | its label has a value and is available strictly before validation's first prediction time |
| validation | its label has a value and is available strictly before test's first prediction time |
| test | its label has a value |

So `max usable train label availability < validation start` and
`max usable validation label availability < test start`.

**Excluded rows stay.** A row that fails is excluded with
`label_unavailable`, `label_crosses_validation_start`, or
`label_crosses_test_start`. It stays in its period and is never moved to a
split, and every source row has an auditable assignment.

**No embargo.** `top_of_book_v1` features have no lookback window, so labels
are the only path across a boundary. A feature set with windows will need an
embargo as well, and that will be a new split version.

### An artifact that proves its own invariants

```text
<split-root>/<artifact-id>/{manifest.json,splits.ndjson}
```

**Rows.** Each row carries its identity, prediction time, label usability
and availability time, period, split, and exclusion.

**Manifest.** It records the boundaries, per-period and per-split counts and
times, the latest usable label availability of each split, and the excluded
counts.

**Who checks the invariants:**
- The **manifest parser** refuses any manifest whose summaries break an
  invariant: an inequality (`label_leakage`), overlapping periods, an empty
  split, or counts that do not add up.
- The **row verifier** checks every row's period, exclusion, and leakage
  inequality, and recomputes the boundaries from the rows' prediction times.
- **Creation** applies the manifest check before publishing.

So nothing leaky can be published, and nothing leaky can be read. The
identity covers the percentages and the source label artifact, and
`--labels` reproduces every assignment.

## Alternatives considered

- **Random or stratified shuffling.** Leaks through overlap and future
  information. Rejected.
- **Splitting by wall-clock time instead of rows.** Equally valid. Row
  shares were chosen because they guarantee proportionate sample sizes at
  any activity level and are simple to verify. A time-based version can be
  added.
- **Moving a crossing label to the next split.** This silently changes the
  periods and hides the leakage. Rejected in favour of explicit exclusion.
- **Dropping excluded rows from the artifact.** This loses the audit trail.
  Rejected.
- **Walk-forward evaluation now.** Valuable but out of scope. A single split
  is the foundation it will be built on.

## Consequences

- **The tail is censored.** The last horizon of the test period has no
  label, which is visible in the counts.
- **Rows near boundaries are purged.** For the one-second demonstration
  that is 391 train rows and 110 validation rows. The smaller the data, the
  larger that share.
- **Small inputs are refused.** Tiny or bursty label artifacts give
  `empty_split` instead of a split that is unreliable in ways nobody can see.
