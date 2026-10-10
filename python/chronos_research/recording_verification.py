"""Independent verification of a raw recording against the plan that asked for it.

A recording counts only if all of these hold, checked here rather than taken
from whoever wrote the files:

- the file is a regular file whose size and SHA-256 equal its manifest's
  `source` (and, for an existing input, the plan's);
- the manifest names the plan's provider, dataset, schema, symbology,
  requested symbols in order, exact window in nanoseconds, and stream, with
  no symbol left unresolved or partially resolved;
- chronos_databento_replay, offline, recomputes exactly that manifest from
  the bytes.

Nothing here writes, renames, or deletes a recording or its manifest.
"""

import hashlib
import json
import stat
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research.bounded_process import EXITED, run_bounded
from chronos_research.pipeline import DATABENTO_REPLAY, PipelineError, Tools
from chronos_research.recording_plan import instant_ns

MANIFEST_SUFFIX = ".manifest.json"
_MAX_MANIFEST_BYTES = 64 * 1024
_REPLAY_TIMEOUT_SECONDS = 1_800
_REPLAY_STDOUT_LIMIT = 4 * 1024 * 1024
_REPLAY_STDERR_LIMIT = 64 * 1024


class RecordingVerificationError(Exception):
    """A recording or its manifest is not the one the plan asked for."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


@dataclass(frozen=True)
class VerifiedRecordingFile:
    path: Path
    sha256: str
    size_bytes: int
    manifest: dict[str, Any]
    manifest_sha256: str

    def as_json(self) -> dict[str, str]:
        return {
            "manifest_sha256": self.manifest_sha256,
            "path": str(self.path),
            "sha256": self.sha256,
            "size_bytes": str(self.size_bytes),
        }


def manifest_path(recording: Path) -> Path:
    return Path(str(recording) + MANIFEST_SUFFIX)


def _refuse(message: str) -> RecordingVerificationError:
    return RecordingVerificationError("recording_mismatch", message)


def _no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    document: dict[str, Any] = {}
    for key, value in pairs:
        if key in document:
            raise ValueError(f"{key} appears more than once")
        document[key] = value
    return document


def _refuse_constant(text: str) -> Any:
    raise ValueError(f"{text} is not a JSON value")


def _manifest_document(data: bytes, what: str) -> dict[str, Any]:
    try:
        document = json.loads(
            data.decode("utf-8"), object_pairs_hook=_no_duplicates, parse_constant=_refuse_constant
        )
    except (UnicodeDecodeError, ValueError) as error:
        raise _refuse(f"{what} is not one strict JSON document: {error}") from error
    if not isinstance(document, dict):
        raise _refuse(f"{what} is not a JSON object")
    return document


def _regular_file(path: Path, what: str) -> int:
    try:
        status = path.lstat()
    except FileNotFoundError as error:
        raise RecordingVerificationError("recording_missing", f"{what} {path} does not exist") from error
    if not stat.S_ISREG(status.st_mode):
        raise _refuse(f"{what} {path} is not a regular file")
    return status.st_size


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise _refuse(message)


def _check_manifest(
    manifest: Mapping[str, Any], plan: Mapping[str, Any], recording: Mapping[str, Any], path: Path
) -> None:
    stream = recording["stream_id"]
    provider, query = manifest.get("provider"), manifest.get("query")
    normalization, source = manifest.get("normalization"), manifest.get("source")
    _require(manifest.get("manifest_version") == 1, f"{stream}: the manifest is not version 1")
    _require(
        isinstance(provider, dict)
        and (provider.get("name"), provider.get("dataset"), provider.get("schema"))
        == (plan["provider"]["name"], plan["provider"]["dataset"], plan["provider"]["schema"]),
        f"{stream}: the manifest's provider, dataset, or schema is not the plan's",
    )
    _require(
        isinstance(query, dict)
        and query.get("start_nanoseconds") == instant_ns(recording["start"])
        and query.get("end_nanoseconds") == instant_ns(recording["end"])
        and query.get("requested_symbols") == plan["symbols"]
        and query.get("input_symbology") == plan["provider"]["input_symbology"]
        and query.get("output_symbology") == plan["provider"]["output_symbology"],
        f"{stream}: the manifest's query is not the plan's window, symbols, and symbology",
    )
    assert isinstance(query, dict)
    _require(
        query.get("partially_resolved_symbols") == [] and query.get("unresolved_symbols") == [],
        f"{stream}: the manifest reports symbols that did not resolve",
    )
    _require(
        isinstance(normalization, dict) and normalization.get("stream") == stream,
        f"{stream}: the manifest's stream is not the plan's",
    )
    _require(
        isinstance(source, dict) and source.get("file_name") == path.name,
        f"{stream}: the manifest names another file",
    )


def verify_recording_file(
    tools: Tools, plan: Mapping[str, Any], recording: Mapping[str, Any], path: Path
) -> VerifiedRecordingFile:
    """The recording at `path`, verified against the plan's entry for it."""
    stream = recording["stream_id"]
    size = _regular_file(path, "recording")
    manifest_file = manifest_path(path)
    manifest_size = _regular_file(manifest_file, "recording manifest")
    _require(manifest_size <= _MAX_MANIFEST_BYTES, f"{stream}: the manifest is unexpectedly large")
    manifest_bytes = manifest_file.read_bytes()
    manifest = _manifest_document(manifest_bytes, f"{stream}: the manifest")
    _check_manifest(manifest, plan, recording, path)
    sha256 = _file_sha256(path)
    source = manifest["source"]
    _require(
        (source.get("sha256"), source.get("size_bytes")) == (sha256, size),
        f"{stream}: the recording's size or SHA-256 is not its manifest's",
    )
    existing = recording.get("existing")
    if existing is not None:
        _require(
            (existing["sha256"], existing["size_bytes"]) == (sha256, str(size)),
            f"{stream}: the existing recording's size or SHA-256 is not the plan's",
        )
    # The replayed manifest holds the SHA-256 of the bytes the replay read,
    # so equality also shows they are the bytes hashed above.
    replayed = _replayed_manifest(tools, path, stream)
    _require(replayed == manifest, f"{stream}: replaying the recording does not recompute its manifest")
    return VerifiedRecordingFile(
        path=path,
        sha256=sha256,
        size_bytes=size,
        manifest=manifest,
        manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
    )


def _replayed_manifest(tools: Tools, path: Path, stream: str) -> dict[str, Any]:
    """The manifest chronos_databento_replay recomputes from the bytes alone."""
    finished = run_bounded(
        [str(tools.path(DATABENTO_REPLAY)), "--input", str(path), "--stream-id", stream],
        {"PATH": "/usr/bin:/bin"},
        _REPLAY_TIMEOUT_SECONDS,
        _REPLAY_STDOUT_LIMIT,
        _REPLAY_STDERR_LIMIT,
    )
    if finished.outcome != EXITED or finished.returncode != 0:
        detail = finished.stderr.decode("utf-8", "replace")[:2_000]
        raise PipelineError(f"{DATABENTO_REPLAY} could not replay {path.name} ({finished.outcome}): {detail}")
    document = _manifest_document(finished.stdout, f"{DATABENTO_REPLAY}'s output")
    replayed = document.get("manifest")
    if not isinstance(replayed, dict):
        raise _refuse(f"{DATABENTO_REPLAY} printed no manifest for {path.name}")
    return replayed
