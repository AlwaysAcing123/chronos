# Normalized market datasets

A normalized dataset is an immutable, content-addressed directory of Chronos
market events, independent of the provider that delivered them. It is the
historical input for replay, feature extraction, and later model training.
The decision record is
[decisions/0012-normalized-market-dataset-and-features.md](decisions/0012-normalized-market-dataset-and-features.md);
features built from datasets are described in
[feature-engine.md](feature-engine.md).

```text
Databento DBN recording (kept unchanged, never committed)
    -> chronos_databento_normalize
    -> <catalog>/<dataset-id>/manifest.json   what the dataset is
                              events.ndjson   one canonical event per line
                              quality.json    what the events show
    -> NormalizedMarketEventReader            verified MarketEventReader
```

## Commands

Build with the Databento adapter to convert recordings; listing and
inspecting need only the default build.

```sh
cmake -S . -B build-databento -DCMAKE_BUILD_TYPE=Release \
  -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON
cmake --build build-databento --parallel

./build-databento/chronos_databento_normalize \
  --input data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
  --catalog data/normalized

./build-databento/chronos_dataset list --catalog data/normalized
./build-databento/chronos_dataset list --catalog data/normalized --verify
./build-databento/chronos_dataset inspect --catalog data/normalized \
  --dataset 02adcb6d6a1658fb0346b5ac5552131f19c1819cadf5d43d58763e9562855ae8
```

- `chronos_databento_normalize` reads a local EQUS.MINI MBP-1 recording
  through the existing streaming reader. It never uses the network or an API
  key and never overwrites. It creates the catalog directory if its parent
  exists. It prints one JSON document with the dataset summary (ID,
  directory, counts, instruments, time range, checksums, sizes), the outcome
  (`published` or `already_present`), the whole quality report, and
  `warning` (normally `null`; see [Publication](#publication-and-failure)).
- `list` prints every valid dataset in ID order and every other entry of the
  catalog under `invalid_entries`, with the reason. By default it checks each
  dataset's structure, manifest, identity, and quality file; `--verify` also
  re-reads every event.
- `inspect` verifies one dataset completely and prints its summary, its
  manifest, and its quality report.

Success goes to standard output with exit status 0. Every failure is one
document on standard error, exit status 1:

```json
{"error": {"code": "ingest_sequence_gap", "field": "header.ingest_sequence",
           "line": "3", "message": "ingest sequence skipped one or more expected events"}}
```

A recording that cannot be read reports `recording_read_failed` with the
reader's field, message, and source record number; raw record bytes, library
internals, and credentials are never printed. An unexpected failure is still
one JSON document, with code `internal_error`. File names and paths are
printed as they are when they are UTF-8, and any other byte as `\xNN`, so a
stray entry cannot stop a listing. Invalid arguments exit with status 2 and
print usage.

`data/normalized/` is ignored by Git, so it is the natural catalog in a
checkout. Datasets derived from licensed data must never be committed.

## Directory and identity

```text
data/normalized/
  02adcb6d6a1658fb0346b5ac5552131f19c1819cadf5d43d58763e9562855ae8/
    events.ndjson      49,465,951 bytes for the demonstration recording
    manifest.json
    quality.json
```

A dataset directory holds exactly these three regular files, all read-only,
and is itself read-only. Its name is its dataset ID: 64 lowercase hex digits,
the SHA-256 of the canonical manifest with the `dataset_id` field removed.

The manifest names the events and quality files by SHA-256 and size and
records the schema, the provenance, and the summary. The ID therefore covers
every event, the provider's recording checksum, the normalizer's version,
and the schema. It covers no path, machine, user, or clock: converting the
same recording twice, anywhere, gives the same ID and byte-identical files.

There is no catalog database. The catalog is the set of directories whose
contents verify as the dataset their name claims.

## Publication and failure

A conversion writes into a fresh owner-only staging directory named
`.chronos-staging-<pid>-<random>` inside the catalog:
1. `events.ndjson` is written as events stream in, and hashed as it is
   written.
2. `quality.json` and `manifest.json` follow.
3. Every file and the directory are `fsync`ed, and the files become
   read-only.
4. The directory is renamed to the dataset ID with a rename that fails
   rather than replace anything, then made read-only.

- **Complete or absent.** Before the rename there is no dataset; after it
  the dataset is complete.
- **Crashes.** A crash leaves only the staging directory, which `list`
  reports as an interrupted write.
- **Failures.** Any error before the rename removes the staging directory
  and nothing else: invalid events, a sequence gap, a checksum failure, or a
  full disk.
- **After the rename** the dataset is published and the command succeeds. If
  making the directory read-only, or synchronizing the catalog directory,
  then fails, the success document says so under `warning` rather than
  reporting a failure for a dataset that exists.
- **Existing ID.** When the ID already exists, the existing directory must
  verify completely and its manifest must be byte-identical; the conversion
  then reports `already_present` and writes nothing. Otherwise the result is
  `collision`, and the existing entry is not touched: a partial copy, a
  different dataset, a file, or a symlink.
- **Symlinks** inside a catalog are never followed.

## Event records

Each line of `events.ndjson` is one canonical JSON object followed by `\n`.
Every exact value is a string: integers are canonical decimal (`"0"`, no
leading zeros, `-` only before a nonzero value), and prices and quantities
are raw fixed-point integers whose scales the manifest declares (price
1e-9, quantity 1e-8). Timestamps are nanoseconds since the Unix epoch in
UTC. There are no JSON numbers, no optional keys (absent values are
`null`), and keys are in byte order.

Fields in every record:

| Field | Value |
|---|---|
| `version` | record version, `"1"` |
| `type` | `trade`, `top_of_book`, `market_status`, or `instrument_definition` |
| `stream` | market-data stream ID |
| `instrument` | instrument ID |
| `ingest_sequence` | Chronos ingest sequence, unsigned |
| `event_timestamp_ns` | the venue or provider event time |
| `receive_timestamp_ns` | when Chronos's source received it |
| `venue` | venue ID or `null` |
| `venue_sequence` | the provider's venue sequence or `null` |
| `quality` | flags in bit order: `snapshot`, `replay`, `questionable_receive_timestamp`, `potentially_invalid_book`, `source_specific` |

By type:

| Type | Fields |
|---|---|
| `trade` | `price_raw`, `quantity_raw`, `aggressor_side` (`buy`, `sell`, `unknown`) |
| `top_of_book` | `bid`, `ask`: each `null` or `{"price_raw", "quantity_raw"}` |
| `market_status` | `status` (`unknown`, `pre_open`, `open`, `halted`, `closed`), `reason` |
| `instrument_definition` | `display_symbol`, `currency`, `minimum_price_increment_raw`, `minimum_quantity_increment_raw`, `effective_from_ns`, `effective_until_ns` (or `null`) |

A real record:

```json
{"ask":{"price_raw":"768790000000","quantity_raw":"12600000000"},"bid":{"price_raw":"768730000000","quantity_raw":"30000000000"},"event_timestamp_ns":"1790343000001646325","ingest_sequence":"0","instrument":"SPY","quality":["replay"],"receive_timestamp_ns":"1790343000001665218","stream":"databento-equs-mini-mbp1-20260925-1330-1340","type":"top_of_book","venue":null,"venue_sequence":null,"version":"1"}
```

Text fields must be valid UTF-8, since JSON cannot carry other bytes exactly;
an event with other bytes is refused (`unrepresentable_text`) rather than
altered.

### What a reader accepts

The reader returns an event only if all of the following hold:
- the line parses;
- it uses exactly the fields of its type;
- the version is supported;
- every value is canonical and fits;
- the event passes Chronos market-event validation;
- it continues the stream: the manifest's stream, the next ingest sequence,
  non-decreasing receive time, and an instrument the manifest lists;
- re-encoding it gives back exactly the same bytes.

The last check makes every other spelling of the same value an error:
whitespace, key order, escapes, or duplicate flags.

Size limits are part of the format, and the writer enforces the same limits
as the reader, so nothing can be published that cannot be read back:

| Item | Limit |
|---|---|
| Event record | 64 KiB |
| `manifest.json` | 16 MiB (about 500,000 instruments) |
| `quality.json` | 64 MiB (about 100,000 instruments) |

At the end of the file the reader also requires:
- the final newline;
- the manifest's event count, SHA-256, and size;
- the summary the events produce;
- a quality report equal to the one the events produce.

Only then does it return `EndOfMarketEvents` carrying the events' SHA-256.
Existing replay code, including the server's `PacedReplaySource`, therefore
replays a dataset without knowing its provider.

## Manifest

`manifest.json` is one canonical JSON object and a newline. The
demonstration dataset's, indented here for reading:

```json
{
  "counts": {
    "by_event_type": {"instrument_definition": "0", "market_status": "0",
                      "top_of_book": "119282", "trade": "2021"},
    "by_instrument": {"AAPL": "28644", "SPY": "92659"},
    "events": "121303"
  },
  "dataset_id": "02adcb6d6a1658fb0346b5ac5552131f19c1819cadf5d43d58763e9562855ae8",
  "encoding": {"name": "canonical_ndjson", "version": "1"},
  "files": {
    "events": {"name": "events.ndjson",
               "sha256": "b6e1a3456147d53070ef9b9a6fe732e108d3ec7016a03e72a35864f44097dd94",
               "size_bytes": "49465951"},
    "quality": {"name": "quality.json",
                "sha256": "0264bba381e4a0a8b93077722aa034c3d809f272c09e8109f3315b46b1f88965",
                "size_bytes": "1723"}
  },
  "ingest_sequence": {"first": "0", "last": "121302"},
  "instruments": ["AAPL", "SPY"],
  "record": {"name": "chronos.market_event", "version": "1"},
  "scales": {"price_raw": "1000000000", "quantity_raw": "100000000",
             "timestamps": "nanoseconds_since_unix_epoch_utc"},
  "schema": {"name": "chronos.normalized_market_dataset", "version": "1"},
  "source": {
    "coverage": {"end_ns": "1790343600000000000",
                 "semantics": "Databento query window by event timestamp; start inclusive, end exclusive",
                 "start_ns": "1790343000000000000"},
    "description": "Databento EQUS.MINI mbp-1 historical recording",
    "normalizer": {"name": "chronos.databento.equs_mini_mbp1", "version": "1"},
    "provider": "databento",
    "provider_dataset": "EQUS.MINI",
    "provider_schema": "mbp-1",
    "recording_sha256": "75238c7175ce96a735cd6a5a4f998ff272d8a0591524f295e4838a678eaced58"
  },
  "stream": "databento-equs-mini-mbp1-20260925-1330-1340",
  "time_range": {
    "event_timestamp_ns": {"earliest": "1790343000001646325", "latest": "1790343599969499529"},
    "receive_timestamp_ns": {"earliest": "1790343000001665218", "latest": "1790343599969574414"}
  }
}
```

`coverage` is the provider query window, which can be wider than the events
it returned; it is `null` when the window cannot be represented.

The parser checks, and fails closed on, each of these:
- **Form:** canonical bytes and one trailing newline.
- **Fields:** no unknown, missing, or duplicate field.
- **Versions and scales:** the supported schema, encoding, and record
  versions, and the build's scales.
- **Values:** 64-digit lowercase hashes; non-empty identifiers; canonical,
  non-overflowing counts.
- **Identity:** the ID equals its content, and the directory is named by it.
- **Consistency:**
  - the instrument list is sorted, unique, and equals the counted
    instruments;
  - every count is positive;
  - totals agree by type and by instrument;
  - the first-to-last sequence span holds exactly the event count;
  - time ranges are ordered.

## Quality report

`quality.json` follows rules `chronos.dataset_quality` version 1 and depends
only on the events, so it is as reproducible as they are. Money and price
values are raw fixed-point strings.

| Field | Meaning |
|---|---|
| `checks` | The fatal checks every published dataset passed: Chronos validation, one stream, contiguous ingest sequences, non-decreasing receive time |
| `events` | Totals and counts by event type |
| `quality_flags` | Events carrying each flag, and events with none (`unflagged`) |
| `instruments.<id>.events`, `by_event_type` | Per-instrument counts |
| `instruments.<id>.receive_timestamp_ns` | First and last receive time |
| `instruments.<id>.maximum_receive_gap_ns` | Largest receive-time gap between consecutive events of the instrument, or `null` with one event |
| `instruments.<id>.event_time_regressions` | Events whose event timestamp precedes the instrument's previous one, which receive order permits |
| `instruments.<id>.top_of_book` | Quotes that are `two_sided`, `bid_only`, `ask_only`, `empty`, `suspect` (flagged invalid book or questionable receive time), `locked` (bid equal to ask), `crossed` (bid above ask, always flagged), and `valid_two_sided` (two-sided and not suspect); `minimum_spread_raw` and `maximum_spread_raw` over valid two-sided quotes, or `null` |
| `instruments_without_valid_two_sided_quote` | Instruments with no valid two-sided quote |
| `venue_sequence` | Events carrying a venue sequence, and `"gap_detection": "not_performed"` |

It reports and never repairs. Receive-time regressions do not appear because
the input contract forbids them: they abort conversion. Provider sequence
gaps are not claimed either way. EQUS.MINI records carry no venue sequence,
and venue sequences are channel-specific in general, so only Chronos's own
ingest continuity is checked. A provider's documented behavior, such as
exchange timestamps that regress in receive order, is counted, not treated
as corruption.

In the demonstration dataset:
- AAPL has 131 exchange-time regressions and SPY 497.
- The largest AAPL receive gap is 1.506 s.
- SPY has 3 locked quotes and a widest valid spread of 23.13.
- There are no crossed, one-sided, or suspect quotes, and both instruments
  have valid two-sided quotes.

## Errors

| Code | When |
|---|---|
| `invalid_argument` | A writer argument or source field is unusable |
| `catalog_unavailable` | The catalog or output root is not a directory |
| `not_found` | A dataset, file, or directory does not exist |
| `symlink_rejected` | A dataset directory or file is a symlink |
| `unexpected_entry` | A file that does not belong, or a staging directory |
| `malformed_json`, `not_canonical` | A record or document does not parse, or is not in canonical form |
| `unsupported_version` | An unknown schema, record, encoding, feature version, or scale |
| `unknown_event_type`, `unknown_field`, `missing_field`, `invalid_value` | A record's type, keys, or a value are not v1 |
| `invalid_event` | A record fails Chronos market-event validation |
| `line_too_long`, `missing_final_newline` | A record over 64 KiB, written or read; a truncated file or one without its final newline |
| `stream_mismatch`, `ingest_sequence_gap`, `duplicate_ingest_sequence`, `ingest_sequence_moved_backward`, `receive_time_moved_backward`, `unknown_instrument` | The stream contract |
| `identity_mismatch` | A manifest edited without its ID, or a directory named for another ID |
| `checksum_mismatch`, `size_mismatch`, `count_mismatch`, `summary_mismatch`, `quality_mismatch` | Files that disagree with their manifest, or a manifest or report over its size limit |
| `empty_dataset` | Conversion produced no events |
| `unrepresentable_text` | A text field is not valid UTF-8 |
| `counter_overflow` | A count cannot be represented |
| `read_failed`, `write_failed`, `publish_failed` | The file system failed |
| `collision` | Something other than this exact dataset has its ID |
| `internal_error` | An unexpected failure of the command itself |

## Performance

Measured on the demonstration recording (1,925,840-byte `.dbn.zst`, 121,303
events), with a Release AppleClang build on an Apple-silicon Mac:

| Operation | Time |
|---|---|
| Decode and replay the DBN recording (`chronos_databento_replay`) | 0.30 s |
| Convert to a normalized dataset | 1.06 s (about 114,000 events/s) |
| `list` (manifests) | 0.19 s |
| `inspect` or `list --verify` (every event) | 1.24 s (about 98,000 events/s) |

Memory is bounded by the number of instruments and one 64 KiB line, not by
the dataset. The canonical text is about 26 times the compressed recording
and 408 bytes per event. These are measurements, not targets.

## Limitations

- **Size.** Text is large and slow compared with a columnar format; a
  Parquet cache derived from and verified against the canonical dataset is
  the intended remedy when data grows.
- **One stream.** A dataset holds one market-data stream, and events must
  already be in ingest order with non-decreasing receive time.
- **One converter.** Only the Databento EQUS.MINI MBP-1 converter exists.
  The server cannot yet be told to replay a dataset from the command line,
  though its replay source can.
- **Scope of verification.** `list` without `--verify` checks the quality
  file's checksum but not that the events reproduce it.
- **Publication platforms.** It needs `renamex_np` (macOS) or `renameat2`
  (Linux); other platforms refuse to publish.
- **Durability.** Uses `fsync`, not macOS's stronger `F_FULLFSYNC`.
