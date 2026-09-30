# Chronos normalized market-data contract

## Scope

This document defines the provider-independent market-data values that Chronos components exchange. Provider adapters translate external records into this contract. Replay, market views, features, execution, and accounting must not depend on provider record types.

Newly normalized values are untrusted `MarketEvent` objects. They become `ValidatedMarketEvent` objects only after passing the single-event rules in this document. Components after this boundary accept validated events rather than relying on every caller to remember the same checks.

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

## Validation boundary

`validate_market_event` consumes an untrusted `MarketEvent` and returns one of two explicit outcomes:

- `ValidatedMarketEvent`, which preserves the accepted event and can be constructed only by the validator
- `MarketEventValidationFailure`, which contains every detected error in deterministic order

Expected data-quality failures do not throw exceptions. Each error contains a stable code, a field path, and a human-readable message. This supports machine-readable metrics and useful diagnostics without making malformed external data an exceptional control-flow mechanism. Valid events do not allocate a collection of errors; error collection occurs only on the rejection path.

### Header rules

- Event and receive timestamps must not be negative.
- Receive time may precede event time only when `QuestionableReceiveTimestamp` is present. Distributed clocks and replay sources can make such observations real, so the flag preserves them without silently presenting the timing as trustworthy.
- Every quality bit must belong to the normalized Chronos quality vocabulary. Raw provider bits must be mapped rather than copied.
- Ingest sequence zero is valid. Monotonicity, duplicates, and gaps require stream history and are not single-event rules.

### Trade rules

- Price and quantity must be positive.
- Buy, sell, and unknown aggressor sides are valid. Any value outside the declared enum is rejected.

### Top-of-book rules

- Each present bid or ask level must have positive price and quantity.
- One-sided books and fully cleared books are valid normalized states.
- A locked book, where bid equals ask, is valid.
- A crossed book, where bid exceeds ask, must carry `PotentiallyInvalidBook`. The observation remains recordable, but later execution code must not treat a suspect book as automatically tradeable.

### Market-status rules

The declared unknown, pre-open, open, halted, and closed states are valid. Values outside the declared enum are rejected. A status reason may be empty.

### Instrument-definition rules

- Display symbol must not be empty.
- Minimum price and quantity increments must be positive.
- Effective timestamps must not be negative.
- When present, effective-until must be strictly later than effective-from.

## Stream acceptance boundary

`MarketDataStreamValidator` accepts only `ValidatedMarketEvent` objects and maintains sequence state for exactly one configured Chronos stream. Its initial expected ingest sequence is explicit and defaults to zero.

For every accepted event, the validator requires:

- The event's stream identifier matches the configured stream.
- The ingest sequence equals the next expected value.
- Advancing the sequence does not wrap the unsigned 64-bit range.

It distinguishes an immediate duplicate from a sequence that moved further backward and from a forward gap. A rejected event never advances validator state, so a missing event can still be supplied after a gap is reported. `reset` establishes an explicit new expected boundary for recovery or a new recording segment.

Market timestamps are deliberately not required to increase. Equal, late, and non-monotonic exchange timestamps occur in real feeds; Chronos ingest sequence is the deterministic stream order. A stream may also contain multiple instruments.

### Remaining stateful validation

The generic stream validator deliberately does not interpret provider venue sequences, detect content duplicates that received different Chronos ingest sequences, enforce freshness policy, or check tick-size compliance against the active instrument definition. Those rules require provider metadata or market-view state and belong to the provider adapter and later market view. Keeping these responsibilities separate makes each contract deterministic and independently testable.

## Persisted form

Normalized datasets persist these events exactly, one canonical JSON record
per event with raw fixed-point strings, and return them only after the same
validation ([normalized-market-datasets.md](normalized-market-datasets.md)).

## Current limitations

- Identifiers enforce non-emptiness but not yet character sets or length limits.
- Validation proves the documented single-event and generic Chronos stream-order rules; it does not prove provider-specific sequence integrity or that an event is suitable for execution.
- The initial status vocabulary may not preserve every venue-specific state.
- Instrument identity is symbol-based for the first slice; corporate-action and cross-listing identity require later reference-data work.
- Provider venue-sequence semantics still require dataset-specific adapters and policies.
