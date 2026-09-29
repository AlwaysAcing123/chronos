# ADR 0001: Use Databento as the primary market-data provider

- Status: Accepted
- Date: 2026-09-26

## Context

Chronos needs real US-equity data for deterministic historical replay, model evaluation, manual paper trading, and eventually live paper trading. The same architecture must support an approachable top-of-book product experience and deeper market-microstructure work without coupling the core engine to one vendor's records.

The provider must support the following project requirements:

- Tick-level trades and quotes rather than bars alone
- Historical data now and live data later
- Level-2 or Level-3 data for order-book reconstruction
- Nanosecond timestamps and explicit data-quality metadata
- Efficient C++ ingestion
- Reproducible downloads and deterministic replay
- A credible path from a small individual project to a public demonstration
- Costs that can remain close to zero until live data is actually required

No market-data source is treated as the complete or canonical view of every US-equity venue. Each recording must identify its provider, dataset, schema, venue coverage, and known limitations.

## Decision

Databento is the primary market-data provider. Chronos will use two Databento datasets for different purposes.

### Product and paper-trading dataset

Use `EQUS.MINI` with the `MBP-1` schema for the first historical replay and the later live paper-trading product.

This dataset provides trades and an aggregated top of book suitable for:

- Best bid and ask display
- Spread and midpoint calculations
- Initial point-in-time features
- Simplified top-of-book execution simulation
- Historical-to-live integration using the same provider schema

`EQUS.MINI` is a derived composite, not the official National Best Bid and Offer. It anonymizes its component venues, reports one aggregated best bid and ask, and sets record sequence numbers to zero. Chronos must not use it for venue-specific sequence-gap detection, full-depth reconstruction, or queue modeling. Product labels and reports must describe it as a Databento synthetic mini-BBO rather than as the complete US market.

The first real recording will contain:

- Dataset: `EQUS.MINI`
- Schema: `MBP-1`
- Symbols: `AAPL` and `SPY`
- Period: a short interval during regular US trading hours
- Delivery: historical DBN, not a live subscription

### Market-microstructure dataset

Use `XNAS.ITCH` with `MBP-10` and later `MBO` for Level-2 and Level-3 work.

This dataset supports:

- Nasdaq order-book reconstruction
- Venue sequence handling
- Add, modify, cancel, and trade processing
- Book invariants and recovery behavior
- Queue-position and passive-fill approximations
- Parser, replay, and book-update benchmarks

`XNAS.ITCH` represents activity on Nasdaq, not a consolidated full-depth US-equity book. Analyses and UI labels must preserve that venue scope.

### Provider boundary

Databento types are confined to a dedicated adapter. The replay engine, market view, feature engine, execution simulator, and accounting system consume Chronos-owned domain types.

The adapter build pins and checksum-verifies an official Databento C++ release. Exact record-to-domain mappings and dependency details are documented in `docs/databento-adapter.md`.

The conceptual boundary is:

```text
Databento DBN or live records
    -> Databento adapter
    -> validated Chronos market events
    -> replay and trading components
```

The provider interface must allow a historical source, live source, and synthetic test source without adding provider conditionals to downstream components. Chronos will not add a second production provider until the first end-to-end Databento path is correct and tested.

### Storage and provenance

Original downloads are immutable inputs. Each recording has a manifest containing:

- Provider, dataset, schema, symbols, and requested time range
- Download time and source parameters
- File size and cryptographic checksum
- Chronos normalizer version
- Record counts and validation results
- Known gaps, corrupt records, or other quality warnings

Raw licensed files, normalized large datasets, credentials, and generated artifacts are not committed to Git. Tests use small synthetic fixtures unless a fixture's redistribution rights have been explicitly verified.

### Cost and live-data policy

Development begins with usage-based historical requests and the provider's cost estimate. Requests remain deliberately small until ingestion metrics and storage requirements are known.

Chronos will not purchase or depend on a live subscription until historical replay, current-market state, and simulated execution work end to end. A private live demonstration may use the Databento US Equities Standard tier. A public multi-user application must not redistribute live records unless the active subscription and written provider terms permit it. The public portfolio demonstration will use historical or sufficiently delayed recordings unless redistribution rights have been confirmed.

### Numeric compatibility

Databento encodes prices as signed integers with a `10^9` scale. Chronos therefore uses a `10^9` price scale while money and quantity use `10^8`. The arithmetic contract and alternatives are documented in `0002-fixed-point-scales.md`.

Undefined provider sentinels and unsigned timestamps are validated before conversion to Chronos types. Unrepresentable values produce structured errors rather than saturation or implicit narrowing.

## Alternatives considered

### Massive

Massive provides broad consolidated US-equity trades, NBBO quotes, historical flat files, and live WebSockets. It was not selected because it does not provide the Level-2 or Level-3 equity data required by the later order-book and queue-modeling work. Its individual plans also do not provide the desired public redistribution path.

### Alpaca

Alpaca provides an inexpensive and approachable stock API. Its free equity feed is limited to IEX, it does not provide the required full-depth equity book, and ordinary API access does not permit redistribution. Using Alpaca's paper broker would also outsource the execution and accounting components that Chronos is intended to demonstrate.

Update (2026-09-29): Databento remains the primary research and historical provider. Alpaca's free IEX stream was later added as an inexpensive, secondary live Level-1 adapter for local paper trading, consuming market data only and labelled as IEX-only everywhere; see [ADR 0009](0009-alpaca-iex-live-adapter.md).

### Direct Nasdaq TotalView-ITCH

A direct feed provides the most authentic raw protocol and remains a valuable future adapter exercise. It was not selected as the primary source because live licensing, distribution rules, connectivity, data volume, and operational work are disproportionate for the first version. Official sample ITCH data may be used later if its use and redistribution terms fit the test-fixture policy.

### Interactive Brokers

Interactive Brokers is appropriate for brokerage and real execution, which are outside Chronos's scope. Its account sessions, subscriptions, pacing limits, and historical-data constraints make it a poor primary research and replay source.

### Kraken

Kraken offers accessible cryptocurrency trades and depth and may become a later asset-class adapter. It was not selected because the primary product and learning objective is US-equity market structure.

## Consequences

### Benefits

- Historical, live, L1, L2, and L3 work can use one provider family.
- The official C++ client and binary records fit the systems-engineering objective.
- Historical usage-based access avoids an early recurring subscription.
- The project can progress from a simple product feed to order-level research.
- Explicit dataset scopes prevent the UI and research reports from overstating market coverage.

### Costs and risks

- Databento is a paid dependency once live data is required.
- Public live redistribution may require a higher subscription tier or a different product model.
- `EQUS.MINI` cannot support per-venue gap detection or a full book.
- `XNAS.ITCH` is a single-venue view and cannot stand in for consolidated US depth.
- A provider adapter and Chronos-owned event schema add deliberate translation work.
- Cross-type arithmetic must preserve the explicit price, money, and quantity scales.

## Verification and revisit conditions

This decision is successful when Chronos can:

1. Normalize a real `EQUS.MINI` recording without floating-point conversion.
2. Replay it repeatedly with identical ordering and final state.
3. Maintain a validated top-of-book view and expose data staleness.
4. Generate simulated fills that reconcile through the existing paper account.
5. Reconstruct and validate a Nasdaq book from a small `XNAS.ITCH` recording.

Revisit the provider decision only if:

- Required data becomes unavailable or unaffordable
- Licensing prevents the intended demonstration model
- Data quality cannot satisfy documented invariants
- A necessary asset class or venue cannot be added through the provider boundary
- A competing source materially improves fidelity, cost, and legal usability without fragmenting replay and live behavior

## References

- [Databento US Equities catalog and current plan details](https://databento.com/catalog/us-equities)
- [Databento US Equities Mini specification](https://databento.com/docs/venues-and-datasets/equs-mini)
- [Databento C++ historical client guide](https://databento.com/docs/getting-started/historical?historical=cpp&live=cpp)
- [Databento live client and intraday replay](https://databento.com/docs/api-reference-live/client/live)
- [Databento common fields, timestamps, and flags](https://databento.com/docs/standards-and-conventions/common-fields-enums-types)
- [Nasdaq TotalView product description](https://www.nasdaq.com/products/data/equities/nasdaq-totalview)
