# ADR 0015: The Python research boundary and baseline evaluation artifacts

- Status: Proposed
- Date: 2026-09-30

## Context

Model research belongs in Python, with NumPy and scikit-learn (plan, §9).
The market data, features, labels, and splits are C++ artifacts with a
strict canonical format and verification that took real effort to get right
(ADRs 0012–0014). A second, looser implementation of those formats in Python
would drift, and a model trained on unverified input proves nothing. The
evaluation result itself must be as reproducible and tamper-evident as its
inputs, without depending on unsafe serialized objects.

## Decision

### C++ verifies, Python consumes

The package `python/chronos_research` never judges a Chronos artifact
itself. It runs the trusted verifier, `chronos_dataset <kind> inspect`, with
reproduction from each source:
- the split from its labels;
- the labels from their features;
- the features from their dataset, when a catalog is given.

It cross-checks the IDs, checksums, dataset, stream, horizon, and row counts
the verifier reported. It then reads the rows files with the standard
library, hashing every byte, and uses rows only if the bytes match the
verified checksum. That closes the window between verification and use
without copying the canonical codec.

**Rejected:**
- *A Python reimplementation of the strict codec*: duplicated, and liable to
  drift.
- *Python bindings to the C++ library*: a native extension and build
  complexity with no benefit at this scale.
- *Trusting files after a single directory check*: misses tampered rows.

### Explicit row identity

Rows join on (stream, instrument, ingest sequence). The join refuses:
- missing, duplicate, or leftover identities;
- disagreeing times or usability;
- a usable label that crosses its boundary.

Line numbers are never used.

### Exact until the model boundary, and nothing post-prediction

Inputs stay raw integers. Differences such as microprice minus midpoint are
taken exactly. Each value is converted to `float64` once, by dividing by its
scale. The model feature set is versioned (`top_of_book_baseline` v1)
separately from the C++ feature set. No label, label time, future value, or
split assignment can be an input, and adversarial tests check that.

### One model per instrument, fitted on train only

Per-instrument models keep an instrument's identity and price level from
becoming an accidental predictor. The majority baseline's prior and the
logistic model's scaler and coefficients see training rows only.

The configuration is fixed: L2, C = 1, lbfgs, tol = 1e-6, max_iter = 1000,
single-threaded, no search. An instrument is reported unevaluable with a reason if it lacks 10 training
rows of every class, if its fit does not converge, or if the fit raises any
warning. Classes are never collapsed.

### The evaluation artifact: plain numbers, never pickles

**Layout.**

```text
<experiment-root>/<experiment-id>/{manifest.json,model.json,metrics.json,predictions.ndjson}
```

**Encoding.**
- Documents are canonical JSON with no numbers.
- Floats are the shortest round-trip decimal string: readable, exact to the
  bit, and tested.
- NaN and infinity cannot be written or read.

**Model.** `model.json` holds the scaler means and scales, coefficients and
intercepts per class, and the majority class counts. Predictions are
computed from those stored numbers. Training proves on every predicted row
that they reproduce scikit-learn's classes exactly and its probabilities
within 1e-12, and fails otherwise.

**Identity.** The experiment ID is the SHA-256 of the canonical manifest
without the ID. The manifest includes:
- the sources;
- the configuration;
- the software environment: Python, NumPy, SciPy, scikit-learn,
  threadpoolctl, BLAS, platform, and threads;
- each file's checksum.

**Rejected:**
- *pickle or joblib*: executable on load, opaque, and version-fragile;
- *ONNX*: planned for uploaded models, but heavy for two linear baselines;
- *hexadecimal floats*: exact but unreadable;
- *an ID from inputs alone*: it would call two different outputs the same
  experiment.

### Publication with the C++ guarantees

Python cannot call the C++ staging code, so it reproduces it narrowly:
- a `.chronos-staging-*` directory;
- `O_EXCL | O_NOFOLLOW` files, `fsync`, and read-only modes;
- a rename that refuses to replace, through `ctypes`:
  `renamex_np(RENAME_EXCL)` on macOS and `renameat2(RENAME_NOREPLACE)` on
  Linux, refused on other platforms;
- a read-only directory afterwards, set through an `O_NOFOLLOW` descriptor.

An existing ID is `already_present` only if it fully verifies with a
byte-identical manifest. A failure after the rename is a warning, as in C++.
`os.rename` was rejected because it silently replaces an empty directory.

### Reproducibility claims

- **Same environment:** byte-identical outputs and ID. Tests check this.
- **Across environments:** not claimed, because BLAS, CPU, and compilers may
  change the last bits. The manifest records what is needed to reproduce.
- **Dependencies:** exact versions in `pyproject.toml`, and every transitive
  package pinned by hash in `requirements.lock`, which CI installs with
  `--require-hashes`.

## Consequences

- **CI builds C++.** The Python CI job builds `chronos_dataset` and the
  test-only `chronos_synthetic_dataset`. Integration tests run the real
  verifier on synthetic data, with no network, credentials, or licensed
  data.
- **Version upgrades.** Changing a NumPy or scikit-learn version changes
  experiment IDs. That is intended.
- **Memory.** The research layer holds the joined rows in memory. A
  streaming or columnar reader is the remedy when datasets grow.
- **Scope.** The artifact is a predictive evaluation. Strategy, costs,
  execution, and profit and loss are separate, later artifacts that will
  name an experiment as their source.
