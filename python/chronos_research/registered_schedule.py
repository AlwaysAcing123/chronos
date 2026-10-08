"""A registered model version's inference schedule (ADR 0021).

`chronos.inference_schedule/2` is a version 1 schedule in everything but
its model: every feature row of one instrument in one window of a verified
split, label-free, the unlabelled tail included, with the same row records.
Its model is a registered version, named by its ID, family, payload
checksum, runtime, and information cutoff with that cutoff's basis; it
never names an experiment, even for a version that came from one.

Each row is predicted alone through registered_inference, the adapter live
deployments use too: a baseline by its row-pure arithmetic on the host, an
import only in the ONNX sandbox. A version is scheduled only when its
cutoff is known and at or before the window's start; a declared cutoff is
recorded as declared. Rebuilding a schedule from its sources and version
gives identical bytes; for an import that needs the same sandbox image and
ONNX Runtime, which the schedule's environment records.
"""

from pathlib import Path
from typing import Any

from chronos_research.artifacts import CLASS_ORDER, Unavailable, VerifiedSources
from chronos_research.experiment import environment, model_feature_set_json, source_json
from chronos_research.model_data import exact_inputs
from chronos_research.onnx_sandbox import Sandbox
from chronos_research.registered_inference import InvalidPredictionError, open_predictor
from chronos_research.registry import ModelVersion, RegistryRoots, verified_model_version
from chronos_research.schedule import (
    REGISTERED_INFERENCE,
    REGISTERED_SCHEMA,
    ROW_RECORD,
    SCOPE,
    Schedule,
    ScheduleError,
    evaluation_window,
    require_cutoff_precedes,
    schedule_files,
    scheduled_rows,
    verify_schedule,
    window_rows,
)


class ScheduleRefusedError(ScheduleError):
    """The version cannot be scheduled here; why is its reason."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def _cutoff(version: ModelVersion) -> tuple[int, str]:
    cutoff = version.information_cutoff_ns
    if cutoff is None:
        raise ScheduleRefusedError(
            "cutoff_unavailable",
            "the version has no information cutoff, so no window is out of sample for it",
        )
    return cutoff, str(version.manifest["training"]["information_cutoff"]["basis"])


def build_registered_schedule(
    sources: VerifiedSources, version: ModelVersion, window_name: str, sandbox: Sandbox | None
) -> Schedule:
    window = evaluation_window(sources, window_name)
    cutoff, basis = _cutoff(version)
    try:
        require_cutoff_precedes(window, cutoff)
    except ScheduleError as error:
        raise ScheduleRefusedError("cutoff_after_window_start", str(error)) from error
    rows = window_rows(sources, version.instrument, window)
    inputs = [exact_inputs(row) for row in rows]
    predictor = open_predictor(version, sandbox)
    try:
        predictions: list[tuple[int, tuple[float, ...]]] = []
        for row, value in zip(rows, inputs, strict=True):
            if isinstance(value, Unavailable):
                continue
            try:
                prediction = predictor.predict(value)
            except InvalidPredictionError as error:
                raise ScheduleRefusedError(
                    error.reason, f"row {row.identity.ingest_sequence} has no valid prediction: {error}"
                ) from error
            predictions.append((prediction.class_index, prediction.probabilities))
        runtime_environment = predictor.runtime_environment()
    finally:
        predictor.close()
    runtime = version.manifest["runtime"]
    manifest: dict[str, Any] = {
        "class_order": list(CLASS_ORDER),
        "environment": {**environment(), **runtime_environment},
        "inference": REGISTERED_INFERENCE,
        "model": {
            "family": version.family,
            "instrument": version.instrument,
            "model_feature_set": model_feature_set_json(),
            "model_version_id": version.model_version_id,
            "payload_sha256": version.manifest["payload"]["sha256"],
            "runtime": {key: runtime[key] for key in ("class", "name", "version")},
        },
        "row": ROW_RECORD,
        "schema": REGISTERED_SCHEMA,
        "scope": SCOPE,
        "source": source_json(sources),
        "training": {"basis": basis, "information_cutoff_ns": str(cutoff)},
        "window": window.as_json(),
    }
    return schedule_files(manifest, scheduled_rows(rows, inputs, predictions))


def reproduce_registered_schedule(
    directory: Path, sources: VerifiedSources, registry: RegistryRoots, sandbox: Sandbox | None
) -> dict[str, Any]:
    """Verifies a version 2 schedule, then rebuilds it from its verified
    sources and model version and requires identical bytes."""
    manifest = verify_schedule(directory)
    if manifest["schema"] != REGISTERED_SCHEMA:
        raise ScheduleError("not a registered model's schedule")
    if manifest["source"] != source_json(sources):
        raise ScheduleError("the schedule names different sources")
    version = verified_model_version(registry, manifest["model"]["model_version_id"])
    rebuilt = build_registered_schedule(sources, version, manifest["window"]["name"], sandbox)
    for name, contents in rebuilt.files.items():
        if (directory / name).read_bytes() != contents:
            raise ScheduleError(f"{name} differs from the schedule rebuilt from its sources and version")
    return manifest
