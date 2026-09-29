# ADR 0007: Web-client foundation

- Status: Proposed
- Date: 2026-09-28

## Context

The paper-trading API now covers accounts, orders, history, and exact
portfolio valuation. The plan's product layer is a React and TypeScript web
application, and the first slice is a manual paper-trading workspace. It must
not become a second accounting engine, must never imply real-money trading,
and must be ready for live updates without being rebuilt when they arrive.

## Decision

### React, TypeScript, and Vite

The client is a single-page React application in `apps/web/`, written in
strict TypeScript and built with Vite.

- React is the plan's product-layer choice, has the most mature accessibility
  and testing ecosystem (Testing Library, axe-core), and needs no server
  runtime.
- Strict TypeScript (`exactOptionalPropertyTypes`, `noUncheckedIndexedAccess`)
  and runtime decoding together make the API contract a checked boundary.
- Vite serves and bundles a static application with a development proxy and
  no framework server. Next.js or server-side rendering would add a second
  backend and runtime without a product need; the C++ API remains the only
  server.

Dependencies are deliberately few:

| Package | Purpose |
|---|---|
| `react`, `react-dom` | Interface |
| `@tanstack/react-query` | Server state: polling, deduplication, cancellation, invalidation |
| `zod` | Runtime decoding of every response |
| `vite`, `@vitejs/plugin-react` | Development server, proxy, bundling, fast refresh |
| `typescript`, `eslint`, `@eslint/js`, `typescript-eslint`, `eslint-plugin-react-hooks` | Type checking and lint |
| `vitest`, `jsdom`, `@testing-library/*` | Unit and component tests |
| `@playwright/test`, `@axe-core/playwright` | Browser tests and accessibility checks |

There is no component library, CSS framework, router, or state library;
CSS modules with design tokens cover the interface. TypeScript is pinned to
6.0 because `typescript-eslint` does not yet support the native TypeScript 7
compiler.

### Financial decimals remain strings

Every decimal, integer, and timestamp stays the server's string from the
response to the screen. Decoders accept only canonical renderings, and
branded types distinguish money, prices, quantities, and percentages.

A JavaScript `number` cannot represent the API's 8- and 9-decimal fixed-point
values or 64-bit integers exactly, and converting would reintroduce the
rounding the server exists to avoid. Recomputing PnL or equity in the browser
would also create a second, divergent accounting truth. The browser therefore
never calculates financial values; it only validates input text lexically and
compares decimal strings by removing trailing zeros.

### Polling is an interim transport

Reads are polled every 2 s (account, portfolio, newest history pages) and 5 s
(health). The server offers no subscription yet, and a static replay changes
only when orders are submitted, which already invalidate the reads. TanStack
Query prevents overlapping reads, pauses while the tab is hidden, and makes
the intervals configuration rather than code.

Polling is not presented as streaming: the header says "Polling every 2 s"
and shows the last confirmed refresh.

### Development proxy instead of CORS

The browser talks only to the Vite origin, which forwards `/api` and `/health`
to the C++ server on `127.0.0.1:8080`. The C++ API keeps its property that it
never approves a cross-origin request, so a malicious page cannot submit
orders to a developer's local server. Vite's own CORS handling is disabled for
the same reason. A production deployment would serve the static build from
the same origin as the API behind one reverse proxy.

### Live updates later, without new components

Components read resources only through hooks in `state/queries.ts`, keyed by
`queryKeys`, and receive decoded values plus loading and error state. A
WebSocket or server-sent-event client will:

1. subscribe per account, and write decoded events into the same query keys
   with `queryClient.setQueryData` (or invalidate them);
2. set the refresh-policy intervals to `false` while connected, and return to
   polling on disconnect;
3. report connection state through the existing header status.

Presentation components need no change because their inputs are the same
query results.

### Uncertain submissions

The ticket implements the API's reconciliation contract: one client order ID
per reviewed order, reused for every retry; a lookup by that ID whenever the
outcome is unknown; resubmission only after the lookup proves the order
unrecorded; and a check that a found order matches the request. Details are
in [../web-client.md](../web-client.md).

## Consequences

- One small backend change: `/health` now lists `tradeable_instruments`, so
  the create form offers the server's configured instruments instead of
  hard-coding them.
- The client works only same-origin or through a proxy; there is still no
  authentication, so the stack remains localhost-only.
- Adding a field to an API resource is compatible (decoders ignore unknown
  keys), but changing a representation, such as a decimal's canonical form,
  fails decoding visibly rather than being rendered wrongly.
- Polling costs one request per resource every 2 s per open tab, all
  serialized by the server's single database connection. That is acceptable
  locally and is the main reason for the planned live transport.

## Alternatives considered

- **Next.js or Remix.** Server rendering and routes add a JavaScript server
  and deployment model the product does not need yet.
- **Plain `fetch` with component state.** Polling, deduplication,
  cancellation, and invalidation would be reimplemented by hand.
- **A decimal library such as decimal.js.** Unnecessary, because the browser
  performs no arithmetic; importing one would invite it.
- **Broad CORS on the C++ server.** It would let any web page drive a
  developer's local paper-trading API.
- **MSW for API mocking.** An in-memory `fetch` fake and Playwright routes
  cover the same need without a service worker or another dependency.
