"""Version 3 inference schedules: one walk-forward fold model's predictions on
every feature row of the instrument in its whole evaluation recording
(ADR 0022).

They are version 1 schedules in their rows and their row-pure inference.
Only the provenance differs: the source is the collection and the evaluation
recording's dataset and features, with no labels and no split; the model is
the plan, fold, and fold model; the training block is the information
boundary and the latest training prediction and label times, both strictly
before it; and the window is explicit. Inference reads verified feature rows
and the fold model's verified parameters, and nothing else: every row of
the window is scheduled, labelled or not, and no label can reach one.

Scoring comes afterwards. `fold_labels` reads the evaluation recording's
verified labels again and joins them to the scheduled rows by identity
(scoring.score_schedule). A row is scored when its inputs were available,
its label is usable, and its label was known before the window's end, the
plan's predeclared cutoff; every other row is counted by reason.
"""

from collections import Counter
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from chronos_research.artifacts import CLASS_ORDER, FEATURE_SET, Unavailable, VerifiedRecording
from chronos_research.encoding import decode_integer
from chronos_research.experiment import environment, model_feature_set_json
from chronos_research.fold_models import FoldModel
from chronos_research.model_data import exact_inputs
from chronos_research.models import Unevaluable
from chronos_research.publication import publish
from chronos_research.schedule import (
    FOLD_SCHEMA,
    INFERENCE,
    MANIFEST,
    ROW_RECORD,
    SCOPE,
    Schedule,
    ScheduleError,
    Window,
    instrument_rows,
    predict_rows,
    schedule_files,
    scheduled_rows,
    verify_schedule,
)
from chronos_research.scoring import WindowLabels
from chronos_research.walk_forward import labelled_rows


def fold_window(fold: Mapping[str, Any]) -> Window:
    window = fold["evaluation"]["window"]
    return Window(window["name"], decode_integer(window["start_at_ns"]), decode_integer(window["end_at_ns"]))


def build_fold_schedule(
    plan: Mapping[str, Any],
    fold: Mapping[str, Any],
    fold_model: FoldModel,
    model_name: str,
    recording: VerifiedRecording,
) -> Schedule:
    """The fold model's predictions on every feature row of its evaluation
    window, from verified features and frozen parameters alone."""
    model = fold_model.models[model_name]
    if isinstance(model, Unevaluable):
        raise ScheduleError(f"the fold {fold['index']} {model_name} model is unevaluable ({model.reason})")
    evaluation = fold["evaluation"]
    if (
        (recording.dataset_id, recording.features.artifact_id)
        != (
            evaluation["dataset_id"],
            evaluation["feature_artifact_id"],
        )
        or fold_model.manifest["plan_id"] != plan["plan_id"]
        or fold_model.manifest["fold"] != fold["index"]
    ):
        raise ScheduleError(f"fold {fold['index']}'s model or recording is not the one its plan names")
    window = fold_window(fold)
    rows = instrument_rows(recording.features, plan["instrument"], window)
    if len(rows) != int(evaluation["feature_rows"]):
        raise ScheduleError(f"fold {fold['index']}'s evaluation recording no longer has its planned rows")
    inputs = [exact_inputs(row) for row in rows]
    classes, probabilities = predict_rows(
        model, [value for value in inputs if not isinstance(value, Unavailable)]
    )
    training = fold_model.manifest["training"]
    manifest = {
        "class_order": list(CLASS_ORDER),
        "environment": environment(),
        "inference": INFERENCE,
        "model": {
            "fold": fold["index"],
            "fold_model_id": fold_model.fold_model_id,
            "instrument": plan["instrument"],
            "model_feature_set": model_feature_set_json(),
            "name": model_name,
            "parameters_sha256": fold_model.manifest["models"][model_name]["parameters_sha256"],
            "plan_id": plan["plan_id"],
        },
        "row": ROW_RECORD,
        "schema": FOLD_SCHEMA,
        "scope": SCOPE,
        "source": {
            "collection_id": plan["collection"]["collection_id"],
            "dataset_id": recording.dataset_id,
            "feature_artifact_id": recording.features.artifact_id,
            "feature_set": FEATURE_SET,
            "features_sha256": recording.features.rows_sha256,
            "horizon_ns": plan["horizon_ns"],
            "stream": recording.stream,
        },
        "training": {
            "information_boundary_ns": training["information_boundary_ns"],
            "label_available_through_ns": training["max_label_available_at_ns"],
            "prediction_through_ns": training["max_prediction_at_ns"],
        },
        "window": window.as_json(),
    }
    predictions = [(int(index), list(row)) for index, row in zip(classes, probabilities, strict=True)]
    return schedule_files(manifest, scheduled_rows(rows, inputs, predictions))


def publish_fold_schedule(schedules: Path, built: Schedule) -> str:
    schedules.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verify_schedule(directory)
        return (directory / MANIFEST).read_bytes()

    outcome, _ = publish(schedules, built.schedule_id, built.files, MANIFEST, existing)
    return outcome.value


def fold_labels(recording: VerifiedRecording, instrument: str, window: Window) -> WindowLabels:
    """The realized class of each scorable row of a fold's evaluation
    window, from its verified labels, and every other row's reason."""
    if window.end_at_ns is None:
        raise ScheduleError("a fold's evaluation window has an explicit end")
    truth = {}
    not_scored: Counter[str] = Counter()
    for row in labelled_rows(recording, instrument):
        if not window.contains(row.feature.available_at_ns):
            continue
        label = row.label
        inputs = exact_inputs(row.feature)
        if isinstance(inputs, Unavailable):
            not_scored[f"feature_unavailable:{inputs.reason}"] += 1
        elif label.direction is None:
            not_scored[f"label_unavailable:{label.unavailable_reason}"] += 1
        elif label.label_available_at_ns is None or label.label_available_at_ns >= window.end_at_ns:
            not_scored["label_after_scoring_cutoff"] += 1
        else:
            truth[row.feature.identity] = CLASS_ORDER.index(label.direction)
    return WindowLabels(truth, dict(not_scored))
