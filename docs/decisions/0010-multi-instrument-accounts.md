# ADR 0010: Multi-instrument paper accounts

- Status: Proposed
- Date: 2026-09-29

## Context

A paper account held exactly one instrument: the instrument was fixed at
creation, the account row stored its position, and an order for any other
symbol was recorded as the risk rejection `instrument_mismatch`. A trader
comparing AAPL and SPY needed two accounts with two cash balances, which is
not how a brokerage account behaves and makes a portfolio across symbols
impossible. The server already streams several instruments on one market
clock, so the market side needed no new capability; the account, risk,
execution, persistence, API, and browser layers did.

## Decision

### An account is cash plus a ledger per instrument

An account holds one USD cash balance and zero or more positions keyed by
instrument. It is long-only and immediate-or-cancel, as before. Every order
names exactly one instrument; there are no multi-leg orders.

A position's ledger — quantity, open cost basis, gross realized PnL, fees
paid, last fill time — is created by the instrument's first fill and never
removed. A closed position is the same ledger with zero quantity and zero
open cost basis, so its realized PnL and fees stay part of the account and
the account totals never lose a term. Account open cost basis, realized PnL,
and fees are derived sums, never stored twice. The identity

```text
cash + Σ open cost basis = starting cash + Σ gross realized PnL − Σ fees
```

is checked after every fill, on recovery, and by PostgreSQL at commit.
Iteration is in instrument order everywhere, so every snapshot, row set, and
response is deterministic.

### Scopes

| Concern | Scope |
|---|---|
| Cash, buying power, fees charged | account |
| Client order ID uniqueness, applied fill IDs | account |
| Serialization of writes (optimistic version) | account |
| Market-data stream binding | account: one stream for every instrument |
| Account timeline (order time ≥ latest fill, valuation clock) | account |
| Position, cost basis, realized PnL, sell coverage | account + instrument |
| Fill-time ordering in accounting | account + instrument |
| Execution lineage (sequence order, one state per sequence) | account + instrument |
| Displayed-liquidity consumption | account + instrument |
| Quote lookup and freshness | instrument |
| Maximum position quantity | account + instrument |
| Maximum order quantity and notional | order |

Accounts are independent simulated worlds: two accounts trading the same
quote each see its full displayed size.

Fill-time ordering is enforced per instrument because accounting alone
cannot know about the shared clock, and requiring it across instruments would
reject correct histories when two instruments' fills are applied out of
receive order. The account-wide rule lives where the clock is known: risk
rejects an order timestamped before the account's latest fill in any
instrument, and valuation refuses a market clock behind it.

### Risk previews the full accounting effect

Account totals are sums, so a fill that fits its own ledger can still make a
total unrepresentable. Before approving, risk applies the candidate fill to a
copy of the account (`PaperAccount::preview_fill`) and rejects an overflow as
the recorded risk rejection `arithmetic_overflow`. Without this, the
processor would reach accounting with a risk-approved fill that accounting
must refuse, and would halt the account for a condition that is an ordinary
limit.

### Tradeable instruments and quotes

The server's configured instruments are the tradeable set. An order for any
other instrument is refused with `422 unsupported_instrument` before anything
is recorded: there is no quote to execute or journal against. The service
reads only the order instrument's quote. Risk still rejects an observation
for another instrument, so an AAPL quote can never fill an SPY order even if
a caller passes one.

`PreTradeRiskErrorCode::InstrumentMismatch` and
`SimulatedExecutionErrorCode::InstrumentMismatch` are no longer produced but
stay declared: their names are persisted in historical journal rows, and the
code vocabulary requires contiguous values.

### Valuation is all or nothing over held positions

Only open positions need a mark. An instrument the account does not hold,
including a closed position, never blocks valuation, so a flat account is
valued at its cash even with no quote at all. Every open position is marked
at its own best bid, all marks and the market clock come from one consistent
`MarketState` read (`MarketState::quotes`), and if any open position cannot be
marked the whole valuation fails with `PositionMarkUnavailable`, listing every
such position with its reason. A missing or stale position is never valued as
zero, and there is no partial total.

The checks run in a fixed order: the account's stream, a market clock behind
the latest fill, each open position, then a missing market clock. Before any
market event an open position therefore reports that it has no quote, rather
than a timeline error, while a replay behind the account is still reported
for the account as a whole.

### Persistence is normalized

Migration 003 moves the position into `paper_positions` and the execution
state into `paper_execution_states`, both keyed by `(account_id,
instrument)`, and adds `paper_accounts.market_stream_id`. Foreign keys make a
position require its execution state and every execution state use the
account's stream; deferred constraint triggers check the identity and the
stream binding at commit. The single-instrument columns are dropped, so no
value has two writable homes. A commit writes the account row and only the
rows the order changed.

The journal keeps its account-level "after" snapshot as the cash plus one
instrument's ledger, and records that instrument: the order's for new rows,
the account's single instrument for migrated ones. These columns remain audit
data; recovery reads only the snapshot tables and the journal's IDs.

The migration validates old data before changing anything, is one
transaction, is idempotent through `chronos_schema_migrations`, and takes its
locks in the same order as the earlier files so concurrent appliers cannot
deadlock.

### API and browser

Account creation takes only `account_id` and `starting_cash`; an `instrument`
field is rejected, not ignored, so an old client learns that its assumption
no longer holds. Account and portfolio resources list open positions and
closed positions separately, with server-computed totals. History accepts an
optional `instrument` filter; cursors are bound to it, and unfiltered cursors
keep their original encoding. The market-data resource adds each
instrument's current quote, so a client can show a reference quote for a
symbol the account does not hold. The browser keeps one selected symbol that
drives the watchlist, quote detail, and ticket, and does no financial
arithmetic.

## Deferred, with hooks

- **Resting (GTC) orders and cancel/replace.** A resting order needs an order
  state machine, working-order persistence, and a rule for which future
  observations it may fill against and how its reservation of shared cash
  interacts with other instruments. Immediate-or-cancel keeps every order's
  life inside one transaction, which is what makes the account-wide version
  check sufficient today. The execution state per instrument is where resting
  quantity would attach.
- **Authentication and ownership.** Accounts are identified by ID only on a
  localhost server. Ownership needs a principal on every request and an
  owner on every account, and a manual order's `source.id` should come from
  it; that is a transport and service concern independent of how many
  instruments an account holds, so it stays with the authentication
  milestone.
- **Currencies, shorting, and margin.** A second currency needs per-currency
  cash and conversion; shorting and margin need borrow and collateral rules.
  The ledger shape (signed realized PnL, non-negative quantity) would change
  with shorting, so it is not anticipated beyond keeping ledgers per
  instrument.
- **A shared exchange.** Accounts stay independent worlds; a shared book
  would make one account's fills depend on another's, which the per-account
  execution state deliberately avoids.

## Alternatives considered

- **One account per instrument.** The existing model. It keeps each account
  simple but splits buying power and makes a cross-symbol portfolio a client
  computation.
- **Delete a ledger when its position closes.** Fewer rows, but the account
  totals would then need a separate stored realized-PnL and fees balance —
  a second source of truth — or would lose history.
- **Store account totals.** Faster reads, but two writable values for one
  fact. The sums are over a handful of rows.
- **Enforce fill-time order across instruments in accounting.** Simpler to
  state, but it rejects valid histories and duplicates a rule the market
  clock already enforces where it is known.
- **Value what can be marked and report the rest.** A partial total is
  easy to mistake for the account's value; the API returns an exact value or
  an error.
- **Accept and ignore `instrument` on account creation.** Friendlier to old
  clients, but silently changes what their request means.
- **Rewrite the whole account snapshot on every commit.** Simpler code, but
  it rewrites every position row on every order and hides which rows an order
  touched.

## Consequences

- Orders for different instruments on one account serialize on the account
  version; concurrent ones conflict and are retried by the client, which
  re-runs risk against the remaining cash.
- An account binds to one stream for all instruments, so a replay and a live
  session, or two recordings, still need separate accounts.
- Historical journal rows remain readable, including the retired
  `instrument_mismatch` rejections, and keep their original client order IDs:
  a new order cannot reuse one, even for another symbol.
- Existing Docker volumes must apply migration 003 once; fresh volumes and
  the integration tests apply it automatically.
- Currencies other than USD, margin, shorting, and a shared exchange across
  accounts remain out of scope.
