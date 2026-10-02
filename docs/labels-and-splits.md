# Future-return labels and chronological splits

Labels say what happened after each feature row; splits say which rows a
model may learn from, which it may be tuned on, and which are held back for
one final measurement. Both are immutable, content-addressed artifacts
derived from the one before, and each records its source's ID and checksum:

```text
normalized dataset -> feature artifact -> label artifact -> split artifact -> baseline evaluation
```

The C++ code owns these formats; the Python research package only reads
them after `chronos_dataset` has verified them (see
[baseline-evaluation.md](baseline-evaluation.md)). The decisions are ADR 0013
([labels](decisions/0013-future-midpoint-return-labels.md)) and ADR 0014
([splits](decisions/0014-chronological-purged-splits.md)).

## Commands

```sh
./build/chronos_dataset labels extract --features data/features \
  --feature-artifact 06e5d86e14e7123801dce78414895bf3fdb22ceaa51a96f89af9a0b041386854 \
  --output data/labels --horizon-ns 1000000000
./build/chronos_dataset labels list --output data/labels
./build/chronos_dataset labels inspect --output data/labels --artifact <LABEL-ID> \
  --features data/features

./build/chronos_dataset splits create --labels data/labels --label-artifact <LABEL-ID> \
  --output data/splits --train-percent 60 --validation-percent 20 --test-percent 20
./build/chronos_dataset splits list --output data/splits
./build/chronos_dataset splits inspect --output data/splits --artifact <SPLIT-ID> \
  --labels data/labels
```

- **Options.** `--horizon-ns` defaults to one second and the percentages to
  60/20/20. Each option is given at most once, with a value that is neither
  empty nor another option. Numbers are whole, positive, and written without
  a sign, leading zero, exponent, or unit, so `1e9`, `01`, and `1s` are
  refused. IDs are 64 lowercase hex digits.
- **Results.** Success prints JSON and exits 0, with `published` or
  `already_present`. A refused input prints `{"error": ...}` and exits 1, and
  malformed usage exits 2.
- **`inspect`** verifies everything below. With `--features` or `--labels`
  it also recomputes the artifact from its source and requires the same
  bytes.
- **Git.** `data/labels/` and `data/splits/` are ignored.

## Labels: `future_midpoint_return` version 1

### Definition

For a feature row of instrument *i* that predicts at time *t*, its
`available_at`, the **future observation** is the first later row of *i*
whose `available_at` is at least *t* + *horizon*. The default horizon is
1,000,000,000 ns (one second); any positive horizon may be chosen, and it is
part of the artifact's identity.

The first qualifying row is used even when its midpoint is unavailable. The
row is then labelled `future_midpoint_unavailable`, and no later row is
searched. Searching further would let the future market choose a longer
horizon for exactly the rows where the market was disrupted.

When both midpoints are available and the current one is positive:

```text
return    = (future midpoint - current midpoint) / current midpoint
return_raw = round_half_away_from_zero(return * 10^9)     signed int64, scale 1e-9
direction  = down | flat | up                              the sign of the exact return
```

The numerator `(future - current) * 10^9` is formed with checked 128-bit
arithmetic and divided once by the project's `rounded_divide`, so nothing
passes through `double`. A result outside int64 is `return_overflow`, never
wrapped.

The direction is the sign of the exact rational return, not of the rounded
value. There is no deadband: a move smaller than half of 1e-9 still counts as
up or down even though its `return_raw` is 0.

### When a label is known

A prediction made at *t* can only be scored once the future observation
arrives. So the label's availability time, `label_available_at_ns`, is that
observation's `available_at`, never *t*. `future.elapsed_ns` is the actual
horizon, at least the requested one. A model trained at time *T* may only use
labels with `label_available_at_ns < T`. The split artifact enforces this.

### Unavailable labels

Each row has exactly one reason, checked in this order:

| Reason | Meaning |
|---|---|
| `current_midpoint_unavailable` | the row's own midpoint is unavailable; its feature reason is kept in `current_midpoint` |
| `nonpositive_current_midpoint` | the current midpoint is zero or negative, so the return is undefined |
| `timestamp_overflow` | *t* + *horizon* exceeds the int64 nanosecond range |
| `no_future_observation` | the artifact ends before *t* + *horizon*: the label is censored |
| `future_midpoint_unavailable` | the first qualifying observation's midpoint is unavailable; its feature reason is kept in `future.midpoint` |
| `return_overflow` | the return does not fit int64 at scale 1e-9 |

The future observation is recorded whenever it exists, even when the label is
unavailable for a current-side reason, because selecting it depends only on
time.

### Rows

There is one label row per feature row, in the same order. Each is
identified by the feature row's `stream`, `instrument`, and
`ingest_sequence`. A real row:

```json
{"current_midpoint":{"raw":"768760000000"},"future":{"elapsed_ns":"1006866098","ingest_sequence":"574","midpoint":{"raw":"768500000000"}},"horizon_ns":"1000000000","ingest_sequence":"0","instrument":"SPY","label":{"direction":"down","return_raw":"-338207"},"label_available_at_ns":"1790343001008531316","label_set":"future_midpoint_return","prediction_at_ns":"1790343000001665218","stream":"databento-equs-mini-mbp1-20260925-1330-1340","version":"1"}
```

As a check: (768.50 − 768.76) / 768.76 = −0.000338207…, which is
`return_raw` −338207 at scale 1e-9. An unavailable label is
`{"unavailable": "<reason>"}`, and `future` and `label_available_at_ns` are
`null` exactly when no future observation exists.

### Artifact

```text
<label-root>/<artifact-id>/{manifest.json,labels.ndjson}
```

**Manifest.** It records the following, and the artifact ID is the SHA-256
of the canonical manifest without `artifact_id`:
- the schema `chronos.label_artifact` v1, the row record
  `chronos.label_row` v1, the label set, and the generator
  `chronos.labels.future_midpoint_return` v1;
- the configuration, which is fixed except for `horizon_ns`, together with
  the return scale and rounding;
- the source: the feature artifact ID, its rows' checksum, its feature set,
  the dataset ID, and the stream;
- the counts: rows, rows by instrument, usable rows, directions, and
  unavailable rows by reason;
- the checksum and size of `labels.ndjson`.

So a different source, horizon, definition, scale, or generator gives a
different ID. Publication works as for datasets: a staging directory,
`fsync`, a rename that never replaces, read-only files and directory, and
`already_present` or `collision` when the ID exists.

**Ordering precondition.** Extraction first verifies the feature artifact
completely. It then checks that `available_at` never decreases within an
instrument and is never negative, and refuses the artifact otherwise. It never
assumes either.

**Lookup.** The future observation is found by one forward scan per
instrument, linear in the number of rows. Memory holds one compact
observation per row: instrument, sequence, time, and midpoint.

**`inspect` without a source** checks:
- every row is canonical, on the manifest's stream and horizon, and in
  ingest order;
- each future observation is later in ingest order, at least one horizon
  away, and consistent with `elapsed_ns`;
- each label equals the one its own midpoints and future observation give;
- the counts, checksum, and ID match.

A second pass then labels the artifact's own prediction times and current
midpoints again and requires every row's exact bytes. That catches:
- a row that skipped its first qualifying observation;
- a row that claims its observation arrived earlier than it did;
- a row whose future midpoint is not that observation's midpoint.

The label reader runs both passes before it reports the end of the rows, so
split creation and split reproduction, which read labels through it, get
the same full verification.

**What only `--features` catches.** A plausible but wrong current midpoint
on a row that no other row observes is self-consistent. Only reproduction
from the feature artifact reveals it.

## Splits: `chronological_purged` version 1

### Why chronological, and why not random

Market data is a time series. Neighbouring rows share their recent past, so
their features are nearly the same, and overlapping horizons make their
labels nearly the same too. If rows were shuffled into train and test,
nearly identical rows would sit on both sides, and the model would be scored
on near copies of what it learned from, while also learning from the
future. The resulting accuracy would overstate what is achievable in real
time.

Chronos therefore never shuffles. Train comes first, then validation, then
test, each a contiguous period. Validation is for comparing models and
choosing settings. Test is touched once, for the final measurement.

### Boundaries

Rows are taken in ingest order, which requires `prediction_at` never to
decrease. For *n* rows and whole percentages *p<sub>train</sub>*,
*p<sub>validation</sub>*, *p<sub>test</sub>* that add up to exactly 100:
1. Validation nominally starts at row ⌊*n* · *p<sub>train</sub>* / 100⌋, and
   test at row ⌊*n* · (*p<sub>train</sub>* + *p<sub>validation</sub>*) /
   100⌋.
2. Each boundary then moves later past every row that shares the previous
   row's prediction time. Rows with equal times, which include every
   instrument quoting at once, are never divided.

The resulting rows, times, and counts are recorded. If train, validation, or
test would be empty, creation is refused with `empty_split`. That happens
with too few rows, or when one prediction time covers a whole share.

### Purging

Labels look forward, so a row near the end of one period has a label that
becomes known in the next. Training on it would use information from inside
the validation period, and the same holds for validation rows at the test
boundary. Such rows are purged: excluded with a reason, never moved to
another split. With strict comparisons:

```text
a train row is usable       only if label_available_at < validation's first prediction_at
a validation row is usable  only if label_available_at < test's first prediction_at
a test row is usable        only if its label has a value
```

So
`max usable train label availability < validation start` and
`max usable validation label availability < test start`, which the manifest
records and every reader checks. A label known exactly at the boundary is
purged.

No embargo beyond purging is needed for `top_of_book_v1`. Its features use
only the triggering event and the instrument's latest trading status, which
is known before the prediction, so no feature window reaches across a
boundary.

### Rows and exclusions

There is one split row per label row, in order, with the same identity. Each
row also carries:
- `prediction_at_ns`;
- `label_usable`;
- `label_available_at_ns`;
- its chronological `period`;
- its `split`: `train`, `validation`, `test`, or `excluded`;
- an `exclusion` reason:

| Exclusion | Meaning |
|---|---|
| `label_unavailable` | the label has no value (see the label reasons above) |
| `label_crosses_validation_start` | a train row whose label is known at or after validation begins |
| `label_crosses_test_start` | a validation row whose label is known at or after test begins |

### Artifact

```text
<split-root>/<artifact-id>/{manifest.json,splits.ndjson}
```

**Manifest.** It records the following, and the artifact ID is the SHA-256
of the canonical manifest without the ID, so different percentages or a
different source give a different ID:
- the schema `chronos.split_artifact` v1, the row record, the split set, and
  the generator;
- the configuration, which is fixed except for the percentages;
- the source label artifact's ID and checksum, the feature artifact and
  dataset IDs, the stream, and the horizon;
- the boundaries, as row positions and times;
- for each period, its rows and first and last prediction times;
- for each split, its usable rows, first and last prediction times, and
  latest label availability;
- the excluded rows by reason;
- the rows' checksum.

Reading a manifest refuses it if the summaries break the invariants. That
means overlapping periods, an empty split, counts that do not add up, or
either leakage inequality violated (`label_leakage`). Creation applies the
same check, so nothing is published that a reader would refuse.

**`inspect` without a source** checks:
- every row is canonical, on the stream, in ingest order, and has
  non-decreasing prediction times;
- each label is known no sooner than one horizon after its prediction, and a
  usable label says when;
- each row's period follows from its position;
- each row's exclusion follows from its label and the boundaries;
- no usable row's label crosses its boundary (`label_leakage`);
- the summaries and checksum match;
- the boundaries are exactly those the percentages give for these
  prediction times.

**`--labels`** also recomputes every assignment from the label artifact.

## The demonstration recording

These figures come from the ignored ten-minute AAPL and SPY recording, with
a one-second horizon and 60/20/20 splits.

**Labels** (artifact
`2edb8ddf894593a6718bd2a33a44aa043e7f54d77c6c1ddfb448bb4dd096efba`):
- 119,282 rows, of which 119,035 are usable and 247 censored
  (`no_future_observation`);
- directions: 53,855 down, 6,853 flat, and 58,327 up.

**Splits** (artifact
`4f4bed5c10acbc2d43b85ca45966c9d69d30bdbfb0837b7be0d230eb8c020fa2`):

| Period | Rows | Usable | Excluded |
|---|---|---|---|
| Train | 71,569 | 71,178 | 391 cross into validation |
| Validation | 23,856 | 23,746 | 110 cross into test |
| Test | 23,857 | 23,610 | 247 censored |

These are counts, not evidence of anything about the market. Ten minutes of
two symbols is one tiny sample. See
[baseline-evaluation.md](baseline-evaluation.md).

## Limitations

- **One label.** The midpoint return over a fixed clock horizon, from Level 1
  only. There is no volume or event-count horizon, no transaction-cost-aware
  target, and no deadband (a v2 could add one).
- **Memory.** The labeler holds one compact observation per row, so labelling
  is bounded by memory, not streamed. That is about 5 MB for the
  demonstration.
- **One split.** A single chronological split, with no walk-forward or
  rolling evaluation yet. A tiny or bursty artifact can be refused as
  `empty_split` rather than split unevenly.
- **Percentages count rows, not time.** A busy hour weighs more than a quiet
  one.
