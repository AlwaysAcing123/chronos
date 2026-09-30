# Point-in-time feature engine

The feature engine turns the validated market-event stream into feature rows
that use only what was known when each row's triggering event arrived. It is
provider-independent C++ in the default build (`chronos_features`), consumes
the same `ValidatedMarketEvent` values as replay and live sources, and writes
immutable, reproducible feature artifacts from
[normalized datasets](normalized-market-datasets.md). The decision record is
[decisions/0012-normalized-market-dataset-and-features.md](decisions/0012-normalized-market-dataset-and-features.md).

There are no labels, targets, splits, or models yet: features are inputs,
never outcomes.

## Commands

```sh
./build/chronos_dataset features extract \
  --catalog data/normalized \
  --dataset 02adcb6d6a1658fb0346b5ac5552131f19c1819cadf5d43d58763e9562855ae8 \
  --output data/features
./build/chronos_dataset features list --output data/features
./build/chronos_dataset features inspect --output data/features \
  --artifact 06e5d86e14e7123801dce78414895bf3fdb22ceaa51a96f89af9a0b041386854 \
  --catalog data/normalized
```

- **`extract`** verifies the whole dataset while it computes the rows, and
  publishes nothing unless every event verifies. `--feature-set` defaults to,
  and currently only accepts, `top_of_book_v1`. It prints the artifact
  summary and `published` or `already_present`.
- **`list`** prints every artifact in a feature root and reports every other
  entry.
- **`inspect`** re-reads and checks every row. With `--catalog` it also
  recomputes the rows from the source dataset and requires the same bytes.

Errors use the same JSON shape and codes as the dataset commands.
`data/features/` is ignored by Git.

## Feature set `top_of_book` version 1

A row is produced for every top-of-book event, after that event is applied.
Other event types produce no row: a market-status event only updates its
instrument's trading status, and trades and instrument definitions change
nothing. No row exists for an instrument before its first quote, so no row is
emitted during warm-up.

| Feature | Definition | Scale and rounding |
|---|---|---|
| `best_bid`, `best_ask` | The quote's prices | Price, exact |
| `bid_size`, `ask_size` | The quote's sizes | Quantity, exact |
| `spread` | `ask − bid` | Price, exact |
| `midpoint` | `(bid + ask) / 2` | Price 1e-9, half away from zero |
| `imbalance` | `(bid_size − ask_size) / (bid_size + ask_size)` | Ratio 1e-9 (nine places), half away from zero |
| `microprice` | `(ask × bid_size + bid × ask_size) / (bid_size + ask_size)` | Price 1e-9, half away from zero |

Everything is computed on raw fixed-point integers with checked 128-bit
arithmetic, then rounded once: `midpoint` of 100 and 100.000000001 is
100.000000001, and an imbalance of exactly ±0.5e-9 rounds to ±1e-9. No value
passes through `double`, and zero has one spelling.

### Availability: one rule

In this order:

1. The instrument's latest market-status event says `halted` or `closed`:
   every feature is `trading_halted` or `market_closed`.
2. The quote is suspect (flagged `potentially_invalid_book` or
   `questionable_receive_timestamp`, which includes every crossed book):
   every feature is `suspect_quote`.
3. A side is missing:
   - that side's price and size are `missing_bid` or `missing_ask`;
   - `spread`, `midpoint`, `imbalance`, and `microprice` are `missing_bid`,
     `missing_ask`, or `missing_both_sides`.
4. Otherwise every feature is computed. A zero denominator or an overflow
   would be `zero_denominator` or `arithmetic_overflow`. Validated quotes,
   with positive prices and sizes, can produce neither, but the calculations
   are tested for both.

An unavailable feature is a typed reason, never zero, and the row is still
emitted, so a model sees the gap. A locked quote is available: its spread is
zero.

Stream interruptions are not a feature state. Offline, a dataset that fails
verification aborts extraction. Live, a source that stops delivering simply
produces no rows.

### Rows

Each line of `features.ndjson` is one canonical JSON object. A real row:

```json
{"available_at_ns":"1790343000001665218","event_timestamp_ns":"1790343000001646325","feature_set":"top_of_book","features":{"ask_size":{"raw":"12600000000"},"best_ask":{"raw":"768790000000"},"best_bid":{"raw":"768730000000"},"bid_size":{"raw":"30000000000"},"imbalance":{"raw":"408450704"},"microprice":{"raw":"768772253521"},"midpoint":{"raw":"768760000000"},"spread":{"raw":"60000000"}},"ingest_sequence":"0","instrument":"SPY","quality":["replay"],"receive_timestamp_ns":"1790343000001665218","stream":"databento-equs-mini-mbp1-20260925-1330-1340","trading_status":"not_reported","version":"1"}
```

- **Tracing to the source event.** `stream`, `instrument`,
  `ingest_sequence`, `event_timestamp_ns`, and `receive_timestamp_ns`
  identify the triggering event.
- **`quality`** repeats its flags.
- **`trading_status`** is the instrument's latest reported status, or
  `not_reported`.
- **`available_at_ns`** is when the row is known: the triggering event's
  receive time, the market clock at which Chronos applies it.
- **Features.** Each is `{"raw": "<integer>"}` in the scale the manifest
  declares, or `{"unavailable": "<reason>"}`.

## Point in time, and why no row can see the future

The engine keeps, per instrument, only the latest trading status. A row is a
function of its triggering event and that status, computed the moment the
event is applied, and the engine is fed in ingest order. Nothing after the
event exists yet, so nothing after it can influence the row. The tests check
this directly:
- **Prefixes.** Running any prefix of a stream gives exactly the first rows
  of the whole run, byte for byte.
- **Later changes.** Changing a later event changes only its own row; every
  earlier row stays byte-identical, in memory and in published artifacts.
- **Ingest order.** Rows follow ingest order, including events with equal
  timestamps.
- **Isolation.** Instruments do not affect each other's state.
- **One engine.** Rows computed from events in memory and from a normalized
  dataset are the same bytes. That is the same engine the replay and live
  paths can use.

## Artifacts

```text
data/features/
  06e5d86e14e7123801dce78414895bf3fdb22ceaa51a96f89af9a0b041386854/
    features.ndjson    71,752,372 bytes for the demonstration dataset
    manifest.json
```

Artifacts live in their own root so a dataset directory is never modified.
The artifact ID is the SHA-256 of the canonical manifest without
`artifact_id`. The manifest contains:
- the source dataset ID and events checksum;
- the feature set and version;
- the configuration that fixes the values' meaning (row trigger, scales,
  rounding, availability rule);
- each feature's kind and scale;
- the row count, rows per instrument, and counts of each unavailability
  reason per feature;
- the rows file's SHA-256 and size.

So:
- the same dataset and configuration give the same ID and bytes;
- any other source, version, or configuration gives another ID;
- a version 1 reader refuses any other version or configuration.

Publication is the same as for datasets:
- a staging directory, `fsync`, a rename that never replaces, and read-only
  files and directory;
- `already_present` or `collision` when the ID exists;
- a `warning` if a step after the rename fails;
- rows and the manifest limited to 64 KiB and 16 MiB, when written as when
  read. Verification checks:
- every row is canonical, in ingest order, on the manifest's stream, and
  available at its receive time;
- the counts and checksum match the manifest.

Only reproduction (`inspect --catalog`) can show that a canonical,
self-consistent row holds the right value.

## Python

Python reads rows with the standard library alone:

```python
import json
from decimal import Decimal

for line in open("features.ndjson"):
    row = json.loads(line)
    microprice = row["features"]["microprice"]
    if "raw" in microprice:
        value = Decimal(int(microprice["raw"])).scaleb(-9)   # exact
```

For the demonstration dataset, a standalone script did the following for all
119,282 rows, and agreed exactly with the C++ output:
- matched each row to its source event in `events.ndjson`;
- recomputed every feature with `fractions.Fraction`;
- rounded half away from zero.

Python is a consumer: it neither writes artifacts nor defines their schema.

## Performance

On the demonstration dataset, with a Release AppleClang build on an
Apple-silicon Mac:

| Operation | Time |
|---|---|
| `features extract` (dataset verification included) | 2.41 s, 119,282 rows |
| `features inspect` (every row) | 2.01 s |
| `features inspect --catalog` (and reproduction) | 4.10 s |

Memory is bounded by the number of instruments and one line.

## Limitations

- One feature set, `top_of_book` version 1, from Level 1 only: no depth,
  trade-flow, or time-window features yet.
- Rows are only emitted on top-of-book events; a status change does not
  emit a row until the next quote.
- The engine is ready for live use but not yet wired to the live server.
- Next: labels (future returns over a declared horizon) as another derived
  artifact, chronological splits, and baseline models.
