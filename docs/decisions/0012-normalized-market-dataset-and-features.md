# ADR 0012: Normalized market datasets and point-in-time features

- Status: Proposed
- Date: 2026-09-30

## Context

Every Chronos consumer of historical data so far read a Databento DBN
recording directly through the adapter. That couples research to one
provider's runtime representation and SDK version, gives no stable identity
to "the data a result was computed from", and leaves nothing a Python
training job could read without linking the Databento C++ SDK. Model
evaluation needs data that is independent of the provider, immutable,
reproducible, inspectable, and identified by content, and features that are
provably computed from the past only, identically in research and in live
inference.

## Decision

Chronos adds two provider-independent libraries in the default build and one
converter in the optional Databento build:

```text
Databento DBN recording (raw, preserved, never modified)
    -> chronos_databento_normalize: existing decoder and validation
    -> normalized dataset: <catalog>/<dataset-id>/{manifest,events,quality}
    -> NormalizedMarketEventReader: a MarketEventReader, verified as it reads
    -> TopOfBookFeatureEngine: point-in-time rows from ValidatedMarketEvent
    -> feature artifact: <feature-root>/<artifact-id>/{manifest,features}
```

### Why a normalized store, and why raw files stay

The normalized store holds Chronos's own events: the exact `MarketEvent`
values the replay, the paper-trading server, and the feature engine consume,
after the provider mapping and Chronos validation have run once. Consumers
then depend on the Chronos event contract, not on DBN or its SDK, and the
provider mapping is recorded as provenance rather than re-executed.

The raw recording remains the evidence of what the provider delivered. It is
kept unchanged and ignored by Git; the dataset names its SHA-256. If the
provider mapping is found wrong, the raw file is the only way to rebuild, so
a normalized dataset never replaces it.

### Canonical NDJSON, not PostgreSQL, Parquet, or a binary format

Version 1 stores one canonical JSON object per line (`events.ndjson`):

- **Exact.** Every price, quantity, timestamp, and sequence is a decimal
  string of the raw fixed-point integer, with scales declared once in the
  manifest. No value ever passes through binary floating point in any
  reader: the files contain no JSON numbers at all.
- **Portable and debuggable.** Any language's standard JSON parser reads it;
  `head`, `jq`, and `diff` work on it; a corrupt line can be located.
- **Canonical.** Keys in byte order, no insignificant whitespace, one escape
  per character, one spelling per integer, and a final newline. A reader
  accepts a record only if it is byte-identical to the re-encoding of what it
  parsed, so equal events always have equal bytes and a checksum identifies
  content, not formatting.
- **No new dependency.** The strict parser and SHA-256 live in the default
  build (`chronos_canonical_data`), so the dataset libraries build without
  Databento, PostgreSQL, OpenSSL, or a JSON library.

PostgreSQL would make a server the owner of immutable research data and add
a mutable surface; Parquet needs Arrow, a large dependency, and its logical
types would need their own exactness contract; a custom binary format is
opaque and needs bespoke tools in every language.

**Tradeoffs.** NDJSON is verbose: the 1.9 MB compressed recording of 121,303
events becomes a 49.5 MB `events.ndjson`, and parsing text is slower than
reading columns. For this demonstration dataset that is acceptable: a Release
build converts it in about 1.1 s and verifies it in about 1.2 s (see
[normalized-market-datasets.md](../normalized-market-datasets.md)). It will not
scale to years of full-depth data.

**A future Parquet representation** can be added as a *derived artifact*
next to the canonical dataset, keyed by the same dataset ID, with a manifest
that records the canonical events checksum it was derived from. The logical
schema stays the NDJSON record schema; Parquet would be a cache that can be
regenerated and verified against it, never the source of truth.

### Identity and immutability

The dataset ID is the lowercase hex SHA-256 of the canonical manifest with the
`dataset_id` field removed. The manifest contains the logical schema and
record versions, the encoding, the scales, the stream, the provenance (the
provider, its dataset and schema, the recording's SHA-256, the query window,
and the normalizer name and version), the counts and time ranges, and the
SHA-256 and size of `events.ndjson` and `quality.json`. So:

- the same recording converted with the same schema gives the same ID and
  byte-identical files, on any machine and with any build (Debug and Release
  builds were checked to agree);
- any different event, source checksum, schema, or provenance field changes
  the ID;
- nothing time-, path-, or machine-dependent is in the manifest; there is no
  creation time in version 1.

There is no mutable catalog: a catalog is a directory whose entries are
content-addressed dataset directories. Listing validates each entry and
reports every other entry, including interrupted writes, as invalid.

### Atomic publication and crash behavior

A writer creates a unique owner-only staging directory `.chronos-staging-*`
beside the destination, streams `events.ndjson` into it while hashing it,
then writes `quality.json` and `manifest.json`, `fsync`s every file and the
directory, makes the files read-only, and renames the directory to its ID
with a rename that refuses to replace anything (`renamex_np(RENAME_EXCL)` on
macOS, `renameat2(RENAME_NOREPLACE)` on Linux; other platforms refuse to
publish). The directory is made read-only right after the rename, because
renaming a directory needs write permission on it on some file systems.

- A reader sees either a complete dataset or none: the manifest cannot claim
  files that are partly written.
- A crash leaves only a staging directory, which listing reports and never
  treats as a dataset.
- A failure before the rename, or abandoning the writer, removes only the
  staging directory and the files this operation created. After the rename
  the dataset is published; a later failure to make the directory read-only
  or to synchronize the catalog is returned as a warning with the published
  result, never as an error for a dataset that exists.
- The writer enforces the reader's size limits (64 KiB records, 16 MiB
  manifests, 64 MiB quality reports), so it cannot publish a dataset that
  its reader would refuse.
- If the ID already exists, the existing directory must verify completely and
  have a byte-identical manifest: then the result is `already_present` and
  nothing is written. Anything else there, including a symlink, is a
  `collision` and is left untouched. Symlinks inside a catalog are never
  followed.

### Validation, fatal errors, and the quality report

Conversion enforces the dataset's logical input contract, which is what
replay needs: every event passes Chronos validation, one stream, ingest
sequences contiguous from the first event, and receive time that never
decreases. Any violation, any reader or checksum error, and any
unrepresentable text abort publication. They are never a warning in a
published report.

`quality.json` describes what the published events show, by rules versioned
separately (`chronos.dataset_quality` 1):
- counts by event type, instrument, and quality flag;
- quote shape counts (two-sided, one-sided, empty, locked, crossed, suspect);
- the valid spread range;
- per-instrument exchange-time regressions and the largest receive-time gap;
- instruments without any valid two-sided quote.

It fixes nothing and claims only what the data supports. EQUS.MINI carries no
venue sequence, and venue sequences are provider- and channel-specific in
general, so the report states that provider sequence-gap detection was not
performed. Chronos's own ingest continuity is a separate, enforced contract.

### Versioning and compatibility

Four versions are recorded independently:
- the dataset schema (`chronos.normalized_market_dataset`);
- the event encoding (`canonical_ndjson`);
- the record schema (`chronos.market_event`);
- the quality rules.

A version 1 reader rejects every other version, every unknown field, and
every unknown event type, flag, or enumeration value. Adding a field or
event type is a new record version, and so a new dataset ID; an old reader
fails closed rather than misreading it. Scales are declared and checked, so
a different scale can never be read as the current one.

### Python consumes, never defines

Python will read `events.ndjson` and `features.ndjson` with its standard
`json` module and exact integer or `Fraction`/`Decimal` arithmetic. It does
not write datasets or define the schema; the C++ writer and this ADR do. A
script that recomputed every feature row of the demonstration dataset this
way agreed exactly with the C++ output.

### Point-in-time features

`top_of_book` version 1 computes, for every top-of-book event, after
applying it:
- best bid and ask, and their sizes;
- spread;
- midpoint;
- imbalance `(B − A)/(B + A)`;
- microprice `(ask·B + bid·A)/(B + A)`.

It uses only that event and the instrument's latest market-status event. The
engine keeps no quote history, so no future event can reach a row, and a
row's `available_at` is its triggering event's receive time.

Arithmetic and unavailability:
- **Exact arithmetic.** Checked 128-bit integer arithmetic. Midpoint and
  microprice round half away from zero at the price scale (1e-9); imbalance
  is a ratio with nine decimal places, rounded the same way; spread is exact.
- **One rule for unavailability, applied in order.** A halted or closed
  instrument makes every feature unavailable; so does a suspect quote. A
  missing side makes its own features, and every two-sided one, unavailable.
- **Typed reasons.** Each unavailable feature carries a typed reason and is
  never zero. A zero denominator or an overflow is also a typed reason.
  Validated quotes cannot produce either.

The engine consumes `ValidatedMarketEvent`, so the same code can run in
replay, extraction, and later live inference.

A feature artifact lives in a separate feature root, never inside the
immutable dataset directory:
- **Identity.** Its ID is the SHA-256 of its manifest without the ID, which
  names the source dataset ID and events checksum, the feature set and
  version, the fixed configuration and scales, the counts, and the rows'
  checksum.
- **Publication.** It is published like a dataset.
- **Verification.** It can be verified alone, or reproduced from its source
  dataset, which proves its bytes are exactly what this code computes.

### What comes next

Nothing here depends on labels, splits, or models. The next milestone adds
future-return labels as another derived artifact keyed by dataset ID, with
their own horizon configuration in their identity, and chronological splits
over row identities.

## Alternatives considered

- **Keep DBN as the only historical format.** It ties every consumer to one
  provider and its SDK, and offers no identity for normalized content.
- **PostgreSQL tables for events.** Mutable, server-owned, and harder to
  hand to an offline training job; the database stays for paper trading.
- **Parquet now.** Needs Arrow and an exactness contract for its types; it
  fits better as a derived cache later.
- **An opaque binary encoding.** Smaller and faster, but unreadable without
  Chronos code and harder to audit.
- **JSON numbers for small values.** Readers disagree about large integers
  and some parse into doubles; strings for everything keep one rule.
- **A mutable catalog index.** A second place for the truth that can
  disagree with the directories.
- **Features inside the dataset directory.** The dataset would stop being
  immutable once written.

## Consequences

- Research and replay can run from provider-independent, content-addressed
  datasets. Because the reader is a `MarketEventReader`, the unchanged
  `PacedReplaySource` the server uses replays a dataset exactly (tested); the
  server has no command-line option to select one yet.
- Every derived result can name the exact dataset ID it came from.
- The canonical text format costs space and parse time; a derived columnar
  cache is the planned remedy when data grows.
- Datasets and feature artifacts are generated, often derived from licensed
  data, and stay out of Git (`data/normalized/` and `data/features/` are
  ignored).
