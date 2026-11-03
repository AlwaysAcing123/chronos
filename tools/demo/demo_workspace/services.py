"""The child services the supervisor owns: each in its own process group,
with an allow-listed environment and its output in its own log.

A child is ready only when all of these hold:

- its own output announces the address it bound;
- it is still alive;
- its address answers with its own identity, such as the trading API's
  paper header or the built index.html.

A random service answering 200 on a port is never taken for it. A child is
stopped through the handle the supervisor holds: SIGTERM to its process
group, then SIGKILL to whatever overstays, the child or anything it started
in the group. A group is signalled only while its leader is unreaped, so a
recycled process ID is never signalled (demo_workspace.processes).
"""

import email.message
import json
import re
import subprocess
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import IO, Any

from demo_workspace.files import WorkspaceError, appending
from demo_workspace.processes import OwnedProcess

_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
# How long each child gets to stop after SIGTERM before SIGKILL.
STOP_TIMEOUTS_S = {"web": 10.0, "trading": 30.0, "research": 15.0}


@dataclass(frozen=True)
class Response:
    status: int
    # Lower-case names: a proxy (Vite's, for one) may change their case.
    headers: Mapping[str, str]
    body: bytes

    def header(self, name: str) -> str | None:
        return self.headers.get(name.lower())

    def json(self) -> Any:
        return json.loads(self.body)


def _names(headers: email.message.Message) -> dict[str, str]:
    return {name.lower(): value for name, value in headers.items()}


def http(url: str, timeout_s: float = 5, method: str = "GET") -> Response | None:
    """One loopback request, never through a proxy; None if nothing answers."""
    data = b"{}" if method == "POST" else None
    request = urllib.request.Request(url, method=method, data=data)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    try:
        with _OPENER.open(request, timeout=timeout_s) as answer:
            return Response(answer.status, _names(answer.headers), answer.read())
    except urllib.error.HTTPError as error:
        return Response(error.code, _names(error.headers), error.read())
    except (OSError, ValueError):
        return None


@dataclass(frozen=True)
class ChildSpec:
    name: str
    command: list[str]
    cwd: Path
    environment: dict[str, str]
    log: Path
    # The line in the child's own output that says it bound its address.
    announcement: re.Pattern[str]


@dataclass
class Child:
    spec: ChildSpec
    process: OwnedProcess | None = None
    started_at: str = ""
    _announced: threading.Event = field(default_factory=threading.Event)
    _reader: threading.Thread | None = None

    def start(self) -> None:
        # Opened first, so an unsafe log is refused before the child exists.
        log = appending(self.spec.log)
        try:
            self.process = OwnedProcess(
                self.spec.command,
                self.spec.cwd,
                self.spec.environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
        except BaseException:
            log.close()
            raise
        self.started_at = datetime.now(UTC).isoformat(timespec="seconds")
        stream = self.process.popen.stdout
        assert stream is not None
        self._reader = threading.Thread(
            target=self._copy, args=(stream, log), daemon=True
        )
        self._reader.start()

    def _copy(self, stream: IO[bytes], log: IO[bytes]) -> None:
        with log, stream:
            for line in stream:
                log.write(line)
                log.flush()
                text = line.decode(errors="replace")
                if self.spec.announcement.search(text):
                    self._announced.set()

    @property
    def pid(self) -> int | None:
        return None if self.process is None else self.process.pid

    def alive(self) -> bool:
        return self.process is not None and not self.process.exited()

    def exit_code(self) -> int | None:
        """Once it is stopped; a negative code is the signal that ended it."""
        return None if self.process is None else self.process.returncode

    def announced(self) -> bool:
        return self._announced.is_set()

    def survivors(self) -> list[int]:
        """Processes of its group that outlived its stop."""
        return [] if self.process is None else self.process.survivors

    def record(self) -> dict[str, Any]:
        return {
            "command": self.spec.command[0],
            # None until it is stopped.
            "exit_code": self.exit_code(),
            "log": str(self.spec.log),
            "name": self.spec.name,
            "pid": self.pid,
            "started_at": self.started_at,
        }

    def stop(self, timeout_s: float) -> int | None:
        """Stops the child's whole process group (OwnedProcess.stop): the exit
        code, or None if it had not started or could not be stopped."""
        if self.process is None:
            return None
        code = self.process.stop(timeout_s)
        if self._reader is not None:
            self._reader.join(timeout=5)
        return code


def wait_ready(
    child: Child, probe: Callable[[], str | None], deadline_s: float
) -> None:
    """Until the child announced its address and `probe` finds it serving as
    itself (None); a child that exits or overstays fails the startup."""
    deadline = time.monotonic() + deadline_s
    problem = "it did not announce its address"
    while time.monotonic() < deadline:
        if not child.alive():
            # Whatever it started goes with it, before its code is known.
            code = child.stop(STOP_TIMEOUTS_S.get(child.spec.name, 10.0))
            raise WorkspaceError(
                f"{child.spec.name} exited during startup (exit {code}); "
                f"see {child.spec.log}"
            )
        if child.announced():
            try:
                found = probe()
            except (KeyError, TypeError, ValueError) as error:
                found = f"it answered unexpectedly ({type(error).__name__})"
            if found is None:
                return
            problem = found
        time.sleep(0.2)
    raise WorkspaceError(
        f"{child.spec.name} was not ready within {deadline_s:.0f} s ({problem}); "
        f"see {child.spec.log}"
    )
