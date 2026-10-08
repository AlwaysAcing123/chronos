# ADR 0021: Model registry, constrained ONNX imports, comparisons, and registered-model deployments

- Status: Proposed
- Date: 2026-10-07

## Context

Until this decision a model existed only inside one experiment
(ADR 0015): the logistic or majority parameters stored in its `model.json`.
Every later step named the model by `(experiment_id, instrument, model)`:
- inference schedules (ADR 0016);
- the research service's evaluation jobs (ADR 0017);
- paper deployments (ADR 0018).

That left four gaps:

- **No durable model identity.** A model could not be named, listed,
  archived, or compared apart from the experiment that trained it.
- **No way to bring a model in.** Only models Chronos itself fitted could
  be evaluated or deployed.
- **No fair comparison.** Two models could be evaluated one at a time, with
  nothing pinning them to the same data, window, and configuration.
- **No deployment of anything but an experiment's model.** Migration 005
  requires an `experiment_id` and a `logistic` or `majority` model of every
  deployment.

Constraints that shape the design:
- **An imported model is untrusted bytes.** The ONNX project's own security
  policy leaves provenance, isolation, resource limits, and runtime
  safeguards to the operator. External-data tensors have had path-traversal
  vulnerabilities.
- **Provenance a file cannot prove.** A checksum says what bytes were
  imported. It says nothing about when, or on what data, the model was
  trained, preprocessed, tuned, or selected.
- **The trusted paths stay.** Every financial figure is the C++ engine's
  exact arithmetic. Every deployment order goes through the policy, risk,
  decision journal, writer fence, and trusted order path of ADRs 0018–0020.
- **Versions, not rewrites.** Version 1 schedules, runs, deployments, and
  canonical requests keep their exact bytes and meaning.

## Decision

### Model versions: `chronos.model_version/1`

`python/chronos_research/registry.py` publishes:

```text
<artifacts>/models/<model_version_id>/manifest.json
<artifacts>/models/<model_version_id>/<payload>          parameters.json or model.onnx
<artifacts>/model-names/<binding_id>/manifest.json       chronos.model_name/1
<artifacts>/model-lifecycle/<model_version_id>.json      chronos.model_lifecycle/1
```

- **Identity.** The version ID is the SHA-256 of the canonical manifest
  without its ID, with its newline, as for experiments. Publication is the
  atomic, never-replacing rename of every artifact; an existing entry is
  `already_present` only if it verifies and holds exactly these bytes.
- **Three identities kept apart:** `model_version_id` (the whole immutable
  record), `payload.sha256` (the executable bytes), and
  `provenance.experiment_id`, present only for a model taken from a
  verified experiment.
- **The record.** The manifest names the family (`logistic`, `majority`, or
  `onnx`), the format and runtime contract, the one instrument, the exact
  inputs (`top_of_book_baseline/1` with units and the integer-to-binary64
  conversion), the outputs (class order down, flat, up, and the
  `confidence_ppb` rule), the target (`future_midpoint_return_v1` and its
  horizon), the information cutoff and its basis, the provenance, and what
  Chronos verified. Nothing is defaulted or invented: a fact Chronos does
  not have is absent or unavailable with a reason.
- **Bounds.** A manifest is at most 256 KiB; a baseline payload at most
  64 KiB; an ONNX payload at most 1 MiB.
- **Verification on consumption.** Every reader goes through
  `verified_model_version`, which reads each file once through pinned
  `O_NOFOLLOW` descriptors and verifies the file set, the canonical
  manifest and its ID, the payload's size and checksum, the per-origin
  structure, and, for a baseline, the parsed parameters. The host never
  parses an ONNX payload.
- **Names.** A name (`[a-z0-9][a-z0-9._-]{0,63}`) and version (one to three
  dot-separated whole numbers) bind to one version for good. The binding ID
  is the SHA-256 of `{name, schema, version}`. Binding it to another version
  is `NameVersionTakenError`, and the name is checked before anything is
  published, so a refused registration leaves no unnamed version.
- **Lifecycle.** `active` (the default, with no record) or `archived`, with
  a reason and a nanosecond time, replaced atomically. An unreadable record
  is an error, never silently active. Archiving changes only what new
  comparisons and deployments may choose; nothing already using a version
  changes.

### Provenance: verified and declared

- **Experiment models.** `register-model` verifies the experiment and its
  sources again with `chronos_dataset`, with reproduction. The cutoff is
  the split's validation start, with basis `verified`. The training rows
  and class counts are recorded. The payload is the verified model's
  canonical parameters, so its SHA-256 is the `parameters_sha256` that
  version 1 schedules and deployments of that model record. A version does
  not need its experiment afterwards; the detail route traces it
  (`matches`, `missing`, `invalid`, or `differs`) without re-running
  predictions.
- **Imports.** Everything the uploader states is under
  `provenance.declared` and is never called verified:
  - a declared cutoff has basis `declared`;
  - with no declared cutoff, the cutoff is unavailable (`not_declared`),
    never zero;
  - `provenance.not_verified` states what a checksum cannot show: the
    cutoff, the training, and the target;
  - the verification statements say only what the sandbox checked.

### Imports: `chronos.model_import/1`

The uploader's manifest is strict JSON with no numbers:
- **Identity.** `name`, `version`, and one `instrument`.
- **Contract.** `inputs` exactly `top_of_book_baseline` version 1;
  `input_tensor` `inputs`, float64, `[1, 5]`; `output_tensor`
  `probabilities`, float64, `[1, 3]`; `class_order` down, flat, up;
  `target` `future_midpoint_return_v1` with a positive `horizon_ns`.
- **Declarations.** `training` (1 to 2000 characters), `source` (up to 500),
  `information_cutoff_ns` (a positive int64, or null), and `cutoff_covers`.
  A declared cutoff must state that it covers exactly "training,
  preprocessing, tuning, and model selection"; `cutoff_covers` is null
  without one.

Upload is one bounded protocol on its own routes. The research service's
16 KiB JSON limit is unchanged for every other route.
1. `POST /imports` checks the manifest and stages it.
2. `PUT /imports/{id}/model` takes `application/octet-stream`, 1 byte to
   1 MiB, refused on its announced `Content-Length` before any byte is
   read, with a 30-second socket timeout. The bytes are frozen: the same
   bytes again are accepted as a repeat, other bytes are
   `409 model_already_uploaded`.
3. `POST /imports/{id}/validation {"model_sha256"}` must name the reviewed
   digest. It queues an `import` job only when the sandbox is available.

The job's child (`import-model --staged`) reads the staged bytes once and
checks their digest against the record and the reviewed one. It validates
them in the sandbox, then publishes those same in-memory bytes and binds
the name. The service keeps at most 64 staged imports, pruning the oldest
that no queued or running job needs.

### The profile: `chronos.onnx_classifier/1`

`onnx_profile.py` decides, mechanically and only inside the sandbox,
whether bytes are a small, static, stateless three-class classifier. It
parses with `ModelProto.ParseFromString`, which never follows external
data, then checks:

- **Size and versions.** At most 1 MiB; IR version 7 to 10; exactly one
  operator set, the default domain, opset 13 to 21.
- **What it may contain.** Operators `Add`, `Div`, `Gemm`, `Identity`,
  `MatMul`, `Mul`, `Softmax`, and `Sub` only. Attributes only `Softmax.axis`
  (`-1` or `1`) and `Gemm`'s `alpha`, `beta`, `transA`, `transB`.
- **What it may not contain.** Functions, training info, metadata
  properties, sparse initializers, quantization annotations, custom
  domains, graph, tensor, or type attributes, or attribute references.
  Hence no control flow, no subgraph, and no randomness.
- **Initializers.** No external data. Double only, finite, not also a graph
  input (so they cannot be overridden), at most 32 of them and 16,384
  elements together.
- **Size of the graph.** At most 64 nodes and 4,096 elements per tensor; no
  value redefined or read before it is defined; names 1 to 128 characters.
- **Strict checks.** `checker.check_model(full_check=True)` and strict
  shape inference. Every node's output must be static, double, and small.

### The sandbox

`onnx_sandbox.py` runs `python -I -m chronos_research.onnx_sandbox_worker`
(protocol `chronos.onnx_sandbox/1`) in an image built from exactly:
- `tools/onnx-sandbox/Dockerfile`, `FROM python:3.12-slim-trixie` pinned by
  digest;
- `python/requirements-onnx.lock` (onnx 1.23.2, onnxruntime 1.30.0, and
  their dependencies by hash, installed with `--require-hashes
  --only-binary :all:`);
- the worker's three modules.

The image's tag and label carry that content's SHA-256.

- **Container restrictions.** Every container runs with `--network none
  --read-only --cap-drop ALL --security-opt no-new-privileges --user
  65534:65534 --pids-limit 32 --memory 256m --memory-swap 256m --cpus 1
  --ulimit nofile=64:64 --ipc none --log-driver none --pull never`.
- **Nothing of the host.** No mount, no environment beyond the image's,
  no credential, no catalog, database, or Docker socket. The model arrives
  on stdin. The Docker CLI itself gets only `PATH` and, if configured,
  `DOCKER_HOST`, so no registry credential is read.
- **Host-side bounds.** 60 s to load, 2 s per prediction, 15 s per Docker
  CLI command. A reply line is at most 64 KiB, and only a 4 KiB tail of
  stderr is kept.
- **No container outlives its owner.** A process that holds a session can
  be killed at a deadline before its own cleanup runs, and the container
  lives in the Docker daemon, outside any process group. So removal does
  not depend on the owner:
  - Every container is named, and labelled with its owner process and with
    its *supervisor token* (32 random hex digits) when a supervisor started
    the process. Supervisors are the research service, per import or
    comparison job, and the paper server, per worker of a registered model.
  - The supervisor removes every container under the token and checks that
    Docker lists none, whenever the child ends: normal exit, failure,
    timeout, output overflow, death by a signal, or shutdown. After a kill
    it looks once more after a settle, in case a create was under way.
  - The research service holds the job's slot until removal is verified.
    It records the token in the job record before the child starts, and
    reclaims unreleased tokens at startup and before every later sandbox
    job. If Docker answers but an earlier job's container stays, a new
    sandbox job fails with `sandbox_cleanup_pending`. Unreleased records
    are not pruned.
  - Each session also starts a watchdog before its container. The
    watchdog is a standard-library-only process in its own session. When
    the owner dies, however it dies, the kernel closes the watchdog's pipe
    and it removes the container. This covers a supervisor that dies too,
    and direct CLI use.
  - The Docker CLI stays in its owner's process group, so killing the
    group cannot leave it to start the container afterwards.
  - Cleanup filters on the exact token or name, never on age or PID, so it
    cannot touch another session's container. `onnx-sandbox status` still
    reaps containers whose owner process on this host is gone.
- **The session.** ONNX Runtime on the CPU provider alone, one intra- and
  inter-op thread, sequential, no graph optimization, no memory arena or
  pattern, released opsets only, strict shape and type inference, no
  spinning, and an external-initializer folder that does not exist.
- **Fail closed.** The capability is `available` only when Docker answers
  and an image of exactly this source exists. Otherwise it is
  `unavailable` with a reason: `docker_not_found`, `docker_unreachable`,
  `image_not_built`, `image_mismatch`, or `sandbox_source_missing`.
  Nothing is ever run another way.

### Validation of an import

The sandbox checks the profile and loads a session. Then a fixed corpus,
`chronos.onnx_smoke/1`, is run one row at a time: 24 rows of exact
`top_of_book_baseline/1` inputs covering tight and wide spreads, both
imbalance extremes, and small and large books. The first row is then run
again and must repeat exactly. Every output is judged on the host. The
version records:
- the ONNX Runtime version and the profile report;
- the sandbox content digest;
- the corpus digest and the outputs' digest.

### One adapter for every prediction: `registered_inference.py`

Historical schedules and deployment workers both predict through
`open_predictor`, one row per call, so batch size and order cannot change a
result. A baseline uses the row-pure arithmetic of version 1 schedules. An
import runs only in a sandbox session; without one, `open_predictor` raises
`SandboxUnavailableError`.

Outputs are judged, never repaired:
- exactly three binary64 values, each finite and from 0 to 1;
- their `math.fsum` within 1e-12 of one, the live checker's tolerance.

The class is the family's rule; for an import, the first largest
probability, so ties go to the earliest class. `confidence_ppb` is
`floor(1e9 × p)`, exactly. A failure has a reason: `wrong_output`,
`non_finite_output`, `probability_out_of_range`,
`probabilities_do_not_sum_to_one`, or the sandbox's.

### Export and parity

`export-onnx` writes a registered logistic version as a profile model (IR
8, opset 17: `Sub`, `Div`, `MatMul`, `Add`, `Softmax` over its verified mean,
scale, coefficients, and intercepts). It also writes an import manifest
whose declared cutoff is the version's verified one, and a parity report
(`chronos.onnx_export_parity/1`).

The report runs every available feature row of the instrument in the
version's verified sources through the host's row-pure runtime and through
the sandbox. It reports three things separately:
- the largest probability difference against a 1e-12 tolerance;
- class agreement;
- agreement of the `long_flat_threshold` signal at the default threshold.

It measures floating-point differences rather than assuming them away.

### Schedules and runs of a registered version

- **`chronos.inference_schedule/2`.** A version 1 schedule in everything
  but its model:
  - the model block names `model_version_id`, `family`, `payload_sha256`,
    and `runtime {class, name, version}`, and never an experiment;
  - `training` is `{basis, information_cutoff_ns}`;
  - the inference definition's class rule is "the model version's own
    class rule, stated in model.runtime.class".

  A version without a cutoff is refused (`cutoff_unavailable`), and a
  cutoff after the window's start is `cutoff_after_window_start`.
  `registered_schedule.py` builds it; rebuilding gives identical bytes.
  For an import, the environment records the ONNX Runtime version and the
  sandbox content digest.
- **`chronos.strategy_run/2`.** The C++ runner reads both schedule
  versions. A version 2 schedule gives a version 2 run, whose `source`
  names `model_version_id`, `family`, `payload_sha256`, and
  `information_cutoff {at_ns, basis}`, and no experiment. A cutoff after
  the window's start is `LabelLeakage`. Version 1 schedules and runs keep
  their exact bytes. A baseline version's version 2 rows equal its version
  1 rows byte for byte, and its results equal the version 1 run's.

### Comparisons

- **The spec: `chronos.model_comparison_spec/1`.** It freezes:
  - 2 to 6 distinct `model_version_ids`, in order;
  - one `split_artifact_id`, `instrument`, and `window`;
  - one strategy `configuration` for every member.
- **Running it.** A `comparison` job verifies the split's sources with
  reproduction. Each member is then checked for compatibility and refused
  with a reason if it fails: `artifact_invalid`, `archived`,
  `instrument_mismatch`, `horizon_mismatch`, `cutoff_unavailable`,
  `cutoff_after_window_start`, or `sandbox_unavailable:<reason>`.
- **Each member that passes:**
  1. it is scheduled as above;
  2. its schedule is rebuilt and must be byte-identical;
  3. it runs in its own `chronos_strategy` run, with its own strategy,
     cash, and buy-and-hold accounts;
  4. its run is replayed with `chronos_strategy inspect`.

  Failures keep the member, with a reason: a sandbox failure
  (`sandbox_<reason>`), an invalid prediction's reason,
  `schedule_failed`, `schedule_not_reproduced`, `schedule_not_published`,
  `run_failed`, or `replay_failed`. Nothing is retrained.
- **Prediction quality, after inference (`scoring.py`).** Only once every
  member's schedule exists does the comparison read the split's labels.
  - Each completed member's stored predictions are scored. Rows are
    joined by stream, instrument, and ingest sequence with the
    experiment's `model_data.join`.
  - A row is scored when the split assigns it to the window with a usable
    label. Every other row of the window is counted under the split's
    reason, such as `label_unavailable:no_future_observation`.
  - The metrics are the experiment's `classification_metrics`: accuracy,
    balanced accuracy, log loss, Brier score, per-class precision, recall,
    and F1, confusion, and distributions. They are binary64, and an
    undefined metric keeps its reason. For an experiment-backed version
    the count-based metrics equal the experiment's own test scores. Log
    loss and Brier score can differ in the last bits, because the
    experiment computed its probabilities in one batch and the schedule
    row by row.
  - A member that cannot be scored keeps its verified trading results, with
    `predictive: {unavailable: labels_unavailable | scoring_failed}`.
  - The model never sees a label. The schedule builder takes no labels,
    and the model is not run again for scoring.
- **The artifact: `chronos.model_comparison/1`.** Content-addressed under
  `<artifacts>/comparisons/<comparison_id>/manifest.json`. It holds:
  - the spec and its digest, the sources, and the window;
  - every member, in order, with its state, reason, schedule and run IDs,
    reproduction and replay facts, class counts, and accounts;
  - the benchmarks, checked identical across completed members rather
    than assumed;
  - exact deltas: each member against cash, against buy and hold, and
    against the first completed member, the reference;
  - each completed member's `predictive` section and the `scoring`
    statements, apart from the trading figures;
  - fairness statements and a `conditional` flag on every member whose
    cutoff is declared.

  Publication outcomes are not part of the content, so the same spec gives
  the same comparison ID on a rerun. It is published whatever its members'
  outcomes. The job fails only when the comparison cannot be built at all.
- **Exact deltas.** They are `Decimal` differences at 100 digits with
  `Inexact` trapped. A delta whose input valuation is unavailable is
  unavailable with that reason.

### Deployments of a registered version

- **Migration 008.** It adds:
  - `model_reference` (`experiment` or `registered`, default
    `experiment`);
  - `model_version_id`;
  - `training_cutoff_basis` (`verified` or `declared`, default `verified`).

  It drops `experiment_id`'s NOT NULL and widens `model_name` to
  `logistic`, `majority`, or `onnx`. One constraint,
  `model_deployments_one_model_reference`, then requires either:
  - an `experiment` row with an experiment ID, no version ID, a `logistic`
    or `majority` model, and a `verified` cutoff, which is exactly
    migration 005's rule for every deployment; or
  - a `registered` row with a version ID and no experiment ID.

  Every existing row is an experiment row and satisfies it unchanged. Files
  001–007, the decision table, the one-account-per-deployment constraint,
  and the order trigger are not touched. The defaults keep a server from
  before 008 correct: it can create only experiment deployments.
- **The request.** A create names its model exactly one way: `experiment_id`
  and `model`, or `model_version_id` alone. An experiment request's
  canonical form is still `chronos.model_deployment_request/1`, byte for
  byte. A registered one's is `chronos.model_deployment_request/2` and names
  the version, never an experiment. A name and version is never stored, so
  nothing retargets a deployment.
- **The worker: `chronos.inference_worker/2`.** It runs as
  `inference-worker --model-version <id> --instrument <symbol>`. The server
  passes `--docker <path>` from `--onnx-docker <absolute path>`; without it
  an import cannot be described or started. The ready line names:
  - `model_version_id`, `family`, and `instrument`;
  - `payload_sha256` and `model_feature_set`;
  - `training_cutoff_ns` and `cutoff_basis`.

  The worker verifies the version and requires a known cutoff and the
  requested instrument. For an import, it loads the sandbox session at
  describe time too, so a deployment is never created for a model that
  cannot run. Requests and predictions are protocol 1's.
- **Failure.** An invalid output is an error line, which the server takes
  as `inference_request_refused`. A failed sandbox ends the worker. Either
  faults the deployment with no order.
- **Start.** Start re-opens the worker, which verifies the version again,
  and requires the identical identity: choice, instrument, family, payload
  checksum, feature set, cutoff, and basis. Otherwise it faults with
  `model_identity_mismatch`.
- **Archive semantics.** Describing an archived version is refused, so it
  cannot be chosen for a new deployment. A deployment that already serves
  it still starts.

## Consequences

- **Durable model identity.** A model has an identity of its own that
  survives its experiment, can be named, archived, compared, and deployed,
  and is verified on every read.
- **Narrow imports.** Outside models can be imported only as a narrow,
  mechanically checked classifier, run only in a restricted container.
  Their training claims stay visibly declared everywhere: library, detail,
  comparisons, and deployments.
- **Fair comparisons.** Comparisons pin data, window, configuration, and
  model versions, keep every failed or refused member, and reuse the C++
  engine and its exact accounting.
- **Unchanged deployment path.** A registered version deploys through the
  same policy, risk, journal, fence, and order path as an experiment model.
  Experiment deployments, version 1 schedules and runs, and their bytes are
  unchanged.
- **Costs.** Docker is required to import, evaluate, or deploy an imported
  model. Imports are one row per prediction through a pipe, about half a
  millisecond each locally. Both the research service and the
  paper-trading server need configuring for the sandbox.

## Alternatives rejected

- **A fabricated or null-in-meaning experiment ID** for imports in version 1
  schedules, runs, and deployments. It would claim provenance that does not
  exist.
- **Parsing or running ONNX on the host,** or in-process with a restricted
  runtime. A parser bug or a hostile graph would then run with the
  service's privileges and data.
- **General ONNX, or an operator allowlist without a static-shape and size
  profile.** Control flow, randomness, external data, and dynamic shapes
  could not then be ruled out by inspection.
- **Pickle, joblib, scripts, uploaded dependencies, custom operator
  libraries, or remote model URLs.** Each is code or remote state, not a
  bounded model.
- **Raising the service's JSON limit, or base64 in JSON,** for uploads. The
  model has its own bounded octet-stream route instead.
- **Retraining imports for comparison.** A comparison scores models as
  registered.
- **One combined multi-model run.** Independent runs keep accounts
  independent and the engine unchanged.
- **A second deployments table.** It would duplicate the ownership trigger
  and the decision journal.
- **Letting archived versions deploy,** or archiving a running deployment's
  version out from under it.
- **Container cleanup that relies on the owner,** by `finally`, a SIGTERM
  handler, or killing the Docker CLI. A deadline's SIGKILL skips all of
  them, and the container lives in the daemon.
- **Reaping containers by age, or only by owner PID.** Age would remove a
  live session's container, and a reused PID hides a dead owner. Cleanup
  uses an exact supervisor token or container name instead.
- **Storing the paper server's sandbox tokens in the database.** That would
  need a schema change for a case the per-session watchdog already covers:
  a server that dies between killing a worker and removing its container.
- **Scoring prediction quality during inference, or in the browser.**
  Labels would then be in reach of the model's path, and figures would be
  computed outside the service. Scoring reads labels only after every
  member is predicted.

## Limitations

- **Docker is required for imports.** Without Docker, or without the image
  of exactly this source, imports cannot be validated, evaluated, or
  deployed. There is no weaker fallback.
- **Declared cutoffs are unverifiable.** An import's result is out of sample
  only if its uploader's declaration is true, and the UI says so.
- **One profile only.** One input contract, one target, three classes, one
  instrument, float64, and the eight operators above.
- **Platform.** The image is built for the host's architecture: tested on
  macOS arm64 under Docker Desktop and specified for Linux amd64. On macOS
  the Docker CLI, run without `HOME`, reaches the daemon through
  `/var/run/docker.sock`, which Docker Desktop provides by default. Another
  setup needs `DOCKER_HOST`.
- **Sandbox bounds.** They come from Docker's cgroup limits and the host's
  deadlines, not a separate kernel or VM boundary.
- **Container cleanup.** One case remains: an owner SIGKILLed while its
  Docker CLI is mid-create, with the daemon completing the create after
  every re-check. That can leave a created container that never started,
  holding no CPU or memory, until `onnx-sandbox status` or the research
  service's recovery removes it. If Docker stops answering, confirming
  removal can hold a paper server's Stop for up to about 45 s (15 s per
  Docker command).
- **No authentication.** The research service is loopback-only and has no
  authentication, as before. Uploaded files are as trusted as the local
  user who sends them.
- **One version at a time.** A comparison is bounded to six members and one
  instrument and window. A deployment serves one version; switching
  versions means a new deployment.
