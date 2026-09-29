# Chronos web workspace

A React and TypeScript client of the local C++ paper-trading API. Simulated
trading only: no real money, broker, or exchange is involved.

```sh
npm ci
npm run dev        # http://127.0.0.1:5173; needs the API on 127.0.0.1:8080
npm run check      # type check, lint, unit and component tests, build
npm run test:e2e   # Playwright against the build, with a mocked API
```

Architecture, the exact-decimal rules, uncertain-order reconciliation, and
the full test guide are in [../../docs/web-client.md](../../docs/web-client.md).
