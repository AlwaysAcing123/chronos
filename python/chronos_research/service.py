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

The conventions are the paper-trading API's (docs/paper-trading-api.md):
strict JSON bodies of at most 16 KiB, decimals and integers as strings, RFC
3339 nanosecond timestamps, a fixed error shape, strict query strings, and
opaque cursors with limits from 1 to 200.

Work never runs on a request thread. A POST validates the request and queues
a job; a bounded pool of workers runs each job as one fixed child command,
`python -m chronos_research evaluate-strategy` with an explicit argument
vector, under a time limit. Every job record is written atomically to the
state directory, so finished jobs survive a restart, and a job that was
queued or running when the service stopped is marked interrupted on the next
start, never resumed silently. An identical request while an equal job is
queued, running, or succeeded returns that job instead of new work.

Artifacts are named only by validated IDs, never by client paths. Every
artifact this service serves is read as bytes and verified before use:
the manifest's identity against its directory name and each consumed file
against the manifest's checksum, through no symlink.
"""

import base64
import binascii
import contextlib
import hashlib
import json
import os
import queue
import re
import secrets
import selectors
import signal
import stat
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

from chronos_research.artifacts import CLASS_ORDER, is_artifact_id
from chronos_research.pipeline import ArtifactRoots, Tools

API = "/api/v1/research"
VERSION = "1"
MAX_BODY_BYTES = 16 * 1024
DEFAULT_LIMIT, MAX_LIMIT = 50, 200
MAX_WORKER_OUTPUT_BYTES = 1024 * 1024
LOG_BYTES = 4096
MAX_JOURNAL_BYTES = 256 * 1024 * 1024
MODELS = ("logistic", "majority")
WINDOWS = ("validation", "test")
ACCOUNTS = ("strategy", "buy_and_hold", "cash")
TERMINAL = ("succeeded", "failed", "interrupted")
LOOPBACK = ("127.0.0.1", "::1", "localhost")
_IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
_DECIMAL = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?")
_UNSIGNED = re.compile(r"0|[1-9][0-9]*")
_JOB_ID = re.compile(r"[0-9a-f]{32}")
_INT64_MAX = 2**63 - 1
_DURATIONS = ("latency_ns", "interval_ns", "age_ns", "horizon_ns")

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


def _canonical(document: Any) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


# --- Verified reads -----------------------------------------------------------------------------------


class ArtifactUnavailableError(Exception):
    """An artifact is missing, or its bytes are not what its manifest says."""


def _regular_file(path: Path) -> bytes:
    try:
        # Pin the catalog and artifact directories before opening the leaf.
        # O_NOFOLLOW on the leaf alone does not protect its parent directories.
        with contextlib.ExitStack() as resources:
            catalog = os.open(path.parent.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            resources.callback(os.close, catalog)
            directory = os.open(
                path.parent.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=catalog
            )
            resources.callback(os.close, directory)
            descriptor = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
            file = resources.enter_context(os.fdopen(descriptor, "rb"))
            if not stat.S_ISREG(os.fstat(file.fileno()).st_mode):
                raise ArtifactUnavailableError(f"{path.name} is not a regular file")
            return file.read(MAX_JOURNAL_BYTES + 1)
    except OSError as error:
        raise ArtifactUnavailableError(f"{path.name} cannot be read as a regular artifact file") from error


def artifact_directory(root: Path, artifact_id: str) -> Path:
    """root/artifact_id: a real directory, never through a symlink."""
    if not is_artifact_id(artifact_id):
        raise ArtifactUnavailableError("not an artifact ID")
    directory = root / artifact_id
    try:
        if not stat.S_ISDIR(os.lstat(root).st_mode):
            raise ArtifactUnavailableError("the artifact catalog is not a real directory")
        status = os.lstat(directory)
    except OSError as error:
        raise ArtifactUnavailableError(f"{artifact_id} does not exist") from error
    if not stat.S_ISDIR(status.st_mode):
        raise ArtifactUnavailableError(f"{artifact_id} is not an artifact directory")
    return directory


def verified_manifest(directory: Path, id_key: str, with_newline: bool) -> dict[str, Any]:
    """A manifest whose canonical bytes hash, without its ID, to its ID,
    which names its directory. The C++ artifacts hash the document without
    its newline and the Python ones with it."""
    data = _regular_file(directory / "manifest.json")
    try:
        manifest = json.loads(data)
    except ValueError as error:
        raise ArtifactUnavailableError("manifest.json is not JSON") from error
    if not isinstance(manifest, dict) or _canonical(manifest) + b"\n" != data:
        raise ArtifactUnavailableError("manifest.json is not canonical")
    body = _canonical({key: value for key, value in manifest.items() if key != id_key})
    if hashlib.sha256(body + (b"\n" if with_newline else b"")).hexdigest() != manifest.get(id_key) or (
        manifest.get(id_key) != directory.name
    ):
        raise ArtifactUnavailableError("the manifest's identity does not match its content or directory")
    return manifest


def verified_file(directory: Path, digest: Mapping[str, str]) -> bytes:
    name = digest["name"]
    if not isinstance(name, str) or name in ("", ".", "..") or any(char in name for char in "/\\\x00"):
        raise ArtifactUnavailableError("an artifact file name must be a single filename")
    data = _regular_file(directory / name)
    if len(data) != int(digest["size_bytes"]) or hashlib.sha256(data).hexdigest() != digest["sha256"]:
        raise ArtifactUnavailableError(f"{name} differs from its manifest's checksum")
    return data


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
        if manifest.get("schema") != {"name": "chronos.strategy_run", "version": "1"}:
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


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    document: dict[str, Any] = {}
    for key, value in pairs:
        if key in document:
            raise malformed("duplicate_field", f"{key} appears more than once", key)
        document[key] = value
    return document


def _refuse_number(text: str) -> Any:
    raise malformed("invalid_type", "numbers are written as strings")


def parse_body(data: bytes) -> dict[str, Any]:
    try:
        text = data.decode("utf-8")
        document = json.loads(
            text,
            object_pairs_hook=_no_duplicates,
            parse_float=_refuse_number,
            parse_int=_refuse_number,
            parse_constant=_refuse_number,
        )
    except UnicodeDecodeError as error:
        raise malformed("invalid_json", "the body is not UTF-8") from error
    except ValueError as error:
        raise malformed("invalid_json", f"the body is not JSON: {error}") from error
    if not isinstance(document, dict):
        raise malformed("invalid_type", "the body must be one JSON object")
    return document


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


def _decimal(text: str, field_name: str, scale: int, positive: bool, nonnegative: bool) -> str:
    if not _DECIMAL.fullmatch(text):
        raise malformed("invalid_decimal", f"{field_name} is not a decimal", field_name)
    whole, _, fraction = text.lstrip("-").partition(".")
    fraction = fraction.rstrip("0")
    if len(fraction) > scale:
        raise malformed(
            "too_many_fractional_digits", f"{field_name} has more than {scale} decimals", field_name
        )
    raw = int(whole) * 10**scale + int(fraction.ljust(scale, "0") or "0")
    if text.startswith("-") and raw:
        raw = -raw
    if raw > _INT64_MAX or raw < -_INT64_MAX - 1:
        raise malformed("decimal_out_of_range", f"{field_name} is out of range", field_name)
    if (positive and raw <= 0) or (nonnegative and raw < 0):
        raise ApiError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "invalid_configuration",
            f"{field_name} must be {'positive' if positive else 'nonnegative'}",
            field_name,
            "out_of_domain",
        )
    canonical = whole.lstrip("0") or "0"
    return f"{'-' if raw < 0 else ''}{canonical}{'.' + fraction if fraction else ''}"


def _integer(text: str, field_name: str, minimum: int, maximum: int) -> str:
    if not _UNSIGNED.fullmatch(text):
        raise malformed(
            "invalid_type", f"{field_name} must be a whole number written as a string", field_name
        )
    if not minimum <= int(text) <= maximum:
        raise ApiError(
            HTTPStatus.UNPROCESSABLE_ENTITY,
            "invalid_configuration",
            f"{field_name} must be from {minimum} to {maximum}",
            field_name,
            "out_of_domain",
        )
    return text


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
    configuration = document["configuration"]
    if not isinstance(configuration, dict):
        raise malformed("invalid_type", "configuration must be an object", "configuration")
    _exact_keys(configuration, CONFIGURATION_FIELDS, "configuration.")
    values = {key: _string(configuration, key, "configuration.") for key in CONFIGURATION_FIELDS}
    prefix = "configuration."
    canonical = {
        "decision_interval_ns": _integer(
            values["decision_interval_ns"], prefix + "decision_interval_ns", 1, _INT64_MAX
        ),
        "fee_per_fill": _decimal(values["fee_per_fill"], prefix + "fee_per_fill", 8, False, True),
        "latency_ns": _integer(values["latency_ns"], prefix + "latency_ns", 1, _INT64_MAX),
        "maximum_order_notional": _decimal(
            values["maximum_order_notional"], prefix + "maximum_order_notional", 8, True, False
        ),
        "maximum_quote_age_ns": _integer(
            values["maximum_quote_age_ns"], prefix + "maximum_quote_age_ns", 1, _INT64_MAX
        ),
        "starting_cash": _decimal(values["starting_cash"], prefix + "starting_cash", 8, True, False),
        "target_quantity": _decimal(values["target_quantity"], prefix + "target_quantity", 8, True, False),
        "threshold_ppb": _integer(values["threshold_ppb"], prefix + "threshold_ppb", 0, 1_000_000_000),
    }
    return {
        "configuration": canonical,
        "experiment_id": experiment_id,
        "instrument": instrument,
        "model": model,
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
    start = 0 if cursor is None else decode_cursor(cursor, *scope)
    if start > len(items):
        raise malformed("invalid_cursor", "the cursor is past the end of this list", "cursor")
    page = items[start : start + limit]
    end = start + len(page)
    return Page(page, encode_cursor(*scope, str(end)) if end < len(items) else None)


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

    def as_json(self) -> dict[str, Any]:
        return {
            "created_at": self.created_at,
            "error": self.error,
            "finished_at": self.finished_at,
            "job_id": self.job_id,
            "log": self.log,
            "request": self.request,
            "request_key": self.request_key,
            "result": self.result,
            "sequence": str(self.sequence),
            "started_at": self.started_at,
            "state": self.state,
        }

    def public(self) -> dict[str, Any]:
        document = self.as_json()
        del document["request_key"], document["sequence"]
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

    def create(self, request: dict[str, Any], key: str) -> tuple[Job, bool]:
        """A new queued job, or the equal job already queued, running, or
        succeeded, and whether it is new."""
        with self.lock:
            for job in self.jobs.values():
                if job.request_key == key and job.state in ("queued", "running", "succeeded"):
                    return job, False
            sequence = max((job.sequence for job in self.jobs.values()), default=0) + 1
            job = Job(secrets.token_hex(16), sequence, request, key, "queued", now())
            self._write(job)
            self.jobs[job.job_id] = job
            self._prune()
            return job, True

    def _prune(self) -> None:
        finished = sorted(
            (job for job in self.jobs.values() if job.state in TERMINAL), key=lambda job: job.sequence
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


def job_arguments(configuration: ServiceConfiguration, request: Mapping[str, Any]) -> list[str]:
    """The child command's argument vector: fixed, never a shell string."""
    arguments = [
        configuration.python, "-m", "chronos_research", "evaluate-strategy",
        "--tools", str(configuration.tools.directory), "--artifacts", str(configuration.artifacts),
        "--experiment", request["experiment_id"], "--instrument", request["instrument"],
        "--model", request["model"], "--window", request["window"],
    ]  # fmt: skip
    for key, value in sorted(request["configuration"].items()):
        arguments += ["--" + key.replace("_", "-"), value]
    return arguments


class WorkerOutputLimitError(Exception):
    """A child exceeded the output limit before it finished."""


def _worker_output(child: subprocess.Popen[bytes], timeout_s: int) -> tuple[bytes, bytes]:
    """Drain both pipes without unbounded communicate() buffers.

    The result has a hard byte limit; diagnostics keep only a bounded tail.
    An overall output limit also stops a worker flooding either pipe.
    """
    assert child.stdout is not None
    assert child.stderr is not None
    stdout, stderr = bytearray(), bytearray()
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
                else:
                    stderr.extend(chunk)
                    del stderr[:-LOG_BYTES]
        child.wait(timeout=max(0, deadline - time.monotonic()))
    return bytes(stdout), bytes(stderr)


class Workers:
    def __init__(self, configuration: ServiceConfiguration, store: JobStore) -> None:
        self.configuration = configuration
        self.store = store
        self.queue: queue.Queue[str] = queue.Queue(maxsize=configuration.max_queued)
        self.stopping = threading.Event()
        self.children: dict[str, subprocess.Popen[bytes]] = {}
        self.lock = threading.Lock()
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
            return len(self.children)

    def _loop(self) -> None:
        while not self.stopping.is_set():
            try:
                job_id = self.queue.get(timeout=0.2)
            except queue.Empty:
                continue
            job = self.store.get(job_id)
            if job is None or job.state != "queued":
                continue
            self._run(job)

    def _run(self, job: Job) -> None:
        job.state, job.started_at = "running", now()
        self.store.save(job)
        try:
            with self.lock:
                # Shutdown cannot miss a child between launch and registration.
                if self.stopping.is_set():
                    child = None
                else:
                    child = subprocess.Popen(
                        job_arguments(self.configuration, job.request),
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        stdin=subprocess.DEVNULL,
                        cwd=str(Path(__file__).resolve().parent.parent),
                        # Its own process group also contains its C++ tools.
                        start_new_session=True,
                    )
                    self.children[job.job_id] = child
        except OSError as error:
            self._finish(
                job, "failed", error={"code": "worker_failed", "message": f"cannot start: {error.strerror}"}
            )
            return
        if child is None:
            self._finish(job, "interrupted", error={"code": "interrupted", "message": "the service stopped"})
            return
        try:
            stdout, stderr = _worker_output(child, self.configuration.job_timeout_s)
        except subprocess.TimeoutExpired as error:
            _signal_group(child, signal.SIGKILL)
            _, remaining_stderr = child.communicate()
            self._finish(
                job,
                "failed",
                error={"code": "timed_out", "message": f"stopped after {self.configuration.job_timeout_s} s"},
                log=(error.stderr or b"") + remaining_stderr,
            )
            return
        except WorkerOutputLimitError:
            _signal_group(child, signal.SIGKILL)
            child.communicate()
            self._finish(
                job, "failed", error={"code": "worker_failed", "message": "the job's output was too large"}
            )
            return
        finally:
            if child.stdout is not None:
                child.stdout.close()
            if child.stderr is not None:
                child.stderr.close()
            with self.lock:
                self.children.pop(job.job_id, None)
        if self.stopping.is_set() and child.returncode != 0:
            self._finish(
                job,
                "interrupted",
                error={"code": "interrupted", "message": "the service stopped"},
                log=stderr,
            )
            return
        if child.returncode != 0:
            self._finish(job, "failed", error=_child_error(stderr), log=stderr)
            return
        try:
            output = json.loads(stdout)
            result = {
                "reproduced": output["reproduced"],
                "run_id": output["run"]["artifact_id"],
                "run_outcome": output["run"]["outcome"],
                "schedule_id": output["schedule"]["artifact_id"],
                "schedule_outcome": output["schedule"]["outcome"],
            }
        except (ValueError, KeyError, TypeError):
            self._finish(
                job, "failed", error={"code": "worker_failed", "message": "the job printed no result"}
            )
            return
        self._finish(job, "succeeded", result=result, log=stderr)

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
            children = list(self.children.items())
        for _, child in children:
            _signal_group(child, signal.SIGTERM)
        deadline = time.monotonic() + 1
        for thread in self.threads:
            thread.join(timeout=max(0, deadline - time.monotonic()))
        if any(thread.is_alive() for thread in self.threads):
            # Include descendants even if their leader has already exited.
            for _, child in children:
                _signal_group(child, signal.SIGKILL)
            deadline = time.monotonic() + 5
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


def _child_error(stderr: bytes) -> dict[str, Any]:
    try:
        error = json.loads(stderr)["error"]
        return {"code": "evaluation_failed", "message": str(error["message"]), "kind": str(error.get("kind"))}
    except (ValueError, KeyError, TypeError):
        return {"code": "evaluation_failed", "message": stderr[-LOG_BYTES:].decode("utf-8", errors="replace")}


# --- HTTP -----------------------------------------------------------------------------------------------


class Service:
    def __init__(self, configuration: ServiceConfiguration) -> None:
        self.configuration = configuration
        self.artifacts = Artifacts(ArtifactRoots.under(configuration.artifacts))
        self.store = JobStore(configuration.state / "jobs", configuration.retain_jobs)
        self.workers = Workers(configuration, self.store)

    # Routes.
    def route(self, method: str, path: str, query: str, body: Callable[[], bytes]) -> tuple[HTTPStatus, Any]:
        if path == API + "/runs" and method == "POST":
            parse_query(query, ())
            return self.start_run(body())
        routes: list[tuple[re.Pattern[str], Callable[..., tuple[HTTPStatus, Any]], tuple[str, ...]]] = [
            (re.compile(r"/status"), self.status, ()),
            (re.compile(r"/experiments"), self.experiments, ("limit", "cursor")),
            (re.compile(r"/experiments/([^/]+)"), self.experiment, ()),
            (re.compile(r"/runs"), self.runs, ("limit", "cursor")),
            (re.compile(r"/runs/([^/]+)"), self.run, ()),
            (re.compile(r"/results"), self.results, ("limit", "cursor")),
            (re.compile(r"/results/([^/]+)"), self.result, ()),
            (re.compile(r"/results/([^/]+)/decisions"), self.decisions, ("limit", "cursor")),
            (re.compile(r"/results/([^/]+)/trades"), self.trades, ("account", "limit", "cursor")),
            (re.compile(r"/results/([^/]+)/equity"), self.equity, ("limit", "cursor")),
        ]
        if not path.startswith(API + "/"):
            raise ApiError(HTTPStatus.NOT_FOUND, "route_not_found", "no such path")
        tail = path[len(API) :]
        for pattern, handler, allowed in routes:
            matched = pattern.fullmatch(tail)
            if matched is None:
                continue
            if method != "GET":
                allow = "GET, POST" if tail == "/runs" else "GET"
                raise MethodNotAllowedError(allow)
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
        page = paginate(self.artifacts.experiment_ids(), limit_of(query), query.get("cursor"), "experiments")
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

    def start_run(self, data: bytes) -> tuple[HTTPStatus, Any]:
        request = validate_run_request(parse_body(data))
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
        key = hashlib.sha256(_canonical(request)).hexdigest()
        job, created = self.store.create(request, key)
        if created:
            self.workers.submit(job)
            return HTTPStatus.ACCEPTED, {"deduplicated": False, "job": job.public()}
        return HTTPStatus.OK, {"deduplicated": True, "job": job.public()}

    def runs(self, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        page = paginate(self.store.newest_first(), limit_of(query), query.get("cursor"), "runs")
        return HTTPStatus.OK, {"next_cursor": page.next_cursor, "runs": [job.public() for job in page.items]}

    def run(self, job_id: str, query: Mapping[str, str]) -> tuple[HTTPStatus, Any]:
        if not _JOB_ID.fullmatch(job_id):
            raise malformed("invalid_identifier", "a job ID is 32 lowercase hexadecimal digits", "job_id")
        job = self.store.get(job_id)
        if job is None:
            raise ApiError(HTTPStatus.NOT_FOUND, "run_not_found", "no such run")
        return HTTPStatus.OK, {"run": job.public()}

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
        page = paginate(self.artifacts.result_ids(), limit_of(query), query.get("cursor"), "results")
        items, invalid = [], []
        for run_id in page.items:
            try:
                items.append(result_listing(self.artifacts.result_manifest(run_id)))
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

        def _body(self) -> bytes:
            content_type = self.headers.get("Content-Type", "")
            if content_type.split(";")[0].strip().lower() != "application/json" or (
                ";" in content_type
                and content_type.split(";", 1)[1].strip().lower() not in ("charset=utf-8",)
            ):
                raise ApiError(
                    HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "unsupported_media_type", "send application/json"
                )
            length_text = self.headers.get("Content-Length", "")
            if not _UNSIGNED.fullmatch(length_text):
                raise malformed("invalid_json", "a body needs a Content-Length")
            length = int(length_text)
            if length > MAX_BODY_BYTES:
                raise ApiError(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "payload_too_large", "the body exceeds 16 KiB"
                )
            return self.rfile.read(length)

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
