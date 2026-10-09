"""Immutable walk-forward plans over a verified collection (ADR 0022).

    <artifacts>/walk_forward_plans/<plan_id>/manifest.json

A plan fixes, before any result exists, everything a walk-forward study will
do: the collection and instrument, every fold's training members and
evaluation window, the information boundary, the models and their
configurations, the strategy configuration, the fee and latency scenarios,
the scoring rule, and the workload. Nothing in it is chosen after seeing a
result.

Folds expand. The members holding the instrument, in collection order, are
the timeline; fold k evaluates timeline member k, for every k at or after
`minimum_training_sources`, and trains on every earlier member. A later fold
therefore learns from earlier evaluation recordings once their observations
and labels are history: predeclared chronological retraining, not tuning.

Each evaluation window is its whole recording, [first receive time, last
receive time + 1), so equal timestamps are never divided, and its start is
the fold's information boundary: exclusive, so every training prediction
and every training label is strictly earlier.

The plan ID is the SHA-256 of the canonical manifest without the ID, with
its newline. Inspection re-verifies the collection's sources and rebuilds
the plan from its request: membership, boundaries, and counts are
recomputed, not only hashed.
"""

import re
from collections import Counter
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any

from chronos_research import registry
from chronos_research.artifact_files import ArtifactUnavailableError, artifact_directory, listed_files
from chronos_research.artifact_files import verified_manifest as verified_manifest_file
from chronos_research.artifacts import (
    CLASS_ORDER,
    ArtifactError,
    FeatureRow,
    LabelRow,
    RowIdentity,
    Unavailable,
    VerifiedRecording,
    is_artifact_id,
    read_feature_rows,
    read_label_rows,
)
from chronos_research.collection import MAX_FEATURE_ROWS_PER_SOURCE, MAX_SOURCES, verified_recordings
from chronos_research.encoding import canonical_json, encode_integer, sha256_hex
from chronos_research.experiment import ExperimentConfiguration, model_feature_set_json
from chronos_research.model_data import ModelRow, exact_inputs
from chronos_research.pipeline import Tools
from chronos_research.publication import PublicationError, publish
from chronos_research.strategy_configuration import (
    CONFIGURATION_KEYS,
    DECIMAL_PLACES,
    INT64_MAX,
    ConfigurationError,
    canonical_configuration,
    canonical_decimal,
    canonical_integer,
    configuration_problem,
    decimal_units,
)

REQUEST_SCHEMA = {"name": "chronos.walk_forward_request", "version": "1"}
SCHEMA = {"name": "chronos.walk_forward_plan", "version": "1"}
MANIFEST = "manifest.json"
MODELS = ("majority", "logistic")
MAX_FOLDS = 10
MAX_SCENARIOS = 4
MAX_STRATEGY_RUNS = MAX_FOLDS * len(MODELS) * MAX_SCENARIOS
MAX_TRAINING_ROWS_PER_FOLD = 4_000_000
BASE_SCENARIO = "base"
STRESS_SCENARIO = "stress"
_REQUEST_KEYS = {
    "collection_id",
    "configuration",
    "instrument",
    "minimum_training_sources",
    "scenarios",
    "schema",
}
_SCENARIO_KEYS = {"fee_per_fill", "latency_ns", "name"}
_SCENARIO_NAME = re.compile(r"[a-z][a-z0-9_]{0,31}")
_POSITIVE = re.compile(r"[1-9][0-9]{0,17}")
# A latency keeps every arrival time (signal + latency) far inside int64.
MAX_LATENCY_NS = 10**18 - 1
DEFINITIONS = {
    "information_boundary": (
        "the evaluation window's start, exclusive: every training prediction time and every training "
        "label's availability time is strictly before it"
    ),
    "membership": (
        "expanding: fold k trains on every earlier member of the collection holding the instrument and "
        "evaluates the next one"
    ),
    "retraining": (
        "a later fold learns from earlier evaluation recordings once their observations and labels are "
        "history; this is predeclared chronological retraining, not tuning"
    ),
    "training_row": (
        "a feature row of the instrument in a training member, with available inputs and a usable label, "
        "whose prediction_at_ns and label_available_at_ns are both strictly before the boundary; each other "
        "row is excluded with the first reason that applies, in the order feature_unavailable, "
        "label_unavailable, prediction_at_not_before_boundary, label_available_at_not_before_boundary"
    ),
    "window": "[the evaluation recording's first receive time, its last receive time + 1)",
}
SCORING = {
    "cutoff": "the evaluation window's end, exclusive",
    "rule": "a scheduled row is scored when its label is usable and label_available_at_ns < end_at_ns",
    "unscored": (
        "every other scheduled row is counted by reason: feature_unavailable:<reason>, "
        "label_unavailable:<reason>, or label_after_scoring_cutoff"
    ),
}
SCENARIO_RULES = (
    "every scenario runs the same predictions through the C++ engine, with only the fee per fill and the "
    "positive latency changed; spreads and recorded prices are never changed",
    "the base scenario is the configuration's own fee and latency",
    "the stress scenario is no cheaper and no faster than the base on either axis, and costlier or slower "
    "on at least one",
    "a zero fee is used only where a scenario names it",
)


class PlanError(Exception):
    """A walk-forward request or plan is not valid, or cannot be planned."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def plans_root(artifacts: Path) -> Path:
    return artifacts / "walk_forward_plans"


def models_configuration() -> dict[str, Any]:
    """The two baselines' configurations, as a plan records them in this
    environment: the experiment's own."""
    experiment = ExperimentConfiguration().as_json()
    return {"logistic": experiment["logistic"], "majority": experiment["majority"]}


# --- The request ----------------------------------------------------------------------------------------


def _engine_text(value: Any, field: str, check: Callable[[str, str], str]) -> str:
    """A value the engine holds exactly, written canonically: the walk-forward
    request is frozen as written, so a non-canonical spelling is refused
    rather than rewritten."""
    if not isinstance(value, str):
        raise PlanError("invalid_request", f"{field} is a string")
    try:
        canonical = check(value, field)
    except ConfigurationError as error:
        raise PlanError("invalid_request", str(error)) from error
    if canonical != value:
        raise PlanError("invalid_request", f"{field} is written canonically as {canonical!r}, not {value!r}")
    return value


def _fee(text: str, field: str) -> str:
    return canonical_decimal(text, field, DECIMAL_PLACES, False, True)


def _latency(text: str, field: str) -> str:
    return canonical_integer(text, field, 1, MAX_LATENCY_NS)


def _validated_scenarios(scenarios: Any, configuration: Mapping[str, str]) -> list[dict[str, str]]:
    if not isinstance(scenarios, list) or not 1 <= len(scenarios) <= MAX_SCENARIOS:
        raise PlanError("invalid_request", f"scenarios is a list of 1 to {MAX_SCENARIOS} cells")
    checked: list[dict[str, str]] = []
    for cell in scenarios:
        if not isinstance(cell, dict) or set(cell) != _SCENARIO_KEYS:
            raise PlanError("invalid_request", f"a scenario has exactly {', '.join(sorted(_SCENARIO_KEYS))}")
        name, fee, latency = (cell.get(key) for key in ("name", "fee_per_fill", "latency_ns"))
        if not isinstance(name, str) or not _SCENARIO_NAME.fullmatch(name):
            raise PlanError("invalid_request", "a scenario name is a lowercase identifier")
        fee = _engine_text(fee, f"scenario {name}: fee_per_fill", _fee)
        latency = _engine_text(latency, f"scenario {name}: latency_ns", _latency)
        checked.append({"fee_per_fill": fee, "latency_ns": latency, "name": name})
    if len({cell["name"] for cell in checked}) != len(checked) or len(
        {(cell["fee_per_fill"], cell["latency_ns"]) for cell in checked}
    ) != len(checked):
        raise PlanError("invalid_request", "scenario names and (fee, latency) cells are unique")
    by_name = {cell["name"]: cell for cell in checked}
    base = by_name.get(BASE_SCENARIO)
    if base is None or (base["fee_per_fill"], base["latency_ns"]) != (
        configuration["fee_per_fill"],
        configuration["latency_ns"],
    ):
        raise PlanError(
            "invalid_request", "the base scenario has the configuration's own fee_per_fill and latency_ns"
        )
    if len(checked) > 1:
        stress = by_name.get(STRESS_SCENARIO)
        if stress is None:
            raise PlanError("invalid_request", "more than one scenario needs one named stress")
        fee_order = _decimal_order(stress["fee_per_fill"], base["fee_per_fill"])
        latency_order = _decimal_order(stress["latency_ns"], base["latency_ns"])
        if fee_order < 0 or latency_order < 0 or (fee_order, latency_order) == (0, 0):
            raise PlanError(
                "invalid_request",
                "the stress scenario is no cheaper and no faster than the base, and costlier or slower",
            )
    return checked


def _decimal_order(left: str, right: str) -> int:
    """-1, 0, or 1 as left is less than, equal to, or greater than right;
    both canonical decimals, compared exactly."""
    difference = Decimal(left) - Decimal(right)
    return (difference > 0) - (difference < 0)


def validated_request(document: Any) -> dict[str, Any]:
    """The request, with every value checked against what the engine holds
    exactly, by the same rules as the research service's
    (strategy_configuration.canonical_configuration), and written
    canonically. Nothing the engine would refuse reaches a plan."""
    if not isinstance(document, dict) or set(document) != _REQUEST_KEYS:
        raise PlanError(
            "invalid_request", f"a walk-forward request has exactly {', '.join(sorted(_REQUEST_KEYS))}"
        )
    if document["schema"] != REQUEST_SCHEMA:
        raise PlanError("invalid_request", f"the request's schema is {REQUEST_SCHEMA}")
    collection_id = document["collection_id"]
    if not isinstance(collection_id, str) or not is_artifact_id(collection_id):
        raise PlanError("invalid_request", "collection_id is a 64-digit lowercase ID")
    instrument = document["instrument"]
    if not isinstance(instrument, str) or not registry.INSTRUMENT.fullmatch(instrument):
        raise PlanError("invalid_request", "instrument must be one valid instrument identifier")
    minimum = document["minimum_training_sources"]
    if not isinstance(minimum, str) or not _POSITIVE.fullmatch(minimum) or int(minimum) >= MAX_SOURCES:
        raise PlanError(
            "invalid_request", f"minimum_training_sources is a whole number from 1 to {MAX_SOURCES - 1}"
        )
    problem = configuration_problem(document["configuration"])
    if problem is not None:
        raise PlanError("invalid_request", problem)
    configuration = dict(document["configuration"])
    try:
        canonical = canonical_configuration(configuration, "configuration.")
    except ConfigurationError as error:
        raise PlanError("invalid_request", str(error)) from error
    for key in CONFIGURATION_KEYS:
        if canonical[key] != configuration[key]:
            written = f"{canonical[key]!r}, not {configuration[key]!r}"
            raise PlanError("invalid_request", f"configuration.{key} is written canonically as {written}")
    if int(configuration["latency_ns"]) > MAX_LATENCY_NS:
        raise PlanError("invalid_request", f"configuration.latency_ns must be from 1 to {MAX_LATENCY_NS}")
    return {
        "collection_id": collection_id,
        "configuration": configuration,
        "instrument": instrument,
        "minimum_training_sources": minimum,
        "scenarios": _validated_scenarios(document["scenarios"], configuration),
        "schema": REQUEST_SCHEMA,
    }


def request_digest(request: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json(request))


# --- Rows, joined by identity, and the training rule ----------------------------------------------------


@dataclass(frozen=True, slots=True)
class LabelledRow:
    feature: FeatureRow
    label: LabelRow


def labelled_rows(recording: VerifiedRecording, instrument: str) -> list[LabelledRow]:
    """One recording's feature rows of the instrument, each with its label
    row, joined by identity. Both are read while hashing; every feature row
    must have exactly one label row with the same prediction time."""
    features = read_feature_rows(recording.features)
    labels: dict[RowIdentity, LabelRow] = {}
    for label in read_label_rows(recording.labels):
        if label.identity in labels:
            raise ArtifactError(f"duplicate label row identity {tuple(label.identity)}")
        labels[label.identity] = label
    rows: list[LabelledRow] = []
    seen: set[RowIdentity] = set()
    for feature in features:
        if feature.identity in seen:
            raise ArtifactError(f"duplicate feature row identity {tuple(feature.identity)}")
        seen.add(feature.identity)
        if feature.identity not in labels:
            raise ArtifactError(f"feature row {tuple(feature.identity)} has no label row")
        label = labels.pop(feature.identity)
        if label.prediction_at_ns != feature.available_at_ns:
            raise ArtifactError(f"row {tuple(feature.identity)}: prediction times differ between artifacts")
        if feature.identity.instrument == instrument:
            rows.append(LabelledRow(feature, label))
    if labels:
        raise ArtifactError(f"{len(labels)} label rows have no feature row")
    return rows


def training_row(row: LabelledRow, boundary_ns: int) -> ModelRow | str:
    """The row as a training row of a model whose information boundary is
    `boundary_ns`, or why it may not train one. Its prediction time and its
    label's availability time must both be strictly before the boundary."""
    inputs = exact_inputs(row.feature)
    if isinstance(inputs, Unavailable):
        return f"feature_unavailable:{inputs.reason}"
    label = row.label
    if label.direction is None or label.return_raw is None:
        return f"label_unavailable:{label.unavailable_reason}"
    if label.prediction_at_ns >= boundary_ns:
        return "prediction_at_not_before_boundary"
    if label.label_available_at_ns is None or label.label_available_at_ns >= boundary_ns:
        return "label_available_at_not_before_boundary"
    return ModelRow(
        identity=row.feature.identity,
        split="train",
        inputs=inputs,
        class_index=CLASS_ORDER.index(label.direction),
        return_raw=label.return_raw,
    )


def _latest(current: int | None, value: int | None) -> int | None:
    if value is None:
        return current
    return value if current is None else max(current, value)


@dataclass(frozen=True)
class TrainingSelection:
    """The rows one member contributes to a fold's training, and why the
    others are excluded."""

    rows: tuple[ModelRow, ...]
    excluded: Mapping[str, int]
    max_prediction_at_ns: int | None
    max_label_available_at_ns: int | None


def select_training(rows: Sequence[LabelledRow], boundary_ns: int) -> TrainingSelection:
    selected: list[ModelRow] = []
    excluded: Counter[str] = Counter()
    latest_prediction: int | None = None
    latest_label: int | None = None
    for row in rows:
        training = training_row(row, boundary_ns)
        if isinstance(training, str):
            excluded[training] += 1
            continue
        selected.append(training)
        latest_prediction = _latest(latest_prediction, row.label.prediction_at_ns)
        latest_label = _latest(latest_label, row.label.label_available_at_ns)
    return TrainingSelection(tuple(selected), dict(sorted(excluded.items())), latest_prediction, latest_label)


# --- The plan -------------------------------------------------------------------------------------------


def _optional_time(value: int | None) -> str | None:
    return None if value is None else encode_integer(value)


def _window(recording: VerifiedRecording) -> dict[str, str]:
    receive = recording.dataset["time_range"]["receive_timestamp_ns"]
    return {"end_at_ns": encode_integer(int(receive["latest"]) + 1), "start_at_ns": str(receive["earliest"])}


@dataclass(frozen=True)
class Timeline:
    """The members holding the instrument, with their joined rows."""

    members: list[Mapping[str, Any]]
    recordings: list[VerifiedRecording]
    rows: list[list[LabelledRow]]


def _fold(index: int, minimum: int, timeline: Timeline) -> dict[str, Any]:
    position = minimum + index - 1
    evaluation = timeline.recordings[position]
    window = _window(evaluation)
    boundary = int(window["start_at_ns"])
    members = []
    total = 0
    classes = [0] * len(CLASS_ORDER)
    latest_prediction: int | None = None
    latest_label: int | None = None
    for recording, rows in zip(timeline.recordings[:position], timeline.rows[:position], strict=True):
        selection = select_training(rows, boundary)
        total += len(selection.rows)
        for row in selection.rows:
            classes[row.class_index] += 1
        latest_prediction = _latest(latest_prediction, selection.max_prediction_at_ns)
        latest_label = _latest(latest_label, selection.max_label_available_at_ns)
        members.append(
            {
                "dataset_id": recording.dataset_id,
                "eligible_rows": encode_integer(len(selection.rows)),
                "excluded": {reason: encode_integer(count) for reason, count in selection.excluded.items()},
                "feature_rows": encode_integer(len(rows)),
                "interval": _window(recording),
                "label_artifact_id": recording.labels.artifact_id,
                "stream": recording.stream,
            }
        )
    if total > MAX_TRAINING_ROWS_PER_FOLD:
        raise PlanError(
            "workload_exceeded",
            f"fold {index} would train on {total} rows; at most {MAX_TRAINING_ROWS_PER_FOLD} per fold",
        )
    return {
        "evaluation": {
            "dataset_id": evaluation.dataset_id,
            "feature_artifact_id": evaluation.features.artifact_id,
            "feature_rows": encode_integer(len(timeline.rows[position])),
            "label_artifact_id": evaluation.labels.artifact_id,
            "stream": evaluation.stream,
            "window": {**window, "name": f"fold_{index}"},
        },
        "index": encode_integer(index),
        "information_boundary_ns": window["start_at_ns"],
        "training": {
            "class_support": dict(zip(CLASS_ORDER, map(encode_integer, classes), strict=True)),
            "eligible_rows": encode_integer(total),
            "max_label_available_at_ns": _optional_time(latest_label),
            "max_prediction_at_ns": _optional_time(latest_prediction),
            "members": members,
        },
    }


def _timeline(
    collection: Mapping[str, Any], recordings: Sequence[VerifiedRecording], instrument: str
) -> Timeline:
    held = [
        (member, recording)
        for member, recording in zip(collection["members"], recordings, strict=True)
        if instrument in member["instruments"]
    ]
    return Timeline(
        members=[member for member, _ in held],
        recordings=[recording for _, recording in held],
        rows=[labelled_rows(recording, instrument) for _, recording in held],
    )


def build_plan(tools: Tools, artifacts: Path, document: Any) -> dict[str, Any]:
    """The plan of a request over its collection, verified from its sources
    now, or the reason there can be none."""
    return plan_sources(tools, artifacts, document).plan


def plan_sources(tools: Tools, artifacts: Path, document: Any) -> "PlanSources":
    """The plan of a request, unpublished, with its verified sources' rows."""
    plan, timeline = _plan_and_timeline(tools, artifacts, document)
    return PlanSources(plan=plan, timeline=timeline)


def _plan_and_timeline(tools: Tools, artifacts: Path, document: Any) -> tuple[dict[str, Any], "Timeline"]:
    request = validated_request(document)
    collection, recordings = verified_recordings(tools, artifacts, request["collection_id"])
    instrument = request["instrument"]
    minimum = int(request["minimum_training_sources"])
    held = [member for member in collection["members"] if instrument in member["instruments"]]
    if len(held) < minimum + 1:
        raise PlanError(
            "insufficient_coverage",
            f"{len(held)} recording(s) of collection {collection['collection_id']} hold {instrument}; "
            f"a study training on at least {minimum} earlier recording(s) needs at least {minimum + 1} "
            "independent recordings of it",
        )
    folds = len(held) - minimum
    if folds * decimal_units(request["configuration"]["starting_cash"]) > INT64_MAX:
        raise PlanError(
            "workload_exceeded",
            f"{folds} folds' accounts of {request['configuration']['starting_cash']} starting cash each "
            "exceed the engine's money range in total",
        )
    scenarios = request["scenarios"]
    runs = folds * len(MODELS) * len(scenarios)
    if folds > MAX_FOLDS or runs > MAX_STRATEGY_RUNS:
        raise PlanError(
            "workload_exceeded",
            f"{folds} folds and {runs} strategy runs; at most {MAX_FOLDS} folds and {MAX_STRATEGY_RUNS} runs",
        )
    timeline = _timeline(collection, recordings, instrument)
    fold_documents = [_fold(index, minimum, timeline) for index in range(1, folds + 1)]
    skipped = [
        {"dataset_id": member["dataset"]["dataset_id"], "reason": "instrument_absent"}
        for member in collection["members"]
        if instrument not in member["instruments"]
    ]
    manifest = {
        "class_order": list(CLASS_ORDER),
        "collection": {
            "collection_id": collection["collection_id"],
            "evidence": collection["evidence"],
            "skipped_members": skipped,
        },
        "configuration": request["configuration"],
        "definitions": DEFINITIONS,
        "folds": fold_documents,
        "horizon_ns": collection["contract"]["horizon_ns"],
        "instrument": instrument,
        "model_feature_set": model_feature_set_json(),
        "models": models_configuration(),
        "request": request,
        "request_sha256": request_digest(request),
        "scenario_rules": list(SCENARIO_RULES),
        "scenarios": scenarios,
        "schema": SCHEMA,
        "scoring": SCORING,
        "workload": {
            "caps": {
                "feature_rows_per_source": encode_integer(MAX_FEATURE_ROWS_PER_SOURCE),
                "folds": encode_integer(MAX_FOLDS),
                "scenarios": encode_integer(MAX_SCENARIOS),
                "sources": encode_integer(MAX_SOURCES),
                "strategy_runs": encode_integer(MAX_STRATEGY_RUNS),
                "training_rows_per_fold": encode_integer(MAX_TRAINING_ROWS_PER_FOLD),
            },
            "evaluation_rows": encode_integer(
                sum(int(fold["evaluation"]["feature_rows"]) for fold in fold_documents)
            ),
            "folds": encode_integer(folds),
            "models": encode_integer(len(MODELS)),
            "scenarios": encode_integer(len(scenarios)),
            "schedules": encode_integer(folds * len(MODELS)),
            "sources": encode_integer(len(held)),
            "strategy_runs": encode_integer(runs),
            "training_rows": encode_integer(
                sum(int(fold["training"]["eligible_rows"]) for fold in fold_documents)
            ),
        },
    }
    return {**manifest, "plan_id": plan_identity(manifest)}, timeline


def plan_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json({key: value for key, value in manifest.items() if key != "plan_id"}))


def verified_plan(root: Path, plan_id: str) -> dict[str, Any]:
    """The plan, its identity and schema verified from bytes read once."""
    try:
        directory = artifact_directory(root, plan_id)
        if listed_files(directory) != [MANIFEST]:
            raise PlanError("artifact_invalid", "a walk-forward plan holds exactly manifest.json")
        manifest = verified_manifest_file(directory, "plan_id", with_newline=True)
    except ArtifactUnavailableError as error:
        raise PlanError("artifact_invalid", str(error)) from error
    if manifest.get("schema") != SCHEMA:
        raise PlanError("artifact_invalid", f"plan {plan_id} is not {SCHEMA}")
    return dict(manifest)


@dataclass(frozen=True)
class PlanSources:
    """A published plan, rebuilt identical from its request over its
    collection's sources verified now, with those sources' rows."""

    plan: dict[str, Any]
    timeline: Timeline

    def fold(self, index: int) -> dict[str, Any]:
        fold: dict[str, Any] = self.plan["folds"][index - 1]
        return fold

    def evaluation(self, index: int) -> tuple[VerifiedRecording, list[LabelledRow]]:
        position = int(self.plan["request"]["minimum_training_sources"]) + index - 1
        return self.timeline.recordings[position], self.timeline.rows[position]

    def training(self, index: int) -> list[tuple[VerifiedRecording, TrainingSelection]]:
        """Each earlier member and the rows it contributes to fold `index`."""
        position = int(self.plan["request"]["minimum_training_sources"]) + index - 1
        boundary = int(self.fold(index)["information_boundary_ns"])
        return [
            (recording, select_training(rows, boundary))
            for recording, rows in zip(
                self.timeline.recordings[:position], self.timeline.rows[:position], strict=True
            )
        ]


def verified_plan_sources(tools: Tools, artifacts: Path, plan_id: str) -> PlanSources:
    """Verifies the plan, then rebuilds it from its request over its
    collection's sources, verified again now, and requires identical bytes."""
    manifest = verified_plan(plans_root(artifacts), plan_id)
    rebuilt, timeline = _plan_and_timeline(tools, artifacts, manifest["request"])
    if canonical_json(rebuilt) != canonical_json(manifest):
        raise PlanError("artifact_invalid", "the plan differs from the one its request and sources give")
    return PlanSources(plan=manifest, timeline=timeline)


def inspect_plan(tools: Tools, artifacts: Path, plan_id: str) -> dict[str, Any]:
    return verified_plan_sources(tools, artifacts, plan_id).plan


def create_plan(tools: Tools, artifacts: Path, document: Any) -> dict[str, Any]:
    manifest = build_plan(tools, artifacts, document)
    root = plans_root(artifacts)
    root.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verified_plan(root, directory.name)
        return (directory / MANIFEST).read_bytes()

    try:
        outcome, warning = publish(
            root, manifest["plan_id"], {MANIFEST: canonical_json(manifest)}, MANIFEST, existing
        )
    except PublicationError as error:
        raise PlanError("publication_failed", str(error)) from error
    return {"outcome": outcome.value, "plan": manifest, "warning": warning}


def plan_ids(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    return sorted(name for name in (entry.name for entry in root.iterdir()) if is_artifact_id(name))
