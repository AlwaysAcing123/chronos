"""A request plan for real recordings: what to acquire, where, and how the
recordings will be evaluated, fixed before anything is downloaded.

    chronos.recording_request_plan/1

A plan binds the provider query exactly (Databento EQUS.MINI, MBP-1,
raw-symbol input, instrument-ID output), its sorted symbols, and for each
recording a unique stream ID, a UTC window (start inclusive, end exclusive,
whole minutes; Databento filters MBP-1 on ts_recv), a declared session
label, and either a destination under the state root's `recordings/` or an
existing recording already on disk, with its size and SHA-256. It also
freezes the evaluation: instruments, label horizon, minimum training
members, the strategy's eight options, and the fee/latency scenarios, each
checked by the walk-forward request's own rules (walk_forward.validated_request)
so a plan never admits a study the planner would refuse.

Every value is a string, every key is known, and nothing is duplicated or
ambiguous: windows are strictly disjoint and in start order. The plan ID is
the SHA-256 of its canonical JSON, so an approval names one exact plan.
Planning is offline: it never reads a credential or contacts a provider.
"""

import itertools
import os
import re
from collections.abc import Mapping
from datetime import UTC, datetime
from typing import Any

from chronos_research.collection import MAX_SESSION_LABEL, MAX_SOURCES
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.walk_forward import MAX_FOLDS, PlanError, validated_request

SCHEMA = {"name": "chronos.recording_request_plan", "version": "1"}
PROVIDER = {
    "dataset": "EQUS.MINI",
    "input_symbology": "raw_symbol",
    "name": "databento",
    "output_symbology": "instrument_id",
    "schema": "mbp-1",
}
MIN_RECORDINGS = 2
MAX_SYMBOLS = 8
MAX_WINDOW_MINUTES = 60
MAX_HORIZON_NS = 3_600 * 10**9
RECORDINGS_DIRECTORY = "recordings"
_PLAN_KEYS = {"evaluation", "provider", "recordings", "schema", "symbols"}
_EVALUATION_KEYS = {"configuration", "horizon_ns", "instruments", "minimum_training_sources", "scenarios"}
_RECORDING_KEYS = {"end", "session_label", "start", "stream_id"}
_EXISTING_KEYS = {"path", "sha256", "size_bytes"}
_SYMBOL = re.compile(r"[A-Z][A-Z0-9.]{0,9}")
_STREAM = re.compile(r"[a-z0-9]+(-[a-z0-9]+)*")
_TIME = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:00Z")
_PATH_PART = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,99}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_POSITIVE = re.compile(r"[1-9][0-9]{0,17}")
_PLACEHOLDER_COLLECTION = "0" * 64
RECORDING_SUFFIX = ".dbn.zst"


class RecordingPlanError(Exception):
    """A request plan is not valid; `reason` says which rule it breaks."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def _refuse(message: str) -> RecordingPlanError:
    return RecordingPlanError("invalid_plan", message)


def _exact_keys(document: Any, keys: set[str], what: str) -> dict[str, Any]:
    if not isinstance(document, dict) or set(document) != keys:
        raise _refuse(f"{what} has exactly {', '.join(sorted(keys))}")
    return document


def _text(value: Any, what: str) -> str:
    if not isinstance(value, str) or not value:
        raise _refuse(f"{what} is a nonempty string")
    return value


def _instant(text: Any, what: str) -> datetime:
    """A UTC instant on a whole minute, written YYYY-MM-DDTHH:MM:00Z."""
    if not isinstance(text, str) or not _TIME.fullmatch(text):
        raise _refuse(f"{what} is a UTC time on a whole minute, written YYYY-MM-DDTHH:MM:00Z")
    try:
        return datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=UTC)
    except ValueError as error:
        raise _refuse(f"{what} is not a real date and time") from error


def instant_ns(text: str) -> int:
    """Nanoseconds since the epoch of a validated plan time."""
    return int(_instant(text, "time").timestamp()) * 10**9


def _symbols(document: Any) -> list[str]:
    if not isinstance(document, list) or not 1 <= len(document) <= MAX_SYMBOLS:
        raise _refuse(f"symbols is a list of 1 to {MAX_SYMBOLS} raw symbols")
    if not all(isinstance(symbol, str) and _SYMBOL.fullmatch(symbol) for symbol in document):
        raise _refuse("a symbol is an uppercase raw symbol such as AAPL or BRK.B")
    if document != sorted(set(document)):
        raise _refuse("symbols are unique and sorted")
    return list(document)


def _session_label(value: Any, stream: str) -> str:
    label = _text(value, f"recording {stream}: session_label")
    if len(label) > MAX_SESSION_LABEL or not label.isprintable():
        raise _refuse(f"recording {stream}: session_label is printable text of at most {MAX_SESSION_LABEL}")
    return label


def _destination(value: Any, stream: str) -> str:
    """A relative path under the state root's recordings directory."""
    text = _text(value, f"recording {stream}: destination")
    parts = text.split("/")
    if (
        len(text) > 200
        or not all(_PATH_PART.fullmatch(part) and part not in (".", "..") for part in parts)
        or not text.endswith(RECORDING_SUFFIX)
    ):
        raise _refuse(
            f"recording {stream}: destination is a relative path of plain names, without . or .., "
            f"ending in {RECORDING_SUFFIX}"
        )
    return text


def _existing(value: Any, stream: str) -> dict[str, str]:
    """A recording already on disk, named by its absolute path, size, and SHA-256."""
    existing = _exact_keys(value, _EXISTING_KEYS, f"recording {stream}: existing")
    path = _text(existing["path"], f"recording {stream}: existing.path")
    if not os.path.isabs(path) or os.path.normpath(path) != path or not path.endswith(RECORDING_SUFFIX):
        raise _refuse(
            f"recording {stream}: existing.path is an absolute, normalized path ending in {RECORDING_SUFFIX}"
        )
    sha256, size = existing["sha256"], existing["size_bytes"]
    if not isinstance(sha256, str) or not _SHA256.fullmatch(sha256):
        raise _refuse(f"recording {stream}: existing.sha256 is 64 lowercase hexadecimal digits")
    if not isinstance(size, str) or not _POSITIVE.fullmatch(size):
        raise _refuse(f"recording {stream}: existing.size_bytes is a positive whole number")
    return {"path": path, "sha256": sha256, "size_bytes": size}


def _recording(document: Any) -> dict[str, Any]:
    if not isinstance(document, dict):
        raise _refuse("a recording is an object")
    source_keys = set(document) - _RECORDING_KEYS
    if set(document) & _RECORDING_KEYS != _RECORDING_KEYS or source_keys not in (
        {"destination"},
        {"existing"},
    ):
        raise _refuse(
            f"a recording has exactly {', '.join(sorted(_RECORDING_KEYS))}, "
            "and one of destination or existing"
        )
    stream = document["stream_id"]
    if not isinstance(stream, str) or len(stream) > 100 or not _STREAM.fullmatch(stream):
        raise _refuse("a stream_id is lowercase letters and digits joined by single hyphens, at most 100")
    start = _instant(document["start"], f"recording {stream}: start")
    end = _instant(document["end"], f"recording {stream}: end")
    minutes = (end - start).total_seconds() / 60
    if not 1 <= minutes <= MAX_WINDOW_MINUTES:
        raise _refuse(f"recording {stream}: the window is 1 to {MAX_WINDOW_MINUTES} minutes, end after start")
    recording: dict[str, Any] = {
        "end": document["end"],
        "session_label": _session_label(document["session_label"], stream),
        "start": document["start"],
        "stream_id": stream,
    }
    if "destination" in document:
        recording["destination"] = _destination(document["destination"], stream)
    else:
        recording["existing"] = _existing(document["existing"], stream)
    return recording


def _recordings(document: Any) -> list[dict[str, Any]]:
    if not isinstance(document, list) or not MIN_RECORDINGS <= len(document) <= MAX_SOURCES:
        raise _refuse(f"recordings is a list of {MIN_RECORDINGS} to {MAX_SOURCES} recordings")
    recordings = [_recording(entry) for entry in document]
    for key in ("stream_id", "destination"):
        values = [recording[key] for recording in recordings if key in recording]
        if len(set(values)) != len(values):
            raise _refuse(f"every recording's {key} is unique")
    paths = [recording["existing"]["path"] for recording in recordings if "existing" in recording]
    if len(set(paths)) != len(paths):
        raise _refuse("every existing recording is a different file")
    for earlier, later in itertools.pairwise(recordings):
        if instant_ns(earlier["end"]) > instant_ns(later["start"]):
            raise _refuse(
                f"recordings {earlier['stream_id']} and {later['stream_id']} overlap or are out of order: "
                "windows are strictly disjoint and listed by start"
            )
    return recordings


def walk_forward_request(
    evaluation: Mapping[str, Any], collection_id: str, instrument: str
) -> dict[str, Any]:
    """The walk-forward request the plan freezes for one instrument."""
    return {
        "collection_id": collection_id,
        "configuration": dict(evaluation["configuration"]),
        "instrument": instrument,
        "minimum_training_sources": evaluation["minimum_training_sources"],
        "scenarios": [dict(cell) for cell in evaluation["scenarios"]],
        "schema": {"name": "chronos.walk_forward_request", "version": "1"},
    }


def validated_evaluation(document: Any, symbols: list[str], recordings: int) -> dict[str, Any]:
    """The frozen evaluation settings, held to the walk-forward planner's
    own rules and limits for this many recordings."""
    evaluation = _exact_keys(document, _EVALUATION_KEYS, "evaluation")
    instruments = evaluation["instruments"]
    if (
        not isinstance(instruments, list)
        or not instruments
        or instruments != sorted(set(instruments))
        or not set(instruments) <= set(symbols)
    ):
        raise _refuse("evaluation.instruments is a sorted, unique, nonempty list of the plan's symbols")
    horizon = evaluation["horizon_ns"]
    if not isinstance(horizon, str) or not _POSITIVE.fullmatch(horizon) or int(horizon) > MAX_HORIZON_NS:
        raise _refuse(f"evaluation.horizon_ns is a whole number from 1 to {MAX_HORIZON_NS}")
    try:
        requests = [
            validated_request(walk_forward_request(evaluation, _PLACEHOLDER_COLLECTION, instrument))
            for instrument in instruments
        ]
    except PlanError as error:
        raise _refuse(f"evaluation: {error}") from error
    minimum = int(evaluation["minimum_training_sources"])
    if minimum >= recordings or recordings - minimum > MAX_FOLDS:
        raise _refuse(
            f"evaluation.minimum_training_sources leaves 1 to {MAX_FOLDS} folds for {recordings} recordings"
        )
    return {
        "configuration": requests[0]["configuration"],
        "horizon_ns": horizon,
        "instruments": list(instruments),
        "minimum_training_sources": evaluation["minimum_training_sources"],
        "scenarios": requests[0]["scenarios"],
    }


def validated_plan(document: Any) -> dict[str, Any]:
    """The plan, every rule checked; refuses with RecordingPlanError."""
    plan = _exact_keys(document, _PLAN_KEYS, "a recording request plan")
    if plan["schema"] != SCHEMA:
        raise _refuse(f"the plan's schema is {SCHEMA}")
    if plan["provider"] != PROVIDER:
        raise RecordingPlanError(
            "unsupported_provider",
            f"the only supported provider binding is {canonical_json(PROVIDER).decode()}",
        )
    symbols = _symbols(plan["symbols"])
    recordings = _recordings(plan["recordings"])
    return {
        "evaluation": validated_evaluation(plan["evaluation"], symbols, len(recordings)),
        "provider": dict(PROVIDER),
        "recordings": recordings,
        "schema": SCHEMA,
        "symbols": symbols,
    }


def plan_identity(plan: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json(plan))


def query_of(plan: Mapping[str, Any], recording: Mapping[str, Any]) -> dict[str, Any]:
    """The query as chronos_databento_historical echoes it."""
    return {
        "dataset": plan["provider"]["dataset"],
        "end": recording["end"],
        "input_symbology": plan["provider"]["input_symbology"],
        "output_symbology": plan["provider"]["output_symbology"],
        "schema": plan["provider"]["schema"],
        "start": recording["start"],
        "symbols": list(plan["symbols"]),
    }


def recording_by_stream(plan: Mapping[str, Any], stream: str) -> dict[str, Any]:
    for recording in plan["recordings"]:
        if recording["stream_id"] == stream:
            return dict(recording)
    raise RecordingPlanError("unknown_recording", f"the plan has no recording {stream}")
