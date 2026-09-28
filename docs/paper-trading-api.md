# Chronos paper-trading HTTP API

## Status and safety

The API exposes the durable paper-trading path to a local client. It is
**simulated trading only**: no component can route an order to a broker,
exchange, or real-money account.

The API is **not production-secure**. It has no authentication,
authorization, TLS, or rate limiting. The server binds to `127.0.0.1` by
default and must not be exposed to other hosts. Every account and order
resource carries `"trading_mode": "paper"`, and every response carries the
`X-Chronos-Trading-Mode: paper` header.

The decision records are
[decisions/0004-paper-trading-http-api.md](decisions/0004-paper-trading-http-api.md)
and, for portfolio valuation,
[decisions/0006-portfolio-valuation.md](decisions/0006-portfolio-valuation.md).

## Architecture

```text
HTTP client
  -> PaperTradingHttpServer        cpp-httplib; the only HTTP-library code
  -> PaperTradingHttpApi           routes, methods, media type
  -> JSON contract                 strict parsing; typed results -> JSON
  -> PaperTradingService           identifiers, quote, clock, durable session
       -> PersistentPaperOrderSession
            -> risk -> top-of-book execution -> accounting
            -> PostgreSQL snapshot and journal (one transaction)

replay driver or future live feed -> MarketState -> PaperTradingService

HTTP GET history -> PaperTradingService -> journal read model
                                           (persisted rows only)

HTTP GET portfolio -> PaperTradingService -> durable account + MarketState
                                          -> value_portfolio (core)
```

Business rules live only in the core and the service. The HTTP layer never
evaluates risk, computes money, or touches PostgreSQL. Manual and model orders
use the same endpoint, service call, and `OrderIntent` path; they differ only
in `source.kind`.

## Running locally

Build the API with the PostgreSQL adapter. Adding the Databento adapter lets
the server replay a local recording into its market state at startup.

```sh
docker compose up -d --wait postgres
cmake -S . -B build-api \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON \
  -DCHRONOS_ENABLE_PAPER_TRADING_API=ON \
  -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON \
  -DPostgreSQL_ROOT=/opt/homebrew/opt/libpq
cmake --build build-api --parallel

CHRONOS_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ./build-api/chronos_paper_trading_server \
    --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
    --instrument AAPL --instrument SPY \
    --replay-dbn data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst
```

The database URL is read from `CHRONOS_POSTGRES_URL`, never from the command
line. The server replays the whole recording before listening, so it serves
the recording's final validated market; replay is not paced. Without
`--replay-dbn`, the market is empty and every order returns
`market_data_unavailable`. `SIGINT` or `SIGTERM` stops the server cleanly.
The server refuses to start on a port that is already in use; the HTTP
library's default `SO_REUSEPORT` is disabled so that an accidental second
server cannot silently share requests with the first.

Server options and their defaults:

| Option | Default | Meaning |
|---|---|---|
| `--host` | `127.0.0.1` | Listening address |
| `--port` | `8080` | Listening port |
| `--instrument` | required, repeatable | Instruments accounts may trade |
| `--max-order-quantity` | `1000` | Risk limit applied to new accounts |
| `--max-order-notional` | `1000000` | Risk limit applied to new accounts |
| `--max-position-quantity` | `10000` | Risk limit applied to new accounts |
| `--max-quote-age-ms` | `5000` | Maximum quote age in market time |
| `--fee-per-fill` | `0` | Flat fee per non-empty fill |

Risk limits are server configuration, persisted with each account when it is
created. Clients cannot choose them, and later configuration changes do not
alter existing accounts.

Example session:

```sh
curl -s localhost:8080/health

curl -s -X POST localhost:8080/api/v1/accounts \
  -H 'Content-Type: application/json' \
  -d '{"account_id":"demo-1","instrument":"AAPL","starting_cash":"100000"}'

curl -s -X POST localhost:8080/api/v1/accounts/demo-1/orders \
  -H 'Content-Type: application/json' \
  -d '{"client_order_id":"buy-1","source":{"kind":"manual","id":"me"},
       "instrument":"AAPL","side":"buy","type":"limit","quantity":"10",
       "limit_price":"337","time_in_force":"ioc"}'

curl -s localhost:8080/api/v1/accounts/demo-1
curl -s 'localhost:8080/api/v1/accounts/demo-1/orders?limit=20'
curl -s localhost:8080/api/v1/accounts/demo-1/orders/buy-1
curl -s localhost:8080/api/v1/accounts/demo-1/fills
curl -s localhost:8080/api/v1/accounts/demo-1/portfolio
```

The history endpoints read indexes added by
`migrations/002_paper_order_read_indexes.sql`. PostgreSQL runs the mounted
migrations only when it initializes an empty volume, so an existing volume
needs the new file applied once. It is idempotent:

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
```

The file is sent on standard input rather than read from the container's
`/docker-entrypoint-initdb.d` mount, which Docker Desktop on macOS may present
as empty when it lacks access to the repository's folder.

The endpoints return correct results without the indexes, but each page may
then scan far more of the journal.

## Representation rules

### Requests

- `POST` bodies must be `Content-Type: application/json`, optionally with
  `charset=utf-8`. Anything else returns `415`. Requiring JSON also means a
  browser cannot submit a cross-origin order without a CORS preflight, which
  the server never approves.
- The body must be one JSON object in valid UTF-8. Comments are rejected.
- Unknown fields are rejected, so a misspelled `limit_price` cannot silently
  become a market order.
- Duplicate keys are rejected; parsers disagree about which duplicate wins.
- Optional fields are omitted when absent. `null` is not accepted.
- Clients never send a quote, price source, submission time, account ID in
  the body, or risk limit.
- Query strings are strict on every route. The history lists accept only
  `limit` and `cursor`, each at most once; every other route accepts no query
  parameters. Unknown, repeated, or malformed parameters return `400` rather
  than being ignored. Names and values are percent-decoded; `+` is not a
  space.

### Decimals

Money, prices, and quantities are JSON strings, never JSON numbers. A request
decimal must match:

```text
-?(0|[1-9][0-9]*)(\.[0-9]+)?
```

Plus signs, exponents, leading zeros, whitespace, digit separators, and
non-ASCII digits are rejected. Fractional digits beyond the field's scale are
rejected rather than rounded. Trailing fractional zeros are accepted.

| Field kind | Fields | Scale | Supported range |
|---|---|---:|---|
| Quantity | `quantity` | 8 | ±92,233,720,368.54775807 |
| Price | `limit_price` | 9 | ±9,223,372,036.854775807 |
| Money | `starting_cash` | 8 | ±92,233,720,368.54775807 |

The minimum of each range is one unit lower than the negated maximum. A value
outside the range returns `decimal_out_of_range`.

A well-formed decimal that violates a domain rule is not rejected by the HTTP
layer. For example, `"quantity": "0"` reaches the risk engine and is recorded
as `nonpositive_quantity`.

Responses render decimals canonically, as produced by the fixed-point types:
no exponent, no plus sign, no trailing fractional zeros, and no decimal point
for whole values (`"101"`, `"0.01"`, `"-2.5"`). Response money and quantities
have at most 8 fractional digits and prices at most 9. The portfolio's
`return_percent` is a percentage with at most 8 fractional digits.

### Integers, timestamps, and absent values

- Responses contain no JSON numbers at all. Integers such as `version`,
  `ingest_sequence`, and `maximum_quote_age_ns` are base-10 strings, because
  JSON numbers above 2⁵³ lose precision in common clients.
- Timestamps are RFC 3339 UTC strings with exactly nine fractional digits,
  such as `"2026-09-25T13:39:59.969574414Z"`. They are exact to the
  nanosecond and sort lexicographically.
- Absent optional values are `null` in responses, so every resource has a
  fixed set of keys.

### Identifiers

`account_id`, `client_order_id`, `source.id`, and `instrument` must be 1 to 64
characters from `[A-Za-z0-9._-]` and start with a letter or digit. The rule
keeps identifiers safe in URL paths, logs, and PostgreSQL text parameters. A
violation returns `400 malformed_request` with reason `invalid_identifier`
and is not recorded.

## Endpoints

### `GET /health`

Returns `200` when PostgreSQL answers a probe and `503` when it does not. The
market-data section is informational: an empty or interrupted market does not
make the server unhealthy, but it does block orders.

```json
{
  "status": "ok",
  "trading_mode": "paper",
  "persistence": "available",
  "market_data": {
    "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
    "state": "accepting",
    "last_ingest_sequence": "121302",
    "market_time": "2026-09-25T13:39:59.969574414Z",
    "interruption": null
  }
}
```

`state` is `accepting` or `interrupted`; `interruption` then describes the
rejected event.

### `POST /api/v1/accounts`

```json
{"account_id": "demo-1", "instrument": "AAPL", "starting_cash": "100000"}
```

`instrument` must be one of the server's tradeable instruments and
`starting_cash` must be positive. Success returns `201` with an account:

```json
{
  "account": {
    "trading_mode": "paper",
    "account_id": "demo-1",
    "instrument": "AAPL",
    "version": "0",
    "status": "active",
    "starting_cash": "100000",
    "cash": "100000",
    "position": "0",
    "open_cost_basis": "0",
    "average_entry_price": null,
    "gross_realized_pnl": "0",
    "fees_paid": "0",
    "last_fill_at": null,
    "risk_limits": {
      "maximum_order_quantity": "1000",
      "maximum_order_notional": "1000000",
      "maximum_position_quantity": "10000",
      "maximum_quote_age_ns": "5000000000",
      "fee_per_fill": "0.35"
    }
  }
}
```

`status` becomes `halted` if the account's processor faulted after an
accounting invariant violation. `average_entry_price` is `null` while flat.

### `GET /api/v1/accounts/{account_id}`

Returns `200` with the same account resource, read from a consistent database
snapshot. It holds no market-dependent values; mark-to-market valuation is the
portfolio resource below.

### `GET /api/v1/accounts/{account_id}/portfolio`

Values the durable account against the server's current market. It is
read-only: it records nothing, changes no account version, and consumes no
shadow liquidity or client order ID. It accepts no query parameters and no
other method. The client cannot supply or influence the mark.

This is the response for the recording's final AAPL quote after a 10-share
market buy that filled 4 at 337 and a model sell of 1.5 at 335.82, with a
0.35 fee per fill:

```json
{
  "portfolio": {
    "trading_mode": "paper",
    "account_id": "demo-1",
    "instrument": "AAPL",
    "account_version": "2",
    "status": "active",
    "valued_at": "2026-09-25T13:39:59.969574414Z",
    "mark_policy": "best_bid",
    "mark_price": "335.82",
    "market": {
      "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
      "ingest_sequence": "121297",
      "venue": null,
      "event_timestamp": "2026-09-25T13:39:59.960824207Z",
      "receive_timestamp": "2026-09-25T13:39:59.960922473Z",
      "quality_flags": ["replay"],
      "bid": {"price": "335.82", "quantity": "80"},
      "ask": {"price": "337", "quantity": "4"}
    },
    "starting_cash": "100000",
    "cash": "99155.03",
    "position": "2.5",
    "open_cost_basis": "842.5",
    "average_entry_price": "337",
    "position_market_value": "839.55",
    "unrealized_pnl": "-2.95",
    "gross_realized_pnl": "-1.77",
    "fees_paid": "0.7",
    "total_pnl": "-5.42",
    "equity": "99994.58",
    "return_percent": "-0.00542"
  }
}
```

**Mark policy.** Accounts are long-only, so the position is marked at the
current displayed best bid (`mark_policy: "best_bid"`), the price at which it
could begin to be sold. **This is mark-to-market valuation, not guaranteed
liquidation proceeds**: it ignores the bid's displayed size, queue position,
market impact, latency, and the fee a closing sale would pay. A position
larger than `market.bid.quantity` could not be sold at the mark in this
simulator either.

A best bid is required even when the account is flat. The server never falls
back to the ask, midpoint, last trade, cost basis, or zero; without a usable
bid it returns `503` instead of a value.

**Fields.** `market` is the exact observation whose best bid is the mark, in
the same shape as an order's `market`. `valued_at` is the market clock when it
was read: the receive timestamp of the latest event accepted on the stream,
which may be later than `market.receive_timestamp` when other instruments
updated since. `account_version` is the durable version valued. The account
fields have the account resource's meaning; `status` is `halted` for a
faulted account, which can still be valued.

**Formulas.** All values are exact fixed-point decimals computed with checked
arithmetic:

```text
position_market_value = round(mark_price × position)          to 0.00000001
unrealized_pnl        = position_market_value − open_cost_basis
equity                = cash + position_market_value
total_pnl             = equity − starting_cash
                      = gross_realized_pnl + unrealized_pnl − fees_paid
return_percent        = round(total_pnl / starting_cash × 100) to 0.00000001
```

`round` is half away from zero, the same rule as every fill's notional. The
server verifies both definitions of `total_pnl` before responding; they agree
exactly for every valid account. `return_percent` is in percentage points:
`"-0.00542"` means −0.00542%, not −0.542%. A value outside the fixed-point
range fails with `500 valuation_failed` rather than being approximated.

**Eligibility.** The observation must be a valid top of book for the
account's instrument without a suspect quality flag, it must be no older than
the account's `maximum_quote_age_ns` relative to `valued_at`, and it must
continue the account's market timeline (see
[Accounts are bound to one market timeline](#accounts-are-bound-to-one-market-timeline)).

### `POST /api/v1/accounts/{account_id}/orders`

```json
{
  "client_order_id": "buy-1",
  "source": {"kind": "manual", "id": "me"},
  "instrument": "AAPL",
  "side": "buy",
  "type": "limit",
  "quantity": "10",
  "limit_price": "337",
  "time_in_force": "ioc"
}
```

| Field | Values |
|---|---|
| `source.kind` | `manual`, `model` |
| `side` | `buy`, `sell` |
| `type` | `market`, `limit` |
| `time_in_force` | `ioc` |
| `limit_price` | required for `limit`, must be omitted for `market` |

Enumerations are case-sensitive and closed.

The server executes the order against its own current quote for the
account's instrument and records it. An executed order returns `201`. This is
the response to a 10-share market buy against the recording's final AAPL
quote, where only 4 shares were displayed:

```json
{
  "order": {
    "trading_mode": "paper",
    "account_id": "demo-1",
    "client_order_id": "buy-1",
    "source": {"kind": "manual", "id": "me"},
    "instrument": "AAPL",
    "side": "buy",
    "type": "market",
    "quantity": "10",
    "limit_price": null,
    "time_in_force": "ioc",
    "submitted_at": "2026-09-25T13:39:59.969574414Z",
    "status": "partially_filled_and_cancelled",
    "filled_quantity": "4",
    "unfilled_quantity": "6",
    "cancellation_reason": "displayed_liquidity_exhausted",
    "fill": {
      "fill_id": "buy-1:fill:0",
      "side": "buy",
      "price": "337",
      "quantity": "4",
      "notional": "1348",
      "fee": "0.35",
      "filled_at": "2026-09-25T13:39:59.969574414Z"
    },
    "rejection": null,
    "market": {
      "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
      "ingest_sequence": "121297",
      "venue": null,
      "event_timestamp": "2026-09-25T13:39:59.960824207Z",
      "receive_timestamp": "2026-09-25T13:39:59.960922473Z",
      "quality_flags": ["replay"],
      "bid": {"price": "335.82", "quantity": "80"},
      "ask": {"price": "337", "quantity": "4"}
    }
  },
  "account": {"...": "the account resource after processing"}
}
```

`status` is `filled`, `partially_filled_and_cancelled`, or `cancelled` for an
executed order. A cancelled order has `cancellation_reason`
`limit_not_marketable` or `displayed_liquidity_exhausted` and `fill: null`.
`market` is the exact observation the server used, including its lineage.

A recorded rejection returns `422` with the same `order` and `account`
objects, `order.status` set to `rejected`, and an `error` object. A recorded
processing fault returns `500` with `order.status` set to `faulted`.

`order.rejection` is `null` for an executed order. For a rejected or faulted
order it has the error shape below and equals the submission's `error`, so an
order resource read back later explains itself. A faulted order reports no
fill even though the journal keeps the simulated fill that accounting
rejected.

### `GET /api/v1/accounts/{account_id}/orders`

Lists the account's recorded submissions, newest first: filled, partially
filled, cancelled, risk-rejected, execution-rejected, and faulted orders.
Every item is the order resource described above, read from the journal
without re-running risk, execution, or accounting.

```json
{
  "orders": [
    {"client_order_id": "buy-2", "status": "rejected", "...": "..."},
    {"client_order_id": "buy-1", "status": "filled", "...": "..."}
  ],
  "next_cursor": "djF8b3JkZXJzfGRlbW8tMXw0Mg"
}
```

An existing account with no orders returns `{"orders": [], "next_cursor":
null}`; an unknown account returns `404 account_not_found`.

### `GET /api/v1/accounts/{account_id}/orders/{client_order_id}`

Returns `{"order": ...}`, the complete persisted outcome of one submission,
with exactly the `order` object its submission returned. An existing account
without that order returns `404 order_not_found`; an unknown account returns
`404 account_not_found`.

### `GET /api/v1/accounts/{account_id}/fills`

Lists the fills accounting booked, newest first, with the same pagination.
Only executed orders book fills; a faulted order's rejected simulated fill is
never listed.

```json
{
  "fills": [
    {
      "trading_mode": "paper",
      "account_id": "demo-1",
      "client_order_id": "buy-1",
      "source": {"kind": "manual", "id": "me"},
      "instrument": "AAPL",
      "fill_id": "buy-1:fill:0",
      "side": "buy",
      "price": "337",
      "quantity": "4",
      "notional": "1348",
      "fee": "0.35",
      "filled_at": "2026-09-25T13:39:59.969574414Z"
    }
  ],
  "next_cursor": null
}
```

`notional` is the accounting contract's value, price times quantity rounded
half away from zero to `0.00000001`. The journal stores the price and
quantity it was computed from, and the read side derives it with the same
function the account used.

### Pagination

- Both lists are newest first, ordered by the journal's record identity.
  Within one account it increases in commit order, because every order record
  is inserted while the account row is locked.
- `limit` is an integer from 1 to 200; the default is 50.
- `next_cursor` is an opaque string, or `null` when no older item exists.
  Pass it back unchanged as `cursor` with the same account and list. A cursor
  from the orders list is rejected by the fills list and by other accounts.
- Following a cursor chain never repeats an item, even while new orders are
  recorded: new orders are newer than the first page and appear only when a
  chain is restarted without a cursor.
- A non-null `next_cursor` means an older item existed when the page was read.
  The journal is append-only, so the next page is not empty.

### Reconciling an uncertain submission

After `503 persistence_unavailable` or `500 internal_error` from a submission,
the order may or may not have been recorded:

1. `GET /api/v1/accounts/{account_id}/orders/{client_order_id}`.
2. `200` means it was recorded; the resource is its final outcome. Do not
   resubmit.
3. `404 order_not_found` means it was not recorded when read. Resubmitting
   the same `client_order_id` is safe.
4. If that resubmission returns `409 duplicate_client_order_id`, the original
   commit completed after the read; repeat step 1.

The database accepts each client order ID at most once per account, so this
procedure can never record an order twice. It does not change the submission
contract.

## Errors

Every error body has the same shape:

```json
{
  "error": {
    "code": "risk_rejected",
    "message": "paper account lacks cash for the order and fee",
    "field": "quantity",
    "reason": "insufficient_cash"
  }
}
```

`code` is the stable category. `reason` is a stable, more specific code or
`null`. `field` names part of the client's request, or is `null` when the
cause is not a request field. `message` is for people and may change.

**A response body contains `order` exactly when the submission was recorded
in the durable journal.** A recorded submission consumes its client order ID,
so a corrected order needs a new ID. An unrecorded submission may be retried
with the same ID.

| Status | `code` | Recorded | Meaning and retry |
|---:|---|:---:|---|
| 400 | `malformed_request` | no | Body, query, decimal, enum, identifier, limit, or cursor cannot form a typed request; `reason` is one of `invalid_json`, `invalid_type`, `missing_field`, `unexpected_field`, `duplicate_field`, `invalid_decimal`, `too_many_fractional_digits`, `decimal_out_of_range`, `unknown_enum_value`, `invalid_identifier`, `invalid_query`, `invalid_limit`, `invalid_cursor` |
| 404 | `account_not_found` | no | No such paper account |
| 404 | `order_not_found` | no | The account has no order with this `client_order_id` |
| 404 | `route_not_found` | no | No such path |
| 405 | `method_not_allowed` | no | Wrong method; `Allow` names the right one |
| 409 | `account_already_exists` | no | The account ID is taken; the original is unchanged |
| 409 | `duplicate_client_order_id` | no | This account already recorded the ID; nothing new was recorded |
| 409 | `account_version_conflict` | no | Another writer changed the account first; retry with the same ID |
| 413 | `payload_too_large` | no | Body exceeds 16 KiB |
| 415 | `unsupported_media_type` | no | `Content-Type` is not JSON |
| 422 | `unsupported_instrument` | no | Account creation for an instrument the server does not trade |
| 422 | `invalid_starting_cash` | no | Starting cash is not positive |
| 422 | `risk_rejected` | yes | Pre-trade risk rejected the order; `reason` is the risk code |
| 422 | `market_data_stale` | yes | The quote is older than the account's maximum quote age |
| 422 | `execution_rejected` | yes | The simulator rejected the approved order; `reason` is the execution code |
| 500 | `processing_fault` | yes | Accounting rejected a risk-approved fill; the account is halted |
| 500 | `valuation_failed` | no | The portfolio could not be valued exactly; `reason` is `nonpositive_starting_cash`, `arithmetic_overflow`, or `accounting_identity_failed` |
| 500 | `internal_error` | unknown | Unexpected server failure; retry the identical request with the same identifiers |
| 503 | `market_data_unavailable` | no | No usable market for the account; `reason` is below |
| 503 | `persistence_unavailable` | unknown | PostgreSQL was unreachable; retry the identical request with the same identifiers |

`market_data_unavailable` reasons:

| `reason` | Returned by | Meaning |
|---|---|---|
| `no_top_of_book` | orders, portfolio | No observation of the account's instrument has been accepted |
| `stream_interrupted` | orders, portfolio | The stream rejected an event; see `/health` |
| `missing_best_bid` | portfolio | The current book has no bid to mark the position |
| `suspect_market_state` | portfolio | The observation is flagged as a potentially invalid book or questionable receive timestamp |
| `stale_market_state` | portfolio | The observation is older than the account's maximum quote age relative to market time |
| `incompatible_market_timeline` | portfolio | The market does not continue the account's timeline |
| `invalid_market_state` | portfolio | The observation fails the market-data contract, such as a crossed book without a quality flag |
| `market_instrument_mismatch` | portfolio | The observation is for another instrument |

The last two cannot occur while `MarketState` enforces its own contract; they
are checked so the core never values against an unverified observation.
Orders are journaled, so they report a suspect book or a missing side as
the recorded `risk_rejected` reasons `suspect_market_state` and
`missing_opposite_book_side` instead.

Risk reasons are the persisted risk codes, such as `insufficient_cash`,
`insufficient_position`, `order_quantity_limit_exceeded`,
`position_limit_exceeded`, `nonpositive_quantity`, `missing_limit_price`,
`unexpected_limit_price`, `instrument_mismatch`, and `suspect_market_state`.
Execution reasons include `market_stream_mismatch`, `stale_market_approval`,
and `market_snapshot_mismatch`. All names are defined once in
`chronos/paper_order_codes.hpp` and shared with the journal.

For `persistence_unavailable` and `internal_error`, a write may or may not have
completed. Retry account creation with the same `account_id`, and retry an
order submission with the same `client_order_id`. The database accepts each
identifier at most once, so an order retry either records the order or returns
`duplicate_client_order_id`. Reads can be retried unchanged. Internal details,
including libpq messages, go to the server log and never to clients.

The Recorded column describes submissions. Reads never record anything. A
read of a journal row that could not have been written fails closed with
`500 internal_error` rather than returning a misleading resource; so does a
record whose values the v1 contract cannot represent, such as a malformed
intent journaled by an in-process caller.

## Market-state ownership

The server owns market data through `MarketState`. A replay driver applies
validated events now; a live feed will call the same `apply` operation later.
Clients cannot supply the quote they trade against.

- **Instrument matching.** Each order executes against the latest top of book
  for the account's instrument. A request `instrument` that differs from the
  account's is recorded as the risk rejection `instrument_mismatch`.
- **Market time.** An order's `submitted_at` is the market clock: the receive
  timestamp of the latest event accepted on the stream. Replays are therefore
  reproducible regardless of wall-clock time.
- **Freshness.** The existing risk rule rejects a quote whose receive time is
  more than the account's `maximum_quote_age_ns` behind market time. A quote
  ages only as later events arrive, so in a replayed market, a quiet
  instrument becomes stale while another instrument keeps updating. A live
  feed must also advance time with heartbeats, or a silent feed would never
  become stale. Portfolio valuation uses the same recency classifier and
  returns `503 market_data_unavailable` / `stale_market_state` for the same
  observation.
- **Missing data.** Without a top of book for the instrument, the order is not
  recorded and returns `503 market_data_unavailable` / `no_top_of_book`. The
  journal requires the observation an order used, so nothing can be recorded
  without one.
- **Sequence behavior.** `MarketState` accepts events only from its configured
  stream, with contiguous ingest sequences and non-decreasing receive time. Any
  rejected event, such as a gap, duplicate, stream mismatch, or time
  regression, interrupts the state permanently: every quote then returns
  `stream_interrupted`, and `/health` reports the interruption. Recovery after
  an interruption is a server restart in this slice.
- **Quality flags.** Flags are preserved unchanged and shown in
  `order.market.quality_flags`. The risk engine rejects a potentially invalid
  book or questionable receive timestamp as `suspect_market_state`, and the
  portfolio refuses to mark against one with the same rule
  (`MarketDataQuality::is_suspect`).

### Accounts are bound to one market timeline

After its first execution, an account's execution simulator accepts only the
same market-data stream with a non-decreasing ingest sequence, and risk
requires order time not to precede the account's last fill. Restarting the
server on the same recording reproduces the same final market, so existing
accounts keep trading and keep their consumed shadow liquidity. Starting the
server on a different stream returns `execution_rejected` /
`market_stream_mismatch` for those accounts, and an earlier point on the same
stream returns `risk_rejected` / `submission_timestamp_moved_backward`. Use new
accounts for a new market timeline.

The portfolio applies the same rule, through the same function as execution
(`market_lineage_violation`), so an account is valued only against a market it
could also trade against. Its observation must be on the stream of the
account's latest execution, at the same or a later ingest sequence, and must
be identical at the same sequence; market time must not precede the latest
fill. Otherwise the portfolio returns `503 market_data_unavailable` /
`incompatible_market_timeline`. An account that has never executed has no
timeline yet and can be valued against any stream.

## Persistence and concurrency

- `PaperTradingService` exclusively owns one PostgreSQL connection and
  serializes every operation. HTTP worker threads may handle connections
  concurrently, but service calls run one at a time.
- Sessions are request-scoped. Each operation loads the account from a
  repeatable-read snapshot, processes one order, commits the snapshot and
  journal atomically, and discards the session. The process keeps no account
  state between requests.
- Optimistic concurrency protects against other processes: if another writer
  commits first, the losing request records nothing and returns
  `account_version_conflict`.
- A lost connection surfaces as `persistence_unavailable` for the request that
  observes it. The next operation reconnects automatically.
- A portfolio read loads the account the same way, in a repeatable-read,
  read-only transaction, then reads `MarketState` while holding the service
  lock. No order can commit between the two within one server process.
- Each history read runs in one repeatable-read, read-only transaction that
  checks the account and fetches at most `limit + 1` rows through an index on
  `(account_id, order_record_id)`, so an empty history is reliably
  distinguished from a missing account and no read loads the whole journal.
- One user's orders never consume another account's shadow liquidity. Within
  one account, orders against the same market snapshot cannot reuse liquidity
  that earlier orders consumed, including across restarts.

## Execution assumptions

The simulator is deliberately simple and is not a realistic exchange:

- Immediate-or-cancel orders against the displayed top of book only; no
  Level-2 sweep, queue position, market impact, or resting orders.
- No latency: an order executes at market time against the latest quote.
- A limit order fills at the opposite best price when marketable; no price
  improvement.
- One flat fee per non-empty fill.
- Long-only, single-instrument accounts.

See [paper-execution.md](paper-execution.md) for the complete rules.

## Extension points

These are intentionally not implemented in this slice:

- **Authentication and ownership.** A transport middleware will resolve an
  authenticated principal and pass it to service operations. Accounts will
  gain an owner, the service will authorize access before loading an account,
  and a manual order's `source.id` will come from the principal instead of the
  request body.
- **Live updates.** A WebSocket endpoint can publish `MarketState` updates and
  journal events. The journal already assigns `paper_events` a database
  sequence for ordered consumers.
- **Cancel/replace** needs a resting-order state machine.
- **Other valuations.** Size-aware liquidation value needs Level-2 depth; an
  equity curve needs a valuation history, which could be derived from the
  journal and market recording without changing this endpoint.
- **Model runtimes** will build `PaperOrderRequest` values with
  `source.kind = model` and call the same service in process.

## Current limitations

- Local development only: no authentication, authorization, TLS, CORS, or
  rate limiting.
- One database connection per server process; a slow database operation
  delays every request, and there is no statement or lock timeout.
- Loading an account per request costs time proportional to its journal.
- Startup replay is unpaced, so the served market does not move while the
  server runs.
- An interrupted market stream recovers only by restarting the server.
- No cancel/replace endpoints yet.
- The portfolio is a point-in-time value at the current best bid; there is no
  valuation history, equity curve, or alternative mark policy. The mark
  ignores displayed size, so it overstates what a position larger than the
  displayed bid could be sold for.
- History lists page backward from the newest item only; there is no time
  range filter or ascending order.
- The top-of-book simulator's assumptions above make simulated PnL optimistic
  or pessimistic in ways not yet measured.
