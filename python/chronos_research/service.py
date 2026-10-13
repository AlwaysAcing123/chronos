"""The local research service: `python -m chronos_research serve`.

A separate loopback-only process beside the C++ paper-trading server, so the
trading server never loads models and research never touches trading
accounts, the replay, or a live source. It uses only the standard library.

    GET  /api/v1/research/status
    GET  /api/v1/research/experiments                 ?limit&cursor
    GET  /api/v1/research/experiments/{experiment_id}
    POST /api/v1/research/runs                        start an evaluation
    GET  /api/v1/research/runs                        ?limit&cursor
    GET  /api/v1/research/runs/{job_id}
    GET  /api/v1/research/results                     ?limit&cursor
    GET  /api/v1/research/results/{run_id}
    GET  /api/v1/research/results/{run_id}/decisions  ?limit&cursor
    GET  /api/v1/research/results/{run_id}/trades     ?account&limit&cursor
    GET  /api/v1/research/results/{run_id}/equity     ?limit&cursor
    GET  /api/v1/research/models                      ?limit&cursor&state
    POST /api/v1/research/models                      register an experiment model
    GET  /api/v1/research/models/{model_version_id}
    POST /api/v1/research/models/{model_version_id}/lifecycle
    POST /api/v1/research/imports                     stage an ONNX import's manifest
    GET  /api/v1/research/imports/{import_id}
    PUT  /api/v1/research/imports/{import_id}/model   its model bytes
    POST /api/v1/research/imports/{import_id}/validation  validate and import
    GET  /api/v1/research/comparisons                 ?limit&cursor
    POST /api/v1/research/comparisons                 start a comparison
    GET  /api/v1/research/comparisons/{comparison_id}
    GET  /api/v1/research/collections                 ?limit&cursor
    GET  /api/v1/research/collections/{collection_id}
    GET  /api/v1/research/walk-forward/plans          ?limit&cursor
    POST /api/v1/research/walk-forward/plans          preview: build and publish a plan
    GET  /api/v1/research/walk-forward/plans/{plan_id}
    GET  /api/v1/research/walk-forward/studies        ?limit&cursor
    POST /api/v1/research/walk-forward/studies        start a reviewed plan's study
    GET  /api/v1/research/walk-forward/studies/{study_id}
    GET  /api/v1/research/strategy-diagnostics        ?limit&cursor&study
    POST /api/v1/research/strategy-diagnostics        explain studies' runs
    GET  /api/v1/research/strategy-diagnostics/{diagnostic_id}
    GET  /api/v1/research/strategy-diagnostics/{diagnostic_id}/fills     ?run&account&limit&cursor
    GET  /api/v1/research/strategy-diagnostics/{diagnostic_id}/episodes  ?run&account&limit&cursor
    GET  /api/v1/research/jobs                        ?limit&cursor&kind
    GET  /api/v1/research/jobs/{job_id}

The conventions are the paper-trading API's (docs/paper-trading-api.md):
strict JSON bodies of at most 16 KiB, decimals and integers as strings, RFC
3339 nanosecond timestamps, a fixed error shape, strict query strings, and
opaque cursors with limits from 1 to 200. The one other body is an import's
model, application/octet-stream of at most 1 MiB, on its own route.

Work never runs on a request thread. A POST validates the request and queues
a job; a bounded pool of workers runs each job as one fixed child command,
`python -m chronos_research evaluate-strategy` with an explicit argument
vector, under a time limit. Every job record is written atomically to the
state directory, so finished jobs survive a restart, and a job that was
queued or running when the service stopped is marked interrupted on the next
start, never resumed silently. An identical request while an equal job is
queued, running, or succeeded returns that job instead of new work. A
walk-forward study's child reports its stage and fold on stderr; the latest
report is the job's bounded `progress`, and the rest of stderr its log.

Artifacts are named only by validated IDs, never by client paths. Every
artifact this service serves is read as bytes and verified before use:
the manifest's identity against its directory name and each consumed file
against the manifest's checksum, through no symlink.
"""

import base64
import binascii
import bisect
import contextlib
import hashlib
import json
import os
import queue
import re
import secrets
import selectors
import signal
import subprocess
import sys
import threading
import time
from collections import OrderedDict
from collections.abc import Callable, Iterator, Mapping
from dataclasses import dataclass, field
from datetime import UTC, datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit

from chronos_research import collection as collections
from chronos_research import (
    comparison,
    diagnostic_sources,
    model_library,
    onnx_profile,
    strategy_diagnostics,
    walk_forward,
    walk_forward_study,
)
from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    canonical_bytes,
    verified_file,
    verified_manifest,
)
from chronos_research.artifacts import CLASS_ORDER, is_artifact_id
from chronos_research.encoding import StrictJsonError, strict_json_object
from chronos_research.import_staging import ImportStore, StagedImport, StagingError
from chronos_research.model_import import ModelImportError, validated_import_manifest
from chronos_research.onnx_sandbox import (
    Sandbox,
    SandboxCleanupError,
    SandboxUnavailableError,
    new_supervisor,
)
from chronos_research.pipeline import ArtifactRoots, Tools
from chronos_research.registry import (
    EXPERIMENT_FAMILIES,
    LIFECYCLE_STATES,
    MODEL_NAME,
    MODEL_VERSION,
    ModelVersion,
    RegistryError,
    RegistryRoots,
    model_version_ids,
    names_by_version,
    set_lifecycle,
    verified_model_version,
)
from chronos_research.strategy_attribution import DiagnosticsError
from chronos_research.strategy_configuration import (
    ConfigurationError,
    canonical_configuration,
    canonical_decimal,
    canonical_integer,
)

API = "/api/v1/research"
VERSION = "1"
MAX_BODY_BYTES = 16 * 1024
DEFAULT_LIMIT, MAX_LIMIT = 50, 200
MAX_WORKER_OUTPUT_BYTES = 1024 * 1024
LOG_BYTES = 4096
MODELS = ("logistic", "majority")
WINDOWS = ("validation", "test")
ACCOUNTS = ("strategy", "buy_and_hold", "cash")
TERMINAL = ("succeeded", "failed", "interrupted")
# Jobs whose child may start ONNX sandbox containers, under a token of the job's own.
SANDBOX_JOB_KINDS = ("comparison", "import")
# How long a job's slot waits for Docker to remove its containers, and how
# long after a killed child it looks again for a create still under way.
RELEASE_TIMEOUT_S = 10.0
RELEASE_SETTLE_S = 1.0
EXPERIMENT_RUN_SCHEMA = {"name": "chronos.strategy_run", "version": "1"}
# A registered model version's run (ADR 0021): readable here, and listed by
# the comparison it belongs to rather than among evaluation results.
REGISTERED_RUN_SCHEMA = {"name": "chronos.strategy_run", "version": "2"}
# A walk-forward fold model's run (ADR 0022): readable here, and listed by
# the study it belongs to.
FOLD_RUN_SCHEMA = {"name": "chronos.strategy_run", "version": "3"}
JOB_KINDS = (
    "evaluation",
    "registration",
    "import",
    "comparison",
    "walk_forward_plan",
    "walk_forward",
    "strategy_diagnostics",
)
# Jobs whose child reports progress, as {"progress": {...}} lines on stderr.
PROGRESS_JOB_KINDS = ("walk_forward", "strategy_diagnostics")
_PROGRESS_KEYS = ("fold", "folds", "model", "scenario", "stage", "step")
_PROGRESS_VALUE = re.compile(r"[a-z0-9_]{1,32}")
LOOPBACK = ("127.0.0.1", "::1", "localhost")
_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
_UNSIGNED = re.compile(r"0|[1-9][0-9]*")
_JOB_ID = re.compile(r"[0-9a-f]{32}")
_DURATIONS = ("latency_ns", "interval_ns", "age_ns", "horizon_ns", "duration_ns")

# What the UI shows next to every model and result.
LIMITATIONS = (
    "Each dataset is one short recorded session: its validation and test windows cover minutes, not market "
    "regimes.",
    "Prediction metrics measure classification of one-second midpoint moves, not trading profit.",
    "Strategy runs are paper simulations against recorded top-of-book quotes: one share at a time, the "
    "displayed best price, no depth, queue position, or market impact, and an assumed fee per fill.",
    "No result here is evidence that a model or policy would be profitable.",
)

# The configuration fields of a run, each with its validator.
CONFIGURATION_FIELDS = (
    "threshold_ppb", "target_quantity", "latency_ns", "decision_interval_ns", "starting_cash", "fee_per_fill",
    "maximum_quote_age_ns", "maximum_order_notional",
)  # fmt: skip
DEFAULT_CONFIGURATION = {
    "threshold_ppb": "500000000",
    "target_quantity": "1",
    "latency_ns": "1000000",
    "decision_interval_ns": "1000000000",
    "starting_cash": "10000",
    "fee_per_fill": "0.01",
    "maximum_quote_age_ns": "5000000000",
    "maximum_order_notional": "1000000",
}


# What a read of strategy diagnostics says about each level of verification
# (ADR 0024); `sources.now` is what the read finds (diagnostic_sources).
DIAGNOSTICS_VERIFICATION: dict[str, Any] = {
    "artifact": {
        "detail": "the manifest's identity and every file's checksum and row count, read now",
        "state": "verified",
    },
    "engine_replay": {
        "detail": (
            "each run's replay by chronos_strategy inspect was recorded by its study when the study ran; "
            "diagnostics never replay or refit"
        ),
        "state": "recorded_by_study",
    },
    "independent_audit": {
        "command": "python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> <diagnostic_id>",
        "detail": "a standard-library recomputation of every figure from the raw files, run from the CLI",
        "state": "not_run_by_this_read",
    },
    "recomputed_inspection": {
        "command": (
            "python -m chronos_research inspect-strategy-diagnostics --tools <tools> "
            "--artifacts <artifacts> --diagnostics <diagnostic_id>"
        ),
        "detail": "rebuilds the artifact from its sources, verified again, and requires identical bytes",
        "state": "not_run_by_this_read",
    },
    "sources": {},
}


class ApiError(Exception):
    def __init__(
        self, status: HTTPStatus, code: str, message: str, field: str | None = None, reason: str | None = None
    ) -> None:
        super().__init__(message)
        self.status, self.code, self.message, self.field, self.reason = status, code, message, field, reason

    def body(self) -> dict[str, Any]:
        return {
            "error": {"code": self.code, "field": self.field, "message": self.message, "reason": self.reason}
        }


def malformed(reason: str, message: str, field: str | None = None) -> ApiError:
    return ApiError(HTTPStatus.BAD_REQUEST, "malformed_request", message, field, reason)


def rfc3339(nanoseconds: int) -> str:
    seconds, fraction = divmod(nanoseconds, 1_000_000_000)
    return datetime.fromtimestamp(seconds, UTC).strftime("%Y-%m-%dT%H:%M:%S") + f".{fraction:09d}Z"


def now() -> str:
    return rfc3339(int(datetime.now(UTC).timestamp() * 1_000_000_000))


def with_timestamps(document: Any) -> Any:
    """Every `*_ns` timestamp as an RFC 3339 `*` field; durations stay
    integer nanoseconds."""
    if isinstance(document, list):
        return [with_timestamps(item) for item in document]
    if not isinstance(document, dict):
        return document
    converted: dict[str, Any] = {}
    for key, value in document.items():
        if key.endswith("_ns") and not key.endswith(_DURATIONS):
            converted[key.removesuffix("_ns") or "at"] = None if value is None else rfc3339(int(value))
        else:
            converted[key] = with_timestamps(value)
    return converted


# --- Verified reads -----------------------------------------------------------------------------------
# The helpers live in artifact_files, shared with the model registry.


class Artifacts:
    """Verified, read-only access to the artifact roots."""

    def __init__(self, roots: ArtifactRoots) -> None:
        self.roots = roots
        self._journals: OrderedDict[str, list[dict[str, Any]]] = OrderedDict()
        self._lock = threading.Lock()

    def _ids(self, root: Path) -> list[str]:
        if not root.is_dir():
            return []
        return sorted(name for name in os.listdir(root) if is_artifact_id(name))

    # Experiments, with their dataset and split.
    def experiment_ids(self) -> list[str]:
        return self._ids(self.roots.experiments)

    def experiment(self, experiment_id: str) -> dict[str, Any]:
        directory = artifact_directory(self.roots.experiments, experiment_id)
        manifest = verified_manifest(directory, "experiment_id", with_newline=True)
        files = manifest["files"]
        model = json.loads(verified_file(directory, files["model.json"]))
        metrics = json.loads(verified_file(directory, files["metrics.json"]))
        sources = manifest["sources"]
        dataset = verified_manifest(
            artifact_directory(self.roots.catalog, sources["dataset_id"]), "dataset_id", with_newline=False
        )
        split = verified_manifest(
            artifact_directory(self.roots.splits, sources["split_artifact_id"]),
            "artifact_id",
            with_newline=False,
        )
        receive = dataset["time_range"]["receive_timestamp_ns"]
        validation_start, test_start = (
            int(sources["validation_start_at_ns"]),
            int(sources["test_start_at_ns"]),
        )
        models = []
        for instrument, entry in sorted(model["instruments"].items()):
            training = metrics["training"].get(instrument, {"rows": "0", "true_distribution": {}})
            evaluated = {}
            for window in WINDOWS:
                part = metrics["splits"][window]["instruments"].get(instrument, {})
                evaluated[window] = {name: _prediction_metrics(part.get(name)) for name in MODELS}
            models.append(
                {
                    "instrument": instrument,
                    "models": {name: _model_status(entry[name], name) for name in MODELS},
                    "prediction_metrics": evaluated,
                    "training": {
                        "rows": training["rows"],
                        "true_distribution": training["true_distribution"],
                    },
                }
            )
        return {
            "dataset": {
                "dataset_id": dataset["dataset_id"],
                "events": dataset["counts"]["events"],
                "instruments": dataset["instruments"],
                "provider": dataset["source"]["provider"],
                "provider_dataset": dataset["source"]["provider_dataset"],
                "receive_time": {
                    "earliest": rfc3339(int(receive["earliest"])),
                    "latest": rfc3339(int(receive["latest"])),
                },
                "stream": dataset["stream"],
            },
            "experiment_id": manifest["experiment_id"],
            "horizon_ns": sources["horizon_ns"],
            "instruments": models,
            "limitations": list(LIMITATIONS),
            "scope": manifest["scope"],
            "split_artifact_id": sources["split_artifact_id"],
            "verified": (
                "the experiment's, dataset's, and split's manifest identities and the model and metrics "
                "checksums; every evaluation verifies and reproduces the whole ancestry again"
            ),
            "windows": {
                "training": {
                    "end_at": rfc3339(validation_start),
                    "labels_available_through": rfc3339(
                        int(split["usable"]["train"]["max_label_available_at_ns"])
                    ),
                    "start_at": rfc3339(int(receive["earliest"])),
                },
                "validation": {"end_at": rfc3339(test_start), "start_at": rfc3339(validation_start)},
                "test": {"end_at": None, "start_at": rfc3339(test_start)},
            },
        }

    # Strategy runs.
    def result_ids(self) -> list[str]:
        return self._ids(self.roots.strategies)

    def result_manifest(self, run_id: str) -> dict[str, Any]:
        directory = artifact_directory(self.roots.strategies, run_id)
        manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        if manifest.get("schema") not in (EXPERIMENT_RUN_SCHEMA, REGISTERED_RUN_SCHEMA, FOLD_RUN_SCHEMA):
            raise ArtifactUnavailableError("not a strategy run of a supported version")
        return manifest

    def journal(self, run_id: str) -> list[dict[str, Any]]:
        with self._lock:
            cached = self._journals.get(run_id)
            if cached is not None:
                self._journals.move_to_end(run_id)
                return cached
        manifest = self.result_manifest(run_id)
        data = verified_file(artifact_directory(self.roots.strategies, run_id), manifest["files"]["journal"])
        entries = [json.loads(line) for line in data.splitlines()]
        if len(entries) != int(manifest["journal"]["entries"]):
            raise ArtifactUnavailableError("the journal's entry count differs from its manifest")
        with self._lock:
            self._journals[run_id] = entries
            while len(self._journals) > 8:
                self._journals.popitem(last=False)
        return entries


def _model_status(entry: Mapping[str, Any], name: str) -> dict[str, Any]:
    if name == "majority":
        if "majority_class" not in entry:
            return {"reason": entry.get("reason"), "status": "unevaluable"}
        return {"majority_class": entry["majority_class"], "status": "fitted"}
    if entry["status"] == "fitted":
        return {"iterations": entry["iterations"], "status": "fitted"}
    return {"reason": entry["reason"], "status": "unevaluable"}


def _prediction_metrics(entry: Mapping[str, Any] | None) -> dict[str, Any] | None:
    """Accuracy and balanced accuracy, or why there are none."""
    if entry is None:
        return None
    if "unevaluable" in entry:
        return {"unevaluable": entry["unevaluable"]}
    return {
        "accuracy": entry["accuracy"],
        "balanced_accuracy": entry["balanced_accuracy"],
        "rows": entry["rows"],
    }


def result_summary(manifest: Mapping[str, Any]) -> dict[str, Any]:
    results = manifest["results"]
    summary: dict[str, Any] = with_timestamps(
        {
            "accounts": results["accounts"],
            "assumptions": manifest["assumptions"],
            "configuration": manifest["configuration"],
            "decisions": results["decisions"],
            "definitions": manifest["definitions"],
            "environment": manifest["environment"],
            "events": {**results["events"], **manifest["events"]},
            "journal_entries": manifest["journal"]["entries"],
            "limitations": list(LIMITATIONS),
            "run_id": manifest["artifact_id"],
            "runner": manifest["runner"],
            "scope": manifest["scope"],
            "source": manifest["source"],
            "window": {
                **manifest["window"],
                "first_event_at_ns": results["first_window_event_at_ns"],
                "valued_at_ns": results["valued_at_ns"],
            },
        }
    )
    return summary


def result_listing(manifest: Mapping[str, Any]) -> dict[str, Any]:
    strategy = manifest["results"]["accounts"]["strategy"]["final"]["valuation"]
    listing: dict[str, Any] = with_timestamps(
        {
            "instrument": manifest["source"]["instrument"],
            "model": manifest["source"]["model"],
            "net_total_pnl": None if "unavailable" in strategy else strategy["net_total_pnl"],
            "run_id": manifest["artifact_id"],
            "experiment_id": manifest["source"]["experiment_id"],
            "valuation": "unavailable" if "unavailable" in strategy else "complete",
            "window": manifest["window"],
        }
    )
    return listing


# --- Requests ------------------------------------------------------------------------------------------


def parse_body(data: bytes) -> dict[str, Any]:
    try:
        return strict_json_object(data)
    except StrictJsonError as error:
        raise malformed(error.reason, str(error), error.field) from error


def _exact_keys(document: Mapping[str, Any], keys: tuple[str, ...], prefix: str = "") -> None:
    for key in document:
        if key not in keys:
            raise malformed(
                "unexpected_field", f"{prefix}{key} is not a field of this request", f"{prefix}{key}"
            )
    for key in keys:
        if key not in document:
            raise malformed("missing_field", f"{prefix}{key} is required", f"{prefix}{key}")
        if not isinstance(document[key], (str, dict)) or document[key] is None:
            raise malformed("invalid_type", f"{prefix}{key} has the wrong type", f"{prefix}{key}")


def _string(document: Mapping[str, Any], key: str, prefix: str = "") -> str:
    value = document[key]
    if not isinstance(value, str):
        raise malformed("invalid_type", f"{prefix}{key} must be a string", f"{prefix}{key}")
    return value


def _configuration_error(error: ConfigurationError) -> ApiError:
    if error.domain:
        return ApiError(
            HTTPStatus.UNPROCESSABLE_ENTITY, "invalid_configuration", error.message, error.field, error.reason
        )
    return malformed(error.reason, error.message, error.field)


def _decimal(text: str, field_name: str, scale: int, positive: bool, nonnegative: bool) -> str:
    try:
        return canonical_decimal(text, field_name, scale, positive, nonnegative)
    except ConfigurationError as error:
        raise _configuration_error(error) from error


def _integer(text: str, field_name: str, minimum: int, maximum: int) -> str:
    try:
        return canonical_integer(text, field_name, minimum, maximum)
    except ConfigurationError as error:
        raise _configuration_error(error) from error


def validate_run_request(document: Mapping[str, Any]) -> dict[str, Any]:
    """The request in canonical form, or why it is not one."""
    _exact_keys(document, ("configuration", "experiment_id", "instrument", "model", "window"))
    experiment_id = _string(document, "experiment_id")
    if not is_artifact_id(experiment_id):
        raise malformed("invalid_identifier", "experiment_id is a 64-digit lowercase ID", "experiment_id")
    instrument = _string(document, "instrument")
    if not _IDENTIFIER.fullmatch(instrument):
        raise malformed("invalid_identifier", "instrument is not a valid identifier", "instrument")
    model, window = _string(document, "model"), _string(document, "window")
    if model not in MODELS:
        raise malformed("unknown_enum_value", f"model is one of {', '.join(MODELS)}", "model")
    if window not in WINDOWS:
        raise malformed("unknown_enum_value", f"window is one of {', '.join(WINDOWS)}", "window")
    return {
        "configuration": validate_configuration(document["configuration"]),
        "experiment_id": experiment_id,
        "instrument": instrument,
        "model": model,
        "window": window,
    }


def validate_configuration(configuration: Any) -> dict[str, str]:
    """A strategy configuration in canonical form, or why it is not one."""
    if not isinstance(configuration, dict):
        raise malformed("invalid_type", "configuration must be an object", "configuration")
    _exact_keys(configuration, CONFIGURATION_FIELDS, "configuration.")
    values = {key: _string(configuration, key, "configuration.") for key in CONFIGURATION_FIELDS}
    try:
        return canonical_configuration(values, "configuration.")
    except ConfigurationError as error:
        raise _configuration_error(error) from error


def validate_registration_request(document: Mapping[str, Any]) -> dict[str, Any]:
    """A registration request in canonical form, or why it is not one."""
    _exact_keys(document, ("experiment_id", "instrument", "model", "name", "version"))
    request = {
        key: _string(document, key) for key in ("experiment_id", "instrument", "model", "name", "version")
    }
    if not is_artifact_id(request["experiment_id"]):
        raise malformed("invalid_identifier", "experiment_id is a 64-digit lowercase ID", "experiment_id")
    if not _IDENTIFIER.fullmatch(request["instrument"]):
        raise malformed("invalid_identifier", "instrument is not a valid identifier", "instrument")
    if request["model"] not in EXPERIMENT_FAMILIES:
        raise malformed("unknown_enum_value", f"model is one of {', '.join(EXPERIMENT_FAMILIES)}", "model")
    if not MODEL_NAME.fullmatch(request["name"]):
        raise malformed(
            "invalid_identifier",
            "name is 1 to 64 characters from [a-z0-9._-], starting with one of [a-z0-9]",
            "name",
        )
    if not MODEL_VERSION.fullmatch(request["version"]):
        raise malformed(
            "invalid_identifier", "version is one to three dot-separated whole numbers", "version"
        )
    return request


def validate_walk_forward_request(document: Mapping[str, Any]) -> dict[str, Any]:
    """A walk-forward preview request as its canonical
    chronos.walk_forward_request/1: fields and values checked here, then
    the plan's own rules (base and stress scenarios, engine bounds)."""
    keys = ("collection_id", "configuration", "instrument", "minimum_training_sources", "scenarios")
    for key in document:
        if key not in keys:
            raise malformed("unexpected_field", f"{key} is not a field of this request", key)
    for key in keys:
        if key not in document:
            raise malformed("missing_field", f"{key} is required", key)
    collection_id, instrument, minimum = (
        _string(document, key) for key in ("collection_id", "instrument", "minimum_training_sources")
    )
    if not is_artifact_id(collection_id):
        raise malformed("invalid_identifier", "collection_id is a 64-digit lowercase ID", "collection_id")
    if not _IDENTIFIER.fullmatch(instrument):
        raise malformed("invalid_identifier", "instrument is not a valid identifier", "instrument")
    minimum = _integer(minimum, "minimum_training_sources", 1, collections.MAX_SOURCES - 1)
    scenarios = document["scenarios"]
    if not isinstance(scenarios, list) or not all(isinstance(cell, dict) for cell in scenarios):
        raise malformed("invalid_type", "scenarios is a list of objects", "scenarios")
    if not 1 <= len(scenarios) <= walk_forward.MAX_SCENARIOS:
        raise ApiError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "invalid_configuration",
            f"a study has 1 to {walk_forward.MAX_SCENARIOS} scenarios",
            "scenarios",
            "out_of_domain",
        )
    cells = []
    for position, cell in enumerate(scenarios):
        prefix = f"scenarios[{position}]."
        _exact_keys(cell, ("fee_per_fill", "latency_ns", "name"), prefix)
        cells.append(
            {
                "fee_per_fill": _decimal(
                    _string(cell, "fee_per_fill", prefix), prefix + "fee_per_fill", 8, False, True
                ),
                "latency_ns": _integer(
                    _string(cell, "latency_ns", prefix), prefix + "latency_ns", 1, 10**18 - 1
                ),
                "name": _string(cell, "name", prefix),
            }
        )
    request = {
        "collection_id": collection_id,
        "configuration": validate_configuration(document["configuration"]),
        "instrument": instrument,
        "minimum_training_sources": minimum,
        "scenarios": cells,
        "schema": walk_forward.REQUEST_SCHEMA,
    }
    try:
        return walk_forward.validated_request(request)
    except walk_forward.PlanError as error:
        raise ApiError(
            HTTPStatus.UNPROCESSABLE_ENTITY, "invalid_configuration", str(error), None, error.reason
        ) from error


def collection_summary(manifest: Mapping[str, Any]) -> dict[str, Any]:
    """What the workflow shows of a verified collection: each member's
    recording, coverage, and declared session, and how many members hold
    each instrument."""

    def member(entry: Mapping[str, Any]) -> dict[str, Any]:
        receive = entry["dataset"]["receive_time_ns"]
        provenance = entry["provenance"]
        return {
            "coverage": entry["coverage"],
            "dataset_id": entry["dataset"]["dataset_id"],
            "events": entry["dataset"]["events"],
            "feature_artifact_id": entry["features"]["artifact_id"],
            "instruments": with_timestamps(entry["instruments"]),
            "label_artifact_id": entry["labels"]["artifact_id"],
            "provenance": {
                key: provenance[key] for key in ("description", "evidence", "provider", "provider_dataset")
            },
            "receive_time": {
                "earliest": rfc3339(int(receive["earliest"])),
                "latest": rfc3339(int(receive["latest"])),
            },
            "session": entry["session"],
            "stream": entry["dataset"]["stream"],
        }

    return {
        "collection_id": manifest["collection_id"],
        "contract": {key: manifest["contract"][key] for key in ("feature_set", "horizon_ns", "label_set")},
        "definitions": manifest["definitions"],
        "evidence": manifest["evidence"],
        "instruments": manifest["counts"]["instruments"],
        "members": [member(entry) for entry in manifest["members"]],
        "verification": manifest["verification"],
    }


def plan_listing(manifest: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "collection_id": manifest["collection"]["collection_id"],
        "evidence": manifest["collection"]["evidence"],
        "folds": str(len(manifest["folds"])),
        "instrument": manifest["instrument"],
        "plan_id": manifest["plan_id"],
        "scenarios": manifest["scenarios"],
    }


def validate_comparison_request(document: Mapping[str, Any]) -> dict[str, Any]:
    """A comparison request as its canonical chronos.model_comparison_spec/1."""
    keys = ("configuration", "instrument", "model_version_ids", "split_artifact_id", "window")
    for key in document:
        if key not in keys:
            raise malformed("unexpected_field", f"{key} is not a field of this request", key)
    for key in keys:
        if key not in document:
            raise malformed("missing_field", f"{key} is required", key)
    models = document["model_version_ids"]
    if not isinstance(models, list) or not all(isinstance(item, str) for item in models):
        raise malformed("invalid_type", "model_version_ids is a list of IDs", "model_version_ids")
    if not all(is_artifact_id(item) for item in models):
        raise malformed(
            "invalid_identifier", "a model version ID is 64 lowercase hexadecimal digits", "model_version_ids"
        )
    if not comparison.MIN_MODELS <= len(models) <= comparison.MAX_MODELS or len(set(models)) != len(models):
        raise ApiError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "invalid_configuration",
            f"a comparison names {comparison.MIN_MODELS} to {comparison.MAX_MODELS} distinct model versions",
            "model_version_ids",
            "out_of_domain",
        )
    split, instrument, window = (
        _string(document, key) for key in ("split_artifact_id", "instrument", "window")
    )
    if not is_artifact_id(split):
        raise malformed(
            "invalid_identifier", "split_artifact_id is a 64-digit lowercase ID", "split_artifact_id"
        )
    if not _IDENTIFIER.fullmatch(instrument):
        raise malformed("invalid_identifier", "instrument is not a valid identifier", "instrument")
    if window not in WINDOWS:
        raise malformed("unknown_enum_value", f"window is one of {', '.join(WINDOWS)}", "window")
    return {
        "configuration": validate_configuration(document["configuration"]),
        "instrument": instrument,
        "model_version_ids": list(models),
        "schema": comparison.SPEC_SCHEMA,
        "split_artifact_id": split,
        "window": window,
    }


@dataclass(frozen=True)
class Page:
    items: list[Any]
    next_cursor: str | None


def encode_cursor(*parts: str) -> str:
    return base64.urlsafe_b64encode("|".join(("v1", *parts)).encode()).decode().rstrip("=")


def decode_cursor(cursor: str, *scope: str) -> int:
    """The position a cursor of this list and scope continues from."""
    try:
        text = base64.urlsafe_b64decode(cursor + "=" * (-len(cursor) % 4)).decode()
    except (binascii.Error, UnicodeDecodeError, ValueError) as error:
        raise malformed("invalid_cursor", "the cursor is not one this list issued", "cursor") from error
    parts = text.split("|")
    if (
        parts[: len(scope) + 1] != ["v1", *scope]
        or len(parts) != len(scope) + 2
        or not _UNSIGNED.fullmatch(parts[-1])
    ):
        raise malformed("invalid_cursor", "the cursor is not one this list issued", "cursor")
    if encode_cursor(*scope, parts[-1]) != cursor:
        raise malformed("invalid_cursor", "the cursor is not one this list issued", "cursor")
    return int(parts[-1])


def paginate(items: list[Any], limit: int, cursor: str | None, *scope: str) -> Page:
    """An offset page, for lists that never change: one result's
    decisions, trades, and equity points."""
    start = 0 if cursor is None else decode_cursor(cursor, *scope)
    if start > len(items):
        raise malformed("invalid_cursor", "the cursor is past the end of this list", "cursor")
    page = items[start : start + limit]
    end = start + len(page)
    return Page(page, encode_cursor(*scope, str(end)) if end < len(items) else None)


# Lists that grow while a client pages them (jobs, experiments, results) use
# keyset cursors instead: a cursor names the last item returned, never a
# position, so an item added between two pages cannot shift the next page
# and make it repeat or skip an item. Items added after the first page
# appear when the list is read again from the start.
_KEYSET_VERSION = "v2"


def _encode_keyset(name: str, key: str) -> str:
    return base64.urlsafe_b64encode(f"{_KEYSET_VERSION}|{name}|{key}".encode()).decode().rstrip("=")


def _decode_keyset(cursor: str, name: str, key: re.Pattern[str]) -> str:
    try:
        text = base64.urlsafe_b64decode(cursor + "=" * (-len(cursor) % 4)).decode()
    except (binascii.Error, UnicodeDecodeError, ValueError) as error:
        raise malformed("invalid_cursor", "the cursor is not one this list issued", "cursor") from error
    parts = text.split("|")
    if (
        len(parts) != 3
        or parts[:2] != [_KEYSET_VERSION, name]
        or not key.fullmatch(parts[2])
        or _encode_keyset(name, parts[2]) != cursor
    ):
        raise malformed("invalid_cursor", "the cursor is not one this list issued", "cursor")
    return parts[2]


_ARTIFACT_KEY = re.compile(r"[0-9a-f]{64}")


def page_after_id(ids: list[str], limit: int, cursor: str | None, name: str) -> Page:
    """IDs in ascending order, continuing after the cursor's ID."""
    start = 0 if cursor is None else bisect.bisect_right(ids, _decode_keyset(cursor, name, _ARTIFACT_KEY))
    page = ids[start : start + limit]
    more = start + len(page) < len(ids)
    return Page(page, _encode_keyset(name, page[-1]) if more and page else None)


def page_before_sequence(jobs: list["Job"], limit: int, cursor: str | None, name: str = "runs") -> Page:
    """Jobs newest first, continuing with those older than the cursor's."""
    # Decoded first, so a malformed cursor is refused even for an empty list.
    before = None if cursor is None else int(_decode_keyset(cursor, name, _UNSIGNED))
    older = [job for job in jobs if before is None or job.sequence < before]
    page = older[:limit]
    more = len(older) > len(page)
    return Page(page, _encode_keyset(name, str(page[-1].sequence)) if more and page else None)


def parse_query(query: str, allowed: tuple[str, ...]) -> dict[str, str]:
    values: dict[str, str] = {}
    if not query:
        return values
    for part in query.split("&"):
        name, separator, value = part.partition("=")
        if re.search(r"%(?![0-9a-fA-F]{2})", part):
            raise malformed("invalid_query", "query parameters need valid percent encoding")
        try:
            name, value = unquote(name, errors="strict"), unquote(value, errors="strict")
        except UnicodeDecodeError as error:
            raise malformed("invalid_query", "query parameters need valid UTF-8") from error
        if not separator or name not in allowed or name in values or not value:
            raise malformed(
                "invalid_query", f"the query parameter {name!r} is not accepted here", name or None
            )
        values[name] = value
    return values


def limit_of(values: Mapping[str, str]) -> int:
    text = values.get("limit")
    if text is None:
        return DEFAULT_LIMIT
    if not _UNSIGNED.fullmatch(text) or not 1 <= int(text) <= MAX_LIMIT:
        raise malformed("invalid_limit", f"limit is a whole number from 1 to {MAX_LIMIT}", "limit")
    return int(text)


# --- Jobs ----------------------------------------------------------------------------------------------


@dataclass
class Job:
    job_id: str
    sequence: int
    request: dict[str, Any]
    request_key: str
    state: str
    created_at: str
    started_at: str | None = None
    finished_at: str | None = None
    result: dict[str, Any] | None = None
    error: dict[str, Any] | None = None
    log: str = ""
    kind: str = "evaluation"
    # The token the job's sandbox containers carry, and whether they are
    # known to be gone; recorded before the child starts.
    sandbox_supervisor: str | None = None
    sandbox_released: bool = True
    # The latest progress a walk-forward study's child reported.
    progress: dict[str, str] | None = None

    def as_json(self) -> dict[str, Any]:
        return {
            "created_at": self.created_at,
            "error": self.error,
            "finished_at": self.finished_at,
            "job_id": self.job_id,
            "kind": self.kind,
            "log": self.log,
            "progress": self.progress,
            "request": self.request,
            "request_key": self.request_key,
            "result": self.result,
            "sandbox_released": self.sandbox_released,
            "sandbox_supervisor": self.sandbox_supervisor,
            "sequence": str(self.sequence),
            "started_at": self.started_at,
            "state": self.state,
        }

    def public(self) -> dict[str, Any]:
        document = self.as_json()
        for internal in ("request_key", "sandbox_released", "sandbox_supervisor", "sequence"):
            del document[internal]
        if self.kind not in PROGRESS_JOB_KINDS:
            del document["progress"]
        return document

    @classmethod
    def from_json(cls, document: Mapping[str, Any]) -> "Job":
        return cls(
            job_id=document["job_id"],
            sequence=int(document["sequence"]),
            request=document["request"],
            request_key=document["request_key"],
            state=document["state"],
            created_at=document["created_at"],
            started_at=document["started_at"],
            finished_at=document["finished_at"],
            result=document["result"],
            error=document["error"],
            log=document["log"],
            # A record from before job kinds is an evaluation's.
            kind=document.get("kind", "evaluation"),
            sandbox_supervisor=document.get("sandbox_supervisor"),
            sandbox_released=document.get("sandbox_released", True),
            progress=document.get("progress"),
        )


class JobStore:
    """Job records, one atomically replaced file each."""

    def __init__(self, directory: Path, retain: int) -> None:
        self.directory = directory
        self.retain = retain
        self.lock = threading.Lock()
        directory.mkdir(parents=True, exist_ok=True)
        self.jobs: dict[str, Job] = {}
        for name in sorted(os.listdir(directory)):
            path = directory / name
            if name.startswith(".") or not name.endswith(".json") or not _JOB_ID.fullmatch(name[:-5]):
                continue
            try:
                job = Job.from_json(json.loads(path.read_bytes()))
            except (OSError, ValueError, KeyError, TypeError):
                continue
            if job.state not in TERMINAL:
                # Never resumed unnoticed.
                job.state = "interrupted"
                job.finished_at = now()
                job.error = {
                    "code": "interrupted",
                    "message": "the research service stopped before this job finished; submit it again",
                }
                self._write(job)
            self.jobs[job.job_id] = job

    def _write(self, job: Job) -> None:
        temporary = self.directory / f".{job.job_id}.{secrets.token_hex(4)}.tmp"
        with open(temporary, "wb") as file:
            file.write(json.dumps(job.as_json(), sort_keys=True).encode())
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, self.directory / f"{job.job_id}.json")
        descriptor = os.open(self.directory, os.O_RDONLY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def save(self, job: Job) -> None:
        with self.lock:
            self._write(job)
            self.jobs[job.job_id] = job

    def create(self, request: dict[str, Any], key: str, kind: str = "evaluation") -> tuple[Job, bool]:
        """A new queued job, or the equal job already queued, running, or
        succeeded, and whether it is new."""
        with self.lock:
            for job in self.jobs.values():
                if job.request_key == key and job.state in ("queued", "running", "succeeded"):
                    return job, False
            sequence = max((job.sequence for job in self.jobs.values()), default=0) + 1
            job = Job(secrets.token_hex(16), sequence, request, key, "queued", now(), kind=kind)
            self._write(job)
            self.jobs[job.job_id] = job
            self._prune()
            return job, True

    def _prune(self) -> None:
        # A job whose containers are not known to be gone keeps its record,
        # which is what reclaims them.
        finished = sorted(
            (job for job in self.jobs.values() if job.state in TERMINAL and job.sandbox_released),
            key=lambda job: job.sequence,
        )
        for job in finished[: max(0, len(self.jobs) - self.retain)]:
            (self.directory / f"{job.job_id}.json").unlink(missing_ok=True)
            del self.jobs[job.job_id]

    def newest_first(self) -> list[Job]:
        with self.lock:
            return sorted(self.jobs.values(), key=lambda job: job.sequence, reverse=True)

    def get(self, job_id: str) -> Job | None:
        with self.lock:
            return self.jobs.get(job_id)


@dataclass(frozen=True)
class ServiceConfiguration:
    tools: Tools
    artifacts: Path
    state: Path
    host: str = "127.0.0.1"
    port: int = 8090
    max_running: int = 1
    max_queued: int = 8
    job_timeout_s: int = 1800
    retain_jobs: int = 200
    # The fixed child command; a test may point it at another interpreter.
    python: str = field(default=sys.executable)
    # How import jobs reach Docker for the ONNX sandbox.
    sandbox: Sandbox = field(default_factory=Sandbox)


def job_arguments(
    configuration: ServiceConfiguration,
    request: Mapping[str, Any],
    kind: str = "evaluation",
    supervisor: str | None = None,
) -> list[str]:
    """The child command's argument vector: fixed, never a shell string."""
    roots = ["--tools", str(configuration.tools.directory), "--artifacts", str(configuration.artifacts)]
    sandbox = configuration.sandbox
    docker = [
        "--docker",
        sandbox.docker,
        *(["--docker-host", sandbox.docker_host] if sandbox.docker_host else []),
        *(["--sandbox-supervisor", supervisor] if supervisor else []),
    ]
    if kind == "comparison":
        spec = json.dumps(request, sort_keys=True, separators=(",", ":"))
        return [
            configuration.python,
            "-m",
            "chronos_research",
            "compare-models",
            *roots,
            "--spec",
            spec,
            *docker,
        ]
    if kind == "walk_forward_plan":
        return [
            configuration.python, "-m", "chronos_research", "plan-walk-forward", *roots,
            "--request", json.dumps(request, sort_keys=True, separators=(",", ":")),
        ]  # fmt: skip
    if kind == "walk_forward":
        return [
            configuration.python, "-m", "chronos_research", "run-walk-forward", *roots,
            "--plan", request["plan_id"], "--progress",
        ]  # fmt: skip
    if kind == "strategy_diagnostics":
        studies = [part for study in request["study_ids"] for part in ("--study", study)]
        return [
            configuration.python, "-m", "chronos_research", "diagnose-strategies", *roots, *studies,
            "--progress",
        ]  # fmt: skip
    if kind == "import":
        return [
            configuration.python, "-m", "chronos_research", "import-model",
            "--artifacts", str(configuration.artifacts),
            "--staged", str(configuration.state / "imports" / request["import_id"]),
            "--model-sha256", request["model_sha256"], *docker,
        ]  # fmt: skip
    if kind == "registration":
        return [
            configuration.python, "-m", "chronos_research", "register-model", *roots,
            "--experiment", request["experiment_id"], "--instrument", request["instrument"],
            "--model", request["model"], "--name", request["name"], "--version", request["version"],
        ]  # fmt: skip
    if kind != "evaluation":
        raise ValueError(f"no child command for {kind} jobs")
    arguments = [
        configuration.python, "-m", "chronos_research", "evaluate-strategy", *roots,
        "--experiment", request["experiment_id"], "--instrument", request["instrument"],
        "--model", request["model"], "--window", request["window"],
    ]  # fmt: skip
    for key, value in sorted(request["configuration"].items()):
        arguments += ["--" + key.replace("_", "-"), value]
    return arguments


def job_result(kind: str, output: Mapping[str, Any]) -> dict[str, Any]:
    """What a succeeded job records from its child's JSON."""
    if kind in ("registration", "import"):
        return {key: output[key] for key in ("binding_id", "binding_outcome", "model_version_id", "outcome")}
    if kind == "comparison":
        return {
            "comparison_id": output["comparison"]["comparison_id"],
            "outcome": output["outcome"],
            "status": output["comparison"]["status"],
        }
    if kind == "walk_forward_plan":
        return {
            "folds": str(len(output["plan"]["folds"])),
            "outcome": output["outcome"],
            "plan_id": output["plan"]["plan_id"],
        }
    if kind == "strategy_diagnostics":
        diagnostics = output["diagnostics"]
        return {
            "diagnostic_id": diagnostics["diagnostic_id"],
            "outcome": output["outcome"],
            "status": diagnostics["status"],
            "study_ids": [study["study_id"] for study in diagnostics["studies"]],
        }
    if kind == "walk_forward":
        study = output["study"]
        return {
            "missing": str(len(study["missing"])),
            "outcome": output["outcome"],
            "status": study["status"],
            "study_id": study["study_id"],
        }
    return {
        "reproduced": output["reproduced"],
        "run_id": output["run"]["artifact_id"],
        "run_outcome": output["run"]["outcome"],
        "schedule_id": output["schedule"]["artifact_id"],
        "schedule_outcome": output["schedule"]["outcome"],
    }


class WorkerOutputLimitError(Exception):
    """A child exceeded the output limit before it finished."""


def progress_report(line: bytes) -> dict[str, str] | None:
    """A child's progress line as a bounded report, or None if the line is
    not one: {"progress": {...}} of at most six known keys, each a short
    lowercase token."""
    try:
        document = json.loads(line)
    except ValueError:
        return None
    report = document.get("progress") if isinstance(document, dict) and len(document) == 1 else None
    if (
        not isinstance(report, dict)
        or not report
        or not set(report) <= set(_PROGRESS_KEYS)
        or not all(isinstance(value, str) and _PROGRESS_VALUE.fullmatch(value) for value in report.values())
    ):
        return None
    return {key: report[key] for key in sorted(report)}


def _worker_output(
    child: subprocess.Popen[bytes],
    timeout_s: int,
    on_progress: Callable[[dict[str, str]], None] | None = None,
) -> tuple[bytes, bytes]:
    """Drain both pipes without unbounded communicate() buffers.

    The result has a hard byte limit; diagnostics keep only a bounded tail.
    An overall output limit also stops a worker flooding either pipe. With
    on_progress, each complete stderr line that is a progress report goes to
    it instead of the log; a line longer than the log tail is only log.
    """
    assert child.stdout is not None
    assert child.stderr is not None
    stdout, stderr = bytearray(), bytearray()
    pending = bytearray()

    def logged(line: bytes) -> None:
        report = progress_report(line) if on_progress is not None else None
        if report is None:
            stderr.extend(line + b"\n")
        else:
            assert on_progress is not None
            on_progress(report)

    output_bytes = 0
    deadline = time.monotonic() + timeout_s
    with selectors.DefaultSelector() as pipes:
        pipes.register(child.stdout, selectors.EVENT_READ, "stdout")
        pipes.register(child.stderr, selectors.EVENT_READ, "stderr")
        while pipes.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise subprocess.TimeoutExpired(
                    child.args, timeout_s, output=bytes(stdout), stderr=bytes(stderr)
                )
            for key, _ in pipes.select(timeout=remaining):
                chunk = os.read(key.fd, 64 * 1024)
                if not chunk:
                    pipes.unregister(key.fd)
                    continue
                output_bytes += len(chunk)
                if output_bytes > MAX_WORKER_OUTPUT_BYTES:
                    raise WorkerOutputLimitError
                if key.data == "stdout":
                    stdout.extend(chunk)
                elif on_progress is None:
                    stderr.extend(chunk)
                    del stderr[:-LOG_BYTES]
                else:
                    pending.extend(chunk)
                    *lines, rest = pending.split(b"\n")
                    pending = bytearray(rest)
                    for line in lines:
                        logged(bytes(line))
                    if len(pending) > LOG_BYTES:
                        stderr.extend(pending)
                        pending.clear()
                    del stderr[:-LOG_BYTES]
        child.wait(timeout=max(0, deadline - time.monotonic()))
    stderr.extend(pending)
    return bytes(stdout), bytes(stderr[-LOG_BYTES:])


class Workers:
    def __init__(self, configuration: ServiceConfiguration, store: JobStore) -> None:
        self.configuration = configuration
        self.store = store
        self.queue: queue.Queue[str] = queue.Queue(maxsize=configuration.max_queued)
        self.stopping = threading.Event()
        # Live children, to signal; and every job holding a slot, until its
        # sandbox containers are known to be gone.
        self.children: dict[str, subprocess.Popen[bytes]] = {}
        self.occupied: set[str] = set()
        self.lock = threading.Lock()
        # Containers of jobs a crash interrupted, or whose cleanup Docker
        # could not confirm, before anything new starts.
        self._reclaim()
        self.threads = [
            threading.Thread(target=self._loop, name=f"research-worker-{index}", daemon=True)
            for index in range(configuration.max_running)
        ]
        for thread in self.threads:
            thread.start()

    def submit(self, job: Job) -> None:
        try:
            self.queue.put_nowait(job.job_id)
        except queue.Full as error:
            job.state, job.finished_at = "failed", now()
            job.error = {"code": "queue_full", "message": "too many evaluations are already queued"}
            self.store.save(job)
            raise ApiError(
                HTTPStatus.SERVICE_UNAVAILABLE, "queue_full", "too many evaluations are already queued"
            ) from error

    def running(self) -> int:
        with self.lock:
            return len(self.occupied)

    def _loop(self) -> None:
        while not self.stopping.is_set():
            try:
                job_id = self.queue.get(timeout=0.2)
            except queue.Empty:
                continue
            job = self.store.get(job_id)
            if job is None or job.state != "queued":
                continue
            with self.lock:
                self.occupied.add(job.job_id)
            try:
                self._run(job)
            finally:
                with self.lock:
                    self.occupied.discard(job.job_id)

    def _run(self, job: Job) -> None:
        if job.kind in SANDBOX_JOB_KINDS:
            job.sandbox_supervisor, job.sandbox_released = new_supervisor(), False
        job.state, job.started_at = "running", now()
        self.store.save(job)
        if job.sandbox_supervisor is not None and not self._reclaim():
            job.sandbox_released = True
            self._finish(
                job,
                "failed",
                error={
                    "code": "sandbox_cleanup_pending",
                    "message": "an earlier job's sandbox containers are still present; nothing was started",
                },
            )
            return
        state, recorded, abnormal = self._supervise(job)
        self._release_sandbox(job, abnormal)
        self._finish(job, state, **recorded)

    def _supervise(self, job: Job) -> tuple[str, dict[str, Any], bool]:
        """Runs the job's child to its end: the job's final state, what to
        record, and whether the child was killed or failed."""
        try:
            with self.lock:
                # Shutdown cannot miss a child between launch and registration.
                if self.stopping.is_set():
                    child = None
                else:
                    child = subprocess.Popen(
                        job_arguments(self.configuration, job.request, job.kind, job.sandbox_supervisor),
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        stdin=subprocess.DEVNULL,
                        cwd=str(Path(__file__).resolve().parent.parent),
                        # Its own process group also contains its C++ tools.
                        start_new_session=True,
                    )
                    self.children[job.job_id] = child
        except OSError as error:
            return (
                "failed",
                {"error": {"code": "worker_failed", "message": f"cannot start: {error.strerror}"}},
                False,
            )
        if child is None:
            return "interrupted", {"error": {"code": "interrupted", "message": "the service stopped"}}, False
        try:
            stdout, stderr = _worker_output(
                child,
                self.configuration.job_timeout_s,
                self._progress_of(job) if job.kind in PROGRESS_JOB_KINDS else None,
            )
        except subprocess.TimeoutExpired as error:
            _signal_group(child, signal.SIGKILL)
            _, remaining_stderr = child.communicate()
            timed_out = {
                "code": "timed_out",
                "message": f"stopped after {self.configuration.job_timeout_s} s",
            }
            return "failed", {"error": timed_out, "log": (error.stderr or b"") + remaining_stderr}, True
        except WorkerOutputLimitError:
            _signal_group(child, signal.SIGKILL)
            child.communicate()
            return (
                "failed",
                {"error": {"code": "worker_failed", "message": "the job's output was too large"}},
                True,
            )
        finally:
            if child.stdout is not None:
                child.stdout.close()
            if child.stderr is not None:
                child.stderr.close()
            with self.lock:
                self.children.pop(job.job_id, None)
        if self.stopping.is_set() and child.returncode != 0:
            interrupted = {"code": "interrupted", "message": "the service stopped"}
            return "interrupted", {"error": interrupted, "log": stderr}, True
        if child.returncode != 0:
            return "failed", {"error": _child_error(stderr, job.kind), "log": stderr}, True
        try:
            result = job_result(job.kind, json.loads(stdout))
        except (ValueError, KeyError, TypeError):
            return (
                "failed",
                {"error": {"code": "worker_failed", "message": "the job printed no result"}},
                False,
            )
        return "succeeded", {"result": result, "log": stderr}, False

    def _progress_of(self, job: Job) -> Callable[[dict[str, str]], None]:
        def record(report: dict[str, str]) -> None:
            job.progress = report
            self.store.save(job)

        return record

    def _release_sandbox(self, job: Job, abnormal: bool) -> None:
        """Removes the job's sandbox containers and checks that none is left,
        while the job still holds its slot. A container Docker does not
        remove is retried for RELEASE_TIMEOUT_S. Docker not answering leaves
        the job unreleased, for _reclaim; without the Docker CLI at all, the
        child could not have started a container."""
        if job.sandbox_supervisor is None or job.sandbox_released:
            return
        deadline = time.monotonic() + RELEASE_TIMEOUT_S
        while True:
            try:
                self.configuration.sandbox.remove_supervised(
                    [job.sandbox_supervisor], settle_s=RELEASE_SETTLE_S if abnormal else 0.0
                )
            except SandboxCleanupError:
                if time.monotonic() >= deadline:
                    return
                time.sleep(0.5)
                continue
            except SandboxUnavailableError as error:
                job.sandbox_released = error.reason == "docker_not_found"
                return
            job.sandbox_released = True
            return

    def _reclaim(self) -> bool:
        """Removes the containers of finished jobs not yet released. False
        only when Docker answers and one of them stays."""
        pending = [
            job
            for job in self.store.newest_first()
            if job.state in TERMINAL and job.sandbox_supervisor is not None and not job.sandbox_released
        ]
        if not pending:
            return True
        try:
            self.configuration.sandbox.remove_supervised([str(job.sandbox_supervisor) for job in pending])
        except SandboxCleanupError:
            return False
        except SandboxUnavailableError as error:
            if error.reason != "docker_not_found":
                # Nothing can start a container while Docker does not answer.
                return True
        for job in pending:
            job.sandbox_released = True
            self.store.save(job)
        return True

    def _finish(
        self,
        job: Job,
        state: str,
        *,
        result: dict[str, Any] | None = None,
        error: dict[str, Any] | None = None,
        log: bytes = b"",
    ) -> None:
        job.state, job.finished_at, job.result, job.error = state, now(), result, error
        job.log = log[-LOG_BYTES:].decode("utf-8", errors="replace")
        self.store.save(job)

    def stop(self) -> None:
        with self.lock:
            self.stopping.set()
            children = list(self.children.values())
        for child in children:
            _signal_group(child, signal.SIGTERM)
        deadline = time.monotonic() + 1
        for thread in self.threads:
            thread.join(timeout=max(0, deadline - time.monotonic()))
        if any(thread.is_alive() for thread in self.threads):
            # Include descendants even if their leader has already exited,
            # but only of children not yet reaped.
            with self.lock:
                children = list(self.children.values())
            for child in children:
                _signal_group(child, signal.SIGKILL)
            # Each slot then removes its job's containers.
            deadline = time.monotonic() + 5 + RELEASE_SETTLE_S + RELEASE_TIMEOUT_S
            for thread in self.threads:
                thread.join(timeout=max(0, deadline - time.monotonic()))
        if any(thread.is_alive() for thread in self.threads):
            raise RuntimeError("research workers did not stop after their process groups were killed")
        # Jobs still queued were never started.
        while True:
            try:
                job_id = self.queue.get_nowait()
            except queue.Empty:
                break
            job = self.store.get(job_id)
            if job is not None and job.state == "queued":
                self._finish(
                    job, "interrupted", error={"code": "interrupted", "message": "the service stopped"}
                )


def _signal_group(child: "subprocess.Popen[bytes]", signum: int) -> None:
    """Signals a job and every process it started."""
    with contextlib.suppress(ProcessLookupError):
        os.killpg(child.pid, signum)


# A failed child's error code, by job kind and by the error kind it reported.
_FAILURE_CODES = {
    "evaluation": "evaluation_failed",
    "registration": "registration_failed",
    "import": "import_failed",
    "comparison": "comparison_failed",
    "walk_forward_plan": "plan_failed",
    "walk_forward": "study_failed",
    "strategy_diagnostics": "diagnostics_failed",
}
_REFUSAL_CODES = {
    "NameVersionTakenError": "name_version_taken",
    "RegistryError": "registration_refused",
    "ModelImportError": "import_refused",
    "SandboxRejectedError": "import_refused",
    "StagingError": "import_refused",
    "SandboxFailedError": "sandbox_failed",
    "SandboxUnavailableError": "sandbox_unavailable",
    "ComparisonError": "comparison_refused",
    "CollectionError": "collection_refused",
    "PlanError": "plan_refused",
    "StudyError": "study_refused",
    "DiagnosticsError": "diagnostics_refused",
}


def _child_error(stderr: bytes, kind: str = "evaluation") -> dict[str, Any]:
    code = _FAILURE_CODES[kind]
    try:
        error = json.loads(stderr)["error"]
        reported = str(error.get("kind"))
        if kind != "evaluation":
            code = _REFUSAL_CODES.get(reported, code)
        document = {"code": code, "message": str(error["message"]), "kind": reported}
        if isinstance(error.get("reason"), str):
            document["reason"] = error["reason"]
        return document
    except (ValueError, KeyError, TypeError):
        return {"code": code, "message": stderr[-LOG_BYTES:].decode("utf-8", errors="replace")}


# --- HTTP -----------------------------------------------------------------------------------------------


class Service:
    def __init__(self, configuration: ServiceConfiguration) -> None:
        self.configuration = configuration
        self.artifacts = Artifacts(ArtifactRoots.under(configuration.artifacts))
        self.registry = RegistryRoots.under(configuration.artifacts)
        self.store = JobStore(configuration.state / "jobs", configuration.retain_jobs)
        self.imports = ImportStore(configuration.state / "imports", now)
        self.workers = Workers(configuration, self.store)
        self.digests = diagnostic_sources.ContentDigests()

    # Routes.
    def route(
        self, method: str, path: str, query: str, body: Callable[[str, int], bytes]
    ) -> tuple[HTTPStatus, Any]:
        # Each path, its handler per method, and the query parameters it takes.
        # A POST or PUT handler gets the body after the query is checked: JSON
        # of at most 16 KiB, or for PUT a model of at most 1 MiB.
        type Handler = Callable[..., tuple[HTTPStatus, Any]]
        routes: list[tuple[str, dict[str, Handler], tuple[str, ...]]] = [
            (r"/status", {"GET": self.status}, ()),
            (r"/experiments", {"GET": self.experiments}, ("limit", "cursor")),
            (r"/experiments/([^/]+)", {"GET": self.experiment}, ()),
            (r"/runs", {"GET": self.runs, "POST": self.start_run}, ("limit", "cursor")),
            (r"/runs/([^/]+)", {"GET": self.run}, ()),
            (r"/results", {"GET": self.results}, ("limit", "cursor")),
            (r"/results/([^/]+)", {"GET": self.result}, ()),
            (r"/results/([^/]+)/decisions", {"GET": self.decisions}, ("limit", "cursor")),
            (r"/results/([^/]+)/trades", {"GET": self.trades}, ("account", "limit", "cursor")),
            (r"/results/([^/]+)/equity", {"GET": self.equity}, ("limit", "cursor")),
            (r"/models", {"GET": self.models, "POST": self.start_registration}, ("limit", "cursor", "state")),
            (r"/models/([^/]+)", {"GET": self.model}, ()),
            (r"/models/([^/]+)/lifecycle", {"POST": self.set_model_lifecycle}, ()),
            (r"/comparisons", {"GET": self.comparisons, "POST": self.start_comparison}, ("limit", "cursor")),
            (r"/comparisons/([^/]+)", {"GET": self.comparison}, ()),
            (r"/imports", {"POST": self.stage_import}, ()),
            (r"/imports/([^/]+)", {"GET": self.staged_import}, ()),
            (r"/imports/([^/]+)/model", {"PUT": self.upload_model}, ()),
            (r"/imports/([^/]+)/validation", {"POST": self.start_import}, ()),
            (r"/collections", {"GET": self.collections}, ("limit", "cursor")),
            (r"/collections/([^/]+)", {"GET": self.collection}, ()),
            (
                r"/walk-forward/plans",
                {"GET": self.walk_forward_plans, "POST": self.start_walk_forward_plan},
                ("limit", "cursor"),
            ),
            (r"/walk-forward/plans/([^/]+)", {"GET": self.walk_forward_plan}, ()),
            (
                r"/walk-forward/studies",
                {"GET": self.walk_forward_studies, "POST": self.start_walk_forward_study},
                ("limit", "cursor"),
            ),
            (r"/walk-forward/studies/([^/]+)", {"GET": self.walk_forward_study}, ()),
            (
                r"/strategy-diagnostics",
                {"GET": self.strategy_diagnostics_list, "POST": self.start_strategy_diagnostics},
                ("limit", "cursor", "study"),
            ),
            (r"/strategy-diagnostics/([^/]+)", {"GET": self.strategy_diagnostics}, ()),
            (
                r"/strategy-diagnostics/([^/]+)/fills",
                {"GET": self.strategy_diagnostics_fills},
                ("run", "account", "limit", "cursor"),
            ),
            (
                r"/strategy-diagnostics/([^/]+)/episodes",
                {"GET": self.strategy_diagnostics_episodes},
                ("run", "account", "limit", "cursor"),
            ),
            (r"/jobs", {"GET": self.jobs}, ("limit", "cursor", "kind")),
            (r"/jobs/([^/]+)", {"GET": self.job}, ()),
        ]
        if not path.startswith(API + "/"):
            raise ApiError(HTTPStatus.NOT_FOUND, "route_not_found", "no such path")
        tail = path[len(API) :]
        for pattern, handlers, allowed in routes:
            matched = re.fullmatch(pattern, tail)
            if matched is None:
                continue
            handler = handlers.get(method)
            if handler is None:
                raise MethodNotAllowedError(", ".join(sorted(handlers)))
            if method == "POST":
                parse_query(query, ())
                return handler(*matched.groups(), body("application/json", MAX_BODY_BYTES))
            if method == "PUT":
                parse_query(query, ())
                return handler(
                    *matched.groups(), body("application/octet-stream", onnx_profile.MAX_MODEL_BYTES)
                )
            return handler(*matched.groups(), parse_query(query, allowed))
        raise ApiError(HTTPStatus.NOT_FOUND, "route_not_found", "no such path")

    def status(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        jobs = self.store.newest_first()
        tools = {
            name: (self.configuration.tools.directory / name).is_file()
            for name in ("chronos_dataset", "chronos_strategy")
        }
        return HTTPStatus.OK, {
            "defaults": DEFAULT_CONFIGURATION,
            "jobs": {
                state: str(sum(job.state == state for job in jobs))
                for state in ("queued", "running", *TERMINAL)
            },
            "limits": {
                "job_timeout_s": str(self.configuration.job_timeout_s),
                "max_queued": str(self.configuration.max_queued),
                "max_running": str(self.configuration.max_running),
                "retain_jobs": str(self.configuration.retain_jobs),
            },
            "imports": {
                "profile": onnx_profile.limits(),
                "sandbox": self.configuration.sandbox.capability(),
            },
            "walk_forward": {
                "max_folds": str(walk_forward.MAX_FOLDS),
                "max_scenarios": str(walk_forward.MAX_SCENARIOS),
                "max_sources": str(collections.MAX_SOURCES),
                "max_strategy_runs": str(walk_forward.MAX_STRATEGY_RUNS),
                "max_training_rows_per_fold": str(walk_forward.MAX_TRAINING_ROWS_PER_FOLD),
            },
            "service": "chronos-research",
            "tools": {name: "available" if present else "missing" for name, present in tools.items()},
            "version": VERSION,
        }

    def _experiment(self, experiment_id: str) -> dict[str, Any]:
        if not is_artifact_id(experiment_id):
            raise malformed(
                "invalid_identifier", "an experiment ID is 64 lowercase hexadecimal digits", "experiment_id"
            )
        if experiment_id not in self.artifacts.experiment_ids():
            raise ApiError(HTTPStatus.NOT_FOUND, "experiment_not_found", "no such experiment")
        try:
            return self.artifacts.experiment(experiment_id)
        except (ArtifactUnavailableError, OSError, ValueError, KeyError, TypeError) as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the experiment cannot be read: {error}"
            ) from error

    def experiments(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        page = page_after_id(
            self.artifacts.experiment_ids(), limit_of(query), query.get("cursor"), "experiments"
        )
        items, invalid = [], []
        for experiment_id in page.items:
            try:
                items.append(self.artifacts.experiment(experiment_id))
            except (ArtifactUnavailableError, OSError, ValueError, KeyError, TypeError) as error:
                invalid.append({"experiment_id": experiment_id, "reason": str(error) or type(error).__name__})
        return HTTPStatus.OK, {
            "experiments": items,
            "invalid_entries": invalid,
            "next_cursor": page.next_cursor,
        }

    def experiment(self, experiment_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        return HTTPStatus.OK, {"experiment": self._experiment(experiment_id)}

    def _fitted_model(self, request: Mapping[str, Any]) -> None:
        """The request's experiment verifies and its model is fitted."""
        experiment = self._experiment(request["experiment_id"])
        entry = next(
            (item for item in experiment["instruments"] if item["instrument"] == request["instrument"]), None
        )
        if entry is None:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "unknown_instrument",
                "the experiment has no model for it",
                "instrument",
            )
        model = entry["models"][request["model"]]
        if model["status"] != "fitted":
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "unevaluable_model",
                f"the {request['model']} model is unevaluable and predicts nothing",
                "model",
                model.get("reason"),
            )

    def start_run(self, data: bytes) -> tuple[HTTPStatus, Any]:
        request = validate_run_request(parse_body(data))
        self._fitted_model(request)
        key = hashlib.sha256(canonical_bytes(request)).hexdigest()
        job, created = self.store.create(request, key)
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    # Evaluations keep their own list, whose shape the workspace relies on.
    def runs(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        evaluations = [job for job in self.store.newest_first() if job.kind == "evaluation"]
        page = page_before_sequence(evaluations, limit_of(query), query.get("cursor"))
        return HTTPStatus.OK, {"next_cursor": page.next_cursor, "runs": [job.public() for job in page.items]}

    def run(self, job_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        job = self._job(job_id)
        if job is None or job.kind != "evaluation":
            raise ApiError(HTTPStatus.NOT_FOUND, "run_not_found", "no such run")
        return HTTPStatus.OK, {"run": job.public()}

    def _job(self, job_id: str) -> Job | None:
        if not _JOB_ID.fullmatch(job_id):
            raise malformed("invalid_identifier", "a job ID is 32 lowercase hexadecimal digits", "job_id")
        return self.store.get(job_id)

    # Every kind of job, with its kind.
    def jobs(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        kind = query.get("kind")
        if kind is not None and kind not in JOB_KINDS:
            raise malformed("unknown_enum_value", f"kind is one of {', '.join(JOB_KINDS)}", "kind")
        selected = [job for job in self.store.newest_first() if kind is None or job.kind == kind]
        page = page_before_sequence(selected, limit_of(query), query.get("cursor"), f"jobs-{kind or 'all'}")
        return HTTPStatus.OK, {"jobs": [job.public() for job in page.items], "next_cursor": page.next_cursor}

    def job(self, job_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        job = self._job(job_id)
        if job is None:
            raise ApiError(HTTPStatus.NOT_FOUND, "job_not_found", "no such job")
        return HTTPStatus.OK, {"job": job.public()}

    # The model library.
    def _model_version(self, model_version_id: str) -> ModelVersion:
        if not is_artifact_id(model_version_id):
            raise malformed(
                "invalid_identifier",
                "a model version ID is 64 lowercase hexadecimal digits",
                "model_version_id",
            )
        if model_version_id not in model_version_ids(self.registry):
            raise ApiError(HTTPStatus.NOT_FOUND, "model_not_found", "no such model version")
        try:
            return verified_model_version(self.registry, model_version_id)
        except RegistryError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "artifact_invalid",
                f"the model version cannot be read: {error}",
            ) from error

    def models(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        state = query.get("state", "active")
        if state not in (*LIFECYCLE_STATES, "all"):
            raise malformed("unknown_enum_value", "state is one of active, archived, all", "state")
        names = names_by_version(self.registry)
        entries: dict[str, dict[str, Any]] = {}
        invalid = []
        for version_id in model_version_ids(self.registry):
            try:
                version = verified_model_version(self.registry, version_id)
                entry = model_library.summary(self.registry, version, names.get(version_id, []))
            except RegistryError as error:
                invalid.append({"model_version_id": version_id, "reason": str(error)})
                continue
            if state in ("all", entry["lifecycle"]["state"]):
                entries[version_id] = entry
        cursor = query.get("cursor")
        page = page_after_id(sorted(entries), limit_of(query), cursor, f"models-{state}")
        return HTTPStatus.OK, {
            # Reported once, on the first page.
            "invalid_entries": invalid if cursor is None else [],
            "models": [with_timestamps(model_library.listing_entry(entries[key])) for key in page.items],
            "next_cursor": page.next_cursor,
        }

    def model(self, model_version_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        version = self._model_version(model_version_id)
        names = names_by_version(self.registry).get(model_version_id, [])
        try:
            document = model_library.detail(
                self.registry,
                self.artifacts.roots.experiments,
                version,
                names,
                self.configuration.sandbox.capability(),
            )
        except RegistryError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "artifact_invalid",
                f"the model version cannot be read: {error}",
            ) from error
        return HTTPStatus.OK, {"model": with_timestamps(document)}

    def start_registration(self, data: bytes) -> tuple[HTTPStatus, Any]:
        request = validate_registration_request(parse_body(data))
        self._fitted_model(request)
        key = hashlib.sha256(canonical_bytes({"kind": "registration", "request": request})).hexdigest()
        job, created = self.store.create(request, key, "registration")
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    def set_model_lifecycle(self, model_version_id: str, data: bytes) -> tuple[HTTPStatus, Any]:
        document = parse_body(data)
        _exact_keys(document, ("reason", "state"))
        state, reason = _string(document, "state"), _string(document, "reason")
        if state not in LIFECYCLE_STATES:
            raise malformed("unknown_enum_value", f"state is one of {', '.join(LIFECYCLE_STATES)}", "state")
        self._model_version(model_version_id)
        try:
            record = set_lifecycle(self.registry, model_version_id, state, reason)
        except RegistryError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "invalid_configuration",
                str(error),
                "reason",
                "out_of_domain",
            ) from error
        return HTTPStatus.OK, {"lifecycle": record, "model_version_id": model_version_id}

    # Imports: a manifest, then its model bytes, then a validation job.
    def _staged(self, import_id: str) -> StagedImport:
        if not _JOB_ID.fullmatch(import_id):
            raise malformed(
                "invalid_identifier", "an import ID is 32 lowercase hexadecimal digits", "import_id"
            )
        staged = self.imports.get(import_id)
        if staged is None:
            raise ApiError(HTTPStatus.NOT_FOUND, "import_not_found", "no such staged import")
        return staged

    def _import_jobs(self, import_id: str) -> list[Job]:
        return [
            job
            for job in self.store.newest_first()
            if job.kind == "import" and job.request["import_id"] == import_id
        ]

    def _busy_imports(self) -> set[str]:
        return {
            job.request["import_id"]
            for job in self.store.newest_first()
            if job.kind == "import" and job.state in ("queued", "running")
        }

    def _import_document(self, staged: StagedImport) -> dict[str, Any]:
        jobs = self._import_jobs(staged.import_id)
        return {"import": staged.public(), "job": jobs[0].public() if jobs else None}

    def stage_import(self, data: bytes) -> tuple[HTTPStatus, Any]:
        try:
            manifest = validated_import_manifest(parse_body(data))
        except ModelImportError as error:
            raise ApiError(HTTPStatus.UNPROCESSABLE_ENTITY, error.reason, str(error)) from error
        staged = self.imports.create(manifest, self._busy_imports)
        return HTTPStatus.CREATED, self._import_document(staged)

    def staged_import(self, import_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        return HTTPStatus.OK, self._import_document(self._staged(import_id))

    def upload_model(self, import_id: str, data: bytes) -> tuple[HTTPStatus, Any]:
        self._staged(import_id)
        try:
            staged, created = self.imports.attach_model(import_id, data)
        except StagingError as error:
            status = (
                HTTPStatus.CONFLICT if error.reason == "model_already_uploaded" else HTTPStatus.BAD_REQUEST
            )
            raise ApiError(status, error.reason, str(error)) from error
        return (HTTPStatus.CREATED if created else HTTPStatus.OK), self._import_document(staged)

    def start_import(self, import_id: str, data: bytes) -> tuple[HTTPStatus, Any]:
        document = parse_body(data)
        _exact_keys(document, ("model_sha256",))
        digest = _string(document, "model_sha256")
        if not is_artifact_id(digest):
            raise malformed(
                "invalid_identifier", "model_sha256 is 64 lowercase hexadecimal digits", "model_sha256"
            )
        staged = self._staged(import_id)
        if staged.model is None:
            raise ApiError(HTTPStatus.CONFLICT, "model_not_uploaded", "upload the model bytes first")
        if staged.model["sha256"] != digest:
            raise ApiError(
                HTTPStatus.CONFLICT,
                "model_mismatch",
                "the staged model is not the one reviewed",
                "model_sha256",
            )
        capability = self.configuration.sandbox.capability()
        if capability["status"] != "available":
            # Fail closed: an import is never validated any other way.
            raise ApiError(
                HTTPStatus.SERVICE_UNAVAILABLE,
                "sandbox_unavailable",
                capability["detail"],
                None,
                capability["reason"],
            )
        request = {
            "import_id": import_id,
            "instrument": staged.manifest["instrument"],
            "manifest_sha256": staged.manifest_sha256,
            "model_sha256": digest,
            "name": staged.manifest["name"],
            "version": staged.manifest["version"],
        }
        key = hashlib.sha256(
            canonical_bytes(
                {"kind": "import", "manifest_sha256": staged.manifest_sha256, "model_sha256": digest}
            )
        ).hexdigest()
        job, created = self.store.create(request, key, "import")
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    # Comparisons: a frozen spec, run as a job, published as one artifact.
    def start_comparison(self, data: bytes) -> tuple[HTTPStatus, Any]:
        spec = validate_comparison_request(parse_body(data))
        known = set(model_version_ids(self.registry))
        missing = [item for item in spec["model_version_ids"] if item not in known]
        if missing:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "model_not_found",
                f"no such model version: {', '.join(missing)}",
                "model_version_ids",
            )
        if not (self.artifacts.roots.splits / spec["split_artifact_id"]).is_dir():
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "split_not_found",
                "no such split artifact",
                "split_artifact_id",
            )
        job, created = self.store.create(spec, comparison.spec_digest(spec), "comparison")
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    def _comparison(self, comparison_id: str) -> dict[str, Any]:
        if not is_artifact_id(comparison_id):
            raise malformed(
                "invalid_identifier", "a comparison ID is 64 lowercase hexadecimal digits", "comparison_id"
            )
        root = comparison.comparisons_root(self.configuration.artifacts)
        if comparison_id not in comparison.comparison_ids(root):
            raise ApiError(HTTPStatus.NOT_FOUND, "comparison_not_found", "no such comparison")
        try:
            return comparison.verified_comparison(root, comparison_id)
        except comparison.ComparisonError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the comparison cannot be read: {error}"
            ) from error

    def comparisons(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        root = comparison.comparisons_root(self.configuration.artifacts)
        page = page_after_id(
            comparison.comparison_ids(root), limit_of(query), query.get("cursor"), "comparisons"
        )
        items, invalid = [], []
        for comparison_id in page.items:
            try:
                items.append(comparison.listing(comparison.verified_comparison(root, comparison_id)))
            except comparison.ComparisonError as error:
                invalid.append({"comparison_id": comparison_id, "reason": str(error)})
        return HTTPStatus.OK, {
            "comparisons": with_timestamps(items),
            "invalid_entries": invalid,
            "next_cursor": page.next_cursor,
        }

    def comparison(self, comparison_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        manifest = self._comparison(comparison_id)
        checks = comparison.member_artifacts(ArtifactRoots.under(self.configuration.artifacts), manifest)
        return HTTPStatus.OK, {"artifacts": checks, "comparison": with_timestamps(manifest)}

    # Walk-forward evaluation (ADR 0022): collections, plans, and studies.
    def _collection(self, collection_id: str) -> dict[str, Any]:
        if not is_artifact_id(collection_id):
            raise malformed(
                "invalid_identifier", "a collection ID is 64 lowercase hexadecimal digits", "collection_id"
            )
        root = collections.collections_root(self.configuration.artifacts)
        if collection_id not in collections.collection_ids(root):
            raise ApiError(HTTPStatus.NOT_FOUND, "collection_not_found", "no such collection")
        try:
            return collections.verified_collection(root, collection_id)
        except collections.CollectionError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the collection cannot be read: {error}"
            ) from error

    def collections(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        root = collections.collections_root(self.configuration.artifacts)
        page = page_after_id(
            collections.collection_ids(root), limit_of(query), query.get("cursor"), "collections"
        )
        items, invalid = [], []
        for collection_id in page.items:
            try:
                items.append(collection_summary(collections.verified_collection(root, collection_id)))
            except (collections.CollectionError, KeyError, TypeError, ValueError) as error:
                invalid.append({"collection_id": collection_id, "reason": str(error)})
        return HTTPStatus.OK, {
            "collections": items,
            "invalid_entries": invalid,
            "next_cursor": page.next_cursor,
        }

    def collection(self, collection_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        return HTTPStatus.OK, {"collection": collection_summary(self._collection(collection_id))}

    def _plan(self, plan_id: str) -> dict[str, Any]:
        if not is_artifact_id(plan_id):
            raise malformed("invalid_identifier", "a plan ID is 64 lowercase hexadecimal digits", "plan_id")
        root = walk_forward.plans_root(self.configuration.artifacts)
        if plan_id not in walk_forward.plan_ids(root):
            raise ApiError(HTTPStatus.NOT_FOUND, "plan_not_found", "no such walk-forward plan")
        try:
            return walk_forward.verified_plan(root, plan_id)
        except walk_forward.PlanError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the plan cannot be read: {error}"
            ) from error

    def walk_forward_plans(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        root = walk_forward.plans_root(self.configuration.artifacts)
        page = page_after_id(walk_forward.plan_ids(root), limit_of(query), query.get("cursor"), "plans")
        items, invalid = [], []
        for plan_id in page.items:
            try:
                items.append(plan_listing(walk_forward.verified_plan(root, plan_id)))
            except (walk_forward.PlanError, KeyError, TypeError) as error:
                invalid.append({"plan_id": plan_id, "reason": str(error)})
        return HTTPStatus.OK, {"invalid_entries": invalid, "next_cursor": page.next_cursor, "plans": items}

    def walk_forward_plan(self, plan_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        return HTTPStatus.OK, {"plan": with_timestamps(self._plan(plan_id))}

    def start_walk_forward_plan(self, data: bytes) -> tuple[HTTPStatus, Any]:
        """Preview: a job that verifies the collection's sources and
        publishes the plan of exactly this request, never a request thread."""
        request = validate_walk_forward_request(parse_body(data))
        try:
            self._collection(request["collection_id"])
        except ApiError as error:
            if error.code != "collection_not_found":
                raise
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "collection_not_found", "no such collection", "collection_id"
            ) from error
        key = hashlib.sha256(canonical_bytes({"kind": "walk_forward_plan", "request": request})).hexdigest()
        job, created = self.store.create(request, key, "walk_forward_plan")
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    def start_walk_forward_study(self, data: bytes) -> tuple[HTTPStatus, Any]:
        """Starts the study of a reviewed plan, named by its ID; the job
        verifies the plan's sources again before anything runs."""
        document = parse_body(data)
        _exact_keys(document, ("plan_id",))
        plan_id = _string(document, "plan_id")
        if not is_artifact_id(plan_id):
            raise malformed("invalid_identifier", "plan_id is a 64-digit lowercase ID", "plan_id")
        try:
            self._plan(plan_id)
        except ApiError as error:
            if error.code != "plan_not_found":
                raise
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "plan_not_found", "no such walk-forward plan", "plan_id"
            ) from error
        request = {"plan_id": plan_id}
        key = hashlib.sha256(canonical_bytes({"kind": "walk_forward", "request": request})).hexdigest()
        job, created = self.store.create(request, key, "walk_forward")
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    def walk_forward_studies(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        root = walk_forward_study.studies_root(self.configuration.artifacts)
        page = page_after_id(
            walk_forward_study.study_ids(root), limit_of(query), query.get("cursor"), "studies"
        )
        items, invalid = [], []
        for study_id in page.items:
            try:
                items.append(walk_forward_study.listing(walk_forward_study.verified_study(root, study_id)))
            except (walk_forward_study.StudyError, KeyError, TypeError) as error:
                invalid.append({"reason": str(error), "study_id": study_id})
        return HTTPStatus.OK, {"invalid_entries": invalid, "next_cursor": page.next_cursor, "studies": items}

    def walk_forward_study(self, study_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        if not is_artifact_id(study_id):
            raise malformed("invalid_identifier", "a study ID is 64 lowercase hexadecimal digits", "study_id")
        root = walk_forward_study.studies_root(self.configuration.artifacts)
        if study_id not in walk_forward_study.study_ids(root):
            raise ApiError(HTTPStatus.NOT_FOUND, "study_not_found", "no such walk-forward study")
        try:
            manifest = walk_forward_study.verified_study(root, study_id)
        except walk_forward_study.StudyError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the study cannot be read: {error}"
            ) from error
        checks = walk_forward_study.study_artifacts(self.configuration.artifacts, manifest)
        return HTTPStatus.OK, {"artifacts": checks, "study": with_timestamps(manifest)}

    # --- Strategy diagnostics (ADR 0024) ---------------------------------------------------------------

    def start_strategy_diagnostics(self, data: bytes) -> tuple[HTTPStatus, Any]:
        """Queues the diagnostics of existing studies, named by their IDs; the
        job verifies every source again and refits, schedules, and runs
        nothing. The only way to generate diagnostics."""
        document = parse_body(data)
        for key in document:
            if key != "study_ids":
                raise malformed("unexpected_field", f"{key} is not a field of this request", key)
        if "study_ids" not in document:
            raise malformed("missing_field", "study_ids is required", "study_ids")
        studies = document["study_ids"]
        if not isinstance(studies, list) or not all(isinstance(study, str) for study in studies):
            raise malformed("invalid_type", "study_ids is a list of study IDs", "study_ids")
        if not all(is_artifact_id(study) for study in studies):
            raise malformed(
                "invalid_identifier", "a study ID is 64 lowercase hexadecimal digits", "study_ids"
            )
        if not 1 <= len(studies) <= strategy_diagnostics.MAX_STUDIES or len(set(studies)) != len(studies):
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "invalid_configuration",
                f"diagnostics name 1 to {strategy_diagnostics.MAX_STUDIES} distinct studies",
                "study_ids",
                "out_of_domain",
            )
        root = walk_forward_study.studies_root(self.configuration.artifacts)
        known = set(walk_forward_study.study_ids(root))
        for study in studies:
            if study not in known:
                raise ApiError(
                    HTTPStatus.UNPROCESSABLE_ENTITY, "study_not_found", f"no such study {study}", "study_ids"
                )
        request = {"study_ids": sorted(studies)}
        key = hashlib.sha256(
            canonical_bytes({"kind": "strategy_diagnostics", "request": request})
        ).hexdigest()
        job, created = self.store.create(request, key, "strategy_diagnostics")
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    def _diagnostics(self, diagnostic_id: str) -> tuple[dict[str, Any], dict[str, bytes]]:
        if not is_artifact_id(diagnostic_id):
            raise malformed(
                "invalid_identifier", "a diagnostics ID is 64 lowercase hexadecimal digits", "diagnostic_id"
            )
        root = strategy_diagnostics.diagnostics_root(self.configuration.artifacts)
        if diagnostic_id not in strategy_diagnostics.diagnostic_ids(root):
            raise ApiError(HTTPStatus.NOT_FOUND, "diagnostics_not_found", "no such strategy diagnostics")
        try:
            return strategy_diagnostics.verified_diagnostics(root, diagnostic_id)
        except DiagnosticsError as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY,
                "artifact_invalid",
                f"the diagnostics cannot be read: {error}",
            ) from error

    def strategy_diagnostics_list(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        root = strategy_diagnostics.diagnostics_root(self.configuration.artifacts)
        study = query.get("study")
        if study is not None and not is_artifact_id(study):
            raise malformed("invalid_identifier", "study is a 64-digit lowercase study ID", "study")
        items, invalid = [], []
        for identifier in strategy_diagnostics.diagnostic_ids(root):
            try:
                entry = strategy_diagnostics.listing(
                    strategy_diagnostics.verified_diagnostics(root, identifier)[0]
                )
            except (DiagnosticsError, KeyError, TypeError) as error:
                invalid.append({"diagnostic_id": identifier, "reason": str(error)})
                continue
            if study is None or study in [member["study_id"] for member in entry["studies"]]:
                items.append(entry)
        page = page_after_id(
            [item["diagnostic_id"] for item in items],
            limit_of(query),
            query.get("cursor"),
            f"diagnostics-{study}",
        )
        selected = [item for item in items if item["diagnostic_id"] in set(page.items)]
        return HTTPStatus.OK, {
            "diagnostics": selected,
            "invalid_entries": invalid,
            "next_cursor": page.next_cursor,
        }

    def strategy_diagnostics(self, diagnostic_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        manifest, _ = self._diagnostics(diagnostic_id)
        return HTTPStatus.OK, {
            "diagnostics": with_timestamps(manifest),
            "verification": self._diagnostics_verification(manifest),
        }

    def _diagnostics_verification(self, manifest: Mapping[str, Any]) -> dict[str, Any]:
        """Each level of verification kept apart: what this read checked now,
        what generation checked, and what only the CLI does. Nothing here
        rebuilds, audits, or replays."""
        verification: dict[str, Any] = json.loads(json.dumps(DIAGNOSTICS_VERIFICATION))
        verification["sources"] = {
            "at_generation": manifest["verification"],
            "now": diagnostic_sources.current_sources(self.configuration.artifacts, manifest, self.digests),
        }
        return verification

    def _diagnostic_rows(
        self, diagnostic_id: str, name: str, query: Mapping[str, str]
    ) -> tuple[list[dict[str, Any]], str | None]:
        manifest, files = self._diagnostics(diagnostic_id)
        run_id = query.get("run")
        if run_id is None or not is_artifact_id(run_id):
            raise malformed("invalid_identifier", "run is a 64-digit lowercase run ID", "run")
        known = {
            entry["run_id"]
            for section in manifest["studies"]
            for fold in section["folds"]
            for entry in fold["runs"]
            if "run_id" in entry
        }
        if run_id not in known:
            raise ApiError(HTTPStatus.NOT_FOUND, "run_not_found", "these diagnostics have no such run")
        account = query.get("account")
        if account is not None and account not in ACCOUNTS:
            raise malformed("unknown_enum_value", f"account is one of {', '.join(ACCOUNTS)}", "account")
        rows = [
            row
            for row in (json.loads(line) for line in files[name].splitlines())
            if row["run_id"] == run_id and (account is None or row["account"] == account)
        ]
        page = paginate(
            rows, limit_of(query), query.get("cursor"), name, diagnostic_id, run_id, account or "all"
        )
        return [with_timestamps(row) for row in page.items], page.next_cursor

    def strategy_diagnostics_fills(
        self, diagnostic_id: str, query: Mapping[str, str]
    ) -> tuple[HTTPStatus, Any]:
        rows, cursor = self._diagnostic_rows(diagnostic_id, strategy_diagnostics.FILLS, query)
        return HTTPStatus.OK, {"fills": rows, "next_cursor": cursor}

    def strategy_diagnostics_episodes(
        self, diagnostic_id: str, query: Mapping[str, str]
    ) -> tuple[HTTPStatus, Any]:
        rows, cursor = self._diagnostic_rows(diagnostic_id, strategy_diagnostics.EPISODES, query)
        return HTTPStatus.OK, {"episodes": rows, "next_cursor": cursor}

    def _manifest(self, run_id: str) -> dict[str, Any]:
        if not is_artifact_id(run_id):
            raise malformed("invalid_identifier", "a result ID is 64 lowercase hexadecimal digits", "run_id")
        if run_id not in self.artifacts.result_ids():
            raise ApiError(HTTPStatus.NOT_FOUND, "result_not_found", "no such result")
        try:
            return self.artifacts.result_manifest(run_id)
        except (ArtifactUnavailableError, OSError, ValueError, KeyError, TypeError) as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the result cannot be read: {error}"
            ) from error

    def _journal(self, run_id: str) -> list[dict[str, Any]]:
        self._manifest(run_id)
        try:
            return self.artifacts.journal(run_id)
        except (ArtifactUnavailableError, OSError, ValueError) as error:
            raise ApiError(
                HTTPStatus.UNPROCESSABLE_ENTITY, "artifact_invalid", f"the journal cannot be read: {error}"
            ) from error

    def results(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        page = page_after_id(self.artifacts.result_ids(), limit_of(query), query.get("cursor"), "results")
        items, invalid = [], []
        for run_id in page.items:
            try:
                manifest = self.artifacts.result_manifest(run_id)
                # A registered model's run is listed in its comparison, and a
                # fold model's in its walk-forward study.
                if manifest["schema"] == EXPERIMENT_RUN_SCHEMA:
                    items.append(result_listing(manifest))
            except (ArtifactUnavailableError, OSError, ValueError, KeyError, TypeError) as error:
                invalid.append({"reason": str(error) or type(error).__name__, "run_id": run_id})
        return HTTPStatus.OK, {"invalid_entries": invalid, "next_cursor": page.next_cursor, "results": items}

    def result(self, run_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        return HTTPStatus.OK, {"result": result_summary(self._manifest(run_id))}

    def decisions(self, run_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        entries = [entry for entry in self._journal(run_id) if entry["type"] == "decision"]
        page = paginate(entries, limit_of(query), query.get("cursor"), "decisions", run_id)
        return HTTPStatus.OK, {
            "decisions": with_timestamps([_entry(item) for item in page.items]),
            "next_cursor": page.next_cursor,
        }

    def trades(self, run_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        account = query.get("account", "strategy")
        if account not in ACCOUNTS:
            raise malformed("unknown_enum_value", f"account is one of {', '.join(ACCOUNTS)}", "account")
        journal = self._journal(run_id)
        orders = {entry["client_order_id"]: entry for entry in journal if entry["type"] == "order"}
        trades = []
        for entry in journal:
            if entry.get("account") != account:
                continue
            order = (
                orders[entry["client_order_id"]] if entry["type"] in ("execution", "order_expired") else None
            )
            if order is None:
                continue
            trade = {**_entry(entry), "quantity": order["quantity"], "side": order["side"]}
            if entry["type"] == "order_expired":
                trade.update(outcome="expired", signal_at_ns=order["signal_at_ns"])
            trades.append(trade)
        page = paginate(trades, limit_of(query), query.get("cursor"), "trades", run_id, account)
        return HTTPStatus.OK, {"next_cursor": page.next_cursor, "trades": with_timestamps(page.items)}

    def equity(self, run_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        points = [_entry(entry) for entry in self._journal(run_id) if entry["type"] == "valuation"]
        page = paginate(points, limit_of(query), query.get("cursor"), "equity", run_id)
        return HTTPStatus.OK, {"next_cursor": page.next_cursor, "points": with_timestamps(page.items)}


def _entry(entry: Mapping[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in entry.items() if key != "record"}


class MethodNotAllowedError(ApiError):
    def __init__(self, allow: str) -> None:
        super().__init__(HTTPStatus.METHOD_NOT_ALLOWED, "method_not_allowed", f"use {allow}")
        self.allow = allow


def handler_for(service: Service) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "chronos-research/1"
        sys_version = ""
        # A client that stops sending mid-request does not hold a thread.
        timeout = 30

        def log_message(self, format: str, *args: Any) -> None:
            return

        def _send(self, status: HTTPStatus, document: Any, headers: Mapping[str, str] | None = None) -> None:
            data = json.dumps(document, sort_keys=True).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            # As the trading API marks its answers, so a proxy error page is never mistaken for one.
            self.send_header("X-Chronos-Service", "research")
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(data)

        def _body(self, media_type: str, limit: int) -> bytes:
            content_type = self.headers.get("Content-Type", "")
            parameter = content_type.split(";", 1)[1].strip().lower() if ";" in content_type else None
            allowed = (None, "charset=utf-8") if media_type == "application/json" else (None,)
            if content_type.split(";")[0].strip().lower() != media_type or parameter not in allowed:
                raise ApiError(
                    HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "unsupported_media_type", f"send {media_type}"
                )
            length_text = self.headers.get("Content-Length", "")
            if not _UNSIGNED.fullmatch(length_text):
                raise malformed("invalid_json", "a body needs a Content-Length")
            length = int(length_text)
            if length > limit:
                size = "16 KiB" if limit == MAX_BODY_BYTES else f"{limit // 1024 // 1024} MiB"
                raise ApiError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "payload_too_large", f"the body exceeds {size}"
                )
            data = self.rfile.read(length)
            if len(data) != length:
                raise malformed("incomplete_body", "the body is shorter than its Content-Length")
            return data

        def _handle(self, method: str) -> None:
            try:
                host = (self.headers.get("Host") or "").rsplit(":", 1)[0].strip("[]")
                if host not in LOOPBACK:
                    raise ApiError(
                        HTTPStatus.MISDIRECTED_REQUEST, "misdirected_request", "use a loopback host name"
                    )
                parts = urlsplit(self.path)
                status, document = service.route(method, parts.path, parts.query, self._body)
                self._send(status, document)
            except MethodNotAllowedError as error:
                self._send(error.status, error.body(), {"Allow": error.allow})
            except ApiError as error:
                self._send(error.status, error.body())
            except Exception as error:  # One JSON document even for the unexpected.
                self._send(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    ApiError(HTTPStatus.INTERNAL_SERVER_ERROR, "internal_error", type(error).__name__).body(),
                )

        def do_GET(self) -> None:
            self._handle("GET")

        def do_POST(self) -> None:
            self._handle("POST")

        def do_PUT(self) -> None:
            self._handle("PUT")

        def do_PATCH(self) -> None:
            self._handle("PATCH")

        def do_DELETE(self) -> None:
            self._handle("DELETE")

    return Handler


def start(configuration: ServiceConfiguration) -> tuple[ThreadingHTTPServer, Service]:
    if configuration.host not in LOOPBACK:
        raise ValueError("the research service listens on a loopback address only")
    service = Service(configuration)
    server = ThreadingHTTPServer((configuration.host, configuration.port), handler_for(service))
    server.daemon_threads = True
    return server, service


def serve(configuration: ServiceConfiguration) -> None:
    server, service = start(configuration)

    def stop(signum: int, frame: Any) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    print(
        json.dumps(
            {
                "listening": f"http://{configuration.host}:{server.server_port}{API}",
                "service": "chronos-research",
            }
        ),
        flush=True,
    )
    try:
        server.serve_forever()
    finally:
        service.workers.stop()
        server.server_close()


def iterate_pages(fetch: Callable[[str | None], Mapping[str, Any]], key: str) -> Iterator[Any]:
    """Every item of a paginated list, for scripts and tests."""
    cursor: str | None = None
    while True:
        page = fetch(cursor)
        yield from page[key]
        cursor = page["next_cursor"]
        if cursor is None:
            return


__all__ = ["CLASS_ORDER", "ServiceConfiguration", "serve", "start"]
