# ADR 0002: Use type-specific fixed-point scales

- Status: Accepted
- Date: 2026-09-26

## Context

Chronos requires deterministic arithmetic across market-data ingestion, execution simulation, and accounting. Binary floating point cannot exactly represent many decimal values, while a single decimal scale does not match every domain value.

Databento prices use signed integers with a `10^9` scale. The first Chronos accounting implementation used a shared `10^8` scale for price, money, and quantity. Keeping that shared scale would require rounding provider prices before the market-data adapter could validate or store them.

Price precision, monetary settlement precision, quantity precision, and instrument tick size are separate concepts. Tick-size validation belongs to instrument metadata and order validation; it must not be encoded by reducing the storage precision of the `Price` type.

## Decision

Use distinct strong fixed-point types backed by signed 64-bit integers:

| Type | Scale | Smallest stored unit |
|---|---:|---:|
| `Price` | `10^9` | `0.000000001` |
| `Money` | `10^8` | `0.00000001` |
| `Quantity` | `10^8` | `0.00000001` |

`FixedDecimal<Tag, Scale>` owns exact string parsing, canonical formatting, comparison, checked addition, and checked subtraction. Its scale must be a positive power of ten known at compile time.

Cross-type arithmetic remains in named domain functions rather than generic mixed-type operators:

```text
notional money raw = round(price raw * quantity raw / price scale)

average price raw  = round(cost-basis raw * price scale / quantity raw)

allocated cost raw = round(total-cost raw * part-quantity raw
                           / whole-quantity raw)
```

The notional formula relies on `Money` and `Quantity` having the same scale. A compile-time assertion protects that assumption. If their scales diverge later, the formula must be redesigned explicitly rather than silently producing different units.

Multiplication uses signed 128-bit intermediates. Every result is range-checked before narrowing to signed 64-bit storage. Parsing never passes through `float` or `double`.

The default rounding mode is nearest with exact halfway values rounded away from zero. Toward-zero rounding remains available when a caller requires it. Accounting documentation identifies the unit to which each result is rounded.

## Alternatives considered

### Keep one `10^8` scale

This minimizes code changes but cannot represent Databento's complete normalized price field. Rejecting or rounding valid input at the provider boundary would make the normalized record lossy.

### Use `10^9` for every type

This makes all raw scales equal but changes the accounting contract unnecessarily, reduces the representable whole-value range for money and quantity, and confuses storage precision with actual settlement or quantity requirements.

### Use binary floating point

This is appropriate inside many statistical models but not for authoritative orders, fills, cash, cost basis, or reconciliation. Results would depend on binary approximation and implicit rounding points.

### Use an arbitrary-precision decimal library

This provides a wider range and flexible precision but adds allocation, dependency, serialization, and performance costs that are not justified by the current instruments. Chronos can revisit this choice if supported assets exceed the signed 64-bit ranges.

### Store instrument ticks as integer units

Tick-unit storage is efficient for one known instrument but does not provide one stable representation across instruments whose tick rules differ or change. Chronos will store precise prices and validate ticks against versioned instrument definitions.

## Consequences

### Benefits

- Databento prices can enter Chronos without precision loss.
- Money and quantity retain their existing precision and range.
- Invalid mixed-type arithmetic remains difficult to express accidentally.
- Unit conversions and rounding occur only at named, tested boundaries.

### Costs and limitations

- Cross-type formulas must account for different scales explicitly.
- A price may be representable even when the resulting notional rounds to zero money units; such fills are rejected.
- Signed 64-bit storage still imposes finite ranges.
- Instrument tick and lot-size validation must be implemented separately.

## Verification

The fixed-point and accounting tests cover:

- Each type's scale and fractional-digit limit
- Positive and negative signed 64-bit boundaries
- Canonical string round trips
- Half-away-from-zero and toward-zero behavior
- Exact price-to-notional-to-average-price round trips
- Overflow rejection
- Existing buy, sell, fee, cost-basis, and PnL behavior

The test executable is also run with AddressSanitizer and UndefinedBehaviorSanitizer while this representation is introduced.
