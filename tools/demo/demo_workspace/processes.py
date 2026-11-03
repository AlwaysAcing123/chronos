"""A process the supervisor starts in its own process group and owns until
that whole group is gone.

The group is signalled only while its leader is unreaped. An exited but
unreaped leader keeps its process ID, which is also the group's ID, so
neither can name another process or group. The leader's exit is therefore
observed without reaping it: waitid(WNOWAIT) on Linux, and a kqueue exit
event on macOS, whose Python has no waitid. Only once no process of the
group is left, or SIGKILL has been sent and waited on, is the leader reaped.
From then on the group is never signalled again.

This covers the processes a child starts in its group. One that leaves the
group (with setsid, as a daemon does) is not this child's any more.
"""

import contextlib
import os
import select
import signal
import subprocess
import sys
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import IO

# How long termination or the leader's exit notification gets to complete.
_KILL_GRACE_S = 5.0


def group_members(group: int) -> list[int]:
    """The processes of the group that still run; an exited one waiting to be
    reaped (a zombie) does not."""
    try:
        os.killpg(group, 0)
    except ProcessLookupError:
        return []
    except PermissionError:
        pass
    listed = subprocess.run(
        ["ps", "-A", "-o", "pid=,pgid=,stat="],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    members = []
    for line in listed.splitlines():
        fields = line.split()
        if (
            len(fields) >= 3
            and fields[1] == str(group)
            and not fields[2].startswith("Z")
        ):
            members.append(int(fields[0]))
    return members


def running(pid: int) -> bool:
    """Whether the process exists and has not exited (a zombie has)."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    state = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return bool(state) and not state.startswith("Z")


class OwnedProcess:
    """A started command, the leader of its own process group."""

    def __init__(
        self,
        command: Sequence[str],
        cwd: Path | None,
        environment: Mapping[str, str],
        stdout: IO[bytes] | int,
        stderr: IO[bytes] | int,
    ) -> None:
        self.popen = subprocess.Popen(
            list(command),
            cwd=cwd,
            env=dict(environment),
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        self.pid = self.popen.pid
        # Processes of the group still running after it was stopped.
        self.survivors: list[int] = []
        self._exit = _ExitWatch(self.pid)

    @property
    def returncode(self) -> int | None:
        """The leader's exit code once it is reaped, negative for the signal
        that ended it."""
        return self.popen.returncode

    def exited(self) -> bool:
        """Whether the leader has exited; it is not reaped."""
        return self.popen.returncode is not None or self._exit.exited()

    def wait(self, timeout_s: float) -> bool:
        """Until the leader exits (True) or the timeout passes (False)."""
        deadline = time.monotonic() + timeout_s
        while not self.exited():
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.05)
        return True

    def _gone(self, timeout_s: float) -> bool:
        deadline = time.monotonic() + timeout_s
        while group_members(self.pid):
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.1)
        return True

    def stop(self, timeout_s: float) -> int | None:
        """Stops the whole group, the leader and whatever it started in it:
        SIGTERM, then SIGKILL to whatever is left after the timeout. Then it
        reaps the leader and returns its exit code. `survivors` lists any
        process of the group that outlived SIGKILL; if the leader is one,
        it stays unreaped and the result is None."""
        if self.popen.returncode is None:
            # A signal the group cannot all receive (one of another user's,
            # say) shows up as a survivor below.
            if not self._gone(0):
                with contextlib.suppress(ProcessLookupError, PermissionError):
                    os.killpg(self.pid, signal.SIGTERM)
                if not self._gone(timeout_s):
                    with contextlib.suppress(ProcessLookupError, PermissionError):
                        os.killpg(self.pid, signal.SIGKILL)
                    if not self._gone(_KILL_GRACE_S):
                        self.survivors = group_members(self.pid)
            # Process-list snapshots and exit notifications are independent:
            # ps can already omit the leader while waitid/kqueue still has
            # no event. Wait for that event before reaping or declaring a
            # cleanup failure; keep the wait bounded if it never arrives.
            if not self.wait(_KILL_GRACE_S):
                return None
            self.popen.wait()
            self._exit.close()
        return self.popen.returncode


if sys.platform == "linux":

    class _ExitWatch:
        """The leader's exit, seen with waitid(WNOWAIT), which leaves it
        unreaped."""

        def __init__(self, pid: int) -> None:
            self._pid = pid
            self._exited = False

        def exited(self) -> bool:
            if not self._exited:
                found = os.waitid(
                    os.P_PID, self._pid, os.WEXITED | os.WNOHANG | os.WNOWAIT
                )
                self._exited = found is not None
            return self._exited

        def close(self) -> None:
            pass

else:

    class _ExitWatch:
        """The leader's exit, seen as a kqueue event, which leaves it
        unreaped. The event comes once, so it is remembered."""

        def __init__(self, pid: int) -> None:
            self._events: select.kqueue | None = select.kqueue()
            self._exited = False
            try:
                self._events.control(
                    [
                        select.kevent(
                            pid,
                            filter=select.KQ_FILTER_PROC,
                            flags=select.KQ_EV_ADD,
                            fflags=select.KQ_NOTE_EXIT,
                        )
                    ],
                    0,
                    0,
                )
            except ProcessLookupError:
                # Exited already: an unreaped child of this process.
                self.close()
                self._exited = True

        def exited(self) -> bool:
            if not self._exited and self._events is not None:
                self._exited = bool(self._events.control(None, 1, 0))
            return self._exited

        def close(self) -> None:
            if self._events is not None:
                self._events.close()
                self._events = None
