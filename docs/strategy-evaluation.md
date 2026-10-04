# Strategy evaluation

Chronos evaluates a fitted model as a trading policy in two steps, across
the Python/C++ boundary of
[ADR 0016](decisions/0016-inference-schedules-and-strategy-runs.md):

1. **Python publishes a label-free inference schedule.** It holds the
   model's predictions on every feature row of one instrument in one
   evaluation window.
2. **C++ runs the schedule.** `chronos_strategy` runs it over the verified
   dataset, through the same `PaperOrderProcessor`, risk, fill, fee,
   liquidity, accounting, and valuation code as paper trading. It runs cash
   and buy-and-hold benchmarks beside it, and publishes an immutable result.

Every figure is a paper simulation against recorded top-of-book quotes. A
result from a few minutes of data is a check of the machinery, not evidence
of profitability. The demonstration's numbers are in
[research-walkthrough.md](research-walkthrough.md).

## Commands

```sh
cd python
# Publish the schedule of one fitted model, instrument, and window.
.venv/bin/python -m chronos_research infer --tools ../build --artifacts ../data \
  --experiment <EXPERIMENT-ID> --instrument AAPL --model logistic --window validation
# Verify a schedule and rebuild it from its verified sources: identical bytes.
.venv/bin/python -m chronos_research inspect-schedule --tools ../build --artifacts ../data --schedule <SCHEDULE-ID>
# Infer, run, and verify in one step (what the research service runs).
.venv/bin/python -m chronos_research evaluate-strategy --tools ../build --artifacts ../data \
  --experiment <EXPERIMENT-ID> --instrument AAPL --model logistic --window test

cd ..
./build/chronos_strategy run --catalog data/normalized --schedules data/schedules \
  --schedule <SCHEDULE-ID> --output data/strategies \
  [--threshold-ppb 500000000] [--target-quantity 1] [--latency-ns 1000000] \
  [--decision-interval-ns 1000000000] [--starting-cash 10000] [--fee-per-fill 0.01] \
  [--maximum-quote-age-ns 5000000000] [--maximum-order-notional 1000000]
./build/chronos_strategy list --output data/strategies
./build/chronos_strategy inspect --output data/strategies --artifact <RUN-ID> \
  [--catalog data/normalized --schedules data/schedules]
```

- **Output and exit codes.** Success prints JSON and exits 0. A refused
  input prints `{"error": ...}` and exits 1, and malformed usage exits 2.
- **Option values.** Every option is given at most once. Decimals must be
  canonical (no sign, exponent, trailing zeros, or unit).
- **Reruns.** `run` prints the whole manifest and `published` or
  `already_present`. An identical rerun in the same build is
  `already_present` with the same ID.
- **`inspect`.** It checks the files, the manifest's identity, and every
  journal record. With `--catalog` and `--schedules` it runs the schedule
  again and requires the same journal and results.

## The inference schedule

`python/chronos_research/schedule.py` writes:

```text
data/schedules/<schedule-id>/manifest.json
data/schedules/<schedule-id>/schedule.ndjson    one row per feature row in the window
```

### Leakage rules

- **No evaluation rows.** `predictions.ndjson` is never the decision
  schedule. That file holds only rows the experiment could score, a set
  chosen using future label availability.
- **Every feature row in the window.** The schedule covers every feature
  row of the instrument with `start_at_ns <= available_at_ns < end_at_ns`:
  - **Validation:** `[validation start, test start)`.
  - **Test:** `[test start, end of the dataset)`.

  That includes rows the split excluded and the unlabelled tail.
- **No label or split rows.** Inference reads the verified feature rows and
  the verified experiment's stored parameters, and nothing else. It never
  opens the label or split rows. The split's manifest gives only the window
  boundaries and `usable.train.max_label_available_at_ns`, which must
  precede the window. A test removes the label and split files after
  verification and still builds the same schedule.
- **Nothing from the future.** A row has its identity, its time, its exact
  model inputs, and the prediction. There is no return, true class, label
  time, or future observation in it.
- **Contemporaneous exclusions only.** A row whose inputs are unavailable
  (a missing side, a suspect quote, a halt) has no prediction from any
  model. Its reason is `feature_unavailable:<reason>`.
- **Ancestry.** The experiment must be verified and trained on exactly the
  verified sources: the same dataset, features, labels, and split. The
  instrument must have a fitted model. An unevaluable model is refused with
  its reason, and an unknown instrument or window is refused.

### Inference

Inference runs on the exact integer inputs (`exact_inputs`, then
`to_floats`) and the stored parameters.

- **Row-pure arithmetic.** A matrix product's summation order can depend on
  the batch's shape, so the same row could get a different last bit in
  another window. The schedule therefore computes each row in one fixed
  order of elementwise operations. It agrees with the experiment's batch
  inference within `INFERENCE_TOLERANCE` (1e-12), and a test checks that.
- **Classes.** The predicted class is the largest logit, with ties going to
  the earliest class.
- **Confidence.** `confidence_ppb` is `floor(1e9 × p)`, computed exactly
  from the binary64 probability `p`.

```json
{"available_at_ns":"…","identity":{"ingest_sequence":"72801","instrument":"AAPL","stream":"…"},
 "inputs":{"raw":["100000000","…"]},
 "prediction":{"class":"up","confidence_ppb":"501539269","probabilities":["…","…","…"]},
 "record":{"name":"chronos.inference_row","version":"1"}}
```

### Schedule IDs and manifest

The schedule ID is the SHA-256 of the canonical manifest, with its newline,
without `artifact_id`, as for experiments. The manifest names:
- its sources (dataset, features, labels, split, and boundaries);
- the experiment, model, instrument, model feature set, and a SHA-256 of the
  model's stored parameters;
- the window and the training labels' availability bound;
- the inference definition, counts, environment, and the rows' checksum.

## The policy: `long_flat_threshold` version 1

One instrument per run. It is long `target_quantity` shares or flat, and
never short.

| Prediction | Confidence | Target |
|---|---|---|
| `up` | `confidence_ppb >= threshold_ppb` | `target_quantity` |
| `down` | `confidence_ppb >= threshold_ppb` | 0 |
| `flat`, or below the threshold | any | hold |

- **Pace.** A decision is made only `decision_interval_ns` or more after
  the previous one, so at most one per second by default. Rows in between,
  and rows without a prediction, are counted but are not decisions.
- **Orders.** An order is the target minus the account's actual position,
  so consecutive "up" predictions never buy twice. While an order is
  pending, the decision is recorded as `order_pending` and no new order is
  made.
- **Partial fills.** The position is what accounting says it is; the next
  decision orders the remainder.

### Defaults

| Default | Value |
|---|---|
| Threshold | 500,000,000 ppb (50%) |
| Target | 1 share |
| Latency | 1 ms |
| Decision interval | 1 s |
| Starting cash | 10,000 |
| Fee | 0.01 per fill |
| Maximum quote age | 5 s |
| Maximum order notional | 1,000,000 |

They were fixed in code before any strategy ran on real data, and no
default was changed after seeing a result. The fee is an assumption, not a
broker's schedule. It is recorded in every result; a zero fee must be given
explicitly and is then recorded as zero.

## Event loop

The runner reads the verified dataset in ingest order, without pacing. For
each event:

1. **Apply it.** It updates the instrument's latest quote and trading
   status, and the feature engine.
2. **Execute earlier orders.** Each account's pending order executes
   against this event when it is a top-of-book event of the instrument,
   received at or after the order's arrival time.
   - That first eligible observation decides the order, whatever it is: a
     suspect quote, a missing side, or a halt rejects it. Waiting for a
     better quote would be selection.
   - Trades and other instruments' quotes never execute an order.
3. **Check the schedule.** A top-of-book event of the instrument inside the
   window takes the schedule's next row. The row must have the same ingest
   sequence, time, and exact model inputs as the features the runner
   recomputes from this event; otherwise the run fails.
4. **Decide.** The policy may decide, and a decision schedules an order
   that arrives `latency_ns` later.

### Timing and the window

- **No fill on its own quote.** Latency is positive and pending orders are
  processed before the policy, so a signal never fills on its triggering
  quote. Equal timestamps are ordered by ingest sequence. A quote received
  exactly at the arrival time is eligible.
- **Warm-up.** Events before the window only warm up the market.
- **After the window.** Events at or after its end are never applied. They
  are read only so the whole dataset verifies before anything is published.
- **Expiry.** A pending order whose arrival comes after the window's last
  applied event expires with `window_ended_before_an_eligible_observation`.
- **Open positions.** They are marked, not liquidated.

### Execution

Orders are market immediate-or-cancel through `PaperOrderProcessor::submit`,
with `submitted_at` equal to the execution observation's receive time:
- buys fill at the ask and sells at the bid, up to the displayed size;
- a shortfall is `partially_filled_and_cancelled`;
- the fee is charged per fill;
- the processor's own risk checks apply, including cash, position,
  notional, quantity, suspect and stale markets, and a missing side;
- displayed liquidity is consumed per account per observation.

The strategy and the benchmarks are separate counterfactual accounts, so
they never share liquidity. The position limit equals the target, and no
order can exceed it by construction.

### Valuation

`value_portfolio` runs after each decision and each execution, and at the
window's last applied event. It values every account at that market time,
marking an open position at the best bid of the instrument's latest quote.
That quote must be valid, unsuspect, and no older than
`maximum_quote_age_ns`. Otherwise the valuation is unavailable, with
`position_mark_unavailable` and each position's reason, and never zero. The
ledger (cash, position, realized PnL, fees) is always exact.

### Metrics

- **Maximum drawdown.** The largest fall of equity from its running peak
  over the valued points, in money and as a percentage of that peak.
  Unvalued points are counted and skipped.
- **Turnover.** The sum of every fill's notional, also as a percentage of
  starting cash.
- **Return.** Net total PnL as a percentage of starting cash.

All arithmetic is fixed point with checked 128-bit intermediates; floating
point exists only in Python's model boundary.

### Benchmarks

Both benchmarks share the strategy's capital, instrument, window, latency,
fills, fees, liquidity rules, and valuation:

- **Cash** never trades.
- **Buy and hold** buys `target_quantity` once, at the strategy's first
  decision, through the same delayed IOC path. It holds to the end, and its
  partial fills are real outcomes.

## Result artifact

```text
data/strategies/<run-id>/manifest.json
data/strategies/<run-id>/journal.ndjson
```

### Journal

Entries, in processing order, are:
- `decision`: the signal row, class, confidence, position before, target,
  and reason;
- `order`: side, quantity, signal and arrival times;
- `execution`: execution time, the observation used, the outcome, the fill
  or rejection, and the cash and position after;
- `order_expired`;
- `valuation`: each account's equity, PnL, and mark, or why it is
  unavailable.

### Manifest

The manifest holds:
- the sources: dataset and events checksum, schedule and rows checksum,
  experiment, features, labels, split, model, parameters, stream, and the
  training labels' bound;
- the window, the configuration, the assumptions, and the definitions;
- the runner's name and version, and the build environment (compiler,
  platform, architecture);
- event counts, including `after_window_unused`;
- each account's final ledger and valuation, orders by outcome and
  rejection code, fills, traded quantity, turnover, and drawdown;
- the decision counts.

### Run IDs

The ID is the SHA-256 of the canonical manifest without its ID, as for the
C++ derived artifacts. The environment is part of it, so another compiler
gives another ID for the same economics. Publication is atomic and never
replaces an entry.

## Hand-checked example

The AAPL validation run (`9ca24b58…cba1`) was recomputed by a standalone
script. It uses only the standard library and exact decimals, and works
from the raw `events.ndjson` and the journal:
- **The first decision.** The quote at ingest sequence 72801 (ask 335.23)
  produced an up prediction at confidence 501,539,269 ppb.
- **The order.** It arrived 1 ms later. The first AAPL quote received at or
  after that came 126.7 ms after the signal (sequence 72817), so it bought
  at its 335.24 ask: one cent worse than the signal's own quote, which it
  could not use. Cash went to 10000 − 335.24 − 0.01 = 9664.75.
- **Every fill.** Replayed with average-cost accounting: 39 fills, fees
  0.39, turnover 13,070.83, and realized PnL −10.92.
- **The end.** It finishes long one share, marked at the last bid of
  334.43. Equity is 9653.56 + 334.43 = 9987.99, and net PnL is −12.01.
- **Drawdown.** 12.01 over 168 valued points.
- **Buy and hold.** Bought one share at 335.24, for equity 9999.18 and net
  −0.82.

The same check passes for the other three runs.

## Tests

`tests/strategy_tests.cpp` covers, in hand-checkable synthetic sessions:
- **Trading.** Buy, hold, and sell with spread cost, fees, and exact PnL;
  the threshold boundary; no signal means no trades; down never goes
  short.
- **Execution.** Partial fills, rejections (cash, notional, suspect,
  missing side, halt), and pending orders blocking new ones.
- **Timing.** Latency and equal timestamps, and expiry at the window's end.
- **Events.** Warm-up; changing a later event never changes an earlier
  decision; events after the window cannot change a result.
- **Schedules and valuation.** Schedules that disagree with recomputed
  features; stale marks that make a valuation unavailable; drawdown and
  turnover.
- **Artifacts.** Deterministic journals; publication, verification,
  reproduction, and reuse; tampered runs and schedules, including a
  training label known in the window.

`python/tests/test_schedule.py` covers:
- complete windows with the unlabelled tail;
- the absence of label fields, and that the label and split rows are never
  read;
- per-row predictions, and the majority model;
- refused models, instruments, windows, and experiments;
- training chronology, tampering, the CLI, and decisions on tail rows.

## Limitations

- **One policy.** One instrument, one share, market IOC orders only.
- **Top-of-book execution.** No depth, queue position, hidden liquidity,
  market impact, borrow, or exchange fees.
- **Assumed costs.** A flat fee per fill, and a fixed latency with no
  jitter.
- **Thin evidence.** One short session per result: no walk-forward, no
  confidence intervals.
