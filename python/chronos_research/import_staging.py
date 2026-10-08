"""Staged imports, in the research service's state directory (ADR 0021).

An import arrives in two requests: its manifest, which is checked at once,
and then its model bytes, which are frozen. The bytes are kept as uploaded,
by their SHA-256; nothing here parses or runs them. An import job reads
them back once, checks the digest, and validates them in the sandbox.

A staged import accepts one model. The same bytes again are accepted as a
repeat; different bytes are refused, so what was reviewed is what is
validated. The oldest staged imports beyond the limit are removed, except
any an import job still needs.
"""

import hashlib
import json
import os
import secrets
import shutil
import threading
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research.artifact_files import ArtifactUnavailableError, read_regular_file
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.model_import import validated_import_manifest
from chronos_research.onnx_profile import MAX_MODEL_BYTES

RECORD = "import.json"
MODEL_FILE = "model.onnx"
MAX_RECORD_BYTES = 64 * 1024
RETAIN_IMPORTS = 64


class StagingError(Exception):
    """A staged import cannot take this request; why is its reason."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


@dataclass
class StagedImport:
    import_id: str
    sequence: int
    created_at: str
    manifest: dict[str, Any]
    model: dict[str, str] | None = None

    @property
    def manifest_sha256(self) -> str:
        return sha256_hex(canonical_json(self.manifest))

    def as_json(self) -> dict[str, Any]:
        return {
            "created_at": self.created_at,
            "import_id": self.import_id,
            "manifest": self.manifest,
            "manifest_sha256": self.manifest_sha256,
            "model": self.model,
            "sequence": str(self.sequence),
        }

    def public(self) -> dict[str, Any]:
        document = self.as_json()
        del document["sequence"]
        return document

    @classmethod
    def from_json(cls, document: Mapping[str, Any]) -> "StagedImport":
        model = document["model"]
        if model is not None and set(model) != {"sha256", "size_bytes", "uploaded_at"}:
            raise ValueError("a staged model records its digest, size, and upload time")
        staged = cls(
            import_id=str(document["import_id"]),
            sequence=int(document["sequence"]),
            created_at=str(document["created_at"]),
            manifest=validated_import_manifest(document["manifest"]),
            model=model,
        )
        if staged.manifest_sha256 != document["manifest_sha256"]:
            raise ValueError("the staged manifest does not match its digest")
        return staged


def _fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_atomically(path: Path, data: bytes) -> None:
    temporary = path.parent / f".{path.name}.{secrets.token_hex(4)}.tmp"
    with open(temporary, "xb") as file:
        file.write(data)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, path)
    _fsync_directory(path.parent)


def read_staged_model(directory: Path, expected_sha256: str) -> tuple[dict[str, Any], bytes]:
    """A staged import's manifest and its model bytes, read once and checked
    against the digest the job was given; for the import job."""
    try:
        record = StagedImport.from_json(json.loads(read_regular_file(directory / RECORD, MAX_RECORD_BYTES)))
        data = read_regular_file(directory / MODEL_FILE, MAX_MODEL_BYTES)
    except (ArtifactUnavailableError, OSError, ValueError, KeyError, TypeError) as error:
        raise StagingError(
            "staged_import_unavailable", f"the staged import cannot be read: {error}"
        ) from error
    if (
        record.model is None
        or record.model["sha256"] != expected_sha256
        or hashlib.sha256(data).hexdigest() != expected_sha256
        or str(len(data)) != record.model["size_bytes"]
    ):
        raise StagingError("corrupt_upload", "the staged model bytes are not the ones uploaded")
    return record.manifest, data


class ImportStore:
    """Staged imports, one directory each, replaced only atomically."""

    def __init__(self, directory: Path, now: Callable[[], str], retain: int = RETAIN_IMPORTS) -> None:
        self.directory = directory
        self.now = now
        self.retain = retain
        self.lock = threading.Lock()
        directory.mkdir(parents=True, exist_ok=True)
        self.imports: dict[str, StagedImport] = {}
        for name in sorted(os.listdir(directory)):
            if name.startswith(".") or not (directory / name).is_dir():
                continue
            try:
                record = json.loads(read_regular_file(directory / name / RECORD, MAX_RECORD_BYTES))
                staged = StagedImport.from_json(record)
            except (ArtifactUnavailableError, OSError, ValueError, KeyError, TypeError):
                # A partial or unreadable record was never a usable import.
                continue
            if staged.import_id == name:
                self.imports[name] = staged

    def path(self, import_id: str) -> Path:
        return self.directory / import_id

    def _save(self, staged: StagedImport) -> None:
        _write_atomically(self.path(staged.import_id) / RECORD, json.dumps(staged.as_json()).encode())
        self.imports[staged.import_id] = staged

    def create(self, manifest: Mapping[str, Any], busy: Callable[[], set[str]]) -> StagedImport:
        with self.lock:
            sequence = max((item.sequence for item in self.imports.values()), default=0) + 1
            staged = StagedImport(
                secrets.token_hex(16), sequence, self.now(), validated_import_manifest(manifest)
            )
            self.path(staged.import_id).mkdir()
            self._save(staged)
            self._prune(busy())
            return staged

    def get(self, import_id: str) -> StagedImport | None:
        with self.lock:
            return self.imports.get(import_id)

    def attach_model(self, import_id: str, data: bytes) -> tuple[StagedImport, bool]:
        """Freezes the import's model bytes, and whether they are new."""
        if not 0 < len(data) <= MAX_MODEL_BYTES:
            raise StagingError("payload_too_large", f"a model is 1 to {MAX_MODEL_BYTES} bytes")
        digest = hashlib.sha256(data).hexdigest()
        with self.lock:
            staged = self.imports.get(import_id)
            if staged is None:
                raise StagingError("import_not_found", "no such staged import")
            if staged.model is not None:
                if staged.model["sha256"] != digest:
                    raise StagingError(
                        "model_already_uploaded",
                        "this import already has other model bytes; start a new import",
                    )
                return staged, False
            _write_atomically(self.path(import_id) / MODEL_FILE, data)
            staged.model = {"sha256": digest, "size_bytes": str(len(data)), "uploaded_at": self.now()}
            self._save(staged)
            return staged, True

    def _prune(self, busy: set[str]) -> None:
        oldest = sorted(self.imports.values(), key=lambda item: item.sequence)
        for staged in oldest[: max(0, len(oldest) - self.retain)]:
            if staged.import_id in busy:
                continue
            shutil.rmtree(self.path(staged.import_id), ignore_errors=True)
            del self.imports[staged.import_id]
