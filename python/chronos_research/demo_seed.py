"""The demo workspace's seed (ADR 0025): one command from the synthetic
scenario to verified research artifacts and a navigation manifest.

    python -m chronos_research seed-demo --tools <build dir> --artifacts <demo artifacts root>

It writes the scenario's event tables (demo_scenario), then makes every
artifact with the existing production builders and nothing else:

    h1, h2, h3   datasets (chronos_dataset synthetic write), features, 1 s labels
    h3           a purged split, both baselines, and their verified experiment
    AAPL         logistic and majority model versions in the library, and a
                 comparison of the two on h3's test window
    h1-h3        a collection, a walk-forward plan and study for AAPL and for
                 SPY, each checked by the independent walk-forward auditor
    studies      strategy diagnostics, checked by their independent auditor
    playback     a dataset only: no research artifact reads the later session

The configuration, scenarios, horizon, and model names below were fixed
before any of this data existed and are never tuned on outcomes; losses and
unavailable outcomes are recorded as the artifacts state them. Chronology is
asserted: the sessions are disjoint and in order, and every historical
artifact and every model's information cutoff precede the playback session.

Only after all of that verifies is `chronos.demo_seed/1` published: a
content-addressed navigation manifest of the scenario, the chronology,
artifact IDs, audit verdicts, and hash routes. It holds no path, metric, or
money. A second run finds every artifact, and the manifest, already present;
a corrupt existing artifact is refused by its builder and left untouched.
"""

import itertools
import json
import os
import stat
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from chronos_research import comparison, demo_scenario, strategy_diagnostics, walk_forward
from chronos_research.artifact_files import ArtifactUnavailableError, artifact_directory, verified_manifest
from chronos_research.artifacts import ArtifactError
from chronos_research.bounded_process import EXITED, run_bounded
from chronos_research.collection import (
    CollectionError,
    collections_root,
    create_collection,
    verified_collection,
)
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.inspection import verify_experiment
from chronos_research.onnx_sandbox import Sandbox
from chronos_research.pipeline import (
    CHRONOS_DATASET,
    ArtifactRoots,
    PipelineError,
    Tools,
    create_split,
    evaluate_baselines,
    extract_features,
    extract_labels,
    inspect_strategy,
    run_tool,
    verified_sources,
    write_synthetic_dataset,
)
from chronos_research.publication import PublicationError, publish
from chronos_research.registry import (
    RegistryError,
    RegistryRoots,
    register_experiment_model,
    verified_model_version,
)
from chronos_research.strategy_attribution import DiagnosticsError
from chronos_research.walk_forward_study import StudyError, run_study, studies_root, verified_study

SCHEMA = {"name": "chronos.demo_seed", "version": "1"}
MANIFEST = "manifest.json"
HORIZON_NS = 1_000_000_000
INSTRUMENT = "AAPL"
EXPERIMENT_SESSION = "h3"
COMPARISON_WINDOW = "test"
# The research service's defaults (service.DEFAULT_CONFIGURATION), fixed
# here so the seed never depends on a later change of default.
CONFIGURATION = {
    "decision_interval_ns": "1000000000",
    "fee_per_fill": "0.01",
    "latency_ns": "1000000",
    "maximum_order_notional": "1000000",
    "maximum_quote_age_ns": "5000000000",
    "starting_cash": "10000",
    "target_quantity": "1",
    "threshold_ppb": "500000000",
}
SCENARIOS = (
    {"fee_per_fill": "0.01", "latency_ns": "1000000", "name": "base"},
    {"fee_per_fill": "0.05", "latency_ns": "200000000", "name": "stress"},
)
MINIMUM_TRAINING_SOURCES = "1"
MODEL_NAMES = {"logistic": "demo-aapl-logistic", "majority": "demo-aapl-majority"}
MODEL_VERSION = "1"
_REPOSITORY = Path(__file__).resolve().parents[2]
WALK_FORWARD_AUDITOR = _REPOSITORY / "tools" / "walk_forward_audit" / "audit.py"
DIAGNOSTICS_AUDITOR = _REPOSITORY / "tools" / "strategy_diagnostics_audit" / "audit.py"
_AUDIT_TIMEOUT_SECONDS = 3_600
NOTICE = (
    "Every price, size, and time in this workspace was invented by scenario chronos.demo_scenario version 1. "
    "AAPL and SPY are used as names only: their prices here are examples, not either security's real price, "
    "and nothing was observed on an exchange. Results show what the machinery does, not what a market or "
    "strategy would do."
)


class SeedError(Exception):
    """The seed cannot be completed; nothing after the failure is published."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def seeds_root(artifacts: Path) -> Path:
    return artifacts / "demo_seeds"


def tables_root(artifacts: Path) -> Path:
    return artifacts / "scenarios" / f"{demo_scenario.NAME}-{demo_scenario.VERSION}"


@dataclass
class _Progress:
    """What each step produced and whether it was already present."""

    steps: list[dict[str, str]] = field(default_factory=list)

    def record(self, kind: str, identifier: str, outcome: str) -> str:
        self.steps.append({"artifact_id": identifier, "kind": kind, "outcome": outcome})
        print(json.dumps({"progress": {"kind": kind, "outcome": outcome}}), file=sys.stderr, flush=True)
        return identifier


def _write_table(directory: Path, name: str, contents: bytes) -> str:
    """Writes a scenario table once, read-only; an existing one must be
    exactly the scenario's bytes, and is otherwise refused untouched."""
    path = directory / name
    try:
        status = os.lstat(path)
    except FileNotFoundError:
        temporary = directory / f".{name}.{os.getpid()}.partial"
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            os.write(descriptor, contents)
            os.fsync(descriptor)
            os.fchmod(descriptor, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
        finally:
            os.close(descriptor)
        os.link(temporary, path)
        os.unlink(temporary)
        return "published"
    if not stat.S_ISREG(status.st_mode):
        raise SeedError("source_invalid", f"the scenario table {name} is not a regular file")
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as file:
        if file.read(len(contents) + 1) != contents:
            raise SeedError(
                "source_invalid", f"the scenario table {name} differs from the scenario; left untouched"
            )
    return "already_present"


@dataclass(frozen=True)
class _Session:
    session: demo_scenario.Session
    table_sha256: str
    dataset_id: str
    events: str
    receive_ns: tuple[int, int]
    feature_id: str | None = None
    label_id: str | None = None


def _datasets(tools: Tools, roots: ArtifactRoots, artifacts: Path, progress: _Progress) -> list[_Session]:
    directory = tables_root(artifacts)
    directory.mkdir(parents=True, exist_ok=True)
    sessions: list[_Session] = []
    for session in demo_scenario.SESSIONS:
        table = demo_scenario.event_table(session).encode()
        progress.record(
            "event_table", sha256_hex(table), _write_table(directory, f"{session.name}.csv", table)
        )
        step = write_synthetic_dataset(
            tools,
            directory / f"{session.name}.csv",
            session.stream,
            roots.catalog,
            demo_scenario.NAME,
            demo_scenario.VERSION,
        )
        progress.record("dataset", step.artifact_id, step.outcome)
        manifest = json.loads((step.directory / "manifest.json").read_bytes())
        if manifest["source"]["provider"] != "synthetic" or manifest["stream"] != session.stream:
            raise SeedError(
                "source_invalid", f"dataset {step.artifact_id} is not the scenario's {session.name}"
            )
        if manifest["source"]["recording_sha256"] != sha256_hex(table):
            raise SeedError("source_invalid", f"dataset {step.artifact_id} is not of the scenario's table")
        receive = _receive_range(manifest)
        sessions.append(
            _Session(session, sha256_hex(table), step.artifact_id, manifest["counts"]["events"], receive)
        )
    for position, entry in enumerate(sessions):
        if entry.session.role != "history":
            continue
        features = extract_features(tools, roots, entry.dataset_id)
        progress.record("features", features.artifact_id, features.outcome)
        labels = extract_labels(tools, roots, features.artifact_id, HORIZON_NS)
        progress.record("labels", labels.artifact_id, labels.outcome)
        sessions[position] = _Session(
            entry.session, entry.table_sha256, entry.dataset_id, entry.events, entry.receive_ns,
            features.artifact_id, labels.artifact_id,
        )  # fmt: skip
    return sessions


def _receive_range(manifest: Mapping[str, Any]) -> tuple[int, int]:
    receive = manifest["time_range"]["receive_timestamp_ns"]
    return int(receive["earliest"]), int(receive["latest"])


def _experiment(tools: Tools, roots: ArtifactRoots, label_id: str, progress: _Progress) -> tuple[str, Any]:
    split = create_split(tools, roots, label_id)
    progress.record("split", split.artifact_id, split.outcome)
    sources = verified_sources(tools, roots, split.artifact_id, reproduce_features=True)
    experiment, _ = evaluate_baselines(sources, roots.experiments)
    verify_experiment(experiment.directory)
    progress.record("experiment", experiment.artifact_id, experiment.outcome)
    return experiment.artifact_id, sources


def _models(
    artifacts: Path, roots: ArtifactRoots, experiment_id: str, sources: Any, progress: _Progress
) -> dict[str, str]:
    registry = RegistryRoots.under(artifacts)
    versions: dict[str, str] = {}
    for family, name in MODEL_NAMES.items():
        registration = register_experiment_model(
            registry, roots.experiments, sources, experiment_id, INSTRUMENT, family, name, MODEL_VERSION
        )
        verified_model_version(registry, registration.model_version_id)
        versions[family] = progress.record(
            "model_version", registration.model_version_id, registration.outcome
        )
    return versions


def _comparison(
    tools: Tools, artifacts: Path, split_id: str, versions: Mapping[str, str], progress: _Progress
) -> dict[str, Any]:
    spec = {
        "configuration": CONFIGURATION,
        "instrument": INSTRUMENT,
        "model_version_ids": [versions["logistic"], versions["majority"]],
        "schema": comparison.SPEC_SCHEMA,
        "split_artifact_id": split_id,
        "window": COMPARISON_WINDOW,
    }
    # Both members are built-in models, so the ONNX sandbox is never used.
    compared = comparison.compare(tools, artifacts, spec, Sandbox())
    manifest = comparison.verified_comparison(
        comparison.comparisons_root(artifacts), compared["comparison"]["comparison_id"]
    )
    roots = ArtifactRoots.under(artifacts)
    for member in manifest["members"]:
        if "run" in member:
            inspect_strategy(tools, roots, member["run"]["artifact_id"])
    progress.record("comparison", manifest["comparison_id"], compared["outcome"])
    return dict(manifest)


def _auditor(auditor: Path, artifacts: Path, identifier: str) -> dict[str, str]:
    """An independent auditor's verdict; the standard library alone (-I -S)."""
    finished = run_bounded(
        [sys.executable, "-I", "-S", str(auditor), str(artifacts), identifier],
        {"PATH": "/usr/bin:/bin"},
        _AUDIT_TIMEOUT_SECONDS,
        64 * 1024 * 1024,
        1024 * 1024,
    )
    if finished.outcome != EXITED or finished.returncode not in (0, 1):
        raise SeedError("audit_failed", f"{auditor.parent.name} did not finish ({finished.outcome})")
    report = json.loads(finished.stdout)
    if report["verdict"] != "pass":
        problems = "; ".join(str(problem) for problem in report["problems"][:5])
        raise SeedError("audit_failed", f"{auditor.parent.name} found problems in {identifier}: {problems}")
    return {"checks_total": str(report["checks_total"]), "verdict": report["verdict"]}


def _studies(
    tools: Tools, artifacts: Path, history: Sequence[_Session], progress: _Progress
) -> tuple[str, dict[str, dict[str, Any]]]:
    created = create_collection(
        tools,
        artifacts,
        [entry.label_id for entry in history if entry.label_id is not None],
        {entry.label_id: entry.session.label for entry in history if entry.label_id is not None},
    )
    collection_id = progress.record("collection", created["collection"]["collection_id"], created["outcome"])
    if created["collection"]["evidence"] != "synthetic":
        raise SeedError("source_invalid", "the collection's evidence is not synthetic")
    studies: dict[str, dict[str, Any]] = {}
    for instrument in demo_scenario.INSTRUMENTS:
        request = {
            "collection_id": collection_id,
            "configuration": CONFIGURATION,
            "instrument": instrument,
            "minimum_training_sources": MINIMUM_TRAINING_SOURCES,
            "scenarios": [dict(scenario) for scenario in SCENARIOS],
            "schema": {"name": "chronos.walk_forward_request", "version": "1"},
        }
        planned = walk_forward.create_plan(tools, artifacts, request)
        plan = planned["plan"]
        progress.record("walk_forward_plan", plan["plan_id"], planned["outcome"])
        run = run_study(tools, artifacts, plan["plan_id"])
        study = run["study"]
        progress.record("walk_forward_study", study["study_id"], run["outcome"])
        studies[instrument] = {
            "audit": _auditor(WALK_FORWARD_AUDITOR, artifacts, study["study_id"]),
            "plan": plan,
            "study": study,
        }
    return collection_id, studies


def _diagnostics(
    tools: Tools, artifacts: Path, study_ids: Sequence[str], progress: _Progress
) -> dict[str, Any]:
    generated = strategy_diagnostics.generate_diagnostics(
        tools, artifacts, strategy_diagnostics.request_for(study_ids)
    )
    diagnostic_id = generated["diagnostics"]["diagnostic_id"]
    progress.record("strategy_diagnostics", diagnostic_id, generated["outcome"])
    manifest, _ = strategy_diagnostics.verified_diagnostics(
        strategy_diagnostics.diagnostics_root(artifacts), diagnostic_id
    )
    return {
        "audit": _auditor(DIAGNOSTICS_AUDITOR, artifacts, diagnostic_id),
        "manifest": manifest,
    }


def _first_run(diagnostics: Mapping[str, Any], study_id: str) -> dict[str, str]:
    """The deep link's run, chosen by a fixed rule before any result is read:
    the study's first fold, its logistic model, its base scenario."""
    for study in diagnostics["studies"]:
        if study["study_id"] != study_id:
            continue
        for fold in study["folds"]:
            for run in fold["runs"]:
                if run["model"] == "logistic" and run["scenario"] == "base":
                    if run["state"] != "completed":
                        return {"unavailable": run.get("reason", run["state"])}
                    return {"run_id": run["run_id"], "fold": run["fold"]}
    return {"unavailable": "no_such_run"}


def _chronology(
    sessions: Sequence[_Session],
    models: Mapping[str, str],
    artifacts: Path,
    studies: Mapping[str, Mapping[str, Any]],
) -> dict[str, Any]:
    """Every assertion the guide relies on, or a SeedError."""
    ordered = sorted(sessions, key=lambda entry: entry.receive_ns[0])
    if [entry.session.name for entry in ordered] != [session.name for session in demo_scenario.SESSIONS]:
        raise SeedError("chronology", "the sessions are not in their declared order")
    for earlier, later in itertools.pairwise(ordered):
        if earlier.receive_ns[1] >= later.receive_ns[0]:
            raise SeedError("chronology", f"{earlier.session.name} does not end before {later.session.name}")
    playback = next(entry for entry in sessions if entry.session.role == "playback")
    playback_start = playback.receive_ns[0]
    registry = RegistryRoots.under(artifacts)
    cutoffs: list[dict[str, str]] = []
    for family, version_id in models.items():
        cutoff = verified_model_version(registry, version_id).manifest["training"]["information_cutoff"]
        if cutoff["basis"] != "verified" or int(cutoff["at_ns"]) >= playback_start:
            raise SeedError("chronology", f"the {family} model's cutoff does not precede playback")
        cutoffs.append({"at_ns": cutoff["at_ns"], "family": family, "model_version_id": version_id})
    for instrument, entry in studies.items():
        for fold in entry["plan"]["folds"]:
            if int(fold["evaluation"]["window"]["end_at_ns"]) > playback_start:
                raise SeedError("chronology", f"the {instrument} study evaluates after playback begins")
    history_end = max(entry.receive_ns[1] for entry in sessions if entry.session.role == "history")
    return {
        "checks": [
            "the four sessions are disjoint and in their declared order",
            "every historical session ends before the playback session begins",
            "each model version's verified information cutoff precedes the playback session",
            "every walk-forward fold evaluates a historical session, never the playback session",
            "no feature, label, split, experiment, model, comparison, collection, or study reads the "
            "playback session",
        ],
        "history_ends_ns": str(history_end),
        "model_cutoffs": cutoffs,
        "playback_starts_ns": str(playback_start),
    }


def seed_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json({key: value for key, value in manifest.items() if key != "seed_id"}))


def verified_seed(artifacts: Path, seed_id: str) -> dict[str, Any]:
    """A published seed manifest whose identity, name, and schema hold."""
    try:
        directory = artifact_directory(seeds_root(artifacts), seed_id)
        manifest = verified_manifest(directory, "seed_id", with_newline=True)
    except ArtifactUnavailableError as error:
        raise SeedError("seed_invalid", f"demo seed {seed_id}: {error}") from error
    if manifest.get("schema") != SCHEMA:
        raise SeedError("seed_invalid", f"demo seed {seed_id} is not {SCHEMA['name']}/{SCHEMA['version']}")
    return manifest


def check_seed(tools: Tools, artifacts: Path, seed_id: str) -> dict[str, Any]:
    """What a demo start verifies before any playback: the seed manifest,
    every event of the playback dataset (chronos_dataset inspect), and each
    model version, the comparison, the collection, both studies, and the
    diagnostics as stored. Nothing is rebuilt or audited again here;
    seed-demo does that, and the research service verifies on every read."""
    manifest = verified_seed(artifacts, seed_id)
    named = manifest["artifacts"]
    try:
        run_tool(
            tools.path(CHRONOS_DATASET),
            ["inspect", "--catalog", str(ArtifactRoots.under(artifacts).catalog),
             "--dataset", manifest["playback"]["dataset_id"]],
        )  # fmt: skip
        registry = RegistryRoots.under(artifacts)
        for version_id in named["model_versions"].values():
            verified_model_version(registry, version_id)
        comparison.verified_comparison(comparison.comparisons_root(artifacts), named["comparison_id"])
        verified_collection(collections_root(artifacts), named["collection_id"])
        for study_id in named["walk_forward_studies"].values():
            verified_study(studies_root(artifacts), study_id)
        strategy_diagnostics.verified_diagnostics(
            strategy_diagnostics.diagnostics_root(artifacts), named["strategy_diagnostics"]["diagnostic_id"]
        )
    except (
        ArtifactError,
        ArtifactUnavailableError,
        CollectionError,
        comparison.ComparisonError,
        DiagnosticsError,
        PipelineError,
        RegistryError,
        StudyError,
    ) as error:
        raise SeedError("seed_invalid", f"demo seed {seed_id} does not verify: {error}") from error
    return {
        "playback": manifest["playback"],
        "seed_id": seed_id,
        "verified": (
            "the seed manifest's identity; every event of the playback dataset; each model version, the "
            "comparison, the collection, both studies, and the diagnostics as stored"
        ),
    }


def _routes(manifest: Mapping[str, Any]) -> dict[str, Any]:
    artifacts = manifest["artifacts"]
    diagnostic_id = artifacts["strategy_diagnostics"]["diagnostic_id"]
    run = artifacts["strategy_diagnostics"]["guide_run"]
    return {
        "comparison": f"#/comparisons/{artifacts['comparison_id']}",
        "diagnostics": f"#/research/walk-forward/diagnostics/{diagnostic_id}",
        "diagnostics_run": (
            {"unavailable": run["unavailable"]}
            if "unavailable" in run
            else f"#/research/walk-forward/diagnostics/{diagnostic_id}/runs/{run['run_id']}/strategy"
        ),
        "models": {
            family: f"#/models/{version_id}" for family, version_id in artifacts["model_versions"].items()
        },
        "research": "#/research",
        "studies": {
            instrument: f"#/research/walk-forward/studies/{study_id}"
            for instrument, study_id in artifacts["walk_forward_studies"].items()
        },
        "trading": "#/",
        "walk_forward": "#/research/walk-forward",
    }


def seed_demo(tools: Tools, artifacts: Path) -> dict[str, Any]:
    roots = ArtifactRoots.under(artifacts)
    progress = _Progress()
    sessions = _datasets(tools, roots, artifacts, progress)
    by_name = {entry.session.name: entry for entry in sessions}
    experiment_session = by_name[EXPERIMENT_SESSION]
    assert experiment_session.label_id is not None
    experiment_id, sources = _experiment(tools, roots, experiment_session.label_id, progress)
    models = _models(artifacts, roots, experiment_id, sources, progress)
    split_id = sources.splits.artifact_id
    compared = _comparison(tools, artifacts, split_id, models, progress)
    history = [entry for entry in sessions if entry.session.role == "history"]
    collection_id, studies = _studies(tools, artifacts, history, progress)
    diagnostics = _diagnostics(
        tools, artifacts, [entry["study"]["study_id"] for entry in studies.values()], progress
    )
    chronology = _chronology(sessions, models, artifacts, studies)
    playback = by_name[demo_scenario.PLAYBACK.name]
    manifest: dict[str, Any] = {
        "artifacts": {
            "collection_id": collection_id,
            "comparison_id": compared["comparison_id"],
            "comparison_status": compared["status"],
            "experiment_id": experiment_id,
            "model_versions": models,
            "split_id": split_id,
            "strategy_diagnostics": {
                "diagnostic_id": diagnostics["manifest"]["diagnostic_id"],
                "guide_run": _first_run(diagnostics["manifest"], studies[INSTRUMENT]["study"]["study_id"]),
                "status": diagnostics["manifest"]["status"],
            },
            "walk_forward_plans": {
                instrument: entry["plan"]["plan_id"] for instrument, entry in studies.items()
            },
            "walk_forward_studies": {
                instrument: entry["study"]["study_id"] for instrument, entry in studies.items()
            },
            "walk_forward_study_status": {
                instrument: entry["study"]["status"] for instrument, entry in studies.items()
            },
        },
        "audits": {
            "strategy_diagnostics": diagnostics["audit"],
            "walk_forward": {instrument: entry["audit"] for instrument, entry in studies.items()},
        },
        "chronology": chronology,
        "fixed_in_advance": {
            "configuration": CONFIGURATION,
            "horizon_ns": str(HORIZON_NS),
            "minimum_training_sources": MINIMUM_TRAINING_SOURCES,
            "model_names": {family: f"{name} {MODEL_VERSION}" for family, name in MODEL_NAMES.items()},
            "scenarios": [dict(scenario) for scenario in SCENARIOS],
        },
        "notice": NOTICE,
        "playback": {
            "dataset_id": playback.dataset_id,
            "events": playback.events,
            "instruments": list(demo_scenario.INSTRUMENTS),
            "receive_first_ns": str(playback.receive_ns[0]),
            "receive_last_ns": str(playback.receive_ns[1]),
            "stream": playback.session.stream,
        },
        "scenario": {
            "evidence": "synthetic",
            "instruments": list(demo_scenario.INSTRUMENTS),
            "name": demo_scenario.NAME,
            "sessions": [
                {
                    "dataset_id": entry.dataset_id,
                    "day": entry.session.day,
                    "declared_label": entry.session.label,
                    "events": entry.events,
                    "feature_id": entry.feature_id,
                    "label_id": entry.label_id,
                    "name": entry.session.name,
                    "receive_first_ns": str(entry.receive_ns[0]),
                    "receive_last_ns": str(entry.receive_ns[1]),
                    "role": entry.session.role,
                    "stream": entry.session.stream,
                    "table_sha256": entry.table_sha256,
                }
                for entry in sessions
            ],
            "version": demo_scenario.VERSION,
        },
        "schema": SCHEMA,
    }
    manifest["routes"] = _routes(manifest)
    manifest["seed_id"] = seed_identity(manifest)
    root = seeds_root(artifacts)
    root.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verified_seed(artifacts, directory.name)
        return (directory / MANIFEST).read_bytes()

    try:
        outcome, warning = publish(
            root, manifest["seed_id"], {MANIFEST: canonical_json(manifest)}, MANIFEST, existing
        )
    except PublicationError as error:
        raise SeedError("publication_failed", str(error)) from error
    progress.record("demo_seed", manifest["seed_id"], outcome.value)
    return {"outcome": outcome.value, "seed": manifest, "steps": progress.steps, "warning": warning}
