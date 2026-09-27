# Databento `EQUS.MINI` MBP-1 adapter

## Responsibility

The adapter is the only layer allowed to depend on Databento record types. Its first normalization component converts one `databento::Mbp1Msg` from the `EQUS.MINI` dataset into validated Chronos events. Replay, market views, execution, accounting, and models remain unaware of the provider SDK.

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

## Current boundary

This component normalizes already decoded records and a resolved symbol. It does not yet open DBN files, validate file metadata, create the timestamp-aware symbol map, calculate a file checksum, or write a recording manifest. Those responsibilities belong to the next streaming file-source component.

## References

- [Official Databento C++ client](https://github.com/databento/databento-cpp)
- [Databento C++ `v0.68.0`](https://github.com/databento/databento-cpp/releases/tag/v0.68.0)
- [`EQUS.MINI` dataset specification](https://databento.com/docs/venues-and-datasets/equs-mini)
- [MBP-1 schema and fields](https://databento.com/docs/schemas-and-data-formats/mbp-1)
- [Databento common fields, flags, and sentinels](https://databento.com/docs/standards-and-conventions)
