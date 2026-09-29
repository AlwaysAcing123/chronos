# Chronos web client

`apps/web/` is the first browser workspace for Chronos: a React and TypeScript
client of the local C++ paper-trading API. It is **simulated trading only**.
Every screen carries a paper-trading notice, every order is labelled a paper
order, and nothing in the client or the server can reach a broker, exchange,
or real-money account.

The decision record is
[decisions/0007-web-client-foundation.md](decisions/0007-web-client-foundation.md).
The API contract it consumes is [paper-trading-api.md](paper-trading-api.md).

## What it does

- Create a paper account (ID, one of the server's tradeable instruments,
  starting cash) or open an existing one by ID.
- Show the server and market-data state: API and database availability,
  stream, state, latest ingest sequence, and market time.
- Show the account's exact portfolio valuation at the best bid, with the
  quote it was marked against: best bid and ask with size, observation
  sequence, event and receive times, venue, and quality flags.
- Submit manual market or limit immediate-or-cancel paper orders, with an
  inline review before the final submission.
- Show each recorded outcome (filled, partially filled, cancelled, rejected,
  faulted) and handle submissions whose outcome is unknown (below).
- List durable order and fill history with keyset "Load older" pages. Manual
  and model orders are labelled by source and share one execution path.
- Remember the selected account across reloads.

It has no charts, streaming, models, authentication, or account list, because
the API does not provide them yet.

## Architecture

```text
apps/web/src/
  api/          the only code that talks HTTP
    client.ts       endpoint functions over fetch
    http.ts         timeouts, cancellation, response checks
    schemas.ts      Zod decoders for every resource
    submission.ts   classifies order-submission responses
    decimal.ts      lexical decimal rules; no number conversion
  state/        server state and client state
    queries.ts      TanStack Query hooks and keys (the polling transport)
    orderTicket.ts  ticket lifecycle and uncertain-order reconciliation
    accountSelection.ts  the only browser-persisted state
  presentation/ one exhaustive mapping from backend vocabulary to text
  components/   presentation only: shell, account, workspace, ticket, history
```

Components never call `fetch` and never see raw JSON. They read resources
through hooks in `state/queries.ts`, which return decoded, typed values.

### Exact values

Money, prices, quantities, percentages, versions, sequences, and timestamps
stay strings from the response to the screen. Decoders accept only the
server's canonical forms (for example `"-2.95"`, never `"-2.950"` or `-2.95`),
and branded types keep a price from being used as money. The client never
computes PnL, equity, market value, cost basis, average price, notional, or
returns; it renders the server's strings. Order inputs are validated lexically
for usability and sent exactly as typed; the server is authoritative.

Timestamps are shown as UTC with all nine fractional digits, formatted by
string slicing rather than `Date`. Browser wall-clock times appear only for
"last refresh", in local time.

### Refresh

Reads are polled; this is an interim transport, labelled in the header as
"Polling every 2 s".

| Resource | Interval |
|---|---|
| `/health` | 5 s |
| Account, portfolio, newest order and fill pages | 2 s |

- TanStack Query never overlaps two reads of one resource, pauses polling
  while the tab is hidden, and refreshes when it becomes visible again.
- A recorded submission invalidates the account's reads immediately.
- Polling of an account stops when the server reports it does not exist.
- Loaded history pages are refetched from the newest page, so a new order
  shifts the pages rather than leaving a gap between them.
- The header's "last refresh" is the oldest successful update among the reads
  on screen, so everything displayed was confirmed at or after that time.

A future WebSocket or server-sent-event transport can write the same query
keys and turn the intervals off, without changing any component.

### Unavailable data

A value the server cannot provide is shown as unavailable, never as zero.

- When the portfolio cannot be valued (`market_data_unavailable` or
  `valuation_failed`), equity, PnL, market value, and return are shown as
  unavailable with the server's reason. The ledger values (cash, position,
  cost basis, realized PnL, fees) then come from the account resource.
- A failed read shows the failure and a retry button instead of the previous
  values.
- History tables keep rows already read when a refresh fails, because recorded
  orders never change; a notice says the refresh failed.

## Orders and uncertain submissions

The ticket follows the API's reconciliation contract
([paper-trading-api.md](paper-trading-api.md#reconciling-an-uncertain-submission)):

1. **Review.** Reviewing an order generates one client order ID
   (`web-<time>-<random>`), shown in the review and the result. The manual
   source ID is `web-ticket`, naming the channel until authentication supplies
   a user.
2. **Submit.** The submit button disappears while the request is in flight,
   and a second submission of the same review is ignored. The request is never
   aborted, because aborting would not stop the server from recording it.
3. **Recorded outcome.** A response that contains `order` was recorded,
   whatever its HTTP status: 201 executed, 422 rejected, 500 faulted. The
   ticket shows the recorded status, never a generic success.
4. **Not recorded.** 400, 404, `account_version_conflict`, and
   `market_data_unavailable` prove nothing was recorded. A transient cause
   offers "Send again with the same ID".
5. **Uncertain.** A network failure, a timeout (20 s),
   `persistence_unavailable`, `internal_error`, an unrecognised error, or a
   response that does not match the contract leaves the outcome unknown. The
   ticket then looks up `GET …/orders/{client_order_id}` before anything else
   is sent:
   - found and matching the request: the recorded outcome is shown, noting it
     was recovered by lookup;
   - found but with different terms: the ID belongs to another order, and
     nothing is presented as this one;
   - `order_not_found`: the ticket offers "Send again with the same ID", which
     is safe because the server records each ID at most once per account;
   - lookup failed: the outcome stays unknown, resubmission is disabled, and
     "Check again" repeats the lookup.
6. **Duplicate.** If a resubmission returns `duplicate_client_order_id`, the
   original commit completed after the lookup; the ticket looks it up again.

The ticket never creates a new client order ID for an order whose outcome is
unknown. "Discard" appears only after a lookup proved the order unrecorded.

## Running locally

Prerequisites:

- Node.js 22.22.2 (`apps/web/.nvmrc`) with npm 10
- Everything the C++ API needs (see the [README](../README.md)), including
  Docker for PostgreSQL

```sh
# 1. PostgreSQL
docker compose up -d --wait postgres

# 2. The C++ API, replaying a local recording (see paper-trading-api.md)
cmake -S . -B build-api-databento -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON -DCHRONOS_ENABLE_PAPER_TRADING_API=ON \
  -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON -DPostgreSQL_ROOT=/opt/homebrew/opt/libpq
cmake --build build-api-databento --parallel --target chronos_paper_trading_server
CHRONOS_POSTGRES_URL='postgresql://chronos:chronos_local@127.0.0.1:55432/chronos' \
  ./build-api-databento/chronos_paper_trading_server \
    --stream-id databento-equs-mini-mbp1-20260925-1330-1340 \
    --instrument AAPL --instrument SPY --fee-per-fill 0.35 \
    --replay-dbn data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst

# 3. The web client, in another terminal
cd apps/web
npm ci
npm run dev        # http://127.0.0.1:5173
```

Without `--replay-dbn` the server has no market, and the workspace shows the
portfolio and orders as unavailable with `no_top_of_book`.

### Development proxy

The browser loads the app from Vite's origin and calls `/api/*` and `/health`
there; Vite forwards them to `http://127.0.0.1:8080`, or to
`CHRONOS_API_ORIGIN` if set. The C++ server therefore stays same-origin and
never needs CORS headers. Vite's own CORS handling is disabled, so a
cross-origin page cannot use the dev server to reach the API either. Both the
dev server (5173) and `npm run preview` (4173) bind to `127.0.0.1` and use
the same proxy.

The client also checks every response for the `X-Chronos-Trading-Mode:
paper` header. Without it, a 502, 503, or 504 means the proxy could not reach
the API and is reported as the API being unreachable; any other response, such
as another process on port 8080, is reported as unexpected rather than
decoded.

## Tests

```sh
cd apps/web
npm run typecheck   # tsc -b, strict, every warning an error
npm run lint        # ESLint strict type-checked rules, --max-warnings 0
npm test            # Vitest + Testing Library (jsdom)
npm run build       # production bundle in dist/
npm run test:e2e    # Playwright, desktop 1440×900 and Pixel 7
npm run check       # the first four together
```

Before the first browser run: `npx playwright install chromium`.

- **Unit and component tests** run against an in-memory fake at the `fetch`
  boundary (`src/test/fakeApi.ts`) using contract JSON from
  `src/test/fixtures.ts`. They assert what users see and the exact requests
  sent: account creation and opening, exact strings, validation, every order
  outcome, the reconciliation protocol, history paging, polling, persistence,
  and keyboard submission.
- **Browser tests** (`e2e/workspace.spec.ts`) run the production build with a
  stateful mock answered inside the browser. They cover keyboard trading,
  reload restoration, a lost submission response, layout without horizontal
  scrolling, mobile section navigation, reduced motion, and axe-core WCAG 2.1
  A/AA checks.
- **Live acceptance** (`e2e/live.spec.ts`) is skipped unless
  `CHRONOS_WEB_LIVE_API=1`. With the C++ server running on the recording, it
  trades through the preview proxy, adds a model-attributed order through the
  API, and checks both in the blotter after a reload:

  ```sh
  CHRONOS_WEB_LIVE_API=1 npm run test:e2e -- live.spec.ts
  ```

CI's `Web client` job runs type checking, lint, unit tests, the build, and the
mocked browser suite on Node 22.22.2 from the lockfile. It needs no server,
database, credentials, or market data; the live spec is skipped there.

## Limitations

- Polling, not streaming. Replay is unpaced, so the served quote does not
  move while the server runs.
- No authentication, and no account list: accounts are opened by ID, and the
  browser remembers only IDs it opened.
- When the portfolio is unavailable, the quote is too: the API's error does
  not include the observation, so the ask is not shown when only the bid is
  missing.
- Manual orders use the fixed source ID `web-ticket`.
- No resting orders, cancel/replace, price chart, valuation history, models,
  or experiments.
- Dark theme only; the plan's light theme is future work.
- A ticket's in-progress state is lost on reload or account switch; the
  blotter shows whatever was recorded.
