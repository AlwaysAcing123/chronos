# Databento `EQUS.MINI` MBP-1 adapter

## Responsibility

The adapter is the only layer allowed to depend on Databento record types. Its normalizer converts one `databento::Mbp1Msg` from the `EQUS.MINI` dataset into validated Chronos events. Its file reader streams those records from DBN files, resolves symbols using the recording timestamp, and produces a checksum-backed manifest only after clean end-of-file. Replay, market views, execution, accounting, and models remain unaware of the provider SDK.

The C++ SDK is an optional build dependency. Chronos pins Databento C++ `v0.68.0` at commit `4e0cc2eded5ee3cff280ca5949f0ba251010afbf` and verifies the source archive with SHA-256 `9a10684c8925c850b22e5895fcdc2eccf791044e9ad364b8ab1a77029c18010c`. Ordinary core builds stay offline after their normal configuration; the dedicated adapter build and CI job fetch the pinned SDK.

## Dataset-specific invariants

The normalizer intentionally targets only Databento US Equities Mini MBP-1. It verifies facts that would be wrong for a generic adapter:

- Record type is MBP-1.
- Publisher is `EQUS.MINI.EQUS`.
- Venue sequence is zero because the derived dataset anonymizes component venues.
- Depth is zero.
- Bid and ask order counts are zero because component order counts are not aggregated.
- The symbol has already been resolved from DBN metadata and is nonempty.

Chronos therefore leaves `venue` and `venue_sequence` absent. A later `XNAS.ITCH` adapter will preserve its explicit Nasdaq venue and sequence instead of reusing these rules.

## Event mapping

Every accepted record emits a `TopOfBookEvent`. A Databento Trade action first emits a `TradeEvent`, followed by the top-of-book observation from the same record. Each emitted Chronos event receives a distinct contiguous ingest sequence:

```text
Databento non-trade MBP-1 record -> top of book
Databento trade MBP-1 record     -> trade -> top of book
```

The normalizer assigns sequence state atomically. A rejected record consumes no sequence values. The stateful normalizer is movable but not copyable so two accidental copies cannot issue overlapping ingest sequences.

Databento fixed prices already use the same `10^9` scale as `chronos::Price` and are copied exactly after sentinel and positivity checks. Integral share sizes are checked and converted into the `10^8` fixed quantity scale without floating-point arithmetic.

An undefined Databento book price with zero size becomes an absent side. Any other mixture of undefined price, undefined size, zero size, or nonpositive price is rejected as an inconsistent level.

Trade sides map as follows:

| Databento side | Chronos aggressor |
| --- | --- |
| Bid | Buy |
| Ask | Sell |
| None | Unknown |

## Timestamp and quality mapping

Unsigned provider timestamps are rejected when undefined or outside the signed Chronos nanosecond range before conversion.

Provider flags map explicitly:

| Databento flag | Chronos quality flag |
| --- | --- |
| Snapshot | `Snapshot` |
| Bad receive timestamp | `QuestionableReceiveTimestamp` |
| Maybe bad book | `PotentiallyInvalidBook` |
| Publisher specific | `SourceSpecific` |

Historical mode also adds `Replay`. Databento structural flags such as last, top-of-book, and market-by-price are recognized but do not become quality flags. Unknown provider bits are rejected. A crossed observed book is preserved and marked `PotentiallyInvalidBook` even when the source did not set that flag.

Every generated event passes the provider-independent Chronos validator before it is returned. Any failure is nested inside a structured adapter error, and no sequence state changes.

## File ingestion

`EquitiesMiniMbp1FileReader` enforces the provider boundary before emitting an event:

- The source path is a regular file and remains the same size and modification time through decoding and hashing.
- DBN metadata declares `EQUS.MINI`, MBP-1, and `instrument_id` output symbology.
- The official timestamp-aware symbol map resolves each record by receive date and instrument ID.
- Every decoded record is MBP-1; decoder warnings such as a trailing partial record are terminal errors.
- Every normalized event passes the stateful Chronos stream validator before it is returned.
- Provider, normalization, symbol, stream, file-integrity, and counter failures remain distinct structured errors.

A trade record remains atomic at the provider boundary but yields two calls from the streaming interface: trade first, then its top-of-book observation. The reader buffers at most one normalized event and never loads the recording into memory.

The recording manifest does not exist until the reader reaches a clean end-of-file and verifies the unchanged source. See `docs/recording-manifest.md` for its contract and write-once publication behavior.

## Current boundary

The adapter can now read an existing historical DBN recording and emit validated events. It does not download licensed data, persist normalized events, or replay them on a clock. The next increment will obtain a short real `AAPL` and `SPY` recording, inspect its manifest and events, and preserve a legal repository-safe fixture or reproducible acquisition instructions.

## References

- [Official Databento C++ client](https://github.com/databento/databento-cpp)
- [Databento C++ `v0.68.0`](https://github.com/databento/databento-cpp/releases/tag/v0.68.0)
- [`EQUS.MINI` dataset specification](https://databento.com/docs/venues-and-datasets/equs-mini)
- [MBP-1 schema and fields](https://databento.com/docs/schemas-and-data-formats/mbp-1)
- [Databento common fields, flags, and sentinels](https://databento.com/docs/standards-and-conventions)
