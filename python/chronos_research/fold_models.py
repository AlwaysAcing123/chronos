"""Walk-forward fold models: a fresh majority model and a fresh logistic
model for each fold of a plan, fitted only on that fold's training rows
(ADR 0022).

    <artifacts>/walk_forward_models/<fold_model_id>/manifest.json
    <artifacts>/walk_forward_models/<fold_model_id>/model.json

Nothing is shared between folds: each fits its own class counts, scaler, and
coefficients, with the experiment's fitting (models.fit_majority and
fit_logistic) and its parity check of the stored numbers. A model that cannot
be fitted is unevaluable with its reason, and nothing takes its place.

A fold model is historical only. It is not a registered model version, is
never deployed, and is never presented as an experiment or an import.

The manifest names the plan and fold, the training members in order with
their IDs and checksums, a digest of the selected rows, the class support,
every exclusion, the latest training prediction and label times, the
information boundary they are strictly before, the configurations, the
environment, and each model's parameters checksum. The ID is the SHA-256 of
the canonical manifest without the ID, with its newline.
"""

import hashlib
import json
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    listed_files,
    verified_file,
    verified_manifest,
)
from chronos_research.artifacts import CLASS_ORDER, VerifiedRecording, is_artifact_id
from chronos_research.encoding import canonical_json, encode_integer, sha256_hex
from chronos_research.experiment import environment, model_feature_set_json
from chronos_research.inspection import InspectionError, stored_logistic, stored_majority
from chronos_research.model_data import ModelRow, to_floats
from chronos_research.models import (
    FittedLogistic,
    LogisticConfiguration,
    LogisticModel,
    MajorityModel,
    Unevaluable,
    fit_logistic,
    fit_majority,
)
from chronos_research.publication import PublicationError, publish
from chronos_research.walk_forward import MODELS, TrainingSelection, models_configuration

SCHEMA = {"name": "chronos.walk_forward_fold_model", "version": "1"}
MODELS_SCHEMA = {"name": "chronos.walk_forward_fold_models", "version": "1"}
MANIFEST = "manifest.json"
MODEL = "model.json"
SELECTED_ROWS = (
    "SHA-256 over the canonical JSON line of each selected row, in training order: its identity, class, "
    "and raw model inputs"
)
SCOPE = (
    "historical evaluation only: fitted for one fold of one walk-forward plan; not a registered model "
    "version, and never deployed"
)

type FoldModelFit = MajorityModel | LogisticModel | Unevaluable


class FoldModelError(Exception):
    """A fold model cannot be fitted for this plan, or is not valid."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def fold_models_root(artifacts: Path) -> Path:
    return artifacts / "walk_forward_models"


@dataclass(frozen=True)
class FoldModel:
    fold_model_id: str
    manifest: Mapping[str, Any]
    models: Mapping[str, FoldModelFit]
    files: Mapping[str, bytes]


def _selected_rows_digest(rows: Sequence[ModelRow]) -> str:
    digest = hashlib.sha256()
    for row in rows:
        digest.update(
            canonical_json(
                {
                    "class": CLASS_ORDER[row.class_index],
                    "identity": {
                        "ingest_sequence": encode_integer(row.identity.ingest_sequence),
                        "instrument": row.identity.instrument,
                        "stream": row.identity.stream,
                    },
                    "inputs_raw": [encode_integer(value) for value in row.inputs],
                }
            )
        )
    return digest.hexdigest()


def fit_fold(rows: Sequence[ModelRow]) -> dict[str, FoldModelFit]:
    """Both baselines, fitted on these rows alone."""
    if not rows:
        empty = Unevaluable(reason="no_training_rows", detail={})
        return {"logistic": empty, "majority": empty}
    classes = np.array([row.class_index for row in rows], dtype=np.int64)
    fitted = fit_logistic(to_floats([row.inputs for row in rows]), classes, LogisticConfiguration())
    return {
        "logistic": fitted.model if isinstance(fitted, FittedLogistic) else fitted,
        "majority": fit_majority(classes),
    }


def _summary(model: FoldModelFit) -> dict[str, str]:
    if isinstance(model, Unevaluable):
        return {"reason": model.reason, "status": "unevaluable"}
    return {"parameters_sha256": sha256_hex(canonical_json(model.as_json())), "status": "fitted"}


# What a fold model's training member and its plan's must agree on.
_PLANNED = ("dataset_id", "eligible_rows", "excluded", "feature_rows", "label_artifact_id", "stream")


def _member(recording: VerifiedRecording, selection: TrainingSelection) -> dict[str, Any]:
    return {
        "dataset_id": recording.dataset_id,
        "eligible_rows": encode_integer(len(selection.rows)),
        "excluded": {reason: encode_integer(count) for reason, count in selection.excluded.items()},
        "feature_artifact_id": recording.features.artifact_id,
        "feature_rows": encode_integer(len(selection.rows) + sum(selection.excluded.values())),
        "features_sha256": recording.features.rows_sha256,
        "label_artifact_id": recording.labels.artifact_id,
        "labels_sha256": recording.labels.rows_sha256,
        "stream": recording.stream,
    }


def _latest(values: Sequence[int | None]) -> str | None:
    present = [value for value in values if value is not None]
    return encode_integer(max(present)) if present else None


def build_fold_model(
    plan: Mapping[str, Any],
    fold: Mapping[str, Any],
    training: Sequence[tuple[VerifiedRecording, TrainingSelection]],
) -> FoldModel:
    """The fold's models, fitted on the rows its training members contribute,
    after checking those rows are exactly the ones its plan counted."""
    if plan["models"] != models_configuration():
        raise FoldModelError(
            "environment_differs", "the plan's model configuration is not this environment's; refit there"
        )
    members = []
    for (recording, selection), claimed in zip(training, fold["training"]["members"], strict=True):
        member = _member(recording, selection)
        if any(member[key] != claimed[key] for key in _PLANNED):
            raise FoldModelError(
                "source_changed", f"training member {recording.dataset_id} no longer gives the planned rows"
            )
        members.append(member)
    rows = [row for _, selection in training for row in selection.rows]
    models = fit_fold(rows)
    model_file = canonical_json(
        {
            "class_order": list(CLASS_ORDER),
            "model_feature_set": model_feature_set_json(),
            "models": {name: models[name].as_json() for name in MODELS},
            "schema": MODELS_SCHEMA,
        }
    )
    classes = np.bincount(
        np.array([row.class_index for row in rows], dtype=np.int64), minlength=len(CLASS_ORDER)
    )
    manifest = {
        "class_order": list(CLASS_ORDER),
        "collection_id": plan["collection"]["collection_id"],
        "configuration": plan["models"],
        "environment": environment(),
        "files": {
            "model": {
                "name": MODEL,
                "sha256": sha256_hex(model_file),
                "size_bytes": encode_integer(len(model_file)),
            }
        },
        "fold": fold["index"],
        "instrument": plan["instrument"],
        "model_feature_set": model_feature_set_json(),
        "models": {name: _summary(models[name]) for name in MODELS},
        "plan_id": plan["plan_id"],
        "schema": SCHEMA,
        "scope": SCOPE,
        "training": {
            "class_support": {
                name: encode_integer(int(count)) for name, count in zip(CLASS_ORDER, classes, strict=True)
            },
            "information_boundary_ns": fold["information_boundary_ns"],
            "max_label_available_at_ns": _latest(
                [selection.max_label_available_at_ns for _, selection in training]
            ),
            "max_prediction_at_ns": _latest([selection.max_prediction_at_ns for _, selection in training]),
            "members": members,
            "rows": encode_integer(len(rows)),
            "selected_rows": SELECTED_ROWS,
            "selected_rows_sha256": _selected_rows_digest(rows),
        },
    }
    fold_model_id = sha256_hex(canonical_json(manifest))
    complete = {**manifest, "fold_model_id": fold_model_id}
    return FoldModel(
        fold_model_id=fold_model_id,
        manifest=complete,
        models=models,
        files={MANIFEST: canonical_json(complete), MODEL: model_file},
    )


def verified_fold_model(root: Path, fold_model_id: str) -> FoldModel:
    """A fold model from bytes read once: its files, identity, checksums,
    and parameters, each model parsed with the experiment's own checks and
    matching the checksum its manifest names."""
    try:
        directory = artifact_directory(root, fold_model_id)
        if listed_files(directory) != sorted([MANIFEST, MODEL]):
            raise FoldModelError("artifact_invalid", f"a fold model holds exactly {MANIFEST} and {MODEL}")
        manifest = verified_manifest(directory, "fold_model_id", with_newline=True)
        if manifest.get("schema") != SCHEMA:
            raise FoldModelError("artifact_invalid", f"{fold_model_id} is not {SCHEMA}")
        model_file = verified_file(directory, manifest["files"]["model"])
    except ArtifactUnavailableError as error:
        raise FoldModelError("artifact_invalid", str(error)) from error
    try:
        stored = json.loads(model_file)
        if canonical_json(stored) != model_file or stored.get("schema") != MODELS_SCHEMA:
            raise FoldModelError("artifact_invalid", "model.json is not a canonical fold models document")
        if manifest["configuration"] != models_configuration():
            raise FoldModelError(
                "environment_differs", "the fold model's configuration is not this environment's"
            )
        models: dict[str, FoldModelFit] = {
            "logistic": stored_logistic(
                stored["models"]["logistic"], "the logistic model", LogisticConfiguration()
            ),
            "majority": stored_majority(stored["models"]["majority"], "the majority model"),
        }
    except (InspectionError, KeyError, TypeError, ValueError) as error:
        raise FoldModelError("artifact_invalid", f"model.json: {error}") from error
    if {name: _summary(model) for name, model in models.items()} != manifest["models"]:
        raise FoldModelError("artifact_invalid", "the stored models do not match the manifest's checksums")
    return FoldModel(
        fold_model_id=fold_model_id,
        manifest=manifest,
        models=models,
        files={MANIFEST: canonical_json(manifest), MODEL: model_file},
    )


def publish_fold_model(artifacts: Path, model: FoldModel) -> str:
    root = fold_models_root(artifacts)
    root.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        return bytes(verified_fold_model(root, directory.name).files[MANIFEST])

    try:
        outcome, _ = publish(root, model.fold_model_id, model.files, MANIFEST, existing)
    except PublicationError as error:
        raise FoldModelError("publication_failed", str(error)) from error
    return outcome.value


def fold_model_ids(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    return sorted(name for name in (entry.name for entry in root.iterdir()) if is_artifact_id(name))
