# Baseline predictive evaluation

`python/chronos_research` trains and evaluates two baselines on verified
Chronos artifacts and publishes the result as an immutable evaluation
artifact:
- a **no-skill majority-class** model;
- a **multinomial logistic regression**.

It is a **predictive** evaluation only. There is no trading strategy,
position sizing, execution model, cost model, or profit and loss, and its
numbers say nothing about profitability. The decisions are in
[ADR 0015](decisions/0015-python-research-boundary-and-baseline-artifacts.md).

## Setup and commands

The package needs Python 3.12 and exactly the pinned dependencies:
- **Pins:** NumPy 2.5.3 and scikit-learn 1.9.1, with every transitive
  package pinned by hash in `python/requirements.lock`.
- **C++ tool:** a built `chronos_dataset`, which it runs to verify its
  inputs.

```sh
cd python
uv venv --python 3.12 .venv                     # or: python3.12 -m venv .venv
uv pip install --python .venv/bin/python --require-hashes -r requirements.lock
# (with pip: .venv/bin/python -m pip install --require-hashes --no-deps -r requirements.lock)

.venv/bin/python -m chronos_research evaluate \
  --chronos-dataset ../build/chronos_dataset \
  --features ../data/features --labels ../data/labels --splits ../data/splits \
  --split-artifact <SPLIT-ID> --output ../data/experiments [--catalog ../data/normalized]
.venv/bin/python -m chronos_research inspect --output ../data/experiments --experiment <EXPERIMENT-ID>
```

Every option is given at most once, never abbreviated, and never empty.
Success prints JSON and exits 0. A refused input, or an unexpected failure,
prints `{"error": ...}` and exits 1, and malformed usage exits 2.
`data/experiments/` is ignored by Git.

Checks: `ruff format --check .`, `ruff check .`, `mypy` (strict), and
`pytest`. The integration tests need `CHRONOS_DATASET` and
`CHRONOS_SYNTHETIC_DATASET` pointing at the built C++ tools, and are skipped
without them unless `CHRONOS_REQUIRE_TOOLS=1`, as in CI.

## The trust boundary

Python never decides whether a Chronos artifact is valid. `evaluate` runs:
1. `chronos_dataset splits inspect --labels <root>`: the split, recomputed
   from its label artifact;
2. `labels inspect --features <root>`: that label artifact, recomputed from
   its feature artifact;
3. `features inspect`: that feature artifact, also recomputed from its
   dataset with `--catalog`.

It then cross-checks the ancestry the verifier reported:
- each artifact's source IDs and checksums;
- the dataset ID, stream, horizon, and row counts.

Then it reads the three rows files itself, hashing every byte. Rows are used
only if those bytes have exactly the checksum the verifier checked, so an
artifact cannot change between verification and use.

Python reads only the fields it needs and copies none of the strict
canonical codec. It refuses unexpected schemas, and every value it takes
stays an exact integer until the model boundary.

## Model-ready rows

**The join.** Rows are joined by identity, (stream, instrument, ingest
sequence), never by line number. Every feature row must have exactly one
label row and one split row, and nothing may be left over. The join refuses:
- missing or duplicate identities, including a row assigned to two splits;
- prediction times or label times that differ between artifacts;
- a split assignment that contradicts its period or its label;
- a usable train or validation label known at or after the next period
  begins.

**Exclusions.** Each row is then either usable or excluded, and every
exclusion is counted by period, instrument, and reason:
- `label_unavailable:<label reason>`;
- `label_crosses_validation_start`;
- `label_crosses_test_start`;
- `feature_unavailable:<feature reason>`.

The model feature set is **`top_of_book_baseline` version 1**: five inputs,
each from the feature row alone:

| Input | Source | Scale |
|---|---|---|
| `spread` | feature `spread` | 1e-9 price |
| `imbalance` | feature `imbalance` | 1e-9 |
| `microprice_minus_midpoint` | feature `microprice` − feature `midpoint`, formed exactly on the raw integers | 1e-9 price |
| `bid_size` | feature `bid_size` | 1e-8 shares |
| `ask_size` | feature `ask_size` | 1e-8 shares |

**Point in time.** Every input is a value of the feature row, computed when
its triggering event arrived. No label, label time, future observation,
return, or split assignment is an input. The conversion to `float64`,
`raw / scale` correctly rounded, happens once, at the model boundary. Tests
mutate everything a leaky fit could use and require byte-identical fitted
models:
- validation and test labels, returns, and inputs;
- every label availability time;
- the size of every training return.

A positive control shows the comparison detects a single changed training
label.

**Targets.** The classes are the label artifact's exact directions, in the
fixed order **down, flat, up**, which every file records. Nothing is
collapsed or fabricated.

## Baselines

Each instrument gets its own models, fitted on its **training rows only**.
That keeps an instrument's identity or price level from becoming an
accidental predictor.

**No-skill.**
- Its probabilities are the training class frequencies.
- Its prediction is the most frequent training class, with ties going to the
  earliest class in the order down, flat, up.
- It never sees validation or test labels.
- It sets the floor: any model that cannot beat "always predict the
  commonest past direction" has learned nothing usable.

**Logistic regression.** A scikit-learn `Pipeline` of `StandardScaler` and
`LogisticRegression`, which is multinomial for three classes:

| Setting | Value |
|---|---|
| Penalty | L2, `C = 1.0`, `l1_ratio = 0` |
| Solver | `lbfgs` (deterministic), `tol = 1e-6`, `max_iter = 1000` |
| Seed | `random_state = 0` (unused by lbfgs, fixed anyway) |
| Threads | 1, via threadpoolctl |
| Class weights | none |
| Shuffling | none |
| Search | none: one fixed configuration |

- **Fitting.** The scaler and the classifier are fitted on training rows
  only. A zero-variance input, such as a constant spread, gets scale 1.
- **Evaluability.** The model is fitted only if every class has at least 10
  training rows (`insufficient_class_support` otherwise). A fit that raises
  `ConvergenceWarning` or reaches `max_iter` is `did_not_converge`, and one
  that raises any other warning, such as a numerical one, is `fit_warning`.
  Either way the model is reported as unevaluable with its reason, never
  used.
- **What it shows.** It tests whether contemporaneous top-of-book shape
  (spread, imbalance, microprice offset, sizes) carries linear information
  about the next horizon's midpoint direction.

Validation results are reported for model comparison. The test results are
the untouched final measurement, and nothing is tuned on them.

## Metrics

Each model is scored per instrument, and pooled, on validation and on test.
The definitions, all over the fixed class order:

| Metric | Definition |
|---|---|
| `rows` | evaluated rows |
| `true_distribution`, `predicted_distribution` | rows per class |
| `accuracy` | correct / rows |
| `precision[k]` | TP / (TP + FP); undefined if *k* is never predicted |
| `recall[k]` | TP / (TP + FN); undefined if *k* never occurs |
| `f1[k]` | 2TP / (2TP + FP + FN); undefined if *k* neither occurs nor is predicted |
| `balanced_accuracy` | mean recall over the classes that occur |
| `log_loss` | −mean ln *p*(true class); undefined if any such *p* is 0 |
| `brier_score` | mean over rows of Σ<sub>k</sub> (*p<sub>k</sub>* − 1[*k* = true])², from 0 to 2 |
| `confusion_matrix[t][p]` | rows of true class *t* predicted as *p*: true rows, predicted columns |
| `return_distribution` | per true class: rows, exact minimum and maximum `return_raw`, and their exact mean rounded once |

**Undefined values.** An undefined metric is `{"undefined": "<reason>"}`,
never a zero.

**Pooling.** The aggregate pools the evaluated rows of every instrument the
model is evaluable for, so each row weighs equally and busy instruments
dominate. The instruments pooled are listed with it.

**Cross-checks.** The definitions are checked against hand calculations and
against scikit-learn's own metrics.

## The evaluation artifact

```text
<experiment-root>/<experiment-id>/
  manifest.json        sources, configuration, environment, counts, file checksums
  model.json           per instrument: majority class counts and prior; scaler means
                       and scales; coefficients and intercepts per class; or why unevaluable
  metrics.json         the metrics above, training class counts, and every exclusion
  predictions.ndjson   one row per evaluated row: identity, split, true class,
                       exact inputs, return, and each model's class and probabilities
```

**Identity.** The experiment ID is the SHA-256 of the canonical manifest
without the ID. The manifest names:
- every source artifact's ID and checksum;
- the model feature set, the class order, and the configuration;
- the software environment: Python, NumPy, SciPy, scikit-learn,
  threadpoolctl, BLAS, platform, and training threads;
- each file's checksum.

So the same sources, configuration, and environment give the same ID and
bytes, and a change to any of them gives another ID.

**Format.** Documents follow Chronos's rules: sorted keys, no whitespace, and
no JSON numbers. A float is written as its shortest round-trip decimal, which
reads back as the same `binary64` bit for bit. NaN and infinity are refused
on writing and on reading.

**Parameters.** Model parameters are plain numbers, never a pickle. Every
persisted prediction is made from those stored numbers, not from the fitted
object. Training also proves, on every row it predicts, that they reproduce
scikit-learn's classes exactly and its probabilities within **1e-12**
absolute, and fails otherwise.

**Publication.** The guarantees are the C++ artifacts':
- files are written into a `.chronos-staging-*` directory, `fsync`ed, and
  made read-only;
- the directory is renamed into place only if nothing has its name
  (`renamex_np(RENAME_EXCL)` on macOS, `renameat2(RENAME_NOREPLACE)` on
  Linux, refused elsewhere), then made read-only;
- an existing ID is `already_present` only when it verifies and its manifest
  is byte-identical, and an error otherwise; it is never touched.

**`inspect`** retrains nothing. It checks:
- the file set, the canonical form of every document with exactly its
  expected keys, the ID, and every checksum;
- the source IDs, checksums, stream, horizon, split boundaries, configuration,
  and recorded environment; the estimator version must agree with the
  recorded environment, which may differ from the inspection environment;
- the model structure and the finiteness of its parameters, and majority
  counts that are non-negative with a positive total; fitted logistic models
  must meet the declared training class support and iteration limits;
- every prediction row;
- that the stored majority models reproduce their predictions exactly, and
  the stored logistic models theirs within 1e-12;
- that the metrics recomputed from the persisted predictions are the
  published ones;
- that the training and exclusion counts agree across the manifest, the
  models, and the metrics.

Inference refuses non-finite intermediate results as well as non-finite
parameters. Finite parameters can still overflow while standardizing inputs
or computing logits, and NaN cannot safely be checked with a numerical
tolerance comparison.

Anything malformed is reported as an inspection error, never a crash.

**Inference threads.** Predictions are computed single-threaded, like
training, so they do not depend on BLAS threading.

## Reproducibility, precisely

- **Same environment:** repeating an evaluation gives byte-identical files
  and the same ID. Tests check this.
- **Across environments:** the result may differ in the last bits of floating
  point, because of BLAS, compiler, or CPU. The ID records the environment
  and differs with it. Bit-identical training across platforms is not
  claimed.
- **What reproduction needs:** the same source artifacts, the pinned lock
  file, and the environment fields in the manifest.

## What the demonstration does not show

The ignored ten-minute AAPL and SPY recording ran through the whole pipeline
as a **smoke test**:

| Measure | Split | Majority | Logistic |
|---|---|---|---|
| Accuracy, pooled | Validation | 0.479 | 0.518 |
| Accuracy, pooled | Test | 0.484 | 0.518 |
| Balanced accuracy | Test | 0.336 | 0.365 |
| Log loss | Test | 0.889 | 0.878 |
| Brier score | Test | 0.558 | 0.550 |

Test accuracy by instrument: AAPL 0.454 (majority) against 0.565
(logistic), and SPY 0.494 against 0.502. The logistic model never predicts
flat.

**This establishes nothing about alpha or profitability:**
- **Sample.** Ten minutes is a single sample from one session, and the test
  period is about two minutes. Its rows overlap heavily in time, so the
  effective sample size is far smaller than the row count. The numbers would
  move a great deal from one day to the next.
- **Microstructure.** Top-of-book imbalance predicting the next midpoint move
  is a familiar effect. It is mostly bid-ask dynamics that the spread and
  fees would consume.
- **Accuracy is not profit.** A direction forecast says nothing about the
  size of the move, whether it covers the spread, fees, and slippage, or
  whether an order could be filled at the assumed price before the move.
  [strategy-evaluation.md](strategy-evaluation.md) now simulates exactly
  that for one simple policy, and on this data it loses money in every
  window.

A credible evaluation needs many days and regimes, walk-forward periods,
and richer costs and policies. That is later work in [plan.md](../plan.md).
[research-walkthrough.md](research-walkthrough.md) runs this experiment and
its strategy evaluations end to end with one command.

## Limitations

- **Models.** Two baselines, one fixed configuration each, no hyperparameter
  search, and no calibration curve or confidence-sliced results yet.
- **Instruments.** One model per instrument, with no pooled cross-instrument
  model.
- **Memory.** The rows of all three artifacts are held in memory. That is
  about 119,000 rows for the demonstration, but it does not scale to months
  of data without a streaming or columnar reader.
- **Platforms.** Publication refuses platforms other than macOS and Linux,
  since there is no safe no-replace rename there.
