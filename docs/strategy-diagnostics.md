# Strategy diagnostics

Strategy diagnostics explain the runs of existing walk-forward studies.
Each run's exact net PnL is split into the spread paid at execution, the
midpoint's move after the signal, fees, the final mark at the bid, and
fixed-point rounding. Its decisions are also described against the labels,
after the fact. The decisions are in
[ADR 0024](decisions/0024-strategy-diagnostics.md).

**What they are not.** Diagnostics do not fit, schedule, or run anything,
and they do not replay a run. They are not a strategy, a threshold search,
or a ranking. A few short recordings describe what happened in them, not
what would happen in others: there is no significance test, annualized
ratio, combined equity curve, or winner.

## Artifact

```text
<artifacts>/strategy_diagnostics/<diagnostic_id>/manifest.json    chronos.strategy_diagnostics/2
<artifacts>/strategy_diagnostics/<diagnostic_id>/fills.ndjson     one row per fill
<artifacts>/strategy_diagnostics/<diagnostic_id>/episodes.ndjson  one row per holding episode
```

- **Request.** The request is `{"schema":
  {"name": "chronos.strategy_diagnostics_request", "version": "1"},
  "study_ids": [...]}`: one to four distinct study IDs, in ascending order.
- **Identity.** The ID is the SHA-256 of the canonical manifest without
  `diagnostic_id`, with its newline. No build environment is recorded,
  because no figure uses floating point. The same sources give the same ID
  anywhere.
- **Publication.** Publication is atomic and read-only, and never replaces
  an entry. An identical request is `already_present`. Version 1, the first
  reviewed draft, is refused as another schema.
- **Manifest.** The manifest names every source by ID and checksum: each
  study, plan, and collection, each fold's dataset events, feature rows, and
  label rows, and each run's journal and schedule rows. It holds every run's attribution per account,
  its decision cohorts, the pools over folds (and over studies with several
  studies), and the definitions.

## Figures

Prices are 10^-9, and quantities and money 10^-8. A midpoint can be half a
price unit, so every figure is an exact decimal of at most 18 places. These
`diagnostic_decimal` strings are their own type in the API and the web
client. The Money schema is unchanged.

For fill *i*: *q_i* is its signed quantity (+ buy, − sell), *p_i* its
price, *n_i* its booked notional (signed like *q_i*), and *f_i* its fee.
*m_i* is the (bid + ask) / 2 of its execution quote, and *s_i* that of its
decision's signal quote. *Q* is the final position, *B* the final mark's
bid, *M* that quote's midpoint, and *V* the booked final market value.

| Term | Definition |
| --- | --- |
| `booked_net_pnl` | the engine's final `net_total_pnl` |
| `midpoint_inventory_pnl` | Q·M − Σ q_i·m_i |
| `execution_spread_cost` | Σ q_i·(p_i − m_i) |
| `fees` | Σ f_i |
| `terminal_bid_markdown` | Q·(M − B) |
| `rounding_adjustment` | (V − Q·B) − Σ (n_i − q_i·p_i) |
| `observed_midpoint_movement` | Σ q_i·(m_i − s_i); positive is adverse |
| `signal_reference_pnl` | Q·M − Σ q_i·s_i |
| `inventory_pnl_at_bid` | Q·B − Σ q_i·m_i |

The two rounding parts (`market_value_rounding` and `notional_rounding`)
are reported too. Both identities must hold exactly, or nothing is
published:

```text
booked_net_pnl         = midpoint_inventory_pnl − execution_spread_cost − fees
                         − terminal_bid_markdown + rounding_adjustment
midpoint_inventory_pnl = signal_reference_pnl − observed_midpoint_movement
```

- **Execution rules.** A balanced ledger is not enough. Every booked
  notional must be the price times the quantity rounded half away from zero
  to 10^-8, exactly as the engine rounds it, and so must the final market
  value *V* of *B*·*Q*; every fill's fee must be the run's `fee_per_fill`.
  Otherwise nothing is published (`ledger_invalid`).
- **Missing values.** A figure that needs a value that does not exist is
  `{"unavailable": "<reason>"}`. Nothing is substituted: not the ask, a
  trade, the cost basis, an earlier quote, or zero.
- **When the mark matters.** *M* and *B* matter only for an open final
  position.
- **Unavailable final valuation.** Every term that needs it is unavailable,
  with the engine's reason.
- **No final ask.** Only *M* is missing, so the first identity is checked
  at the bid (`reconciled_at_bid`).
- **Wide quotes.** A midpoint is taken between a quote's own sides. When
  one side is far from the other, the split between spread and inventory
  PnL moves with that midpoint, but their sum and the booked figures do
  not.

**Joins.** Every join uses a complete identity:
- An event is an ingest sequence of the run's own dataset. Only top-of-book
  events of its instrument are quotes.
- A decision is joined by its schedule row's identity, time, class, and
  confidence. Its signal quote's receive time must be the order's signal
  time.
- An order is joined by (account, decision), and its one resolution by
  (account, client order ID).
- The execution quote must be the first quote at or after arrival, before
  the exclusive window end.

A trade print, another instrument, another dataset, or a duplicate or
missing link is refused (`join_failed`).

**Timing.** The configured latency is reported apart from three observed
durations per execution: signal to arrival, arrival to execution, and
signal to execution. **Episodes** run from leaving a flat position to
returning to it (`completed`, with booked PnL), or to the window's end
(`censored`, marked at the final valuation).

## Decisions

There are four cohorts, each within the one before:
1. scorable predictions (schedule rows with a prediction);
2. sampled decisions (the strategy's journaled decisions);
3. order-scheduling decisions;
4. orders with fills.

Each cohort reports:
- its items, and each item's label status under the plan's scoring rule
  (`scored`, `label_unavailable:<reason>`, `label_after_scoring_cutoff`);
- predicted and realized classes, and confusion counts;
- accuracy and balanced accuracy, as exact fractions with a 6-place
  rounding, or undefined when nothing is scored;
- ten fixed confidence bins, tenths of [0, 1] by the predicted class's
  confidence;
- reasons or order outcomes;
- its scored items' future returns in the labels' own unit, `return_raw`
  ((future midpoint − current midpoint) / current midpoint, in parts per
  billion): rows, exact sum, minimum, maximum, and exact mean, overall, per
  predicted class, and per confidence bin, with the items not scored counted.

Cohort 1 must equal the study's own scores, every count, and the accuracy
to the bit. Pools sum counts before computing ratios and means, and never
average them. A pool with no completed run has no financial figures
(`no_completed_runs`), though its counts are zero; across studies, a study
with no completed fold of a cell is listed as missing, not added as a zero.
Labels never reach a model, a policy, an order, or a choice of what to run.

## Commands

```sh
cd python
.venv/bin/python -m chronos_research diagnose-strategies --tools ../build --artifacts <artifacts> \
  --study <study-id> [--study <study-id> ...] [--progress]
.venv/bin/python -m chronos_research list-strategy-diagnostics --artifacts <artifacts>
# Verified as stored: identity and every file's checksum.
.venv/bin/python -m chronos_research show-strategy-diagnostics --artifacts <artifacts> --diagnostics <id>
# Rebuilt from its sources, verified again, with identical bytes required. Publishes nothing.
.venv/bin/python -m chronos_research inspect-strategy-diagnostics --tools ../build --artifacts <artifacts> --diagnostics <id>
# Independent: the standard library alone, nothing from Chronos.
.venv/bin/python -I -S ../tools/strategy_diagnostics_audit/audit.py <artifacts> <id>
```

Generation verifies its sources without refitting:
- each study and plan by identity, with the plan rebuilt from its request
  over the collection's members;
- each member through the C++ verifiers, which recompute features and labels
  in memory and write nothing;
- every schedule, every run's manifest and journal, and each dataset's events
  by checksum;
- each run against its cell, as the study requires it.

It never calls `inspect-walk-forward-study`, which refits fold models.

The auditor first authenticates every source: each manifest's identity and
schema, each consumed file's size, checksum, and row count, and every
binding from study to plan, collection member, dataset, features, labels,
schedule, and run. It then recomputes every row, account, cohort, and pool
from the raw journals, events, schedules, features, and labels. It compares everything
except prose field by field, prints one JSON report, and exits 1 on any
difference. Its tests plant errors with consistent checksums and identity,
so that only recomputation can find them.

## Service and browser

| Route | Purpose |
| --- | --- |
| `POST /api/v1/research/strategy-diagnostics` | `{"study_ids": [...]}`: queue a `strategy_diagnostics` job; the only way to generate |
| `GET /api/v1/research/strategy-diagnostics?limit&cursor&study` | Verified diagnostics, optionally of one study |
| `GET /api/v1/research/strategy-diagnostics/{id}` | One artifact, with every verification level reported apart |
| `GET /api/v1/research/strategy-diagnostics/{id}/fills?run&account&limit&cursor` | One run's fill rows |
| `GET /api/v1/research/strategy-diagnostics/{id}/episodes?run&account&limit&cursor` | One run's holding episodes |

- **Validation.** Unknown fields and identifiers that are not IDs are
  `400`. Counts and duplicates are `422 out_of_domain`, and an unknown study
  is `422 study_not_found`, before any job exists. Studies in another order
  are the same deduplicated job.
- **Jobs.** The job runs `diagnose-strategies --progress` on the bounded
  workers and reports its stage, fold, model, and scenario. Jobs have a
  timeout, are marked interrupted on restart and never resumed, and have
  their process group killed at shutdown. A refusal is `diagnostics_refused`
  with its reason.
- **Reads.** Reads never start work and never name a path.
- **Verification levels.** A detail read reports each level apart:
  - the artifact's checksums, read now;
  - every source, checked now: each study's, plan's, and collection's
    identity and what it names, and each dataset's, feature artifact's,
    label artifact's, schedule's, and run journal's identity, bindings, and
    consumed file's size and SHA-256, compared with what was read. A
    file's SHA-256 is reused from this service's own earlier hash only while
    its device, inode, size, modification time, and change time are
    unchanged; the read says how many files it hashed and how many it reused;
  - each run's, fold prediction cohort's, and pool's trust: verified only if
    every source it rests on verified, otherwise the sources that did not;
  - what generation verified;
  - recomputed inspection and the independent audit, which a page never
    runs (their commands are shown);
  - the engine replay, which the study recorded.

In the web client, a study page has a **Strategy diagnostics** panel that
lists the study's diagnostics and offers **Explain this study's runs**. The
diagnostics page is at `#/research/walk-forward/diagnostics/<id>`. A run is
deep-linked as `…/runs/<run-id>/<strategy|buy_and_hold|cash>`. It shows:
- the verification levels, and every run with its strategy terms;
- the selected run, with fold, model, and scenario selectors and the
  simulated account, which is never the paper account;
- the run's PnL bridge, account, orders, and timing, its differences from
  each benchmark, its decision cohorts and confidence bins, and its episodes
  and fills, paged;
- pools over folds, across studies, and the definitions.

Every figure is the service's exact string. Missing and unverified values
are labelled, never zero. A run, cohort, or pool resting on a source that
did not verify is labelled unverified, and its identities say only that
they held when generated; figures resting only on verified sources are not
downgraded.

## Tests

```sh
cd python
CHRONOS_REQUIRE_TOOLS=1 CHRONOS_DATASET=../build/chronos_dataset CHRONOS_SYNTHETIC_DATASET=../build/chronos_synthetic_dataset \
  .venv/bin/python -m pytest tests/test_strategy_attribution.py tests/test_decision_diagnostics.py \
    tests/test_strategy_diagnostics.py tests/test_strategy_diagnostics_audit.py tests/test_strategy_diagnostics_routes.py \
    tests/test_diagnostic_sources.py
cd ../apps/web
npx vitest run src/components/walkForward/Diagnostics.test.tsx && npx playwright test diagnostics.spec.ts
```

- **Hand-built journals.** Hand-built journals in the engine's format
  (`tests/attribution_runs.py`) cover:
  - hand-computed round trips, open positions, favourable and adverse moves;
  - partial and multiple fills, fees on booked fills only;
  - rejections, cancellations, halts, and expiries;
  - missing signal midpoints, missing final asks, and unavailable, stale, or
    suspect valuations;
  - half-unit midpoints, ties in both directions, and encodings;
  - ledgers that balance but were booked with ties rounded toward zero, a
    notional one unit off, or another fee, each refused by the builder and
    by the auditor;
  - trade prints, same-time quotes, other instruments, and the exclusive
    end;
  - repeated sequences across datasets, and every wrong, duplicate, missing,
    or malformed link.
- **Synthetic studies.** Synthetic studies run through the engine cover:
  - sub-cent prices and fractional displayed sizes;
  - unmarkable positions;
  - partial studies and failed cells;
  - pooling across studies;
  - labels hidden without changing any account;
  - deterministic reuse and inspection;
  - tampering, symlinks, and corruption;
  - changed sources, the CLI, the auditor's planted errors, and the
    service's jobs and routes;
  - changed, missing, and forged feature, label, and dataset sources,
    refused by the auditor by name;
  - pools with no completed run, partial coverage, a missing study, an
    unavailable valuation, and a genuine zero, each distinct;
  - a read after events, labels, or features change (in place, at the same
    size and modification time) or go missing, downgrading exactly the
    runs, cohorts, and pools resting on them.

## Limitations

- Top-of-book only: the spread cost is measured against the displayed
  midpoint, and is not market impact.
- Quotes are taken as recorded: a quote with one side far from the other is
  not flagged suspect by the feed, and no threshold is invented to call it
  wide. The quoted spreads are reported instead.
- A read checks every source, but reuses a digest while the file system
  says the file is unchanged: a change that kept the device, inode, size,
  modification time, and change time would not be seen until the CLI
  inspection or the audit, which always read everything.
