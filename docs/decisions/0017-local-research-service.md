# ADR 0017: A separate local research service

- Status: Proposed
- Date: 2026-10-04

## Context

The web workspace needs to start strategy evaluations and read their
results. An evaluation verifies artifacts, runs model inference in Python,
and runs the C++ strategy runner: tens of seconds of work.

The existing C++ paper-trading server owns trading accounts, PostgreSQL,
the market state, and the replay. It never loads Python or models. Research
must not change any of that.

## Decision

Research gets its own local process, `python -m chronos_research serve`.
- **Isolation.** It is loopback only, uses the standard library, and is
  separate from the trading server. The web client reaches it through the
  Vite proxy under `/api/v1/research`, with its own response header.
- **Conventions.** It copies the trading API's: strict JSON, string
  decimals, RFC 3339 nanoseconds, the error envelope, strict queries, and
  opaque cursors with limits from 1 to 200.
- **Jobs.**
  - A `POST` only validates and queues.
  - A bounded worker pool runs each job as one fixed child command,
    `evaluate-strategy`, with an argument vector and a time limit.
  - Job records are replaced atomically.
  - Unfinished jobs become `interrupted` on restart.
  - Identical requests deduplicate.
- **Results.** The immutable artifacts are the results. The service reads
  them by validated ID only, never through a symlink, and verifies the
  exact bytes it serves.

**Rejected:**
- *Routes in the C++ server*: that would mean embedding Python, or running
  subprocesses from the trading server, and would couple model work to
  account state.
- *A web framework*: a new dependency tree for a dozen routes.
- *Threads running jobs in-process*: Python inference and numpy in the HTTP
  process, with no hard time limit.
- *Durable resumption of interrupted jobs*: honest interruption is simpler
  and never claims work that did not finish.

## Consequences

- **Isolation by construction.** A research failure cannot affect trading,
  and the trading API never sees research traffic.
- **Two local processes.** The developer runs two, and the UI says how to
  start the missing one.
- **No authentication.** Like the trading server, the service is for the
  local machine only.
