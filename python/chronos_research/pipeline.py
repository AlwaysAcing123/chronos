"""The research pipeline as fixed steps over immutable artifacts.

Each step runs one fixed Chronos executable, chosen by name inside a
configured tool directory, with an explicit argument vector (never a shell
string), and takes the artifact ID it produced from that tool's JSON output.
Artifacts are content-addressed and never overwritten, so running a step
again verifies and reuses the existing artifact (`already_present`) instead
of writing a new one.

    recording or synthetic events -> dataset -> features -> labels -> split
        -> baseline experiment -> inference schedule -> strategy run

The roots below `ArtifactRoots.under(base)` are the ignored `data/`
directories; nothing here downloads data or needs a credential.
"""

import json
import subprocess
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research import schedule
from chronos_research.artifacts import ArtifactError, VerifiedSources, is_artifact_id, verify_sources
from chronos_research.experiment import MANIFEST, ExperimentConfiguration, build_experiment, load_model_data
from chronos_research.inspection import verify_experiment
from chronos_research.publication import publish

# Every executable a step may run, by its fixed name.
CHRONOS_DATASET = "chronos_dataset"
DATABENTO_NORMALIZE = "chronos_databento_normalize"
SYNTHETIC_DATASET = "chronos_synthetic_dataset"
CHRONOS_STRATEGY = "chronos_strategy"
_TOOLS = (CHRONOS_DATASET, DATABENTO_NORMALIZE, SYNTHETIC_DATASET, CHRONOS_STRATEGY)
_TOOL_TIMEOUT_SECONDS = 3600


class PipelineError(Exception):
    """A step's tool failed or refused its input."""


@dataclass(frozen=True)
class Tools:
    """The directory that holds the built Chronos executables."""

    directory: Path

    def path(self, name: str) -> Path:
        if name not in _TOOLS:
            raise ValueError(f"{name!r} is not a Chronos pipeline tool")
        executable = self.directory / name
        if not executable.is_file():
            raise PipelineError(f"{executable} is not built; build the C++ tools first")
        return executable


@dataclass(frozen=True)
class ArtifactRoots:
    catalog: Path
    features: Path
    labels: Path
    splits: Path
    experiments: Path
    schedules: Path
    strategies: Path

    @classmethod
    def under(cls, base: Path) -> "ArtifactRoots":
        return cls(
            catalog=base / "normalized",
            features=base / "features",
            labels=base / "labels",
            splits=base / "splits",
            experiments=base / "experiments",
            schedules=base / "schedules",
            strategies=base / "strategies",
        )


@dataclass(frozen=True)
class Step:
    """One produced artifact: what it is, its ID, and whether this run
    published it or found it already present."""

    kind: str
    artifact_id: str
    outcome: str
    directory: Path
    # Published, but a step after the atomic rename failed.
    warning: Any = None

    def as_json(self) -> dict[str, Any]:
        return {
            "artifact_id": self.artifact_id,
            "directory": str(self.directory),
            "kind": self.kind,
            "outcome": self.outcome,
            "warning": self.warning,
        }


def run_tool(executable: Path, arguments: Sequence[str]) -> dict[str, Any]:
    """Runs one tool with an explicit argument vector and returns its JSON."""
    try:
        completed = subprocess.run(
            [str(executable), *arguments],
            capture_output=True,
            text=True,
            check=False,
            timeout=_TOOL_TIMEOUT_SECONDS,
        )
    except OSError as error:
        raise PipelineError(f"cannot run {executable.name}: {error.strerror}") from error
    except subprocess.TimeoutExpired as error:
        raise PipelineError(f"{executable.name} did not finish within {_TOOL_TIMEOUT_SECONDS} s") from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise PipelineError(f"{executable.name} {' '.join(arguments[:2])} failed: {detail}")
    try:
        document = json.loads(completed.stdout)
    except ValueError as error:
        raise PipelineError(f"{executable.name} printed something other than JSON") from error
    if not isinstance(document, dict):
        raise PipelineError(f"{executable.name} printed an unexpected document")
    return document


def _artifact_step(kind: str, root: Path, document: dict[str, Any], key: str = "artifact") -> Step:
    artifact = document[key]
    identifier = artifact["dataset_id"] if key == "dataset" else artifact["artifact_id"]
    return Step(
        kind=kind,
        artifact_id=identifier,
        outcome=document["outcome"],
        directory=root / identifier,
        warning=document.get("warning"),
    )


def normalize_recording(tools: Tools, recording: Path, stream_id: str, catalog: Path) -> Step:
    document = run_tool(
        tools.path(DATABENTO_NORMALIZE),
        ["--input", str(recording), "--stream-id", stream_id, "--catalog", str(catalog)],
    )
    return _artifact_step("dataset", catalog, document, key="dataset")


def synthesize_dataset(tools: Tools, events: Path, stream_id: str, catalog: Path) -> Step:
    """Writes a dataset from synthetic CSV events with the test-only tool.
    It publishes like every dataset writer, so a repeat finds it present."""
    catalog.mkdir(parents=True, exist_ok=True)
    document = run_tool(tools.path(SYNTHETIC_DATASET), [str(events), str(catalog), stream_id])
    dataset_id = document["dataset_id"]
    return Step(
        kind="dataset", artifact_id=dataset_id, outcome=document["outcome"], directory=catalog / dataset_id
    )


def extract_features(tools: Tools, roots: ArtifactRoots, dataset_id: str) -> Step:
    document = run_tool(
        tools.path(CHRONOS_DATASET),
        ["features", "extract", "--catalog", str(roots.catalog), "--dataset", dataset_id,
         "--output", str(roots.features)],
    )  # fmt: skip
    return _artifact_step("features", roots.features, document)


def extract_labels(tools: Tools, roots: ArtifactRoots, feature_id: str, horizon_ns: int) -> Step:
    document = run_tool(
        tools.path(CHRONOS_DATASET),
        ["labels", "extract", "--features", str(roots.features), "--feature-artifact", feature_id,
         "--output", str(roots.labels), "--horizon-ns", str(horizon_ns)],
    )  # fmt: skip
    return _artifact_step("labels", roots.labels, document)


def create_split(tools: Tools, roots: ArtifactRoots, label_id: str) -> Step:
    document = run_tool(
        tools.path(CHRONOS_DATASET),
        ["splits", "create", "--labels", str(roots.labels), "--label-artifact", label_id,
         "--output", str(roots.splits)],
    )  # fmt: skip
    return _artifact_step("split", roots.splits, document)


def verified_sources(
    tools: Tools, roots: ArtifactRoots, split_id: str, reproduce_features: bool
) -> VerifiedSources:
    """The split and its ancestry, verified by the C++ tool, each reproduced
    from its source; the features from the dataset too when asked."""
    return verify_sources(
        tools.path(CHRONOS_DATASET),
        features_root=roots.features,
        labels_root=roots.labels,
        splits_root=roots.splits,
        split_artifact_id=split_id,
        catalog=roots.catalog if reproduce_features else None,
    )


def _verified_manifest(directory: Path) -> bytes:
    verify_experiment(directory)
    return (directory / MANIFEST).read_bytes()


def evaluate_baselines(sources: VerifiedSources, experiments: Path) -> tuple[Step, dict[str, Any]]:
    """Trains and evaluates both baselines on verified sources and publishes
    the experiment. Returns the step and the experiment's metrics."""
    experiment = build_experiment(sources, load_model_data(sources), ExperimentConfiguration())
    experiments.mkdir(parents=True, exist_ok=True)
    outcome, warning = publish(
        experiments, experiment.experiment_id, experiment.files, MANIFEST, _verified_manifest
    )
    step = Step(
        kind="experiment",
        artifact_id=experiment.experiment_id,
        outcome=outcome.value,
        directory=experiments / experiment.experiment_id,
        warning=warning,
    )
    metrics: dict[str, Any] = json.loads(experiment.files["metrics.json"])
    return step, metrics


def require_artifact(kind: str, directory: Path) -> None:
    if not directory.is_dir():
        raise ArtifactError(f"the {kind} {directory.name} does not exist under {directory.parent}")


def experiment_split(experiments: Path, experiment_id: str) -> str:
    """The split an experiment names. Only a locator: the experiment and the
    split are both verified before anything uses them."""
    require_artifact("experiment", experiments / experiment_id)
    try:
        split_id = json.loads((experiments / experiment_id / MANIFEST).read_bytes())["sources"][
            "split_artifact_id"
        ]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ArtifactError(f"experiment {experiment_id} has no readable manifest") from error
    if not isinstance(split_id, str) or not is_artifact_id(split_id):
        raise ArtifactError(f"experiment {experiment_id} names no split artifact")
    return split_id


def _verified_schedule_manifest(directory: Path) -> bytes:
    schedule.verify_schedule(directory)
    return (directory / schedule.MANIFEST).read_bytes()


def infer_schedule(
    sources: VerifiedSources,
    roots: ArtifactRoots,
    experiment_id: str,
    instrument: str,
    model: str,
    window: str,
) -> Step:
    """Builds and publishes one model's inference schedule for one
    instrument and window from verified sources."""
    built = schedule.build_schedule(sources, roots.experiments / experiment_id, instrument, model, window)
    roots.schedules.mkdir(parents=True, exist_ok=True)
    outcome, warning = publish(
        roots.schedules, built.schedule_id, built.files, schedule.MANIFEST, _verified_schedule_manifest
    )
    return Step(
        kind="schedule",
        artifact_id=built.schedule_id,
        outcome=outcome.value,
        directory=roots.schedules / built.schedule_id,
        warning=warning,
    )


@dataclass(frozen=True)
class StrategyOptions:
    """The strategy runner's configuration; None keeps the runner's
    documented default. Values are passed as canonical decimal text."""

    threshold_ppb: int | None = None
    target_quantity: str | None = None
    latency_ns: int | None = None
    decision_interval_ns: int | None = None
    starting_cash: str | None = None
    fee_per_fill: str | None = None
    maximum_quote_age_ns: int | None = None
    maximum_order_notional: str | None = None

    def arguments(self) -> list[str]:
        options = {
            "--threshold-ppb": self.threshold_ppb,
            "--target-quantity": self.target_quantity,
            "--latency-ns": self.latency_ns,
            "--decision-interval-ns": self.decision_interval_ns,
            "--starting-cash": self.starting_cash,
            "--fee-per-fill": self.fee_per_fill,
            "--maximum-quote-age-ns": self.maximum_quote_age_ns,
            "--maximum-order-notional": self.maximum_order_notional,
        }
        return [
            part for option, value in options.items() if value is not None for part in (option, str(value))
        ]


def run_strategy(
    tools: Tools, roots: ArtifactRoots, schedule_id: str, options: StrategyOptions
) -> tuple[Step, dict[str, Any]]:
    """Runs one schedule through the C++ strategy runner and returns the
    step and the run's manifest."""
    roots.strategies.mkdir(parents=True, exist_ok=True)
    document = run_tool(
        tools.path(CHRONOS_STRATEGY),
        ["run", "--catalog", str(roots.catalog), "--schedules", str(roots.schedules),
         "--schedule", schedule_id, "--output", str(roots.strategies), *options.arguments()],
    )  # fmt: skip
    manifest = {key: value for key, value in document["artifact"].items() if key != "directory"}
    step = _artifact_step("strategy", roots.strategies, document)
    return step, manifest


def inspect_strategy(tools: Tools, roots: ArtifactRoots, run_id: str) -> dict[str, Any]:
    """Verifies a run and runs its schedule again over its dataset."""
    return run_tool(
        tools.path(CHRONOS_STRATEGY),
        ["inspect", "--output", str(roots.strategies), "--artifact", run_id, "--catalog", str(roots.catalog),
         "--schedules", str(roots.schedules)],
    )  # fmt: skip
