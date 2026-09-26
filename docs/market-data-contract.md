# Chronos normalized market-data contract

## Scope

This document defines the provider-independent market-data values that Chronos components exchange. Provider adapters translate external records into this contract. Replay, market views, features, execution, and accounting must not depend on provider record types.

The contract is structural. Semantic acceptance rules and structured validation errors are implemented in the next market-data validation step.

## Strong identifiers

The following identifiers are distinct types even though each stores a nonempty string:

- `InstrumentId`: the normalized instrument identity, such as `AAPL`
- `MarketDataStreamId`: the recording or live-stream identity
- `VenueId`: the market venue when the source identifies one
- `CurrencyCode`: the currency used by an instrument definition

Code cannot accidentally compare or substitute identifiers from different domains. A composite feed may omit `VenueId` rather than inventing a venue.

## Event header

Every normalized event contains one `MarketEventHeader`:

- Instrument
- Market-data stream
- Optional venue
- Source event timestamp
- Chronos receive timestamp
- Chronos ingest sequence
- Optional venue sequence
- Normalized quality flags

The ingest sequence is assigned by Chronos and provides deterministic order within one stream. It is not an exchange sequence number. Timestamps do not replace this ordering field because equal, late, and non-monotonic timestamps occur in real data.

The venue sequence is optional. A source such as Nasdaq TotalView-ITCH can supply one; a derived composite such as `EQUS.MINI` cannot. Components must never interpret a missing venue sequence as zero.

## Quality flags

`MarketDataQuality` contains Chronos-owned flags:

- `Snapshot`: the event belongs to a state snapshot
- `Replay`: the event came from recorded or provider replay data
- `QuestionableReceiveTimestamp`: receive time is not trustworthy
- `PotentiallyInvalidBook`: the source detected an unrecoverable or suspect book state
- `SourceSpecific`: correct interpretation requires source metadata

Adapters map provider flags explicitly. Provider bit values are not stored directly as Chronos quality flags.

## Event types

### Trade event

A trade event contains price, quantity, and aggressor side. Aggressor side is `Buy`, `Sell`, or `Unknown` and is intentionally distinct from the `Side` of a Chronos order.

### Top-of-book event

A top-of-book event contains optional bid and ask levels. Each present level contains price and quantity.

Optional sides are required because valid feeds can be one-sided before an opening, during recovery, or in an illiquid instrument. Both sides may be absent to represent a cleared book. Consumers decide whether their operation requires a two-sided market.

### Market-status event

A market-status event contains a normalized status and an optional human-readable reason. Initial statuses are unknown, pre-open, open, halted, and closed.

### Instrument-definition event

An instrument-definition event contains the display symbol, currency, minimum price increment, minimum quantity increment, and effective-time range. Tick and quantity-increment rules are data, not properties of the fixed-point storage type.

## Event envelope

`MarketEvent` is a closed variant of the supported event types. Downstream code must handle every alternative explicitly. Adding a new event category is therefore a visible interface change rather than an untyped string convention.

An adapter may produce more than one Chronos event from one provider record. For example, a provider record can contain a trade and the resulting top of book. Each emitted Chronos event receives its own ingest sequence so ordering remains total and reproducible.

## Current limitations

- Identifiers enforce non-emptiness but not yet character sets or length limits.
- Event field relationships are not yet validated by constructors.
- The initial status vocabulary may not preserve every venue-specific state.
- Instrument identity is symbol-based for the first slice; corporate-action and cross-listing identity require later reference-data work.
- Stream-level uniqueness and monotonic ingest sequences are properties of a sequence of events, not one event in isolation.
