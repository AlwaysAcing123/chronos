# Research walkthrough: from a recording to a strategy result

One command takes the local `AAPL`/`SPY` recording through every research
artifact Chronos has, verifies all of it, and evaluates a trading policy
beside two benchmarks. Running it again verifies and reuses everything.

> **What this shows, and what it does not.** It verifies the machinery: that
> every artifact is reproducible from its source, that no step can see the
> future, and that simulated trading is exact. It cannot show that a model
> generalizes or that a strategy would make money. The data is ten minutes
> of one session; each evaluation window is about two minutes; prediction
> accuracy is not profit; and the simulated fills use top-of-book quotes
> with an assumed fee. The strategy lost money in all four runs below.

## Run it

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCHRONOS_ENABLE_DATABENTO_ADAPTER=ON
cmake --build build --parallel
cd python
.venv/bin/python -m chronos_research demo --tools ../build --artifacts ../data \
  --recording ../data/raw/databento/equs-mini/2026-09-25-aapl-spy.dbn.zst \
  --stream-id databento-equs-mini-mbp1-20260925-1330-1340
```

- **Tools.** `--tools` is the build directory. The demo runs only these
  fixed executables, with explicit argument vectors: `chronos_databento_normalize`,
  `chronos_dataset`, and `chronos_strategy`. (For synthetic data it runs the
  test-only `chronos_synthetic_dataset` instead of the Databento normalizer.)
- **Artifacts.** `--artifacts` is a directory of artifact roots:
  `normalized/`, `features/`, `labels/`, `splits/`, `experiments/`,
  `schedules/`, and `strategies/`. Under `data/` they are all ignored by
  Git, as are the licensed recording and everything derived from it.
- **IDs.** Every ID comes from the tool that produced the artifact. Nothing
  is hard-coded.
- **Synthetic mode.** `--synthetic-events <csv>` replaces `--recording`, for
  CI and tests; `python/tests/test_demo.py` runs it.

It takes about a minute on an Apple-silicon Mac. The output is one JSON
document with:
- every artifact, its kind, ID, directory, and `published` or `already_present`;
- each model, fitted or unevaluable with its reason;
- the prediction headline, and each strategy run's outcome;
- the limitations.

No network, credential, or paid data is involved.

## What each step makes

| Step | Artifact | What it is | ID (2026-09-25 recording) |
|---|---|---|---|
| 1 | Dataset | Every event of the recording, normalized and verified | `02adcb6d…55ae8` |
| 2 | Features | One `top_of_book` v1 row per quote, using only that quote and the instrument's latest status | `06e5d86e…86854` |
| 3 | Labels | The midpoint return to the first quote at least 1 s later, and when it became known | `2edb8ddf…6efba` |
| 4 | Split | 60/20/20 chronological train, validation, and test; labels that cross a boundary are purged | `4f4bed5c…20fa2` |
| 5 | Experiment | A majority-class and a logistic-regression model per instrument, trained on train only and scored on validation and test | `05be6dd4…16ba62` |
| 6 | Schedules | One fitted model's predictions on every feature row of one instrument in one window, without labels | four, below |
| 7 | Strategy runs | The policy and two benchmarks simulated over each schedule | four, below |

The full IDs are in the demo's output. Steps 1–5 are described in
[normalized-market-datasets.md](normalized-market-datasets.md),
[feature-engine.md](feature-engine.md),
[labels-and-splits.md](labels-and-splits.md), and
[baseline-evaluation.md](baseline-evaluation.md). Steps 6–7 are in
[strategy-evaluation.md](strategy-evaluation.md).

### The data and windows

- **Data.** 121,303 events from 13:30:00 to 13:40:00 UTC on 2026-09-25:
  119,282 quotes and 2,021 trades.
- **Training.** Feature rows from 13:30:00.0017 to 13:35:19.0903. Every
  usable training label was known by 13:35:19.0870, before validation began.
- **Validation.** From 13:35:19.0903 to 13:37:46.5779.
- **Test.** From 13:37:46.5779 to the end of the data.

### Training, validation, and test

Models are fitted per instrument on training rows only. Both logistic
models converged: AAPL in 34 iterations, SPY in 60. They are scored on
validation and test, with no test row used for any choice. Pooled over both
instruments:

| Window | Model | Accuracy | Balanced accuracy | Rows |
|---|---|---:|---:|---:|
| Validation | Majority class | 0.479 | 0.334 | 23,746 |
| Validation | Logistic | 0.518 | 0.366 | 23,746 |
| Test | Majority class | 0.484 | 0.336 | 23,610 |
| Test | Logistic | 0.518 | 0.365 | 23,610 |

These are classification scores over three classes (down, flat, up). The
logistic model never predicts "flat". Its edge over the majority baseline
is small and comes from one short session.

### From predictions to simulated trades

Each schedule infers on every AAPL or SPY feature row of its window. That
includes rows the experiment never scored: the rows whose labels cross the
next boundary, and the final rows whose labels were never known. The C++
runner recomputes each row's features from the verified dataset and refuses
a schedule that disagrees.

The fixed default policy:
- **Signals.** An up prediction with confidence of at least 50% targets 1
  share; a down prediction with at least 50% targets flat; anything else
  holds.
- **Pace.** At most one decision per second.
- **Orders.** Market IOC orders arrive 1 ms after their signal and cost a
  0.01 fee per fill.
- **Capital.** 10,000 of starting cash.

| Run | Strategy net PnL | Fills | Fees | Max drawdown | Buy and hold net PnL | Cash |
|---|---:|---:|---:|---:|---:|---:|
| AAPL validation | −12.01 | 39 | 0.39 | 12.01 | −0.82 | 0 |
| AAPL test | −8.94 | 36 | 0.36 | 9.33 | 0.72 | 0 |
| SPY validation | −0.50 | 13 | 0.13 | 0.68 | −0.24 | 0 |
| SPY test | −0.45 | 19 | 0.19 | 0.74 | 0.25 | 0 |

The strategy loses in every window, and loses more than buying and
holding. Each round trip pays the spread (it buys at the ask and sells at
the bid) plus two fees. Predicting one-second moves 52% of the time does
not pay for that.

These figures were checked independently. A standalone standard-library
script with exact decimals recomputed them from the raw dataset events and
the journal:
- every fill, and the first order's latency and execution quote;
- the fees, turnover, and realized PnL;
- equity, and the maximum drawdown.

See [strategy-evaluation.md](strategy-evaluation.md#hand-checked-example).

## Inspect and reuse

```sh
.venv/bin/python -m chronos_research inspect --output ../data/experiments --experiment <EXPERIMENT-ID>
.venv/bin/python -m chronos_research inspect-schedule --tools ../build --artifacts ../data --schedule <SCHEDULE-ID>
../build/chronos_strategy inspect --output ../data/strategies --artifact <RUN-ID> \
  --catalog ../data/normalized --schedules ../data/schedules
```

A second `demo` run reports `already_present` for every artifact, with
output identical apart from those outcomes. The same evaluations are
available from the web workspace's Research view; see
[research-service.md](research-service.md).
