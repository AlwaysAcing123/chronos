# ADR 0013: Future-midpoint-return labels

- Status: Proposed
- Date: 2026-09-30

## Context

Point-in-time feature artifacts (ADR 0012) say what was known at each top
of book. To evaluate a model we also need what happened next: a label for
each feature row. A label is the easiest place to leak the future into
research. If it is read at the wrong time, chosen by looking ahead for
"good" data, or rounded through binary floating point, every later result is
wrong while looking plausible. The label must therefore be exact, auditable
row by row, typed when absent, and explicit about when it becomes known.

## Decision

### The definition

Version 1 is `future_midpoint_return` (full definition in
docs/labels-and-splits.md). The feature row's `available_at`, *t*, is its
prediction time. Its future observation is the first later row of the same
stream and instrument whose `available_at` ≥ *t* + *horizon*.

- **Horizon.** A positive number of nanoseconds, by default one second, and
  part of the identity.
- **Return.** `(future − current) / current` in 128-bit checked integer
  arithmetic, rounded half away from zero once to an int64 at scale 1e-9.
  This is the project's established rule (ADR 0002), the same one features
  use.
- **Direction.** The sign of the exact rational return.

### The first qualifying observation, never a later one

When the first qualifying row's midpoint is unavailable, the label is
`future_midpoint_unavailable`, and no later row is used. The alternative,
skipping ahead to the next usable midpoint, would lengthen the horizon for
exactly the rows where the market was halted, crossed, or one-sided. Future
market state would then decide what is being predicted.

### A label is known only when its observation arrives

Each row records:
- `prediction_at_ns`;
- `label_available_at_ns`, which is the future observation's `available_at`;
- `future.elapsed_ns`;
- the future observation's ingest sequence and midpoint.

Splits use the availability time to purge (ADR 0014). Using the prediction
time instead would let a model train on outcomes that had not happened yet.

### Typed unavailability, one reason per row

There are six reasons, checked in a fixed order. Current-side reasons come
first:
1. `current_midpoint_unavailable`;
2. `nonpositive_current_midpoint`.

Then the future-side ones:
3. `timestamp_overflow`;
4. `no_future_observation` (censored);
5. `future_midpoint_unavailable`;
6. `return_overflow`.

Every unavailable row is still written and counted. The feature
unavailability behind a missing midpoint is kept in `current_midpoint` or
`future.midpoint` rather than folded into new reason names, so the label
vocabulary stays small and the cause stays visible.

The return and the direction are one `label` object, either
`{"direction", "return_raw"}` or `{"unavailable"}`, so neither can be
present without the other.

### No deadband in version 1

The direction is the exact sign. A deadband would be an arbitrary parameter
that changes the class balance. It can be a later version, with its own ID.
A consequence: a move smaller than half of 1e-9 is up or down while
`return_raw` is 0. That is documented and tested.

### A separate artifact, with the feature artifact's guarantees

```text
<label-root>/<artifact-id>/{manifest.json,labels.ndjson}
```

**Identity and publication.** The artifact ID is the SHA-256 of the
canonical manifest without the ID, so different sources, horizons,
definitions, scales, or generator versions give different IDs. Publication
is atomic and never replaces anything.

**Shared code.** The directory checks, the verified row stream, the
publication, and the listing were extracted from the feature artifact code
into `datasets/derived_artifact.hpp`. Features, labels, and splits share one
implementation. The feature artifact's bytes and ID are unchanged, which was
confirmed by re-extracting the demonstration artifact.

**Verified source reading.** A narrow `FeatureArtifactReader` streams feature
rows and reports the end only once count, checksum, and summary verify.
`verify_feature_artifact` is now a loop over it.

### Linear lookup over compact observations

Extraction holds one compact observation per row (instrument, sequence,
time, midpoint) and scans forward once per instrument. Every row's horizon
end is at least the previous row's, so the scan never moves back. That is
O(*n*) time, and memory proportional to rows. The precondition, times
non-decreasing within each instrument and never negative, is checked and
refused when false, never assumed.

### Verification that does not trust the generator

`inspect` checks every row on its own:
- its stream and horizon;
- its ingest order;
- that the future observation is later in ingest order and at least one
  horizon away;
- its elapsed time;
- that its label is the one its own midpoints give.

A second pass then labels the artifact's own prediction times and current
midpoints again and requires identical bytes. That catches a skipped, early,
or wrong future observation.

`LabelArtifactReader` runs both passes before it reports the end of the
rows, so every consumer, the split code included, gets the full check.
`--features` reproduces the whole artifact from its source.

## Alternatives considered

- **Labels inside the feature artifact.** This would mutate or re-identify
  features whenever the label changes, and mix "known now" with "known
  later" in one row. Rejected.
- **The first usable future midpoint.** This lets future market state choose
  the horizon. Rejected.
- **A fixed number of rows ahead.** That is not a time horizon: it means
  different things at different activity rates. It could be a later label
  version.
- **Floating-point returns.** Not exact and not reproducible across readers.
  Rejected.
- **Timestamp in the prediction row only.** Without the label's availability
  time, purging is impossible. Rejected.
- **Streaming with per-instrument queues.** Equivalent results with more
  code, and the output must still be in global order. The compact two-step
  approach is simpler.

## Consequences

- **Every row is auditable.** Every row can be recomputed by hand from its
  own fields. The real artifact's first row is worked through in the docs.
- **Memory.** Labelling needs memory proportional to rows, about 5 MB for
  the demonstration. A streaming design is a later option if datasets grow
  by orders of magnitude.
- **Versioning.** A different target (a deadband, another horizon type, a
  cost-aware return) is a new label version with its own ID.
