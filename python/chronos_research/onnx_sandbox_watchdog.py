"""Removes one ONNX sandbox container when its owner goes (ADR 0021).

`python -I onnx_sandbox_watchdog.py <docker> <container>`, started by a
SandboxSession before its container, in a session of its own and with the
Docker CLI's minimal environment. Its stdin is a pipe whose only write end
the session holds:

    "closed", then end of input   the session closed and removed the
                                  container itself: check that it is gone
    end of input alone            the owner died, however it died: remove
                                  the container, then look again after a
                                  settle, for a create still under way

It exits 0 once Docker no longer lists the container and 1 if it cannot
confirm that within DEADLINE_S. It uses the standard library only and never
imports the package whose containers it removes.
"""

import contextlib
import subprocess
import sys
import time

DEADLINE_S = 60.0
SETTLE_S = 2.0
DOCKER_TIMEOUT_S = 15.0


def _listed(docker: str, name: str) -> bool | None:
    """Whether Docker lists the container, or None if Docker did not answer."""
    try:
        listed = subprocess.run(
            [docker, "ps", "-a", "--filter", f"name={name}", "--format", "{{.Names}}"],
            capture_output=True,
            text=True,
            timeout=DOCKER_TIMEOUT_S,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if listed.returncode != 0:
        return None
    return name in listed.stdout.split()


def _remove(docker: str, name: str) -> None:
    with contextlib.suppress(OSError, subprocess.TimeoutExpired):
        subprocess.run([docker, "rm", "-f", name], capture_output=True, timeout=DOCKER_TIMEOUT_S, check=False)


def watch(docker: str, name: str) -> int:
    # Blocks until the session closes its end or its owner dies.
    settled = sys.stdin.buffer.read() == b"closed"
    deadline = time.monotonic() + DEADLINE_S
    while True:
        present = _listed(docker, name)
        if present is False and settled:
            return 0
        if present:
            _remove(docker, name)
        elif present is False:
            settled = True
        if time.monotonic() >= deadline:
            return 1
        time.sleep(SETTLE_S if present is False else 0.5)


if __name__ == "__main__":
    sys.exit(watch(sys.argv[1], sys.argv[2]))
