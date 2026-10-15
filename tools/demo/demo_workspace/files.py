"""Owned files: private real directories, owned files, records, the lock.

Every directory the workspace writes is a real directory owned by this user
and closed to others; a symlink anywhere in the chain is refused rather than
followed, so nothing is ever written into another root through one. A file
is written in one of two ways, and neither writes through a link:

- replaced: records, captured output, and stamps are written to a new file
  that is then renamed over the name (write, fsync, rename). A reader sees
  the old content or the new, never part of either, and a link at the name
  is replaced, never followed or truncated.
- appended: logs. An existing entry must be this user's own plain file with
  no other name; a link, a hard link, or anything else is refused before
  anything runs or is written.

A read never follows a link either. The directories' owner is trusted not
to change them while the supervisor runs (docs/demo-workspace.md).
"""

import contextlib
import errno
import fcntl
import json
import os
import secrets
import stat
from collections.abc import Iterator
from pathlib import Path
from typing import IO, Any


class WorkspaceError(Exception):
    """The workspace cannot continue; the message says what to do."""


def private_directory(path: Path) -> Path:
    """`path` as a real, owner-only directory, created if missing. Its parent
    must already exist; nothing on the way may be a symlink."""
    try:
        status = os.lstat(path)
    except FileNotFoundError:
        # Two starts may create it at once; either one's directory will do.
        with contextlib.suppress(FileExistsError):
            os.mkdir(path, 0o700)
        status = os.lstat(path)
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise WorkspaceError(f"{path} must be a real directory, not a link or a file")
    if status.st_uid != os.getuid():
        raise WorkspaceError(f"{path} belongs to another user")
    if stat.S_IMODE(status.st_mode) & 0o077:
        os.chmod(path, 0o700, follow_symlinks=False)
    return path


def private_tree(root: Path, *parts: str) -> Path:
    """root/parts..., each a private real directory."""
    path = private_directory(root)
    for part in parts:
        path = private_directory(path / part)
    return path


def refuse_link(path: Path) -> None:
    """An existing `path` must be a real directory or file, not a link."""
    with contextlib.suppress(FileNotFoundError):
        if stat.S_ISLNK(os.lstat(path).st_mode):
            raise WorkspaceError(f"{path} is a link; it was not followed")


def _parent(path: Path) -> None:
    status = os.lstat(path.parent)
    if not stat.S_ISDIR(status.st_mode) or status.st_uid != os.getuid():
        raise WorkspaceError(f"{path.parent} must be a real directory of this user")


def _open(path: Path, flags: int, mode: int = 0o600) -> int:
    try:
        return os.open(path, flags | os.O_NOFOLLOW | os.O_CLOEXEC, mode)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise WorkspaceError(f"{path} is a link; it was not followed") from error
        raise


def _plain(path: Path, status: os.stat_result) -> None:
    if (
        not stat.S_ISREG(status.st_mode)
        or status.st_nlink != 1
        or status.st_uid != os.getuid()
    ):
        raise WorkspaceError(
            f"{path} is not this user's own plain file (a link, a hard link, or "
            "another kind of file); it was not written"
        )


def read_file(path: Path) -> bytes | None:
    """A plain file's bytes, or None if it does not exist. A link is never
    followed."""
    try:
        # Non-blocking, so a FIFO put there cannot stall the open.
        descriptor = _open(path, os.O_RDONLY | os.O_NONBLOCK)
    except FileNotFoundError:
        return None
    with os.fdopen(descriptor, "rb") as file:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise WorkspaceError(f"{path} is not a plain file; it was not read")
        return file.read()


def read_json(path: Path) -> dict[str, Any] | None:
    """A record, or None if it does not exist. A link is never followed."""
    data = read_file(path)
    if data is None:
        return None
    document = json.loads(data)
    if not isinstance(document, dict):
        raise WorkspaceError(f"{path} is not a workspace record")
    return document


@contextlib.contextmanager
def replacing(path: Path) -> Iterator[IO[bytes]]:
    """A new file that replaces `path` atomically and durably when the block
    ends; if the block fails, `path` is left as it was."""
    _parent(path)
    temporary = path.with_name(f".{path.name}.{secrets.token_hex(4)}.partial")
    descriptor = _open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    try:
        with os.fdopen(descriptor, "wb") as file:
            yield file
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    except BaseException:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)
        raise
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def write_json(path: Path, document: dict[str, Any]) -> None:
    """Replaces a record atomically and durably."""
    with replacing(path) as file:
        file.write((json.dumps(document, indent=2, sort_keys=True) + "\n").encode())


def appending(path: Path) -> IO[bytes]:
    """`path` opened to append, created owner-only if missing. An existing
    entry that is not this user's own plain file is refused before anything
    is written to it."""
    _parent(path)
    try:
        before: os.stat_result | None = os.lstat(path)
    except FileNotFoundError:
        before = None
    if before is not None:
        _plain(path, before)
    descriptor = _open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT)
    try:
        opened = os.fstat(descriptor)
        _plain(path, opened)
        if before is not None and (opened.st_dev, opened.st_ino) != (
            before.st_dev,
            before.st_ino,
        ):
            raise WorkspaceError(f"{path} changed while it was opened")
    except BaseException:
        os.close(descriptor)
        raise
    return os.fdopen(descriptor, "ab")


def write_secret(path: Path, length: int = 24) -> str:
    """A random secret, created once with owner-only permissions."""
    existing = read_file(path)
    if existing is not None:
        return existing.decode().strip()
    value = secrets.token_urlsafe(length)
    descriptor = _open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
    with os.fdopen(descriptor, "w") as file:
        file.write(value)
        file.flush()
        os.fsync(file.fileno())
    return value


class Lock:
    """An exclusive, non-blocking flock on the root, held for the
    supervisor's lifetime. The kernel releases it when the process dies, so
    a hard-killed supervisor never leaves the root locked."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._descriptor: int | None = None

    def acquire(self) -> bool:
        descriptor = _open(self.path, os.O_RDWR | os.O_CREAT)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(descriptor)
            return False
        if not self._enforced():
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)
            raise WorkspaceError(
                f"{self.path.parent} is on a filesystem that does not enforce file "
                "locks (a network share or a container's bind mount, for instance), "
                "so two supervisors could share it; use a root on a local disk"
            )
        self._descriptor = descriptor
        return True

    def _enforced(self) -> bool:
        """Whether the lock just taken excludes another open of the file, as
        it would another supervisor's."""
        probe = _open(self.path, os.O_RDWR)
        try:
            fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return True
        finally:
            os.close(probe)
        return False

    def held(self) -> bool:
        """Whether this process holds it."""
        return self._descriptor is not None

    def release(self) -> None:
        if self._descriptor is not None:
            fcntl.flock(self._descriptor, fcntl.LOCK_UN)
            os.close(self._descriptor)
            self._descriptor = None

    def held_elsewhere(self) -> bool:
        """Whether another process holds the lock now; this one never keeps it."""
        if not self.acquire():
            return True
        self.release()
        return False


@contextlib.contextmanager
def holding(lock: Lock) -> Iterator[bool]:
    acquired = lock.acquire()
    try:
        yield acquired
    finally:
        if acquired:
            lock.release()
