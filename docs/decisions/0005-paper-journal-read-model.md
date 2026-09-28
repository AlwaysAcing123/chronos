# ADR 0005: Paper-journal read model and keyset pagination

- Status: Proposed
- Date: 2026-09-28

## Context

Clients need a durable order and fill blotter, and a way to reconcile a
submission that returned an uncertain `500` or `503`. The journal already
stores every submission with its exact intent, market observation, outcome,
and simulated fill. The read side must return those facts without re-running
risk, execution, or accounting, with bounded work per request, and without
leaking storage details into the API.

## Decision

### Read from the journal through explicit record types

`PostgresPaperOrderRepository` gains `list_orders`, `find_order`, and
`list_fills`, returning `PaperOrderRecord` and `PaperFillRecord`. Outcomes
reuse domain types where the journal is exact (`SimulatedExecutionReport`,
`PreTradeRiskRejection`, `SimulatedExecutionError`); a fault uses
`PaperOrderFaultRecord`, which omits the accounting error because the journal
never stored it. Mapping fails closed on combinations the writer cannot
produce. Enumerations decode through the same `canonical_name` vocabulary
that wrote them, via `from_canonical_name`; no second mapping exists.

### Newest first, keyed by the journal identity

Both lists are newest first by `order_record_id`, the order a blotter needs.
The identity is sufficient as a keyset because, within one account, it
increases in commit order: the sequence allocates in call order and each
record is inserted only after the submission locks the account row. A page
continues with `order_record_id < last`, fetching at most `limit + 1` rows;
concurrent inserts are newer and never appear inside an existing chain.

Migration `002` adds `(account_id, order_record_id)` and a partial index for
booked fills so a page never walks other accounts' records.

### Opaque cursors bound to account and kind

A cursor is the unpadded base64url encoding of
`v1|<orders|fills>|<account_id>|<record id>`. Only the canonical spelling
decodes; version, kind, account, and a positive `int64` are all checked. The
cursor is not a security token: every query is scoped by the requested
account regardless of its contents.

### Validation at the right boundary

The HTTP contract parses query syntax strictly on every route: unknown,
repeated, or malformed parameters are errors, and a limit must be a canonical
integer. The service owns the rules that apply to every transport: limits from
1 to 200 (default 50), cursor binding, and identifier syntax.

### One order resource

Journal records and fresh submissions render through one outcome model, so
`GET .../orders/{client_order_id}` returns exactly the `order` object its
submission returned. The resource gains `rejection`, equal to a recorded
submission's `error`, so a rejected order explains itself when read later.

## Alternatives considered

- **Offset pagination.** Simple, but pages shift when orders arrive and the
  database scans every skipped row.
- **Ascending order.** Natural for audit replay, but a blotter shows recent
  activity first, and an ascending chain would still need a newest-first
  entry point. Ascending lists can be added later with their own cursor kind.
- **Keying on `submitted_at`.** Market time is not unique and a restarted
  replay can repeat it; the identity is unique and commit-ordered per account.
- **Cursor as a raw record ID.** Exposes storage identity, cannot be checked
  against the account or list kind, and could not change format without
  breaking clients.
- **Signed (HMAC) cursors.** Tamper-proof, but a server secret and key
  rotation buy nothing while queries are already scoped by account.
- **Persisting fill notional.** More future-proof if the accounting rounding
  rule ever changes, but it needs a new column, a backfill of existing rows
  with the same formula, and a change to the write path. The read side
  instead derives notional with the accounting function itself.
- **Reconstructing `PaperOrderResult`.** Its accounting receipt contains the
  pre-trade state and realized-PnL change, which are not journaled, so a
  faithful reconstruction is impossible without re-running accounting.

## Consequences

- A blotter and uncertain-submission reconciliation use only persisted facts.
- The history endpoints depend on the per-account identity-order invariant;
  changing how records are inserted requires revisiting this decision.
- Fill notional stays exact only while accounting uses the current rounding
  rule. A future change must version the rule or persist notional.
- A journal row the v1 contract cannot represent makes that read fail with a
  generic `500` instead of returning a misleading resource.
- Existing local volumes must apply migration `002` once; reads remain correct
  without it, only slower.
