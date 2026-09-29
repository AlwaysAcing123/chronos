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

market-data source (paced replay
  or Alpaca IEX live)              -> MarketStateFeed -> MarketState
                                                     -> MarketDataNotifier -> SSE
MarketState -> PaperTradingService

HTTP GET history -> PaperTradingService -> journal read model
                                           (persisted rows only)

HTTP GET portfolio -> PaperTradingService -> durable account + MarketState
                                          -> value_portfolio (core)
```

Business rules live only in the core and the service. The HTTP layer never
evaluates risk, computes money, or touches PostgreSQL. The web workspace in
`apps/web/` is a client of this API through a same-origin development proxy
([web-client.md](web-client.md)); it performs no financial calculation. Manual and model orders
use the same endpoint, service call, and `OrderIntent` path; they differ only
in `source.kind`.

## Running locally

Build the API with the PostgreSQL adapter. Adding the Databento adapter lets
the server play a local recording into its market state; adding the Alpaca
adapter (`-DCHRONOS_ENABLE_ALPACA_ADAPTER=ON`) lets it stream live Alpaca IEX
quotes instead ([alpaca-adapter.md](alpaca-adapter.md)).

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
line. With `--replay-dbn` the server listens at once and plays the recording
in the background, paced by its receive timestamps
([market-replay.md](market-replay.md#paced-replay)). Playback waits in
`ready` until a client sends Start, unless `--replay-autostart` is given;
`--replay-speed` selects `1x` (the default), `10x`, `100x`, or `max`. With
`--alpaca-iex` the server listens at once while the live feed connects,
reading credentials only from `APCA_API_KEY_ID` and `APCA_API_SECRET_KEY`;
a missing variable stops startup with a message naming it. Without a
source, the market is empty and every order returns
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
| `--replay-dbn` | none | Local Databento EQUS.MINI MBP-1 recording to replay (Databento builds) |
| `--replay-speed` | `1x` | `1x`, `10x`, `100x`, or `max`; requires `--replay-dbn` |
| `--replay-autostart` | off | Start playing at once instead of waiting for Start; requires `--replay-dbn` |
| `--alpaca-iex` | off | Stream live Alpaca IEX trades and quotes (Alpaca builds); at most 30 instruments; not with `--replay-dbn` |

Every option except `--instrument` may be given once, and each instrument once.
An option for a source that is not selected, or not built, is rejected, and
at most one source may be selected. Switching sources requires a restart;
use a new stream ID and new accounts for each source and each live session.

Risk limits are server configuration, persisted with each account when it is
created. Clients cannot choose them, and later configuration changes do not
alter existing accounts.

Example session:

```sh
curl -s localhost:8080/health

curl -s -X POST localhost:8080/api/v1/accounts \
  -H 'Content-Type: application/json' \
  -d '{"account_id":"demo-1","starting_cash":"100000"}'

curl -s -X POST localhost:8080/api/v1/accounts/demo-1/orders \
  -H 'Content-Type: application/json' \
  -d '{"client_order_id":"buy-1","source":{"kind":"manual","id":"me"},
       "instrument":"AAPL","side":"buy","type":"limit","quantity":"10",
       "limit_price":"337","time_in_force":"ioc"}'

curl -s -X POST localhost:8080/api/v1/accounts/demo-1/orders \
  -H 'Content-Type: application/json' \
  -d '{"client_order_id":"buy-2","source":{"kind":"manual","id":"me"},
       "instrument":"SPY","side":"buy","type":"market","quantity":"2",
       "time_in_force":"ioc"}'

curl -s localhost:8080/api/v1/accounts/demo-1
curl -s 'localhost:8080/api/v1/accounts/demo-1/orders?limit=20'
curl -s 'localhost:8080/api/v1/accounts/demo-1/orders?instrument=SPY'
curl -s localhost:8080/api/v1/accounts/demo-1/orders/buy-1
curl -s localhost:8080/api/v1/accounts/demo-1/fills
curl -s localhost:8080/api/v1/accounts/demo-1/portfolio

curl -s localhost:8080/api/v1/market-data
curl -s -X POST localhost:8080/api/v1/market-data/replay/start \
  -H 'Content-Type: application/json' -d '{}'
curl -s -N localhost:8080/api/v1/market-data/events
```

The history endpoints read indexes added by
`migrations/002_paper_order_read_indexes.sql` and
`migrations/003_multi_instrument_accounts.sql`, and every account endpoint
needs the multi-instrument schema of 003. PostgreSQL runs the mounted
migrations only when it initializes an empty volume, so an existing volume
needs each newer file applied once, in order. Each is idempotent. Stop the
server and back up the volume before applying 003 (see
[postgresql-persistence.md](postgresql-persistence.md#upgrading-an-existing-volume)):

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/003_multi_instrument_accounts.sql
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
  "tradeable_instruments": ["AAPL", "SPY"],
  "market_data": {
    "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
    "state": "accepting",
    "last_ingest_sequence": "121302",
    "market_time": "2026-09-25T13:39:59.969574414Z",
    "interruption": null
  },
  "market_data_source": {
    "mode": "historical_replay",
    "label": "Databento EQUS.MINI MBP-1 replay",
    "state": "running"
  }
}
```

`state` is `accepting` or `interrupted`; `interruption` then describes the
rejected event. `market_data_source` summarizes the source; a live feed that
is still establishing itself reports `connecting`, `authenticating`, or
`subscribing` while the server is already healthy. The full source status is
in `/api/v1/market-data`.

`tradeable_instruments` lists the instruments every account may trade, in the
order the server was configured with (`--instrument`), so a client can offer
them without hard-coding server configuration.

### `POST /api/v1/accounts`

```json
{"account_id": "demo-1", "starting_cash": "100000"}
```

An account holds one cash balance and may trade every instrument the server
offers, so it is not created for one: the body has exactly these two fields,
and an `instrument` field is rejected as `400 malformed_request` /
`unexpected_field` rather than silently ignored. `starting_cash` must be
positive. Success returns `201` with an account:

```json
{
  "account": {
    "trading_mode": "paper",
    "account_id": "demo-1",
    "version": "0",
    "status": "active",
    "market_stream_id": null,
    "starting_cash": "100000",
    "cash": "100000",
    "open_cost_basis": "0",
    "gross_realized_pnl": "0",
    "fees_paid": "0",
    "last_fill_at": null,
    "positions": [],
    "closed_positions": [],
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
accounting invariant violation. `maximum_position_quantity` applies to each
instrument separately; the order quantity and notional limits apply to each
order.

### `GET /api/v1/accounts/{account_id}`

Returns `200` with the same account resource, read from a consistent database
snapshot. It holds no market-dependent values; mark-to-market valuation is the
portfolio resource below. This account, from a replay of the AAPL/SPY
recording with a 0.35 fee per fill, bought 5 AAPL at 336.04 and 3 SPY at
768.23, then sold the 5 AAPL at 336.33:

```json
{
  "account": {
    "trading_mode": "paper",
    "account_id": "demo-1",
    "version": "3",
    "status": "active",
    "market_stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
    "starting_cash": "100000",
    "cash": "97695.71",
    "open_cost_basis": "2304.69",
    "gross_realized_pnl": "1.45",
    "fees_paid": "1.05",
    "last_fill_at": "2026-09-25T13:30:41.245292529Z",
    "positions": [
      {
        "instrument": "SPY",
        "quantity": "3",
        "open_cost_basis": "2304.69",
        "average_entry_price": "768.23",
        "gross_realized_pnl": "0",
        "fees_paid": "0.35",
        "last_fill_at": "2026-09-25T13:30:09.338099170Z"
      }
    ],
    "closed_positions": [
      {
        "instrument": "AAPL",
        "gross_realized_pnl": "1.45",
        "fees_paid": "0.7",
        "last_fill_at": "2026-09-25T13:30:41.245292529Z"
      }
    ],
    "risk_limits": {"...": "..."}
  }
}
```

- `positions` lists every open position, in instrument order (byte order of
  the symbol). `average_entry_price` is `open_cost_basis / quantity` rounded
  to `0.000000001`, and is defined because the position is open.
- `closed_positions` lists every instrument the account traded and no longer
  holds, in the same order. A closed position keeps its ledger at zero
  quantity and zero cost basis, so what it realized and paid remains part of
  the account; it has no quantity, cost basis, entry price, or mark.
- `open_cost_basis`, `gross_realized_pnl`, and `fees_paid` are the server's
  sums over every open and closed position, and `last_fill_at` is the latest
  fill in any instrument. `cash + open_cost_basis = starting_cash +
  gross_realized_pnl − fees_paid` exactly.
- `market_stream_id` is the market-data stream the account executes on,
  fixed by its first executed order, or `null` before one.

### `GET /api/v1/accounts/{account_id}/portfolio`

Values the durable account against the server's current market. It is
read-only: it records nothing, changes no account version, and consumes no
shadow liquidity or client order ID. It accepts no query parameters and no
other method. The client cannot supply or influence a mark.

This is the response for the recording's final AAPL quote after a 10-share
market buy that filled 4 at 337 and a model sell of 1.5 at 335.82, with a
0.35 fee per fill:

```json
{
  "portfolio": {
    "trading_mode": "paper",
    "account_id": "demo-1",
    "account_version": "2",
    "status": "active",
    "market_stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
    "valued_at": "2026-09-25T13:39:59.969574414Z",
    "mark_policy": "best_bid",
    "starting_cash": "100000",
    "cash": "99155.03",
    "open_cost_basis": "842.5",
    "market_value": "839.55",
    "unrealized_pnl": "-2.95",
    "gross_realized_pnl": "-1.77",
    "fees_paid": "0.7",
    "total_pnl": "-5.42",
    "equity": "99994.58",
    "return_percent": "-0.00542",
    "positions": [
      {
        "instrument": "AAPL",
        "quantity": "2.5",
        "open_cost_basis": "842.5",
        "average_entry_price": "337",
        "gross_realized_pnl": "-1.77",
        "fees_paid": "0.7",
        "last_fill_at": "2026-09-25T13:39:59.969574414Z",
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
        "market_value": "839.55",
        "unrealized_pnl": "-2.95"
      }
    ],
    "closed_positions": []
  }
}
```

**Mark policy.** Accounts are long-only, so each open position is marked at
its instrument's current displayed best bid (`mark_policy: "best_bid"`), the
price at which it could begin to be sold. **This is mark-to-market valuation,
not guaranteed liquidation proceeds**: it ignores the bid's displayed size,
queue position, market impact, latency, and the fee a closing sale would pay.
A position larger than `market.bid.quantity` could not be sold at the mark in
this simulator either.

**Only held positions need a mark.** An instrument the account does not hold,
including a closed position, needs no quote and never prevents valuation, so
a flat account is valued at its cash whatever the market is doing. Every open
position needs a usable bid: the server never falls back to the ask,
midpoint, last trade, cost basis, or zero, and it never values a position it
cannot mark as zero. If any open position cannot be marked, the whole
response is `503 market_data_unavailable` / `position_mark_unavailable`,
listing every such position (see [Errors](#errors)); there is no partial
total.

**Fields.** Each position's `market` is the exact observation whose best bid
is its mark, in the same shape as an order's `market`. All marks and
`valued_at` come from one consistent read of the market state, so they belong
to the same instant. `valued_at` is the market clock: the receive timestamp
of the latest event accepted on the stream, which may be later than a
position's `market.receive_timestamp` when other instruments updated since;
it is `null` only before the stream's first event, when nothing can be held.
`account_version` is the durable version valued. The ledger fields have the
account resource's meaning; `status` is `halted` for a faulted account, which
can still be valued.

**Formulas.** All values are exact fixed-point decimals computed with checked
arithmetic:

```text
position market_value   = round(mark_price × quantity)             to 0.00000001
position unrealized_pnl = position market_value − position open_cost_basis
market_value            = Σ position market_value
unrealized_pnl          = Σ position unrealized_pnl
equity                  = cash + market_value
total_pnl               = equity − starting_cash
                        = gross_realized_pnl + unrealized_pnl − fees_paid
return_percent          = round(total_pnl / starting_cash × 100)   to 0.00000001
```

`round` is half away from zero, the same rule as every fill's notional. The
server verifies both definitions of `total_pnl` and every per-position value
before responding; they agree exactly for every valid account. The browser
does no financial arithmetic: every total is the server's. `return_percent`
is in percentage points: `"-0.00542"` means −0.00542%, not −0.542%. A value
outside the fixed-point range, including a sum over positions, fails with
`500 valuation_failed` rather than being approximated.

**Eligibility.** Each open position's observation must be a valid top of book
for its instrument without a suspect quality flag, it must be no older than
the account's `maximum_quote_age_ns` relative to `valued_at`, and it must
continue that instrument's execution timeline (see
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

`instrument` must be one of the server's `tradeable_instruments`; any other
returns `422 unsupported_instrument` and records nothing. The server executes
the order against its own current quote for that instrument, and no other,
and records it. An executed order returns `201`. This is
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
without re-running risk, execution, or accounting. `?instrument=SPY` lists
only the orders for that instrument (see [Pagination](#pagination)).

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

Lists the fills accounting booked, newest first, with the same pagination
and the same optional `instrument` filter. Only executed orders book fills; a
faulted order's rejected simulated fill is never listed.

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
- `instrument` is optional. Present, it keeps only that instrument's records;
  it must be a valid identifier (`400 malformed_request` /
  `invalid_identifier` otherwise) but need not be tradeable now, so the
  history of an instrument no longer configured stays readable. An instrument
  the account never traded has an empty history.
- `next_cursor` is an opaque string, or `null` when no older item exists.
  Pass it back unchanged as `cursor` with the same account, list, and
  `instrument` filter. A cursor is rejected (`invalid_cursor`) by the other
  list, by other accounts, and under a different filter, including no filter.
  Cursors for unfiltered lists keep their original encoding, so ones issued
  before filters existed remain valid.
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

### `GET /api/v1/market-data`

The server's market-data source and market state. It reads no database, so
browsers can refresh it on every change notice. This is the AAPL/SPY
recording after a `10x` replay completed:

```json
{
  "market_data": {
    "trading_mode": "paper",
    "revision": "121305",
    "source": {
      "mode": "historical_replay",
      "provider": "Databento",
      "feed": "EQUS.MINI MBP-1",
      "label": "Databento EQUS.MINI MBP-1 replay",
      "coverage_notice": null,
      "coverage_label": null,
      "state": "completed",
      "replay_speed": "10x",
      "events_applied": "121303",
      "diagnostic": null,
      "recording_sha256": "75238c7175ce96a735cd6a5a4f998ff272d8a0591524f295e4838a678eaced58",
      "instruments": ["AAPL", "SPY"]
    },
    "stream": {
      "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
      "state": "accepting",
      "last_ingest_sequence": "121302",
      "market_time": "2026-09-25T13:39:59.969574414Z",
      "interruption": null
    },
    "instruments": [
      {
        "instrument": "AAPL",
        "execution_quote_available": true,
        "unavailable_reason": null,
        "quote": {
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
      {
        "instrument": "SPY",
        "execution_quote_available": true,
        "unavailable_reason": null,
        "quote": {
          "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
          "ingest_sequence": "121302",
          "venue": null,
          "event_timestamp": "2026-09-25T13:39:59.969499529Z",
          "receive_timestamp": "2026-09-25T13:39:59.969574414Z",
          "quality_flags": ["replay"],
          "bid": {"price": "769.7", "quantity": "50"},
          "ask": {"price": "769.78", "quantity": "340"}
        }
      }
    ]
  }
}
```

Before an instrument's first quote, its entry is
`{"instrument": "SPY", "execution_quote_available": false,
"unavailable_reason": "no_top_of_book", "quote": null}`.

- `source.mode` is `none`, `historical_replay`, or `live`; `provider`,
  `feed`, `coverage_notice`, and `coverage_label` are `null` when not
  applicable. `coverage_label` restates the notice in a few words for compact
  displays; a client must still show the full notice.
- `source.state` is one of `none`, `ready`, `running`, `paused`,
  `completed`, `connecting`, `authenticating`, `subscribing`, `live`,
  `reconnecting`, `failed`, or `stopped`
  ([market-data-sources.md](market-data-sources.md#lifecycle)).
- `replay_speed` is present exactly for a replay. `events_applied` counts
  the events the source applied to the market state, exactly.
- `diagnostic` is the latest safe explanation, such as why a replay failed.
  `recording_sha256` is the verified checksum once a recording completes.
- `stream` has the same shape as `/health`'s `market_data`.
- `instruments` lists every tradeable instrument; `unavailable_reason` is the
  source-level `market_data_unavailable` reason for that instrument, and
  `quote` is the observation an order would execute against now, present
  exactly when one is available. All quotes come from one consistent read.
  They are for reference: an order executes against the server's quote at
  the moment it is submitted.
  `source_unavailable` means the source cannot establish that its last quote
  is current (for example, a live disconnect or failed replay), and
  `awaiting_fresh_quote` means a live source resumed but has not delivered a
  new quote for that instrument. Account-specific rules such as maximum quote
  age can still reject a source-level available quote.

A live Alpaca source looks like this; its `coverage_notice` must be shown
wherever its quotes are:

```json
"source": {
  "mode": "live",
  "provider": "Alpaca",
  "feed": "IEX",
  "label": "Alpaca IEX",
  "coverage_notice": "IEX only: quotes and trades from the Investors Exchange alone. This is not consolidated SIP data, not the NBBO, and not every US exchange.",
  "coverage_label": "IEX only, not SIP or NBBO",
  "state": "reconnecting",
  "replay_speed": null,
  "events_applied": "18342",
  "diagnostic": "the connection closed: the WebSocket connection closed; reconnecting in 1.000 s",
  "recording_sha256": null,
  "instruments": ["AAPL", "SPY"]
}
```
- `revision` is the change-notice revision this view is at least as new as.

### `POST /api/v1/market-data/replay/{command}`

`command` is `start`, `pause`, or `resume`. The body must be exactly `{}`
with `Content-Type: application/json`; any field, any other JSON value,
malformed JSON, duplicate keys, or a query string is rejected with
`400 malformed_request`. Success returns `200` with the market-data resource
after the command.

| Command | Allowed in | Result |
|---|---|---|
| `start` | `ready` | `running`; the first event is delivered at once |
| `pause` | `running` | `paused`; market time stops |
| `resume` | `paused` | `running`; playback continues where it stopped |

Any other combination returns `409 invalid_replay_transition` with `reason`
set to the current state, such as `completed`. A server without a replay
source returns `409 replay_control_unavailable` with `reason` set to the
source mode (`none` or `live`). There is no rewind or restart: a completed
replay stays at its final state until the server is restarted.

### `GET /api/v1/market-data/events`

A Server-Sent Events stream of change notices for local browsers. It carries
no market data: each notice only says that the market or source changed, and
the client refetches the resources it shows.

```text
retry: 3000

event: market-data
data: {"revision":"4817"}

: heartbeat
```

- The first notice is sent at once, so a reconnecting view refreshes.
- Notices are coalesced: at most one per 250 ms per connection, carrying the
  latest revision. Every market event still reaches the market state; only
  notices are merged, and there is no per-connection queue.
- A comment heartbeat every 10 s keeps idle connections open and detects
  closed ones.
- The response is `text/event-stream` with `Cache-Control: no-store`,
  `X-Content-Type-Options: nosniff`, and `X-Chronos-Trading-Mode: paper`. A
  query string returns `400`.
- At most 8 streams are open at once; another returns
  `503 event_stream_unavailable` with reason `too_many_event_streams`, and
  during shutdown `server_stopping`. Clients then fall back to polling.

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

A portfolio that cannot mark every open position adds `instruments`, listing
each such position in instrument order with its own reason:

```json
{
  "error": {
    "code": "market_data_unavailable",
    "message": "2 open positions cannot be marked",
    "field": null,
    "reason": "position_mark_unavailable",
    "instruments": [
      {"instrument": "AAPL", "reason": "stale_market_state", "message": "top-of-book state is older than the account's configured risk limit"},
      {"instrument": "SPY", "reason": "awaiting_fresh_quote", "message": "no quote for the instrument has arrived since the live source reconnected"}
    ]
  }
}
```

No other error carries `instruments`. `code` is the stable category. `reason` is a stable, more specific code or
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
| 422 | `unsupported_instrument` | no | An order for an instrument the server does not trade; retry with a tradeable one and the same ID |
| 422 | `invalid_starting_cash` | no | Starting cash is not positive |
| 422 | `risk_rejected` | yes | Pre-trade risk rejected the order; `reason` is the risk code |
| 422 | `market_data_stale` | yes | The quote is older than the account's maximum quote age |
| 422 | `execution_rejected` | yes | The simulator rejected the approved order; `reason` is the execution code |
| 500 | `processing_fault` | yes | Accounting rejected a risk-approved fill; the account is halted |
| 500 | `valuation_failed` | no | The portfolio could not be valued exactly; `reason` is `nonpositive_starting_cash`, `arithmetic_overflow`, or `accounting_identity_failed` |
| 500 | `internal_error` | unknown | Unexpected server failure; retry the identical request with the same identifiers |
| 409 | `invalid_replay_transition` | no | The replay command does not apply in the current state; `reason` is the state |
| 409 | `replay_control_unavailable` | no | The server's source is not a replay; `reason` is the source mode |
| 503 | `event_stream_unavailable` | no | Too many change streams are open, or the server is stopping |
| 503 | `market_data_unavailable` | no | No usable market for the order's instrument, or for the account's open positions; `reason` is below |
| 503 | `persistence_unavailable` | unknown | PostgreSQL was unreachable; retry the identical request with the same identifiers |

`market_data_unavailable` reasons:

| `reason` | Returned by | Meaning |
|---|---|---|
| `position_mark_unavailable` | portfolio | At least one open position cannot be marked; `instruments` gives each one's reason from the rows below |
| `no_top_of_book` | orders, portfolio | No observation of the instrument has been accepted |
| `stream_interrupted` | orders, portfolio | The stream rejected an event; see `/health` |
| `source_unavailable` | orders, portfolio | The active source cannot establish that its last quote is current |
| `awaiting_fresh_quote` | orders, portfolio | A live source resumed and has not delivered a new quote for the instrument |
| `missing_best_bid` | portfolio | The current book has no bid to mark the position |
| `suspect_market_state` | portfolio | The observation is flagged as a potentially invalid book or questionable receive timestamp |
| `stale_market_state` | portfolio | The observation is older than the account's maximum quote age relative to market time |
| `incompatible_market_timeline` | portfolio | The market does not continue the account's timeline, or a position's instrument timeline |
| `invalid_market_state` | portfolio | The observation fails the market-data contract, such as a crossed book without a quality flag |
| `market_instrument_mismatch` | portfolio | The observation is for another instrument |

For the portfolio, `stream_interrupted`, `source_unavailable`,
`awaiting_fresh_quote`, and the rows after them are per-position reasons
inside `instruments`; `incompatible_market_timeline` is also returned as the
top-level reason when the account as a whole is on another stream or behind
its latest fill. `invalid_market_state` and `market_instrument_mismatch`
cannot occur while `MarketState` enforces its own contract; they are checked
so the core never values against an unverified observation.
Orders are journaled, so they report a suspect book or a missing side as
the recorded `risk_rejected` reasons `suspect_market_state` and
`missing_opposite_book_side` instead.

Risk reasons are the persisted risk codes, such as `insufficient_cash`,
`insufficient_position`, `order_quantity_limit_exceeded`,
`position_limit_exceeded`, `nonpositive_quantity`, `missing_limit_price`,
`unexpected_limit_price`, and `suspect_market_state`. `instrument_mismatch`
appears only in orders recorded before accounts held several instruments,
when an order named an instrument other than its account's single one.
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

The server owns market data through `MarketState`. Its one market-data
source applies validated events through `MarketStateFeed`
([market-data-sources.md](market-data-sources.md)). Clients cannot supply the
quote they trade against.

- **Instrument matching.** Each order executes against the latest top of book
  for the instrument it names, and only that quote is read. An order for an
  instrument the server does not trade is refused as
  `422 unsupported_instrument` before anything is recorded. Risk still
  rejects an observation for another instrument (`market_instrument_mismatch`),
  so one instrument's quote can never fill another's order.
- **Market time.** An order's `submitted_at` is the market clock: the receive
  timestamp of the latest event accepted on the stream; for a live source, the
  local receive time of its latest read, including idle polls. Replays are therefore
  reproducible regardless of wall-clock time.
- **Freshness.** The existing risk rule rejects a quote whose receive time is
  more than the account's `maximum_quote_age_ns` behind market time. In a
  replayed market a quote ages only as later events arrive, so a quiet
  instrument becomes stale while another instrument keeps updating. A live
  source also advances market time to its local receive clock on every read,
  including idle polls every second, so a silent live feed's quotes become
  stale in real time; and while it is not connected and subscribed, no quote
  is served at all (`source_unavailable`, then `awaiting_fresh_quote` until
  each instrument's first quote on the new connection)
  ([market-data-sources.md](market-data-sources.md#live-freshness)).
  Portfolio valuation uses the same recency classifier and reports a held
  position's observation as `stale_market_state` in the same way.
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

An account uses one market-data stream for every instrument. After its first
execution, in any instrument, its execution simulator accepts only that
stream; per instrument, the ingest sequence must not decrease and one
sequence must not identify two different observations. Instruments are
independent within the stream: an AAPL order at sequence 8 is valid after an
SPY execution at sequence 12. Risk requires order time not to precede the
account's latest fill in any instrument, because the market clock is shared.

Restarting the server on the same recording reproduces the same final market,
so existing accounts keep trading and keep their consumed shadow liquidity.
Starting the server on a different stream returns `execution_rejected` /
`market_stream_mismatch` for those accounts, and an earlier point on the same
stream returns `risk_rejected` / `submission_timestamp_moved_backward`. Use new
accounts for a new market timeline.

A paced replay always starts again from the first event when the server
restarts. An account that traded later in the recording is therefore behind
the replay until playback passes its latest execution, and its orders and
portfolio fail with the errors above until then. Create a new account for
each replay session.

The portfolio applies the same rules, through the same function as execution
(`market_lineage_violation`), so an account is valued only against a market it
could also trade against. The market must be the account's stream and market
time must not precede its latest fill, even when it holds nothing; otherwise
the portfolio returns `503 market_data_unavailable` /
`incompatible_market_timeline`. Each open position's observation must also
be at the same or a later ingest sequence than that instrument's latest
execution, and identical at the same sequence; otherwise that position is
listed as `incompatible_market_timeline` under `position_mark_unavailable`.
An account that has never executed has no timeline yet and can be valued
against any stream.

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
  read-only transaction, then reads every held instrument's quote from
  `MarketState` in one consistent read while holding the service lock. No
  order can commit between the two within one server process.
- Each history read runs in one repeatable-read, read-only transaction that
  checks the account and fetches at most `limit + 1` rows through an index on
  `(account_id, order_record_id)`, or `(account_id, instrument,
  order_record_id)` with an instrument filter, so an empty history is reliably
  distinguished from a missing account and no read loads the whole journal.
- One user's orders never consume another account's shadow liquidity. Within
  one account, orders against the same instrument snapshot cannot reuse
  liquidity that earlier orders consumed, including across restarts, and
  consuming one instrument's liquidity never reduces another's.
- Cash is shared by an account's instruments, so two concurrent orders for
  different instruments on one account race for the same account version:
  one commits and the other returns `account_version_conflict`. Retrying it
  re-checks risk against the cash that is left, so cash can never be spent
  twice.

## Execution assumptions

The simulator is deliberately simple and is not a realistic exchange:

- Immediate-or-cancel orders against the displayed top of book only; no
  Level-2 sweep, queue position, market impact, or resting orders.
- No latency: an order executes at market time against the latest quote.
- A limit order fills at the opposite best price when marketable; no price
  improvement.
- One flat fee per non-empty fill.
- Long-only accounts holding any number of instruments from one USD cash
  balance, all on one market-data stream.

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
- A replay cannot be rewound or restarted in place; restart the server, and
  use new accounts, to replay again.
- An interrupted market stream recovers only by restarting the server. A live
  feed's disconnections recover by themselves; a failed live source (such as
  rejected credentials or malformed data) needs a restart.
- Alpaca IEX is one exchange, not SIP or the NBBO, and publishes no venue
  sequence, so upstream gaps cannot be detected.
- Every instrument is priced in USD and shares the account's one cash
  balance; there are no per-instrument cash sub-accounts or currencies.
- An account's instruments must all come from one market-data stream, so a
  replay of one recording and a live session cannot share an account.
- No cancel/replace endpoints yet.
- The portfolio is a point-in-time value at the current best bid; there is no
  valuation history, equity curve, or alternative mark policy. The mark
  ignores displayed size, so it overstates what a position larger than the
  displayed bid could be sold for.
- History lists page backward from the newest item only; there is no time
  range filter or ascending order.
- The top-of-book simulator's assumptions above make simulated PnL optimistic
  or pessimistic in ways not yet measured.
