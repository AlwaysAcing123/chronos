"""Atomic, never-replacing publication of an artifact directory.

The same guarantees as the C++ artifacts' StagingDirectory: files are
written into a private staging directory beside the destination, synced,
and made read-only; the directory is then renamed to its ID only if nothing
has that name, and made read-only too. A reader sees a complete artifact or
none, and a crash leaves only a staging directory, which every Chronos
listing reports as an interrupted write.

Python's os.rename replaces an empty destination directory, so the rename
goes through the platform call that refuses to: renamex_np(RENAME_EXCL) on
macOS and renameat2(RENAME_NOREPLACE) on Linux. Any other platform refuses
to publish rather than risk replacing an artifact.
"""

import ctypes
import errno
import os
import secrets
import shutil
import stat
import sys
from collections.abc import Callable, Mapping
from enum import Enum
from pathlib import Path

STAGING_PREFIX = ".chronos-staging-"
_RENAME_EXCL = 0x00000004
_RENAME_NOREPLACE = 1
_AT_FDCWD = -100


class PublicationError(Exception):
    pass


class Outcome(Enum):
    PUBLISHED = "published"
    ALREADY_PRESENT = "already_present"


def _rename_without_replacing(source: Path, destination: Path) -> None:
    """Raises OSError with the call's errno, EEXIST when destination exists."""
    libc = ctypes.CDLL(None, use_errno=True)
    function = "renamex_np" if sys.platform == "darwin" else "renameat2"
    supported = sys.platform == "darwin" or sys.platform.startswith("linux")
    if not supported or not hasattr(libc, function):
        raise PublicationError(f"no rename that refuses to replace exists on {sys.platform}")
    if sys.platform == "darwin":
        result = libc.renamex_np(os.fsencode(source), os.fsencode(destination), ctypes.c_uint(_RENAME_EXCL))
    else:
        result = libc.renameat2(
            _AT_FDCWD,
            os.fsencode(source),
            _AT_FDCWD,
            os.fsencode(destination),
            ctypes.c_uint(_RENAME_NOREPLACE),
        )
    if result != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(destination))


def _sync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_read_only(path: Path, contents: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        remaining = memoryview(contents)
        while remaining:
            remaining = remaining[os.write(descriptor, remaining) :]
        os.fsync(descriptor)
        os.fchmod(descriptor, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
    finally:
        os.close(descriptor)


def publish(
    root: Path,
    artifact_id: str,
    files: Mapping[str, bytes],
    manifest_name: str,
    verified_manifest: Callable[[Path], bytes],
) -> tuple[Outcome, str | None]:
    """Publishes `files` as root/artifact_id and returns the outcome and a
    warning, if a step after the rename failed: the artifact is then
    published but may not be read-only or durable yet.

    If something already has that name it is never touched: the outcome is
    ALREADY_PRESENT only when `verified_manifest` fully verifies it and
    returns exactly this manifest, and a PublicationError otherwise."""
    if not root.is_dir():
        raise PublicationError(f"{root} must be an existing directory")
    destination = root / artifact_id
    staging = root / f"{STAGING_PREFIX}{os.getpid()}-{secrets.token_hex(8)}"
    os.mkdir(staging, 0o700)
    renamed = False
    try:
        for name, contents in sorted(files.items()):
            _write_read_only(staging / name, contents)
        _sync_directory(staging)
        try:
            _rename_without_replacing(staging, destination)
            renamed = True
        except OSError as error:
            if error.errno not in (errno.EEXIST, errno.ENOTEMPTY):
                raise PublicationError(f"could not publish {artifact_id}: {error.strerror}") from error
            try:
                existing = verified_manifest(destination)
            except Exception as invalid:
                raise PublicationError(
                    f"an entry named {artifact_id} already exists and is not a valid artifact ({invalid}); "
                    "it was left untouched"
                ) from invalid
            if existing != files[manifest_name]:
                raise PublicationError(
                    f"a different artifact is already published as {artifact_id}"
                ) from None
            return Outcome.ALREADY_PRESENT, None
    finally:
        if not renamed:
            _remove_staging(staging)
    try:
        descriptor = os.open(destination, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fchmod(
                descriptor,
                stat.S_IRUSR | stat.S_IXUSR | stat.S_IRGRP | stat.S_IXGRP | stat.S_IROTH | stat.S_IXOTH,
            )
        finally:
            os.close(descriptor)
        _sync_directory(root)
    except OSError as error:
        return (
            Outcome.PUBLISHED,
            f"published, but could not finish making it read-only and durable: {error.strerror}",
        )
    return Outcome.PUBLISHED, None


def _remove_staging(staging: Path) -> None:
    if staging.exists():
        os.chmod(staging, stat.S_IRWXU)
        shutil.rmtree(staging)
