"""An inference schedule: one fitted model's predictions on every feature row
of one instrument in one evaluation window, published as an immutable
artifact for the C++ strategy runner:

    <schedule-root>/<schedule-id>/manifest.json
    <schedule-root>/<schedule-id>/schedule.ndjson   one row per feature row

It is label-free by construction. It reads the verified feature rows and
the verified experiment's stored parameters, and never opens the label or
split rows: no future return, true class, label time, or future observation
can reach a row. The split's manifest contributes only its window
boundaries and the latest time a training label became available, which
must precede the window.

The window holds every feature row of the instrument available in it,
labelled or not, so the rows after the last label (the tail) are scheduled
too. The validation window is [validation start, test start) and the test
window is [test start, the end of the dataset). A row whose model inputs are
unavailable has no prediction from any model, with the reason.

The schedule ID is the SHA-256 of the canonical manifest, with its newline,
without the ID, as for experiments.
"""

import hashlib
import json
import math
import os
import stat
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

import numpy as np

from chronos_research.artifacts import (
    CLASS_ORDER,
    ArtifactError,
    FeatureRow,
    Unavailable,
    VerifiedSources,
    is_artifact_id,
    read_feature_rows,
)
from chronos_research.encoding import canonical_json, decode_float, decode_integer, encode_float, sha256_hex
from chronos_research.experiment import environment, model_feature_set_json, source_json
from chronos_research.inspection import verified_experiment
from chronos_research.model_data import exact_inputs, to_floats
from chronos_research.models import LogisticModel, MajorityModel, Unevaluable

SCHEMA = {"name": "chronos.inference_schedule", "version": "1"}
# A registered model version's schedule (ADR 0021; registered_schedule.py).
# It names the version, never an experiment, and its model's information
# cutoff, verified or declared, which must be at or before the window.
REGISTERED_SCHEMA = {"name": "chronos.inference_schedule", "version": "2"}
ROW_RECORD = {"name": "chronos.inference_row", "version": "1"}
MANIFEST = "manifest.json"
ROWS = "schedule.ndjson"
WINDOWS = ("validation", "test")
SCHEDULE_MODELS = ("logistic", "majority")
CONFIDENCE_SCALE = 1_000_000_000
SCOPE = (
    "model predictions only: a schedule says what the model would have predicted at each feature row; "
    "whether trading on it pays is the strategy runner's question"
)
INFERENCE = {
    "class": "the model's predicted class: the largest logit, ties to the earliest class in class_order",
    "confidence_ppb": "floor(1e9 x the predicted class's probability), exact from its binary64 value",
    "rows": "every feature row of the instrument available in the window, labelled or not",
    "unavailable": "a row whose model inputs are unavailable has no prediction from any model",
    "window": "start_at_ns <= available_at_ns, and available_at_ns < end_at_ns when the window has an end",
}
REGISTERED_INFERENCE = {
    **INFERENCE,
    "class": "the model version's own class rule, stated in model.runtime.class",
}
CUTOFF_BASES = ("verified", "declared")
_REGISTERED_MODEL_KEYS = {
    "family",
    "instrument",
    "model_feature_set",
    "model_version_id",
    "payload_sha256",
    "runtime",
}


class ScheduleError(Exception):
    """A schedule cannot be built from these inputs, or is not valid."""


@dataclass(frozen=True)
class Window:
    name: str
    start_at_ns: int
    # None: through the end of the dataset.
    end_at_ns: int | None

    def contains(self, available_at_ns: int) -> bool:
        return self.start_at_ns <= available_at_ns and (
            self.end_at_ns is None or available_at_ns < self.end_at_ns
        )

    def as_json(self) -> dict[str, Any]:
        return {
            "end_at_ns": None if self.end_at_ns is None else str(self.end_at_ns),
            "name": self.name,
            "start_at_ns": str(self.start_at_ns),
        }


def evaluation_window(sources: VerifiedSources, name: str) -> Window:
    if name == "validation":
        return Window(name, sources.validation_start_at_ns, sources.test_start_at_ns)
    if name == "test":
        return Window(name, sources.test_start_at_ns, None)
    raise ScheduleError(f"a schedule covers one evaluation window, {' or '.join(WINDOWS)}, not {name!r}")


def training_labels_available_through(sources: VerifiedSources) -> int:
    """The latest time any usable training label became available."""
    return int(sources.splits.summary["usable"]["train"]["max_label_available_at_ns"])


def require_training_precedes(window: Window, labels_available_through_ns: int) -> None:
    if labels_available_through_ns >= window.start_at_ns:
        raise ScheduleError(
            f"a training label is available at {labels_available_through_ns}, not before the {window.name} "
            f"window begins at {window.start_at_ns}"
        )


@dataclass(frozen=True)
class Schedule:
    schedule_id: str
    files: Mapping[str, bytes]


def confidence_ppb(probability: float) -> int:
    return math.floor(Fraction(probability) * CONFIDENCE_SCALE)


def _identity_json(row: FeatureRow) -> dict[str, str]:
    return {
        "ingest_sequence": str(row.identity.ingest_sequence),
        "instrument": row.identity.instrument,
        "stream": row.identity.stream,
    }


def _stored_model(
    directory: Path, sources: VerifiedSources, instrument: str, model_name: str
) -> tuple[str, MajorityModel | LogisticModel, dict[str, Any]]:
    manifest, models = verified_experiment(directory)
    if manifest["sources"] != source_json(sources):
        raise ScheduleError(f"experiment {directory.name} was not trained on these verified sources")
    if model_name not in SCHEDULE_MODELS:
        raise ScheduleError(f"the model is one of {', '.join(SCHEDULE_MODELS)}, not {model_name!r}")
    if instrument not in models:
        raise ScheduleError(f"experiment {directory.name} has no models for {instrument!r}")
    model = models[instrument][model_name]
    if isinstance(model, Unevaluable):
        raise ScheduleError(
            f"the {model_name} model for {instrument} is unevaluable ({model.reason}) and predicts nothing"
        )
    # Hash the parameters we actually verified and use for inference. Opening
    # model.json again could pair predictions with different, unchecked bytes.
    entry = model.as_json()
    return manifest["experiment_id"], model, entry


def logistic_predictions(
    model: LogisticModel, inputs: Sequence[tuple[int, ...]]
) -> tuple[np.ndarray[Any, np.dtype[np.int64]], np.ndarray[Any, np.dtype[np.float64]]]:
    """The stored logistic model on each row alone. A matrix product's
    summation order can depend on the batch's shape, so the same row could
    get a different last bit in another window; here every row is computed
    with elementwise operations in one fixed order:

        logit_k = ((((s_0 c_k0 + s_1 c_k1) + s_2 c_k2) + s_3 c_k3) + s_4 c_k4) + b_k

    with s the standardized inputs, then the softmax of the logits shifted by
    their maximum. It agrees with the experiment's batch inference within
    INFERENCE_TOLERANCE, which the tests check."""
    rows = len(inputs)
    if rows == 0:
        return np.zeros(0, dtype=np.int64), np.zeros((0, len(CLASS_ORDER)), dtype=np.float64)
    try:
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            standardized = (to_floats(inputs) - model.mean) / model.scale
            logits = np.empty((rows, len(CLASS_ORDER)), dtype=np.float64)
            for k in range(len(CLASS_ORDER)):
                total = standardized[:, 0] * model.coefficients[k, 0]
                for j in range(1, standardized.shape[1]):
                    total = total + standardized[:, j] * model.coefficients[k, j]
                logits[:, k] = total + model.intercepts[k]
            largest = logits[:, 0]
            for k in range(1, len(CLASS_ORDER)):
                largest = np.maximum(largest, logits[:, k])
            shifted = np.exp(logits - largest[:, None])
            normalizer = shifted[:, 0]
            for k in range(1, len(CLASS_ORDER)):
                normalizer = normalizer + shifted[:, k]
            probabilities = shifted / normalizer[:, None]
    except FloatingPointError as error:
        raise ScheduleError("non-finite logistic inference") from error
    if not np.isfinite(logits).all() or not np.isfinite(probabilities).all():
        raise ScheduleError("non-finite logistic inference")
    return np.argmax(logits, axis=1).astype(np.int64), probabilities


def _predict(
    model: MajorityModel | LogisticModel, inputs: list[tuple[int, ...]]
) -> tuple[np.ndarray[Any, np.dtype[np.int64]], np.ndarray[Any, np.dtype[np.float64]]]:
    if isinstance(model, MajorityModel):
        return model.predict(len(inputs))
    return logistic_predictions(model, inputs)


def window_rows(sources: VerifiedSources, instrument: str, window: Window) -> list[FeatureRow]:
    """Every feature row of the instrument available in the window."""
    rows = [
        row
        for row in read_feature_rows(sources.features)
        if row.identity.instrument == instrument and window.contains(row.available_at_ns)
    ]
    if not rows:
        raise ScheduleError(f"{instrument} has no feature rows in the {window.name} window")
    return rows


@dataclass(frozen=True)
class ScheduledRows:
    contents: bytes
    counts: dict[str, Any]


def scheduled_rows(
    rows: Sequence[FeatureRow],
    inputs: Sequence[tuple[int, ...] | Unavailable],
    predictions: Sequence[tuple[int, Sequence[float]]],
) -> ScheduledRows:
    """The schedule file and its counts. `predictions` holds one predicted
    class index and its probabilities for each row whose inputs are
    available, in order."""
    records: list[bytes] = []
    unavailable: dict[str, int] = {}
    class_counts = [0] * len(CLASS_ORDER)
    predicted = iter(predictions)
    count = 0
    for row, value in zip(rows, inputs, strict=True):
        record: dict[str, Any] = {
            "available_at_ns": str(row.available_at_ns),
            "identity": _identity_json(row),
            "record": ROW_RECORD,
        }
        if isinstance(value, Unavailable):
            reason = f"feature_unavailable:{value.reason}"
            unavailable[reason] = unavailable.get(reason, 0) + 1
            record["inputs"] = {"unavailable": value.reason}
            record["prediction"] = {"unavailable": reason}
        else:
            index, probabilities = next(predicted)
            count += 1
            class_counts[index] += 1
            record["inputs"] = {"raw": [str(raw) for raw in value]}
            record["prediction"] = {
                "class": CLASS_ORDER[index],
                "confidence_ppb": str(confidence_ppb(float(probabilities[index]))),
                "probabilities": [encode_float(float(p)) for p in probabilities],
            }
        records.append(canonical_json(record))
    if next(predicted, None) is not None:
        raise ScheduleError("there are more predictions than rows with available inputs")
    return ScheduledRows(
        contents=b"".join(records),
        counts={
            "classes": {name: str(n) for name, n in zip(CLASS_ORDER, class_counts, strict=True)},
            "predicted": str(count),
            "rows": str(len(rows)),
            "unavailable": {reason: str(n) for reason, n in sorted(unavailable.items())},
        },
    )


def schedule_files(manifest: dict[str, Any], scheduled: ScheduledRows) -> Schedule:
    """The published files of a manifest without its ID or files entry."""
    manifest = {
        **manifest,
        "counts": scheduled.counts,
        "files": {
            "schedule": {
                "name": ROWS,
                "sha256": sha256_hex(scheduled.contents),
                "size_bytes": str(len(scheduled.contents)),
            }
        },
    }
    schedule_id = schedule_identity(manifest)
    return Schedule(
        schedule_id=schedule_id,
        files={ROWS: scheduled.contents, MANIFEST: canonical_json({**manifest, "artifact_id": schedule_id})},
    )


def build_schedule(
    sources: VerifiedSources, experiment_directory: Path, instrument: str, model_name: str, window_name: str
) -> Schedule:
    window = evaluation_window(sources, window_name)
    through = training_labels_available_through(sources)
    require_training_precedes(window, through)
    experiment_id, model, entry = _stored_model(experiment_directory, sources, instrument, model_name)
    rows = window_rows(sources, instrument, window)
    inputs = [exact_inputs(row) for row in rows]
    available = [value for value in inputs if not isinstance(value, Unavailable)]
    classes, probabilities = _predict(model, available)
    scheduled = scheduled_rows(
        rows, inputs, [(int(index), list(row)) for index, row in zip(classes, probabilities, strict=True)]
    )
    manifest = {
        "class_order": list(CLASS_ORDER),
        "environment": environment(),
        "inference": INFERENCE,
        "model": {
            "experiment_id": experiment_id,
            "instrument": instrument,
            "model_feature_set": model_feature_set_json(),
            "name": model_name,
            "parameters_sha256": sha256_hex(canonical_json(entry)),
        },
        "row": ROW_RECORD,
        "schema": SCHEMA,
        "scope": SCOPE,
        "source": source_json(sources),
        "training": {"label_available_through_ns": str(through), "split": "train"},
        "window": window.as_json(),
    }
    return schedule_files(manifest, scheduled)


def schedule_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json({key: value for key, value in manifest.items() if key != "artifact_id"}))


_MANIFEST_KEYS = {
    "artifact_id", "class_order", "counts", "environment", "files", "inference", "model", "row", "schema",
    "scope", "source", "training", "window",
}  # fmt: skip
_ROW_KEYS = {"available_at_ns", "identity", "inputs", "prediction", "record"}


def _canonical(data: bytes, what: str) -> Any:
    try:
        document = json.loads(data)
    except ValueError as error:
        raise ScheduleError(f"{what} is not JSON: {error}") from error
    if canonical_json(document) != data:
        raise ScheduleError(f"{what} is not in canonical form")
    return document


def _require_keys(document: Any, keys: set[str], what: str) -> None:
    if not isinstance(document, dict) or set(document) != keys:
        raise ScheduleError(f"{what} must have exactly the keys {', '.join(sorted(keys))}")


def _check_prediction(prediction: Any, inputs: Any, what: str) -> str | None:
    """The row's predicted class, or None for an unavailable row."""
    if "unavailable" in inputs:
        _require_keys(inputs, {"unavailable"}, f"{what} inputs")
        if prediction != {"unavailable": f"feature_unavailable:{inputs['unavailable']}"}:
            raise ScheduleError(f"{what}: unavailable inputs must give an unavailable prediction")
        return None
    _require_keys(inputs, {"raw"}, f"{what} inputs")
    if len(inputs["raw"]) != len(model_feature_set_json()["inputs"]):
        raise ScheduleError(f"{what} has the wrong number of inputs")
    for raw in inputs["raw"]:
        decode_integer(raw)
    _require_keys(prediction, {"class", "confidence_ppb", "probabilities"}, f"{what} prediction")
    probabilities = [decode_float(p) for p in prediction["probabilities"]]
    if len(probabilities) != len(CLASS_ORDER) or not all(0.0 <= p <= 1.0 for p in probabilities):
        raise ScheduleError(f"{what} must have one probability in [0, 1] per class")
    if prediction["class"] not in CLASS_ORDER:
        raise ScheduleError(f"{what} predicts an unknown class")
    expected = confidence_ppb(probabilities[CLASS_ORDER.index(prediction["class"])])
    if decode_integer(prediction["confidence_ppb"]) != expected:
        raise ScheduleError(f"{what}: confidence_ppb does not follow from the class's probability")
    return str(prediction["class"])


def require_cutoff_precedes(window: Window, information_cutoff_ns: int) -> None:
    if information_cutoff_ns > window.start_at_ns:
        raise ScheduleError(
            f"the model's information cutoff {information_cutoff_ns} is after the {window.name} window "
            f"begins at {window.start_at_ns}"
        )


def _check_registered_model(model: Any, training: Any, window: Window) -> None:
    _require_keys(model, _REGISTERED_MODEL_KEYS, "the registered model")
    _require_keys(model["runtime"], {"class", "name", "version"}, "the model's runtime")
    if not is_artifact_id(model["model_version_id"]) or not is_artifact_id(model["payload_sha256"]):
        raise ScheduleError("a registered model is named by its version ID and payload checksum")
    if not all(isinstance(text, str) and text for text in (model["family"], *model["runtime"].values())):
        raise ScheduleError("the model's family and runtime must be named")
    _require_keys(training, {"basis", "information_cutoff_ns"}, "the model's training")
    if training["basis"] not in CUTOFF_BASES:
        raise ScheduleError("an information cutoff is verified or declared")
    require_cutoff_precedes(window, decode_integer(training["information_cutoff_ns"]))


def verify_schedule(directory: Path) -> dict[str, Any]:
    """Checks a schedule's files, identity, checksum, and every row, and
    returns its manifest. It does not recompute the predictions; see
    reproduce_schedule."""
    try:
        return _verify(directory)
    except ScheduleError:
        raise
    except (AttributeError, IndexError, KeyError, TypeError, ValueError) as error:
        raise ScheduleError(
            f"{directory.name} is not a well-formed schedule ({type(error).__name__}: {error})"
        ) from error


def _verify(directory: Path) -> dict[str, Any]:
    if directory.is_symlink() or not directory.is_dir() or not is_artifact_id(directory.name):
        raise ScheduleError(f"{directory} is not a schedule directory named by its ID")
    if sorted(os.listdir(directory)) != sorted([MANIFEST, ROWS]):
        raise ScheduleError(f"a schedule holds exactly {MANIFEST} and {ROWS}")
    for name in (MANIFEST, ROWS):
        if not stat.S_ISREG(os.lstat(directory / name).st_mode):
            raise ScheduleError(f"{name} must be a regular file")
    manifest = _canonical((directory / MANIFEST).read_bytes(), MANIFEST)
    _require_keys(manifest, _MANIFEST_KEYS, MANIFEST)
    if manifest["schema"] not in (SCHEMA, REGISTERED_SCHEMA) or manifest["row"] != ROW_RECORD:
        raise ScheduleError("unsupported schedule schema or row record")
    registered = manifest["schema"] == REGISTERED_SCHEMA
    if manifest["scope"] != SCOPE or manifest["class_order"] != list(CLASS_ORDER):
        raise ScheduleError("unsupported scope or class order")
    if manifest["inference"] != (REGISTERED_INFERENCE if registered else INFERENCE):
        raise ScheduleError("unsupported inference definition")
    if schedule_identity(manifest) != manifest["artifact_id"] or manifest["artifact_id"] != directory.name:
        raise ScheduleError("the schedule ID does not match the manifest's content or the directory name")
    window = Window(
        name=manifest["window"]["name"],
        start_at_ns=decode_integer(manifest["window"]["start_at_ns"]),
        end_at_ns=None
        if manifest["window"]["end_at_ns"] is None
        else decode_integer(manifest["window"]["end_at_ns"]),
    )
    if window.name not in WINDOWS or (window.end_at_ns is None) != (window.name == "test"):
        raise ScheduleError(f"unsupported window {manifest['window']}")
    model = manifest["model"]
    if registered:
        _check_registered_model(model, manifest["training"], window)
    else:
        require_training_precedes(window, decode_integer(manifest["training"]["label_available_through_ns"]))
        if model["name"] not in SCHEDULE_MODELS:
            raise ScheduleError("unsupported model")
    if model["model_feature_set"] != model_feature_set_json():
        raise ScheduleError("unsupported model feature set")

    digest = hashlib.sha256()
    count = predicted = 0
    classes = dict.fromkeys(CLASS_ORDER, 0)
    unavailable: dict[str, int] = {}
    previous: int | None = None
    previous_at: int | None = None
    with (directory / ROWS).open("rb") as file:
        for number, line in enumerate(file, start=1):
            digest.update(line)
            count += 1
            what = f"{ROWS} line {number}"
            record = _canonical(line, what)
            _require_keys(record, _ROW_KEYS, what)
            identity = record["identity"]
            _require_keys(identity, {"ingest_sequence", "instrument", "stream"}, f"{what} identity")
            sequence = decode_integer(identity["ingest_sequence"])
            available_at = decode_integer(record["available_at_ns"])
            if (
                record["record"] != ROW_RECORD
                or identity["stream"] != manifest["source"]["stream"]
                or identity["instrument"] != model["instrument"]
                or not window.contains(available_at)
                or (previous is not None and sequence <= previous)
                or (previous_at is not None and available_at < previous_at)
            ):
                raise ScheduleError(f"{what} is not a later row of this instrument and window")
            previous, previous_at = sequence, available_at
            predicted_class = _check_prediction(record["prediction"], record["inputs"], what)
            if predicted_class is None:
                reason = record["prediction"]["unavailable"]
                unavailable[reason] = unavailable.get(reason, 0) + 1
            else:
                predicted += 1
                classes[predicted_class] += 1
    files = manifest["files"]
    _require_keys(files, {"schedule"}, "the manifest's files")
    size = (directory / ROWS).stat().st_size
    if files["schedule"] != {"name": ROWS, "sha256": digest.hexdigest(), "size_bytes": str(size)}:
        raise ScheduleError(f"{ROWS} differs from the manifest's checksum or size")
    counts = {
        "classes": {name: str(n) for name, n in classes.items()},
        "predicted": str(predicted),
        "rows": str(count),
        "unavailable": {reason: str(n) for reason, n in sorted(unavailable.items())},
    }
    if manifest["counts"] != counts or count == 0:
        raise ScheduleError("the manifest's counts differ from the rows")
    return dict(manifest)


def reproduce_schedule(directory: Path, sources: VerifiedSources, experiments: Path) -> dict[str, Any]:
    """Verifies a schedule, then rebuilds it from its verified sources and
    experiment and requires identical bytes."""
    manifest = verify_schedule(directory)
    if manifest["source"] != source_json(sources):
        raise ScheduleError("the schedule names different sources")
    model = manifest["model"]
    rebuilt = build_schedule(
        sources,
        experiments / model["experiment_id"],
        model["instrument"],
        model["name"],
        manifest["window"]["name"],
    )
    for name, contents in rebuilt.files.items():
        if (directory / name).read_bytes() != contents:
            raise ScheduleError(f"{name} differs from the schedule rebuilt from its sources")
    return manifest


def require_schedule(directory: Path) -> None:
    if not directory.is_dir():
        raise ArtifactError(f"the schedule {directory.name} does not exist under {directory.parent}")
