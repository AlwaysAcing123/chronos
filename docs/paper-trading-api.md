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
[decisions/0004-paper-trading-http-api.md](decisions/0004-paper-trading-http-api.md),
for portfolio valuation,
[decisions/0006-portfolio-valuation.md](decisions/0006-portfolio-valuation.md),
and, for working orders,
[decisions/0011-durable-gtc-working-orders.md](decisions/0011-durable-gtc-working-orders.md).

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
                                                     -> WorkingOrderMatcher
                                                          (own connection;
                                                           each account with a
                                                           marketable working
                                                           order, one transaction)
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
| `--fee-per-fill` | `0` | Flat fee per order, paid by its first fill |
| `--replay-dbn` | none | Local Databento EQUS.MINI MBP-1 recording to replay (Databento builds) |
| `--replay-dataset` | none | A normalized dataset directory to replay, in every build, verified completely before the server listens; its stream must be `--stream-id` and its instruments exactly the `--instrument` list ([market-data-sources.md](market-data-sources.md#normalized-dataset-replay)) |
| `--replay-speed` | `1x` | `1x`, `10x`, `100x`, or `max`; requires `--replay-dbn` or `--replay-dataset` |
| `--replay-autostart` | off | Start playing at once instead of waiting for Start; requires `--replay-dbn` or `--replay-dataset` |
| `--alpaca-iex` | off | Stream live Alpaca IEX trades and quotes (Alpaca builds); at most 30 instruments; not with a replay |
| `--adopt-live-stream-history` | off | Continue a live stream whose durable history predates migration 006, after the largest sequence it references (ADR 0020); requires `--alpaca-iex` |

Every option except `--instrument` may be given once, and each instrument once.
An option for a source that is not selected, or not built, is rejected, and
at most one source may be selected. Switching sources requires a restart;
use a new stream ID and new accounts for each source.

A live stream continues across restarts with the same stream ID and
accounts. Its ingest sequences come from blocks reserved durably under a
single-writer lease, so they never repeat or move back
([ADR 0020](decisions/0020-restart-safe-live-stream-identity.md)). They are
identities, not counts: after a restart they jump by up to 2^20. Until the
server holds the stream's lease, the source reports `reconnecting` with the
reason in its diagnostic, and no quote is executable.

Ownership is enforced where it matters, in the database. Every order, fill,
model decision, and valuation checks that the quote it uses belongs to the
stream's current writer (migration 007). A server whose stream was taken
over while it was paused or cut off answers `503 market_data_unavailable` /
`source_unavailable` for orders and valuations on its quotes, records
nothing, and consumes no client order ID. Locally, its quotes also stop being
executable when its lease lapses, even with no new traffic, and stay so
until a new connection delivers a fresh quote.

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

curl -s -X POST localhost:8080/api/v1/accounts/demo-1/orders \
  -H 'Content-Type: application/json' \
  -d '{"client_order_id":"rest-1","source":{"kind":"manual","id":"me"},
       "instrument":"AAPL","side":"buy","type":"limit","quantity":"10",
       "limit_price":"330","time_in_force":"gtc"}'
curl -s localhost:8080/api/v1/accounts/demo-1/working-orders
curl -s localhost:8080/api/v1/accounts/demo-1/orders/rest-1/events
curl -s -X POST localhost:8080/api/v1/accounts/demo-1/orders/rest-1/cancel \
  -H 'Content-Type: application/json' -d '{"cancel_request_id":"cancel-1"}'

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
`migrations/002_paper_order_read_indexes.sql`,
`migrations/003_multi_instrument_accounts.sql`, and
`migrations/004_working_orders.sql`, and every account endpoint needs the
working-order schema of 004. PostgreSQL runs the mounted migrations only
when it initializes an empty volume, so an existing volume needs each newer
file applied once, in order. Each is idempotent. Stop the server and back up
the volume before applying 003 or 004 (see
[postgresql-persistence.md](postgresql-persistence.md#upgrading-an-existing-volume)):

```sh
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/002_paper_order_read_indexes.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/003_multi_instrument_accounts.sql
docker compose exec -T postgres psql -U chronos -d chronos \
  -v ON_ERROR_STOP=1 < migrations/004_working_orders.sql
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
- Query strings are strict on every route. The account-wide lists (orders,
  fills, and working orders) accept only `limit`, `cursor`, and
  `instrument`, and one order's fills and events only `limit` and `cursor`,
  each at most once; every other route accepts no query parameters. Unknown, repeated, or malformed parameters return `400` rather
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

`account_id`, `client_order_id`, `cancel_request_id`, `source.id`, and
`instrument` must be 1 to 64
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
    "first_ingest_sequence": "0",
    "market_time": "2026-09-25T13:39:59.969574414Z",
    "interruption": null
  },
  "market_data_source": {
    "mode": "historical_replay",
    "label": "Databento EQUS.MINI MBP-1 replay",
    "state": "running"
  },
  "order_matching": {
    "state": "running",
    "observations": "119282",
    "account_matches": "2",
    "fills": "2",
    "retries": "0",
    "failed_account_matches": "0",
    "total_matching_ns": "60410914902",
    "longest_matching_ns": "85042125"
  }
}
```

`state` is `accepting` or `interrupted`; `interruption` then describes the
rejected event. `first_ingest_sequence` is the first event this server
accepted since it started, kept for its lifetime, or `null` before any.
Sequences only increase within a server, so for a live stream it shows that
every quote since the start followed the stream's durable history
(`validate.py verify-continuity` checks it). `market_data_source` summarizes the source; a live feed that
is still establishing itself reports `connecting`, `authenticating`, or
`subscribing` while the server is already healthy. The full source status is
in `/api/v1/market-data`.

`tradeable_instruments` lists the instruments every account may trade, in the
order the server was configured with (`--instrument`), so a client can offer
them without hard-coding server configuration.

`order_matching` reports the working-order matcher (see
[Working orders](#working-orders)); it is absent from a server without one.
`state` is `running`, `retrying` (the matcher lost PostgreSQL and is
retrying the same observation, so the source is held), or `stopped`.
`observations` counts the top-of-book observations offered to working
orders, `account_matches` the accounts matched against one (each in its own
transaction), `fills` the fills booked, `retries` the attempts repeated
because PostgreSQL was unavailable, and `failed_account_matches` the account
matches abandoned because the account's stored state could not be recovered
or written; each such account is logged once and left unchanged. The two
durations are wall-clock nanoseconds spent matching, in total and for the
slowest observation. None of them affects health.

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
    "reserved_cash": "0",
    "available_cash": "100000",
    "working_order_count": "0",
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
order. `fee_per_fill` keeps its name, but is paid once per order, by its
first fill; an immediate-or-cancel order fills at most once, so nothing
changes for one.

### `GET /api/v1/accounts/{account_id}`

Returns `200` with the same account resource, read from a consistent database
snapshot. It holds no market-dependent values; mark-to-market valuation is the
portfolio resource below. This account, from a replay of the AAPL/SPY
recording with `fee_per_fill` 0.35, bought 5 AAPL at 336.04 and 3 SPY at
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
    "reserved_cash": "0",
    "available_cash": "97695.71",
    "working_order_count": "0",
    "open_cost_basis": "2304.69",
    "gross_realized_pnl": "1.45",
    "fees_paid": "1.05",
    "last_fill_at": "2026-09-25T13:30:41.245292529Z",
    "positions": [
      {
        "instrument": "SPY",
        "quantity": "3",
        "reserved_quantity": "0",
        "available_quantity": "3",
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
- `reserved_cash` is what the account's working orders hold back (see
  [Reservations](#reservations)), `available_cash = cash − reserved_cash`
  is what a new order may spend, and `working_order_count` counts the
  working orders. Per position, `reserved_quantity` is the shares its
  working sells hold, and `available_quantity = quantity −
  reserved_quantity` is what a new sell may sell. Every value is the
  server's; clients never compute them.
- `owner` is `null` for a manual account, or `{"kind": "model_deployment",
  "deployment_id": "..."}` for an account created with a
  [model deployment](#model-deployments). Ownership is fixed when the account
  is created. Only the deployment trades an owned account. Manual orders and
  cancellations on it return `409 account_owned_by_deployment`, whatever
  `source` they declare.

### `GET /api/v1/accounts/{account_id}/portfolio`

Values the durable account against the server's current market. It is
read-only: it records nothing, changes no account version, and consumes no
shadow liquidity or client order ID. It accepts no query parameters and no
other method. The client cannot supply or influence a mark.

This is the response for the recording's final AAPL quote after a 10-share
market buy that filled 4 at 337 and a model sell of 1.5 at 335.82, with a
`fee_per_fill` of 0.35:

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
    "reserved_cash": "0",
    "available_cash": "99155.03",
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
`account_version` is the durable version valued. The ledger fields, and
`reserved_cash` and `available_cash`, have the account resource's meaning;
`status` is `halted` for a faulted account, which can still be valued.
Reservations do not change valuation: reserved cash is still cash, and a
position a working sell reserves is still held and marked.

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
| `time_in_force` | `ioc` (immediate or cancel), `gtc` (good til cancelled) |
| `limit_price` | required for `limit`, must be omitted for `market` |

Enumerations are case-sensitive and closed. Only a limit order can be
`gtc`; a market order with `gtc` is recorded as `422 risk_rejected` /
`market_order_requires_ioc` with `field: "time_in_force"`, and consumes its
client order ID like any recorded rejection.

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
    "remaining_quantity": "6",
    "unfilled_quantity": "6",
    "filled_notional": "1348",
    "fill_count": "1",
    "fee_charged": true,
    "cumulative_fee": "0.35",
    "reserved_cash": "0",
    "reserved_quantity": "0",
    "priority": null,
    "updated_at": "2026-09-25T13:39:59.969574414Z",
    "cancellation_reason": "displayed_liquidity_exhausted",
    "cancel_request_id": null,
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

An immediate-or-cancel order's `status` is `filled`,
`partially_filled_and_cancelled`, or `cancelled` once executed. A cancelled
order has `cancellation_reason` `limit_not_marketable` or
`displayed_liquidity_exhausted` and `fill: null`. `market` is the exact
observation the server used, including its lineage.

A good-til-cancelled order also executes at once against the current quote
as far as it can, and the rest **works**: it stays open and is filled by
later quotes (see [Working orders](#working-orders)). Its submission also
returns `201`, with `status` `filled`, `partially_filled` (some filled, the
rest working), or `working` (nothing filled yet). This limit buy of 200
filled the 15 shares displayed at 334.92 and rests for 185:

```json
{
  "order": {
    "trading_mode": "paper",
    "account_id": "smoke-gtc-1",
    "client_order_id": "gtc-buy-1",
    "source": {"kind": "manual", "id": "smoke"},
    "instrument": "AAPL",
    "side": "buy",
    "type": "limit",
    "quantity": "200",
    "limit_price": "336.3",
    "time_in_force": "gtc",
    "submitted_at": "2026-09-25T13:31:49.322049622Z",
    "status": "partially_filled",
    "filled_quantity": "15",
    "remaining_quantity": "185",
    "unfilled_quantity": "185",
    "filled_notional": "5023.8",
    "fill_count": "1",
    "fee_charged": true,
    "cumulative_fee": "0.35",
    "reserved_cash": "62215.5",
    "reserved_quantity": "0",
    "priority": "1",
    "updated_at": "2026-09-25T13:31:49.322049622Z",
    "cancellation_reason": null,
    "cancel_request_id": null,
    "fill": {
      "fill_id": "gtc-buy-1:fill:0",
      "side": "buy",
      "price": "334.92",
      "quantity": "15",
      "notional": "5023.8",
      "fee": "0.35",
      "filled_at": "2026-09-25T13:31:49.322049622Z"
    },
    "rejection": null,
    "market": {"...": "the observation it was accepted on, ingest_sequence 32933"}
  },
  "account": {
    "...": "...",
    "cash": "94975.85",
    "reserved_cash": "62215.5",
    "available_cash": "32760.35",
    "working_order_count": "1"
  }
}
```

The order fields are the order's **current** state wherever an order
appears, not its state at submission:

- `filled_quantity + remaining_quantity = quantity` always.
  `unfilled_quantity` repeats `remaining_quantity` for older clients.
- `filled_notional`, `fill_count`, and `cumulative_fee` sum the order's
  fills. `fee_charged` says whether its one fee has been paid.
- `reserved_cash` and `reserved_quantity` are what the order holds back now
  ([Reservations](#reservations)); both are `0` once it is terminal.
- `priority` is the order's place among the account's working orders, an
  opaque string that sorts as an integer: lower was accepted earlier. It is
  `null` for an immediate-or-cancel order.
- `updated_at` is the market time of the order's latest change: its
  submission, latest fill, or cancellation.
- `cancel_request_id` is the request that cancelled it, or `null`.
- `fill` is the fill booked at submission, if any; every fill is at
  [`/orders/{client_order_id}/fills`](#get-apiv1accountsaccount_idordersclient_order_idfills).
- `market` is the observation it was accepted on. Later fills name their
  own observations only through their fill IDs and times.

Order `status` values:

| `status` | Terminal | Meaning |
|---|:---:|---|
| `working` | no | GTC; nothing filled; the whole quantity works |
| `partially_filled` | no | GTC; some filled; the rest works |
| `filled` | yes | Everything filled |
| `partially_filled_and_cancelled` | yes | Some filled; the rest was cancelled (`displayed_liquidity_exhausted`, or `user_requested` for a GTC order) |
| `cancelled` | yes | Nothing filled; cancelled (`limit_not_marketable`, `displayed_liquidity_exhausted`, or `user_requested`) |
| `rejected` | yes | Recorded risk or execution rejection |
| `faulted` | yes | Recorded processing fault; the account is halted |

A recorded rejection returns `422` with the same `order` and `account`
objects, `order.status` set to `rejected`, and an `error` object. A recorded
processing fault returns `500` with `order.status` set to `faulted`.

`order.rejection` is `null` for an executed order. For a rejected or faulted
order it has the error shape below and equals the submission's `error`, so an
order resource read back later explains itself. A faulted order reports no
fill even though the journal keeps the simulated fill that accounting
rejected.

### `GET /api/v1/accounts/{account_id}/orders`

Lists the account's recorded submissions, newest first by submission:
working, filled, partially filled, cancelled, risk-rejected,
execution-rejected, and faulted orders, each in its current state.
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

Returns `{"order": ...}`, the order's persisted submission and current
state. For an order that can no longer change (every immediate-or-cancel
order, and a terminal GTC order), it is exactly the `order` object its
submission or cancellation returned. An existing account without that order
returns `404 order_not_found`; an unknown account returns
`404 account_not_found`.

### `GET /api/v1/accounts/{account_id}/fills`

Lists the fills accounting booked, newest first, with the same pagination
and the same optional `instrument` filter, including every fill of a
working order. Only executed orders book fills; a faulted order's rejected
simulated fill is never listed.

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
      "filled_at": "2026-09-25T13:39:59.969574414Z",
      "fill_index": "0"
    }
  ],
  "next_cursor": null
}
```

`notional` is the accounting contract's value, price times quantity rounded
half away from zero to `0.00000001`, stored with the fill and checked against
its price and quantity when read. `fill_index` numbers the order's fills
from `0`, and `fill_id` is `{client_order_id}:fill:{fill_index}`. Only fill
`0` pays a fee.

### `GET /api/v1/accounts/{account_id}/working-orders`

Lists the account's working orders (`working` and `partially_filled`) in
priority order, oldest accepted first, as order resources in their current
state. It pages forward: `limit`, `cursor`, and the optional `instrument`
filter behave as in [Pagination](#pagination), and `next_cursor` continues
after the last order returned.

```json
{
  "orders": [
    {"client_order_id": "gtc-spy-1", "status": "working", "priority": "4",
     "remaining_quantity": "5", "reserved_cash": "3835.35", "...": "..."},
    {"client_order_id": "gtc-sell-1", "status": "working", "priority": "5",
     "remaining_quantity": "50", "reserved_cash": "0.35",
     "reserved_quantity": "50", "...": "..."}
  ],
  "next_cursor": null
}
```

An order that fills or is cancelled leaves the list, so a cursor chain read
while orders change may skip an order that became terminal, but never
repeats one or returns one out of priority order. Restart the chain to see
the list as it is now.

### `POST /api/v1/accounts/{account_id}/orders/{client_order_id}/cancel`

Cancels a working order's remainder. The body is exactly:

```json
{"cancel_request_id": "cancel-1"}
```

`cancel_request_id` is an [identifier](#identifiers) the client chooses for
this cancellation. Success returns `200` with the order after cancellation,
the account after it, and whether this request had already cancelled it:

```json
{
  "order": {
    "client_order_id": "gtc-buy-2",
    "status": "cancelled",
    "filled_quantity": "0",
    "remaining_quantity": "10",
    "reserved_cash": "0",
    "reserved_quantity": "0",
    "updated_at": "2026-09-25T13:35:42.946522062Z",
    "cancellation_reason": "user_requested",
    "cancel_request_id": "cancel-2a",
    "...": "..."
  },
  "account": {"...": "the account resource after cancellation"},
  "already_cancelled": false
}
```

- The order's fills and its fee stand; only the remainder is cancelled, and
  its reservation is released in the same transaction. A partially filled
  order becomes `partially_filled_and_cancelled`, an unfilled one
  `cancelled`, both with `cancellation_reason: "user_requested"`.
- **Idempotent.** Repeating the request with the same `cancel_request_id`
  for the same order returns `200` with the order as that cancellation left
  it and `already_cancelled: true`, whatever happened since. A
  `cancel_request_id` cancels at most one order: using it for another order
  of the account returns `409 duplicate_cancel_request_id`.
- **Nothing to cancel.** An order that is already terminal (filled,
  cancelled, rejected, or faulted, including cancelled by another request)
  returns `409 order_not_cancellable` with the order as it is, so a client
  that lost a race with a fill sees what happened:
  `{"error": {"code": "order_not_cancellable", "field": "client_order_id",
  ...}, "order": {...}}`. Nothing is recorded.
- It needs no market data, and works while the market is unavailable,
  stale, or interrupted, and on a halted account, whose working orders never
  fill. `updated_at` is the market time when a market clock exists, and
  never earlier than the order's latest change.
- It is serialized with matching and submissions under the account's lock:
  a cancel that commits first stops every later fill, and a fill that
  commits first stands.

After `503 persistence_unavailable` or `500 internal_error`, the
cancellation may or may not have committed. Resend the same request with
the same `cancel_request_id`: it either cancels, or returns `200` with
`already_cancelled: true`, or returns `409 order_not_cancellable` if the
order filled or was cancelled otherwise first. Reading the order also
settles it: `cancel_request_id` names the request that cancelled it.

### `GET /api/v1/accounts/{account_id}/orders/{client_order_id}/fills`

Lists one order's fills, oldest first by `fill_index`, as fill resources.
It pages forward with `limit` and `cursor`; `next_cursor` continues after
the last fill returned. An immediate-or-cancel order has at most one fill.

### `GET /api/v1/accounts/{account_id}/orders/{client_order_id}/events`

Lists one order's audit events in the order they happened, with `limit` and
`cursor` paging forward. Each event has its number within the order, its
type, its market time, and, for `fill_applied`, the fill it booked:

```json
{
  "events": [
    {"sequence": "1", "type": "order_submitted", "occurred_at": "2026-09-25T13:31:49.322053489Z", "fill": null},
    {"sequence": "2", "type": "order_accepted", "occurred_at": "2026-09-25T13:31:49.322053489Z", "fill": null},
    {"sequence": "3", "type": "account_state_updated", "occurred_at": "2026-09-25T13:31:49.322053489Z", "fill": null},
    {"sequence": "4", "type": "order_cancelled", "occurred_at": "2026-09-25T13:35:42.946522062Z", "fill": null},
    {"sequence": "5", "type": "account_state_updated", "occurred_at": "2026-09-25T13:35:42.946522062Z", "fill": null}
  ],
  "next_cursor": null
}
```

Event types are `order_submitted`, `risk_rejected`, `execution_rejected`,
`order_completed` (an immediate-or-cancel order executed), `order_accepted`
(a GTC order was accepted), `fill_applied`, `order_filled` (a GTC order has
nothing left), `order_cancelled`, `account_state_updated`, and
`processor_faulted`. Events recorded before migration 004 carry their
submission's time.

For each per-order list, an existing account without that order returns
`404 order_not_found`, and an unknown account `404 account_not_found`.

### Pagination

- The account-wide orders and fills lists are newest first, ordered by
  record identity: the journal's for orders, the fill table's for fills.
  Within one account each increases in commit order, because every order
  and fill is inserted while the account row is locked.
- The working-orders list and the per-order fills and events lists are
  oldest first and page forward: `next_cursor` continues after the last item
  returned, and is `null` when nothing followed it when the page was read.
- `limit` is an integer from 1 to 200; the default is 50.
- `instrument` is optional. Present, it keeps only that instrument's records;
  it must be a valid identifier (`400 malformed_request` /
  `invalid_identifier` otherwise) but need not be tradeable now, so the
  history of an instrument no longer configured stays readable. An instrument
  the account never traded has an empty history.
- `next_cursor` is an opaque string, or `null` when no further item exists.
  Pass it back unchanged as `cursor` with the same account, list, order, and
  `instrument` filter. A cursor is rejected (`invalid_cursor`) by any other
  list, by other accounts and orders, and under a different filter,
  including no filter.
  Cursors for unfiltered lists keep their original encoding, so ones issued
  before filters existed remain valid.
- Following a cursor chain never repeats an item, even while new orders are
  recorded: new orders are newer than the first page and appear only when a
  chain is restarted without a cursor.
- A non-null `next_cursor` from an orders or fills list means an older item
  existed when the page was read. Both are append-only, so the next page is
  not empty. A working-orders page can be followed by an empty one, when
  the orders after it stopped working.

### Reconciling an uncertain submission

After `503 persistence_unavailable` or `500 internal_error` from a submission,
the order may or may not have been recorded:

1. `GET /api/v1/accounts/{account_id}/orders/{client_order_id}`.
2. `200` means it was recorded. Do not resubmit. The resource is its current
   state: for a GTC order, fills may have followed since.
3. `404 order_not_found` means it was not recorded when read. Resubmitting
   the same `client_order_id` is safe.
4. If that resubmission returns `409 duplicate_client_order_id`, the original
   commit completed after the read; repeat step 1.

The database accepts each client order ID at most once per account, so this
procedure can never record an order twice. It does not change the submission
contract. `503 order_matching_busy` is different: it is returned before
anything is written, so the order was certainly not recorded and the same
request can be retried at once.

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
      "synthetic": false,
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
      "first_ingest_sequence": "0",
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
- `source.synthetic` is `true` exactly for a replay of a synthetic dataset
  (`--replay-dataset`, ADR 0025): its events were invented by a generator,
  not observed on a market. Its `provider` is `synthetic`, its label says
  "invented prices, not market data", and its `coverage_notice` says what
  generated it; a client must say so wherever its quotes are shown.
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
  "synthetic": false,
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
data: {"revision":"4817","activity_revision":"10"}

: heartbeat
```

- `revision` changes whenever the market or source changes.
  `activity_revision` changes whenever an account changed without a request
  from this client: a working order filled, or matching halted an account.
  It also changes after every submission and cancellation. A client refetches
  account resources only when `activity_revision` changes, and market
  resources when `revision` does, so a busy market does not make it reload
  every account view.
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
| 409 | `account_version_conflict` | no | Another writer changed the account first; retry with the same ID. The server's own writers serialize on the account lock instead, so only a writer outside it can cause this |
| 409 | `order_not_cancellable` | no | The order is terminal; the body carries it as it is |
| 409 | `duplicate_cancel_request_id` | no | This `cancel_request_id` already cancelled another order of the account |
| 409 | `account_owned_by_deployment` | no | The account belongs to a model deployment; only it trades the account, whatever `source` the request declares |
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
| 503 | `persistence_unavailable` | unknown | PostgreSQL was unreachable, or the account stayed locked longer than the server's lock timeout; read the resource and retry the identical request with the same identifiers |
| 503 | `order_matching_busy` | no | Working orders were still being matched against the latest market event after 2 s; retry the same request |

`market_data_unavailable` reasons:

| `reason` | Returned by | Meaning |
|---|---|---|
| `position_mark_unavailable` | portfolio | At least one open position cannot be marked; `instruments` gives each one's reason from the rows below |
| `no_top_of_book` | orders, portfolio | No observation of the instrument has been accepted |
| `stream_interrupted` | orders, portfolio | The stream rejected an event; see `/health` |
| `source_unavailable` | orders, portfolio | The active source cannot establish that its last quote is current, or the live stream's quote belongs to an earlier writer (ADR 0020) |
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
its latest fill, and `source_unavailable` when the marks belong to an earlier
writer of a live stream. `invalid_market_state` and `market_instrument_mismatch`
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
`market_order_requires_ioc` rejects a market order sent as `gtc`.
Execution reasons include `market_stream_mismatch`, `stale_market_approval`,
and `market_snapshot_mismatch`. All names are defined once in
`chronos/paper_order_codes.hpp` and shared with the journal.

For `persistence_unavailable` and `internal_error`, a write may or may not have
completed. Retry account creation with the same `account_id`, and retry an
order submission with the same `client_order_id`. The database accepts each
identifier at most once, so an order retry either records the order or returns
`duplicate_client_order_id`. Reads can be retried unchanged. Internal details,
including libpq messages, go to the server log and never to clients.

The Recorded column describes submissions. A cancellation records only its
`200`; after `503 persistence_unavailable` or `500 internal_error` it may or
may not have committed, and the same request settles it (see
[the cancel endpoint](#post-apiv1accountsaccount_idordersclient_order_idcancel)).
Reads never record anything. A
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

## Working orders

A good-til-cancelled limit order works until it fills or is cancelled.
There is no expiry, no end of day, and no cancellation by the server:
nothing but a fill or a cancel request ends it, across restarts.

**Acceptance.** A GTC order needs a usable quote to be accepted, exactly as
an IOC order: the same risk checks run against it (with the reservations
below), and an unusable market is rejected or refused in the same way. It
then executes at once against that quote, as far as the displayed
liquidity the account has not already taken allows, and the remainder
rests. An order that fills nothing at once takes no liquidity.

**Matching.** Each later top-of-book observation of the order's instrument
is offered to the account's working orders on that instrument, in priority
order, before the next market event is applied. An observation fills an
order only if:

- the account is active (not halted) and on the observation's stream;
- the observation is a valid top of book without a suspect quality flag
  (it is matched as it arrives, so it is always current);
- it is later than the observation the order was accepted on, and later
  than the instrument's latest execution in the account, so an observation
  fills at most once and never after a newer one;
- the order is marketable against it: the best ask is at or below a buy's
  limit, or the best bid at or above a sell's.

The fill is at the opposite best price, for at most the displayed size less
what the account's earlier orders took from the same observation: accounts
never share liquidity, and within an account the earlier order fills first.
There is no depth sweep, queue position, or trade-driven fill. Its time is
the observation's receive time. An observation that does not qualify is
skipped: the order keeps working and a later observation can still fill it.
Three edge cases also skip an observation rather than cancel: a fill whose
notional rounds to zero, one whose arithmetic would overflow, and a partial
buy fill whose rounding would leave the order's reservation a unit short.
The remaining displayed liquidity on that side is then unavailable to newer
orders of the account for that observation, preserving FIFO priority.

Every fill is one transaction per account, committed before the next market
event is applied, and every view reads it from PostgreSQL. Matching does
not depend on the change stream or any client being connected.

**Fees.** An order pays one fee, `fee_per_fill`, with its first fill; later
fills pay nothing. An order that never fills pays nothing.

### Reservations

Each working order holds back what it could still spend, so no two orders,
and no immediate-or-cancel order, can spend the same cash or shares:

| Order | `reserved_cash` | `reserved_quantity` |
|---|---|---|
| Buy | `limit × remaining`, rounded half away from zero to `0.00000001`, plus the fee until it is paid | `0` |
| Sell | the fee until it is paid | `remaining` |

An account's `reserved_cash` sums its orders', and a position's
`reserved_quantity` sums its working sells'. Risk checks a new order, of
either time in force, against what is not reserved: a buy's notional and
fee against `available_cash`, a sell's quantity against the position's
`available_quantity`, and its fee against `available_cash`. The order
notional limit uses `quantity × limit_price` for a GTC order, since it may
fill at its limit; the position limit counts the shares working buys could
still add. A reservation that cannot be represented fails the order closed
as a risk rejection. A fill at a better price than the limit releases the
difference; cancellation and a full fill release everything.

## Persistence and concurrency

- `PaperTradingService` exclusively owns one PostgreSQL connection and
  serializes every operation. HTTP worker threads may handle connections
  concurrently, but service calls run one at a time. The working-order
  matcher owns a second connection, so API reads never wait for matching.
- Sessions are request-scoped. A submission or cancellation locks the
  account row, recovers the account inside that transaction, processes one
  request, commits its state, order rows, and journal atomically, and
  discards the session. The matcher does the same for each account it
  matches. The process keeps no account state between requests.
- Changes of one account are therefore serialized, across connections and
  processes, in the order they take the lock. A lock waited on longer than
  5 s (1 s for the matcher) fails with `persistence_unavailable` and records
  nothing; the matcher retries.
- A submission also waits, up to 2 s, for matching of the market event
  being applied to finish, then holds the market until it has committed and
  read its order back. It therefore executes against the market working
  orders have already been matched against, the next event is matched after
  the order is recorded, and its response shows the order and account at
  the same instant. Submissions wait one at a time, within the same 2 s,
  and without holding the connection, so reads, cancellations, and
  `/health` never wait for matching. A longer wait returns
  `503 order_matching_busy`.
- Optimistic versions remain a second line of defense: a writer that does
  not take the lock and commits first makes the other return
  `account_version_conflict`, and nothing is recorded.
- A lost connection surfaces as `persistence_unavailable` for the request that
  observes it. The next operation reconnects automatically. A matcher that
  loses its connection retries the same observation, holding the source,
  until it succeeds or the server stops; `/health` shows `retrying`.
- A portfolio read loads the account the same way, in a repeatable-read,
  read-only transaction, then reads every held instrument's quote from
  `MarketState` in one consistent read while holding the service lock. No
  order can commit between the two within one server process.
- Each history read runs in one repeatable-read, read-only transaction that
  checks the account (and the order, for a per-order list) and fetches at
  most `limit + 1` rows through an index: the journal's on `(account_id,
  order_record_id)`, or `(account_id, instrument, order_record_id)` with an
  instrument filter, for orders; the fill table's matching pair for fills;
  the partial working-order index for working orders; and per-order unique
  keys for one order's fills and events. An empty list is therefore reliably
  distinguished from a missing account or order, and no read loads the
  whole journal.
- One user's orders never consume another account's shadow liquidity. Within
  one account, orders against the same instrument snapshot cannot reuse
  liquidity that earlier orders consumed, including across restarts, and
  consuming one instrument's liquidity never reduces another's.
- Cash is shared by an account's instruments, so two concurrent orders for
  different instruments on one account serialize on the account lock: the
  second is checked against the cash and reservations the first left, so
  cash can never be spent twice.
- Shutdown stops the HTTP server, then the matcher, which ends any retry,
  then the source. Each account match either commits or rolls back as a
  whole; a restart recovers every working order from PostgreSQL and matches
  it against the events that follow.

## Execution assumptions

The simulator is deliberately simple and is not a realistic exchange:

- Immediate-or-cancel and good-til-cancelled orders against the displayed
  top of book only; no Level-2 sweep, queue position, market impact, or
  trade-driven fills. A working order is not part of any book and never
  provides liquidity.
- No latency: an order executes at market time against the latest quote,
  and a working order against each later quote as it arrives.
- A limit order fills at the opposite best price when marketable; no price
  improvement beyond that.
- One flat fee per order, paid by its first fill.
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
- **Replace** (amending a working order's quantity or price) can build on
  the working-order state machine and its priority; it would need a rule
  for when an amendment keeps priority.
- **Other time-in-force values** such as day or good-til-date need a
  session calendar or expiry clock.
- **Other valuations.** Size-aware liquidation value needs Level-2 depth; an
  equity curve needs a valuation history, which could be derived from the
  journal and market recording without changing this endpoint.
- **Model runtimes** submit through `PaperTradingService::submit_deployment_order`,
  in process and with no route (see [Model deployments](#model-deployments)).
  A request to the order route that declares `source.kind = model` is still
  a manual-path request, and is refused on an owned account.

## Model deployments

A model deployment trades one verified research model in a paper account of
its own, only after an explicit start. See
[model-deployments.md](model-deployments.md) for what it does and
[ADR 0018](decisions/0018-model-paper-deployments.md) for why. The routes
exist on every server. Unless it was started with `--model-deployments`,
creating and starting return `409 deployments_disabled`, while listing,
Stop, and the kill switch still work.

| Route | Does |
|---|---|
| `GET /api/v1/deployments` | Newest first, with `enabled`; `?limit` (1 to 200, default 50) and `?cursor` |
| `POST /api/v1/deployments` | Creates a deployment, stopped, and its new account |
| `POST /api/v1/deployments/kill-switch` | Stops every deployment (`kill_switch`); body `{}` |
| `GET /api/v1/deployments/{deployment_id}` | One deployment |
| `POST /api/v1/deployments/{deployment_id}/start` | Starts a new run; body `{}` |
| `POST /api/v1/deployments/{deployment_id}/stop` | Stops it, keeping the position; body `{}` |
| `GET /api/v1/deployments/{deployment_id}/decisions` | Its decisions, newest first; `?limit` and `?cursor` |

No route submits a deployment's order.

### Creating

Every field is required and is a string:

```json
{
  "deployment_id": "demo-aapl",
  "account_id": "demo-aapl-acct",
  "starting_cash": "10000",
  "experiment_id": "05be6dd441394993e67b5efadda6dc4f4ac395750fd9a4bc3ae449ac2c16ba62",
  "model": "logistic",
  "instrument": "AAPL",
  "threshold": "0.5",
  "target_quantity": "1",
  "decision_interval_ms": "1000",
  "latency_ms": "1",
  "max_signal_age_ms": "5000",
  "max_orders_per_run": "200",
  "max_consecutive_rejections": "3"
}
```

- **Model.** Either `experiment_id` and `model`, an experiment's model
  (above), or `model_version_id` alone, a registered model version from the
  research service's model library
  ([model-deployments.md](model-deployments.md#registered-model-versions)).
  A body with fields of both is `400 malformed_request`.
- **Identifiers.** `deployment_id` is at most 32 identifier characters, so
  that `<deployment_id>-<12-digit decision sequence>` is a valid client order
  ID. `kill-switch` is reserved for the control route. `account_id` must not
  exist yet.
- **Policy.** `threshold` is a decimal from 0 to 1. `target_quantity` is
  positive and at most `1`.
- **Timing.** The three durations are whole milliseconds, at most a day, and
  `max_signal_age_ms` must exceed `latency_ms`.
- **Verification.** Before anything is written, the server starts the
  inference worker with `--describe`, which verifies the experiment or the
  model version and reports the model's parameter or payload checksum and
  training cutoff. The deployment records both, and the cutoff's basis. An
  archived version, a version without a cutoff, and an imported version the
  server cannot run in the ONNX sandbox are `model_unavailable`.
- **Account limits.** The new account takes the server's risk limits, with
  its order and position quantity limits lowered to `target_quantity`.

The response is `201` with `{"deployment": ..., "created": true}`. The
identical request again returns `200` with `"created": false`. The
canonical request's SHA-256 identifies it.

| Status | `code` | Meaning |
|---:|---|---|
| 400 | `malformed_request` | Not a JSON object, an unknown, duplicate, missing, or non-string field, or an unparseable value; `field` names it |
| 409 | `deployment_exists` | The ID was created from a different request |
| 409 | `account_exists` | The account ID is taken |
| 409 | `deployments_disabled` | The server was started without `--model-deployments` |
| 422 | `invalid_deployment` | A domain rule, such as a target above one share; `field` names it |
| 422 | `model_unavailable` | The model could not be verified; `reason` is, for example, `model_rejected` or `inference_timeout` |
| 503 | `persistence_unavailable` | PostgreSQL is unavailable; nothing is known to be created, so retry the identical request |

### The deployment resource

A deployment after the demonstration replay ([model-deployments.md](model-deployments.md)) and a clean server shutdown:

```json
{
  "deployment": {
    "trading_mode": "paper",
    "deployment_id": "demo-aapl-2",
    "account_id": "demo-aapl-2-acct",
    "creation_sequence": "3",
    "state": "stopped",
    "state_reason": "server_stopped",
    "state_changed_at": "2026-10-04T19:08:46.489397000Z",
    "created_at": "2026-10-04T19:07:18.810428000Z",
    "active": false,
    "run_generation": "1",
    "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
    "starting_cash": "10000",
    "model": {
      "reference": "experiment",
      "experiment_id": "05be6dd441394993e67b5efadda6dc4f4ac395750fd9a4bc3ae449ac2c16ba62",
      "name": "logistic",
      "training_cutoff_basis": "verified",
      "instrument": "AAPL",
      "parameters_sha256": "a0e07429e1654ee4cbe07ba6e2ded7b079e35a9e3671da50c7e281567c538939",
      "feature_set": "top_of_book_baseline/1",
      "training_cutoff": "2026-09-25T13:35:19.090266460Z"
    },
    "policy": {
      "name": "long_flat_threshold/1",
      "threshold": "0.5",
      "target_quantity": "1"
    },
    "timing": {
      "decision_interval_ms": "1000",
      "latency_ms": "1",
      "max_signal_age_ms": "5000"
    },
    "limits": {
      "max_orders_per_run": "200",
      "max_consecutive_rejections": "3",
      "orders_this_run": "76",
      "consecutive_rejections": "0"
    },
    "watermark": {
      "received_at": "2026-09-25T13:39:59.960901336Z",
      "ingest_sequence": "121296"
    },
    "next_decision_sequence": "238",
    "pending_decision": null,
    "version": "316",
    "run": null
  }
}
```

- **`state` and `state_reason`.** `state` is `stopped`, `running`,
  `stopping`, or `faulted`. `state_reason` says why; for example `created`,
  `started`, `stop_requested`, `kill_switch`, `server_restarted`,
  `server_stopped`, `model_identity_mismatch`, `rejection_limit_reached`,
  `order_limit_reached`, or `inference_timeout`.
- **`model`.** `reference` is `experiment`, with `experiment_id`, or
  `registered`, with `model_version_id` and no `experiment_id`. `name` is
  the family (`logistic`, `majority`, or `onnx`). `training_cutoff_basis` is
  `verified`, or `declared` for an import whose uploader declared it.
- **`active`.** True while this server process runs the deployment's
  runner.
- **`watermark`.** The latest decided observation. It is durable and never
  moves back, so no observation is decided twice.
- **`run`.** The current or last run's counters in this process, kept in
  memory only. They are `null` after a restart, as in the example. The
  counters are:
  - `candidates` and `superseded`, the rows replaced in the one-slot
    hand-off before the runner took them;
  - `skipped` and `waits`, counted by reason;
  - `predictions`, `holds`, `order_decisions`, `orders_recorded`, and
    `abandoned`;
  - the wall time of inference and of the trusted order path, as
    `last_inference_us`, `max_inference_us`, `last_submission_us`, and
    `max_submission_us`;
  - `last_error`.
- **Errors.** Start can return `409 invalid_deployment_state`, or
  `422 model_unavailable` when the model no longer loads or verifies; the
  deployment is then `faulted` with that reason. The server allows at most
  eight active deployments; an additional Start returns
  `409 invalid_deployment_state`, reason `deployment_capacity_reached`,
  without starting a worker. Start, Stop, and
  `GET /api/v1/deployments/{deployment_id}` return
  `404 deployment_not_found`.

### Decisions

Its first recorded decision. The deployment's latency was 1 ms, yet the order executed on a quote received 767 ms after the signal: the 10x replay held the market-event gate, and the order waited for it ([limitations](model-deployments.md#limitations)).

```json
{
  "decisions": [
    {
      "decision_sequence": "41",
      "run_generation": "1",
      "state": "recorded",
      "observation": {
        "stream_id": "databento-equs-mini-mbp1-20260925-1330-1340",
        "instrument": "AAPL",
        "ingest_sequence": "81332",
        "event_at": "2026-09-25T13:36:05.308244633Z",
        "received_at": "2026-09-25T13:36:05.308358348Z"
      },
      "decided_at": "2026-09-25T13:36:05.371816559Z",
      "inputs": [
        "880000000",
        "764705882",
        "336470588",
        "30000000000",
        "4000000000"
      ],
      "prediction": {
        "class": "up",
        "probabilities": [
          "0.3318806796538787",
          "0.06930539751800728",
          "0.598813922828114"
        ],
        "confidence": "0.598813922"
      },
      "position_before": "0",
      "target_quantity": "1",
      "policy_reason": "order_scheduled",
      "order": {
        "client_order_id": "demo-aapl-2-000000000041",
        "side": "buy",
        "quantity": "1",
        "not_before_received_at": "2026-09-25T13:36:05.309358348Z",
        "expires_at": "2026-09-25T13:36:10.308358348Z"
      },
      "resolution": "filled",
      "execution": {
        "ingest_sequence": "81434",
        "received_at": "2026-09-25T13:36:06.075800942Z"
      },
      "resolved_at": "2026-09-25T13:36:06.156701724Z",
      "created_at": "2026-10-04T19:08:05.784544000Z"
    }
  ],
  "next_cursor": null
}
```

- **The observation.** `inputs` are the five exact model inputs of
  `top_of_book_baseline/1`, in their scales, from the observation the signal
  used.
- **`state`.** `held` (no order intended), `pending`, `recorded` (its one
  order is in the journal, with `resolution` its outcome, such as `filled`
  or `risk_rejected:insufficient_cash`), or `abandoned` (no order will ever
  exist; `resolution` is why, such as `signal_expired`, `stop_requested`,
  `kill_switch`, `faulted`, or `server_restarted`).
- **The order.** The deployment's order is an ordinary journaled order of
  the account, with `source: {"kind": "model", "id": "<deployment_id>"}` and
  the decision's `client_order_id`.
- **Cursors.** A cursor is `d1.<n>` for deployments and
  `x2.<deployment_id>.<n>` for decisions: a keyset, so entries added meanwhile
  cause no repeats or gaps. Decision cursors are tied to their deployment.
  A cursor from another deployment or list is `400 invalid_cursor`. Clients
  treat cursors as opaque and return them unchanged.

### What a deployment is doing

Every deployment resource also has `activity`, `market`, and
`condition` ([ADR 0019](decisions/0019-deployment-observability.md)).
Reading them changes nothing. Here they are for a deployment stopped after a
completed replay, while its run's outcomes are still in this process:

```json
{
  "activity": {
    "last_candidate": {
      "code": "decision_interval",
      "ingest_sequence": "121297",
      "observation_received_at": "2026-09-25T13:39:59.960922473Z",
      "decision_sequence": null,
      "at_market": "2026-09-25T13:39:59.969574414Z",
      "at_wall": "2026-10-05T01:53:10.873999000Z"
    },
    "last_submission": {
      "code": "recorded:filled",
      "ingest_sequence": null,
      "observation_received_at": null,
      "decision_sequence": "238",
      "at_market": "2026-09-25T13:39:57.089980372Z",
      "at_wall": "2026-10-05T01:53:10.595345000Z"
    },
    "pending": null
  },
  "market": {
    "market_time": "2026-09-25T13:39:59.969574414Z",
    "source_state": "completed",
    "trading_status": null,
    "quote": {
      "available": true,
      "unavailable_reason": null,
      "ingest_sequence": "121297",
      "received_at": "2026-09-25T13:39:59.960922473Z",
      "age_ns": "8651941",
      "maximum_age_ns": "5000000000",
      "stale": false,
      "suspect": false
    }
  },
  "condition": {
    "code": "stopped",
    "reason": "stop_requested",
    "observation_received_at": null,
    "training_cutoff": null,
    "next_eligible_received_at": null,
    "not_before_received_at": null,
    "expires_at": null,
    "decision_sequence": null,
    "observed_at_market": null,
    "observed_at_wall": null
  }
}
```

- **`condition`** is what the deployment is doing now, the first that
  applies of:
  1. its lifecycle: `stopped`, `stopping`, or `faulted`, with
     `reason` the lifecycle reason;
  2. `not_running_here`;
  3. the source: `no_market_data_source`, `source_not_started`,
     `source_paused`, `source_completed`, `source_connecting`,
     `source_reconnecting`, `source_failed`, or `source_stopped`;
  4. the instrument's quote: `instrument_not_trading`, then
     `quote_unavailable` with the market state's reason, then
     `quote_suspect`, then `quote_stale`;
  5. `order_pending`, with `reason` the latest check, such as
     `awaiting_latency`, `order_matching_busy`, or `uncertain`;
  6. the latest candidate of this run: `before_training_cutoff`,
     `waiting_for_decision_interval`, `not_after_watermark`,
     `inputs_unavailable` (`reason` names the input), `stale_signal`,
     `account_unavailable`, `persistence_unavailable`,
     `decision_refused`, or `watching` after a decision;
  7. `waiting_for_candidate`.

  Its times, in the market clock, are those that apply:
  - `training_cutoff`;
  - `observation_received_at`;
  - `next_eligible_received_at`;
  - `not_before_received_at`;
  - `expires_at`.

  `observed_at_market` and `observed_at_wall` say when the fact behind it
  was observed. They are null for a lifecycle condition, whose fact is the
  durable state.
- **`activity`** holds the runner's latest outcomes, in memory only, so it
  is null after a restart:
  - `last_candidate`: a skip reason, `decided_hold`, `decided_order`,
    `order_pending`, or `inference_failed:<code>`;
  - `last_submission`: a check (`awaiting_latency`,
    `market_data_unavailable`, …), `recorded:<resolution>`,
    `abandoned:<reason>`, `not_pending:<reason>`, or `uncertain`;
  - the order it is `pending` on.

  `at_market` is the market clock and `at_wall` the server's UTC wall
  clock when each was recorded.
- **`market`** is read with the response:
  - the market clock, and the source's state;
  - the instrument's latest trading status, or null before any;
  - its execution quote: whether it is available and why not, its age in
    the market clock, the account's maximum age, and whether it is stale or
    suspect.

  It is null for a deployment of another stream.

A decision also has `fill` and `timing`:

```json
{
  "decision_sequence": "238",
  "state": "recorded",
  "resolution": "filled",
  "fill": {
    "count": "1",
    "quantity": "1",
    "notional": "335.82",
    "fees": "0.01",
    "price": "335.82",
    "first_filled_at": "2026-09-25T13:39:57.089523133Z",
    "last_filled_at": "2026-09-25T13:39:57.089523133Z"
  },
  "timing": {
    "signal_to_decision": {
      "clock": "market",
      "nanoseconds": "74358"
    },
    "execution_quote_offset": {
      "clock": "market",
      "nanoseconds": "499367813"
    },
    "signal_to_resolution": {
      "clock": "market",
      "nanoseconds": "544041796"
    },
    "signal_to_fill": {
      "clock": "market",
      "nanoseconds": "544041796"
    }
  }
}
```

- **`fill`** sums the booked fills of the decision's order: exact
  quantity, notional, and fees, the first and last fill times, and the
  price when there is exactly one fill. It is null when nothing was booked:
  a held, pending, or abandoned decision, or a recorded order that was
  cancelled for lack of liquidity or rejected. Only `fill` says something
  filled.
- **`timing`** gives market-clock durations from the signal's receive
  time, exact in nanoseconds, each derived from saved times:
  - `signal_to_decision`: to the saved decision.
  - `execution_quote_offset`: to the receive time of the quote the order
    used. This is not the order's latency.
  - `signal_to_resolution`: to the order's acceptance, which is its
    `submitted_at`, or to the abandonment.
  - `signal_to_fill`: to the last booked fill.

  A missing duration is `{"clock": "market", "unavailable": <reason>}`,
  with the reason one of `no_order`, `not_resolved`, `no_execution`,
  `no_fill`, `inconsistent_clock` (it would be negative), or
  `overflow`. A duration is never zero by default.

## Current limitations

- Local development only: no authentication, authorization, TLS, CORS, or
  rate limiting.
- One database connection for requests and one for matching; a slow
  database operation delays every request, and there is no statement
  timeout (lock waits time out).
- Loading an account per request, and per matched account, costs time
  proportional to its history.
- Matching is synchronous with the source: each top-of-book observation
  costs at least one query, and a fill several round trips, so a replay at
  `max` runs at the database's pace (about 60 s for the 10-minute recording
  locally) and a busy live feed could fall behind.
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
- Working orders can be cancelled but not replaced or amended, and the
  only time in force besides immediate-or-cancel is good-til-cancelled.
- The portfolio is a point-in-time value at the current best bid; there is no
  valuation history, equity curve, or alternative mark policy. The mark
  ignores displayed size, so it overstates what a position larger than the
  displayed bid could be sold for.
- Order and fill history pages backward from the newest item only; there is
  no time range filter or ascending order. Working orders and one order's
  fills and events page forward only.
- The top-of-book simulator's assumptions above make simulated PnL optimistic
  or pessimistic in ways not yet measured.
- Model deployments are limited as
  [model-deployments.md](model-deployments.md#limitations) describes: one
  instrument, at most one share, market IOC orders, and run counters in
  memory only.
