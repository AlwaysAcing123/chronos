"""A child process held to a deadline and to bounded output.

The child runs in its own session (so its process group can be signalled
as a whole), from a fixed argument vector and an explicit environment. Its
stdout and stderr are read concurrently; when either passes its limit, or
the deadline passes, or the parent itself fails while waiting, the whole
group is killed and reaped. Nothing is written to disk here.
"""

import contextlib
import os
import selectors
import signal
import subprocess
import time
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

EXITED = "exited"
TIMED_OUT = "timed_out"
OVERFLOWED = "output_overflowed"


@dataclass(frozen=True)
class Finished:
    """How the child ended, and the output read before it did."""

    outcome: str
    returncode: int | None
    stdout: bytes
    stderr: bytes


def _kill_group(process: subprocess.Popen[bytes]) -> None:
    with contextlib.suppress(ProcessLookupError):
        os.killpg(process.pid, signal.SIGKILL)
    process.wait()


def run_bounded(
    argv: Sequence[str],
    environment: Mapping[str, str],
    timeout_seconds: float,
    stdout_limit: int,
    stderr_limit: int,
    pass_fds: Sequence[int] = (),
    cwd: Path | None = None,
) -> Finished:
    process = subprocess.Popen(
        list(argv),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=dict(environment),
        start_new_session=True,
        pass_fds=tuple(pass_fds),
        cwd=cwd,
    )
    assert process.stdout is not None
    assert process.stderr is not None
    limits = {process.stdout.fileno(): stdout_limit, process.stderr.fileno(): stderr_limit}
    output: dict[int, bytearray] = {descriptor: bytearray() for descriptor in limits}
    deadline = time.monotonic() + timeout_seconds
    outcome = EXITED
    try:
        with selectors.DefaultSelector() as selector:
            for stream in (process.stdout, process.stderr):
                selector.register(stream, selectors.EVENT_READ)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    outcome = TIMED_OUT
                    break
                for key, _ in selector.select(remaining):
                    chunk = os.read(key.fd, 65_536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    output[key.fd] += chunk
                    if len(output[key.fd]) > limits[key.fd]:
                        outcome = OVERFLOWED
                        break
                if outcome != EXITED:
                    break
        if outcome == EXITED:
            remaining = max(deadline - time.monotonic(), 0.0)
            try:
                process.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                outcome = TIMED_OUT
        if outcome != EXITED:
            _kill_group(process)
    except BaseException:
        _kill_group(process)
        raise
    finally:
        process.stdout.close()
        process.stderr.close()
    stdout, stderr = (bytes(output[descriptor][: limits[descriptor]]) for descriptor in limits)
    return Finished(
        outcome=outcome,
        returncode=process.returncode if outcome == EXITED else None,
        stdout=stdout,
        stderr=stderr,
    )
