"""Reading an artifact's files as exactly the bytes that are verified.

An artifact is `<root>/<id>/<file>`. Every read pins the root and the
artifact directory with O_NOFOLLOW descriptors before opening the leaf, so a
symlink or a parent directory swapped for one cannot redirect it, and the
leaf must be a regular file. A caller verifies the bytes it got and uses
those bytes; nothing reopens the path.
"""

import contextlib
import hashlib
import json
import os
import stat
from collections.abc import Mapping
from pathlib import Path
from typing import Any

from chronos_research.artifacts import is_artifact_id

MAX_FILE_BYTES = 256 * 1024 * 1024


class ArtifactUnavailableError(Exception):
    """An artifact is missing, or its bytes are not what its manifest says."""


def read_regular_file(path: Path, limit: int = MAX_FILE_BYTES) -> bytes:
    """The bytes of root/id/name, at most `limit` of them."""
    try:
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
            data = file.read(limit + 1)
    except OSError as error:
        raise ArtifactUnavailableError(f"{path.name} cannot be read as a regular artifact file") from error
    if len(data) > limit:
        raise ArtifactUnavailableError(f"{path.name} exceeds {limit} bytes")
    return data


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


def listed_files(directory: Path) -> list[str]:
    """The names in an artifact directory, opened without following links."""
    try:
        descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError as error:
        raise ArtifactUnavailableError(f"{directory.name} is not an artifact directory") from error
    try:
        return sorted(os.listdir(descriptor))
    finally:
        os.close(descriptor)


def canonical_bytes(document: Any) -> bytes:
    """The service's canonical spelling: sorted keys, no whitespace."""
    return json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def verified_manifest(directory: Path, id_key: str, with_newline: bool) -> dict[str, Any]:
    """A manifest whose canonical bytes hash, without its ID, to its ID,
    which names its directory. The C++ artifacts hash the document without
    its newline and the Python ones with it."""
    data = read_regular_file(directory / "manifest.json")
    try:
        manifest = json.loads(data)
    except ValueError as error:
        raise ArtifactUnavailableError("manifest.json is not JSON") from error
    if not isinstance(manifest, dict) or canonical_bytes(manifest) + b"\n" != data:
        raise ArtifactUnavailableError("manifest.json is not canonical")
    body = canonical_bytes({key: value for key, value in manifest.items() if key != id_key})
    if hashlib.sha256(body + (b"\n" if with_newline else b"")).hexdigest() != manifest.get(id_key) or (
        manifest.get(id_key) != directory.name
    ):
        raise ArtifactUnavailableError("the manifest's identity does not match its content or directory")
    return manifest


def is_leaf_name(name: object) -> bool:
    return (
        isinstance(name, str) and name not in ("", ".", "..") and not any(char in name for char in "/\\\x00")
    )


def verified_file(directory: Path, digest: Mapping[str, str], limit: int = MAX_FILE_BYTES) -> bytes:
    name = digest["name"]
    if not is_leaf_name(name):
        raise ArtifactUnavailableError("an artifact file name must be a single filename")
    data = read_regular_file(directory / name, limit)
    if len(data) != int(digest["size_bytes"]) or hashlib.sha256(data).hexdigest() != digest["sha256"]:
        raise ArtifactUnavailableError(f"{name} differs from its manifest's checksum")
    return data
