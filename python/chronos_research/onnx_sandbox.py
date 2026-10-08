"""The ONNX sandbox, from the host (ADR 0021).

An imported model is parsed, checked, and executed only inside a container
started from the image `chronos-onnx-sandbox:<content hash>`:

    --network none --read-only --cap-drop ALL --security-opt no-new-privileges
    --user 65534:65534 --pids-limit 32 --memory 256m --memory-swap 256m
    --cpus 1 --ulimit nofile=64:64 --ipc none --log-driver none --pull never

Nothing of the host is mounted: the model arrives on stdin, so the container
sees no file, credential, catalog, database, or Docker socket. Its
environment is the image's alone. The host bounds what Docker cannot: every
write and read has a deadline (60 s to load, 2 s per prediction), a reply
line is at most 64 KiB, and only a 4 KiB tail of stderr is kept.

A container never outlives whoever owns it. Each is named, and labelled
with its owner process and, when one launched it, its supervisor's token.
The session kills and removes it on timeout, failure, or close. A watchdog
in a session of its own removes it when the owner dies without closing,
however it dies (onnx_sandbox_watchdog.py). A supervisor that runs
sandbox users as children (the research service for its jobs, the paper
server for its inference workers) removes every container under its token
when the child ends, and checks that none is left (remove_supervised).

Without Docker, or without the image built from exactly this source, the
capability is unavailable and nothing is run. There is no weaker fallback.
"""

import contextlib
import hashlib
import io
import json
import os
import secrets
import selectors
import shutil
import socket
import subprocess
import sys
import tarfile
import threading
import time
from collections.abc import Collection, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research import onnx_profile

PROTOCOL = "chronos.onnx_sandbox/1"
IMAGE_REPOSITORY = "chronos-onnx-sandbox"
ROLE_LABEL = "org.chronos.onnx-sandbox"
CONTENT_LABEL = "org.chronos.onnx-sandbox.content"
OWNER_LABEL = "org.chronos.onnx-sandbox.owner"
SUPERVISOR_LABEL = "org.chronos.onnx-sandbox.supervisor"
SUPERVISOR_DIGITS = 32
LOAD_TIMEOUT_S = 60.0
PREDICT_TIMEOUT_S = 2.0
DOCKER_TIMEOUT_S = 15.0
MAX_REPLY_BYTES = 64 * 1024
STDERR_TAIL_BYTES = 4096
BUILD_COMMAND = "python -m chronos_research onnx-sandbox build"
LIMITS = {
    "cpus": "1",
    "load_timeout_s": str(int(LOAD_TIMEOUT_S)),
    "memory": "256m",
    "nofile": "64",
    "pids": "32",
    "predict_timeout_ms": str(int(PREDICT_TIMEOUT_S * 1000)),
    "reply_bytes": str(MAX_REPLY_BYTES),
}
RESTRICTIONS = (
    "--network", "none", "--read-only", "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
    "--user", "65534:65534", "--pids-limit", LIMITS["pids"], "--memory", LIMITS["memory"],
    "--memory-swap", LIMITS["memory"], "--cpus", LIMITS["cpus"],
    "--ulimit", f"nofile={LIMITS['nofile']}:{LIMITS['nofile']}", "--ipc", "none", "--log-driver", "none",
    "--pull", "never",
)  # fmt: skip
WATCHDOG = Path(__file__).resolve().with_name("onnx_sandbox_watchdog.py")
WATCHDOG_WAIT_S = 10.0
# The image's whole build context, by its name in the context.
_PACKAGE = Path(__file__).resolve().parent
_CONTEXT = {
    "Dockerfile": _PACKAGE.parent.parent / "tools" / "onnx-sandbox" / "Dockerfile",
    "requirements-onnx.lock": _PACKAGE.parent / "requirements-onnx.lock",
    "chronos_research/__init__.py": _PACKAGE / "__init__.py",
    "chronos_research/onnx_profile.py": _PACKAGE / "onnx_profile.py",
    "chronos_research/onnx_sandbox_worker.py": _PACKAGE / "onnx_sandbox_worker.py",
}


class SandboxUnavailableError(Exception):
    """The sandbox cannot run at all here; nothing was executed."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


class SandboxRejectedError(Exception):
    """The model is not one the profile and runtime accept."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


class SandboxFailedError(Exception):
    """The sandbox stopped answering within the protocol: a timeout, an exit,
    or a malformed reply. The session is closed."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


class SandboxCleanupError(Exception):
    """Docker answered, but containers it was told to remove are still listed."""

    def __init__(self, names: Sequence[str]) -> None:
        super().__init__(f"sandbox containers are still present: {', '.join(names)}")
        self.names = list(names)


def new_supervisor() -> str:
    """A supervisor's token: unguessable, so its label names its containers only."""
    return secrets.token_hex(SUPERVISOR_DIGITS // 2)


def is_supervisor(text: str) -> bool:
    return len(text) == SUPERVISOR_DIGITS and all(character in "0123456789abcdef" for character in text)


def context_files() -> dict[str, bytes]:
    files = {}
    for name, path in _CONTEXT.items():
        try:
            files[name] = path.read_bytes()
        except OSError as error:
            raise SandboxUnavailableError(
                "sandbox_source_missing", f"the sandbox's {name} cannot be read here ({error.strerror})"
            ) from error
    return files


def content_digest(files: dict[str, bytes]) -> str:
    listing = "".join(f"{name}\n{hashlib.sha256(data).hexdigest()}\n" for name, data in sorted(files.items()))
    return hashlib.sha256(listing.encode()).hexdigest()


@dataclass(frozen=True)
class Sandbox:
    """How to reach Docker. The CLI gets a minimal environment: no HOME, so
    no registry credential, and DOCKER_HOST only when configured. A process
    run by a supervisor labels its containers with the supervisor's token."""

    docker: str = "docker"
    docker_host: str | None = None
    supervisor: str | None = None

    def __post_init__(self) -> None:
        if self.supervisor is not None and not is_supervisor(self.supervisor):
            raise ValueError(f"a sandbox supervisor is {SUPERVISOR_DIGITS} lowercase hexadecimal digits")

    def executable(self) -> str:
        found = shutil.which(self.docker) if os.sep not in self.docker else self.docker
        if found is None or not os.access(found, os.X_OK):
            raise SandboxUnavailableError("docker_not_found", f"the Docker CLI {self.docker!r} cannot be run")
        return found

    def environment(self) -> dict[str, str]:
        environment = {"PATH": "/usr/bin:/bin"}
        if self.docker_host is not None:
            environment["DOCKER_HOST"] = self.docker_host
        return environment

    def docker_command(
        self, *arguments: str, timeout: float = DOCKER_TIMEOUT_S
    ) -> subprocess.CompletedProcess[str]:
        try:
            return subprocess.run(
                [self.executable(), *arguments],
                capture_output=True,
                text=True,
                timeout=timeout,
                env=self.environment(),
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise SandboxUnavailableError(
                "docker_unreachable", f"docker {arguments[0]} did not answer"
            ) from error
        except OSError as error:
            raise SandboxUnavailableError(
                "docker_not_found", f"the Docker CLI cannot be run: {error}"
            ) from error

    def image(self) -> str:
        return f"{IMAGE_REPOSITORY}:{content_digest(context_files())[:16]}"

    def capability(self) -> dict[str, Any]:
        """Whether imports can run here, with the reason when they cannot."""
        try:
            files = context_files()
            digest = content_digest(files)
            image = f"{IMAGE_REPOSITORY}:{digest[:16]}"
            inspected = self.docker_command("image", "inspect", "--format", "{{json .Config.Labels}}", image)
        except SandboxUnavailableError as error:
            return {"detail": str(error), "limits": LIMITS, "reason": error.reason, "status": "unavailable"}
        if inspected.returncode != 0:
            # Any failure but a missing image means Docker itself did not answer.
            unreachable = "no such image" not in inspected.stderr.lower()
            return {
                "detail": (
                    "the Docker daemon is not reachable"
                    if unreachable
                    else f"the sandbox image {image} is not built; run {BUILD_COMMAND}"
                ),
                "limits": LIMITS,
                "reason": "docker_unreachable" if unreachable else "image_not_built",
                "status": "unavailable",
            }
        try:
            labels = json.loads(inspected.stdout) or {}
        except ValueError:
            labels = {}
        if labels.get(CONTENT_LABEL) != digest:
            return {
                "detail": f"{image} was not built from this source",
                "limits": LIMITS,
                "reason": "image_mismatch",
                "status": "unavailable",
            }
        return {"content_sha256": digest, "image": image, "limits": LIMITS, "status": "available"}

    def require_available(self) -> str:
        capability = self.capability()
        if capability["status"] != "available":
            raise SandboxUnavailableError(capability["reason"], capability["detail"])
        return str(capability["image"])

    def build(self) -> str:
        """Builds the image from exactly the context files; the base image and
        every wheel are pinned. Needs the network once, for those."""
        files = context_files()
        digest = content_digest(files)
        image = f"{IMAGE_REPOSITORY}:{digest[:16]}"
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode="w") as tar:
            for name, data in sorted(files.items()):
                info = tarfile.TarInfo(name)
                info.size, info.mode, info.mtime = len(data), 0o644, 0
                tar.addfile(info, io.BytesIO(data))
        completed = subprocess.run(
            [self.executable(), "build", "--quiet", "--label", f"{CONTENT_LABEL}={digest}", "-t", image, "-"],
            input=archive.getvalue(),
            capture_output=True,
            timeout=1800,
            env=self.environment(),
            check=False,
        )
        if completed.returncode != 0:
            raise SandboxUnavailableError(
                "image_build_failed", completed.stderr.decode("utf-8", errors="replace")[-STDERR_TAIL_BYTES:]
            )
        return image

    def run_command(self, name: str, image: str, entrypoint: Sequence[str] = ()) -> list[str]:
        """The `docker run` command of one container. A test may replace the
        entrypoint to probe the limits themselves."""
        override = ["--entrypoint", entrypoint[0]] if entrypoint else []
        supervised = ["--label", f"{SUPERVISOR_LABEL}={self.supervisor}"] if self.supervisor else []
        return [
            self.executable(), "run", "--rm", "-i", "--name", name, "--label", f"{ROLE_LABEL}=1",
            "--label", f"{OWNER_LABEL}={owner()}", *supervised, *RESTRICTIONS, *override, image,
            *entrypoint[1:],
        ]  # fmt: skip

    def remove(self, name: str) -> None:
        with contextlib.suppress(SandboxUnavailableError):
            self.docker_command("rm", "-f", name)

    def supervised(self, supervisors: Collection[str]) -> list[str]:
        """The containers, running or not, labelled with any of these tokens."""
        listed = self.docker_command(
            "ps",
            "-a",
            "--filter",
            f"label={ROLE_LABEL}=1",
            "--format",
            f'{{{{.Names}}}}\t{{{{.Label "{SUPERVISOR_LABEL}"}}}}',
        )
        if listed.returncode != 0:
            raise SandboxUnavailableError("docker_unreachable", "the Docker daemon did not list containers")
        wanted = set(supervisors)
        lines = (line.partition("\t") for line in listed.stdout.splitlines())
        return [name for name, _, supervisor in lines if supervisor in wanted]

    def remove_supervised(self, supervisors: Collection[str], settle_s: float = 0.0) -> None:
        """Removes every container of these supervisors, then checks that
        Docker lists none. After a child was killed, `settle_s` waits for a
        container whose creation was still under way and looks again.
        SandboxUnavailableError when Docker cannot be asked;
        SandboxCleanupError when a container stays."""
        removed = False
        for look in range(2 if settle_s > 0 else 1):
            if look:
                time.sleep(settle_s)
            names = self.supervised(supervisors)
            if names:
                self.docker_command("rm", "-f", *names)
                removed = True
        if removed and (remaining := self.supervised(supervisors)):
            raise SandboxCleanupError(remaining)

    def reap_stale(self) -> list[str]:
        """Removes sandbox containers whose owning process on this host is
        gone; another live process's containers are never touched."""
        listed = self.docker_command(
            "ps",
            "-a",
            "--filter",
            f"label={ROLE_LABEL}=1",
            "--format",
            f'{{{{.Names}}}}\t{{{{.Label "{OWNER_LABEL}"}}}}',
        )
        removed = []
        for line in listed.stdout.splitlines():
            name, _, owned_by = line.partition("\t")
            host, _, pid = owned_by.rpartition(":")
            if host != socket.gethostname() or not pid.isdigit() or _alive(int(pid)):
                continue
            self.remove(name)
            removed.append(name)
        return removed


def owner() -> str:
    return f"{socket.gethostname()}:{os.getpid()}"


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _start_watchdog(sandbox: Sandbox, name: str) -> tuple["subprocess.Popen[bytes]", int]:
    """The watchdog of container `name`, and the only write end of its
    stdin: when the owner dies, the kernel closes it and the watchdog
    removes the container. Not inheritable, so no other child holds it."""
    read_end, write_end = os.pipe()
    try:
        watchdog = subprocess.Popen(
            [sys.executable, "-I", str(WATCHDOG), sandbox.executable(), name],
            stdin=read_end,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=sandbox.environment(),
            # Outside the owner's process group, so it outlives a kill of it.
            start_new_session=True,
        )
    except OSError as error:
        os.close(write_end)
        raise SandboxUnavailableError(
            "watchdog_failed", f"the sandbox watchdog cannot start: {error}"
        ) from error
    finally:
        os.close(read_end)
    return watchdog, write_end


class SandboxSession:
    """One loaded model in one container. Not thread-safe: one user owns it."""

    def __init__(
        self,
        sandbox: Sandbox,
        model: bytes,
        *,
        load_timeout_s: float = LOAD_TIMEOUT_S,
        entrypoint: Sequence[str] = (),
    ) -> None:
        if not 0 < len(model) <= onnx_profile.MAX_MODEL_BYTES:
            # Refused before any container starts.
            raise SandboxRejectedError(
                "model_too_large", f"a model is 1 to {onnx_profile.MAX_MODEL_BYTES} bytes, not {len(model)}"
            )
        image = sandbox.require_available()
        self._sandbox = sandbox
        self.name = f"chronos-onnx-{secrets.token_hex(8)}"
        self._closed = False
        self._buffer = bytearray()
        self._stderr = bytearray()
        self._requests = 0
        # Watched before it exists, so no container is ever unwatched.
        self._watchdog, self._lifeline = _start_watchdog(sandbox, self.name)
        try:
            # In the owner's process group: whoever kills the owner's group
            # kills this CLI too, so it cannot go on to start the container.
            self._process = subprocess.Popen(
                sandbox.run_command(self.name, image, entrypoint),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=sandbox.environment(),
            )
        except OSError as error:
            self._release_watchdog()
            raise SandboxUnavailableError(
                "docker_not_found", f"the Docker CLI cannot be run: {error}"
            ) from error
        assert self._process.stdin is not None
        assert self._process.stdout is not None
        os.set_blocking(self._process.stdin.fileno(), False)
        self._drain = threading.Thread(target=self._keep_stderr_tail, daemon=True)
        self._drain.start()
        try:
            deadline = time.monotonic() + load_timeout_s
            header = {"sha256": hashlib.sha256(model).hexdigest(), "size": str(len(model)), "type": "load"}
            self._send(json.dumps(header).encode() + b"\n" + model, deadline)
            ready = self._reply(deadline)
            if ready.get("type") == "rejected":
                raise SandboxRejectedError(str(ready.get("reason")), str(ready.get("message"))[:1000])
            if (
                set(ready) != {"onnxruntime", "protocol", "providers", "report", "type"}
                or ready["type"] != "ready"
                or ready["protocol"] != PROTOCOL
                or ready["providers"] != ["CPUExecutionProvider"]
                or not isinstance(ready["report"], dict)
                or ready["report"].get("profile") != onnx_profile.PROFILE
            ):
                raise SandboxFailedError("protocol_error", "the sandbox's ready line is not the protocol's")
            self.report: dict[str, Any] = ready["report"]
            self.onnxruntime = str(ready["onnxruntime"])
        except BaseException:
            self.close()
            raise

    def _keep_stderr_tail(self) -> None:
        assert self._process.stderr is not None
        for chunk in iter(lambda: self._process.stderr.read(4096), b""):  # type: ignore[union-attr]
            self._stderr.extend(chunk)
            del self._stderr[:-STDERR_TAIL_BYTES]

    def stderr_tail(self) -> str:
        return bytes(self._stderr).decode("utf-8", errors="replace")

    def _fail(self, reason: str, message: str) -> SandboxFailedError:
        self.close()
        return SandboxFailedError(reason, message)

    def _send(self, data: bytes, deadline: float) -> None:
        assert self._process.stdin is not None
        view = memoryview(data)
        with selectors.DefaultSelector() as ready:
            ready.register(self._process.stdin, selectors.EVENT_WRITE)
            while view:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise self._fail("timeout", "the sandbox did not accept its input in time")
                if not ready.select(timeout=remaining):
                    continue
                try:
                    view = view[os.write(self._process.stdin.fileno(), view) :]
                except BlockingIOError:
                    continue
                except (BrokenPipeError, OSError) as error:
                    raise self._fail("sandbox_exited", f"the sandbox stopped reading ({error})") from error

    def _reply(self, deadline: float) -> dict[str, Any]:
        assert self._process.stdout is not None
        with selectors.DefaultSelector() as ready:
            ready.register(self._process.stdout, selectors.EVENT_READ)
            while b"\n" not in self._buffer:
                if len(self._buffer) > MAX_REPLY_BYTES:
                    raise self._fail("protocol_error", "a sandbox reply is too long")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise self._fail("timeout", "the sandbox did not reply in time")
                if not ready.select(timeout=remaining):
                    continue
                chunk = os.read(self._process.stdout.fileno(), 65536)
                if not chunk:
                    raise self._fail("sandbox_exited", "the sandbox exited: " + self.stderr_tail()[-300:])
                self._buffer.extend(chunk)
        line, _, rest = bytes(self._buffer).partition(b"\n")
        self._buffer = bytearray(rest)
        if len(line) > MAX_REPLY_BYTES:
            raise self._fail("protocol_error", "a sandbox reply is too long")
        try:
            document = json.loads(line)
        except ValueError as error:
            raise self._fail("protocol_error", "a sandbox reply is not JSON") from error
        if not isinstance(document, dict):
            raise self._fail("protocol_error", "a sandbox reply is not an object")
        return document

    def predict(self, values: Sequence[float], timeout_s: float = PREDICT_TIMEOUT_S) -> list[str]:
        """The model's raw output values for one row, as the shortest
        round-trip strings the sandbox reported. The caller judges them. A
        row the runtime could not run is SandboxRejectedError, and the
        session stays usable; anything else outside the protocol closes it."""
        if self._closed:
            raise SandboxFailedError("closed", "the sandbox session is closed")
        self._requests += 1
        identifier = str(self._requests)
        request = {"id": identifier, "inputs": [repr(float(value)) for value in values], "type": "predict"}
        deadline = time.monotonic() + timeout_s
        self._send(json.dumps(request).encode() + b"\n", deadline)
        reply = self._reply(deadline)
        if reply.get("type") == "error" and reply.get("id") == identifier:
            raise SandboxRejectedError("runtime_error", str(reply.get("message"))[:1000])
        if (
            set(reply) != {"dtype", "id", "shape", "type", "values"}
            or reply["type"] != "prediction"
            or reply["id"] != identifier
            or not isinstance(reply["values"], list)
            or not all(isinstance(value, str) for value in reply["values"])
        ):
            raise self._fail("protocol_error", "a sandbox prediction is not the protocol's")
        if reply["dtype"] != "float64" or reply["shape"] != [str(d) for d in onnx_profile.OUTPUT_SHAPE]:
            raise SandboxRejectedError(
                "wrong_output",
                f"the output is {reply['dtype']} with shape {reply['shape']}, not float64 [1, 3]",
            )
        return list(reply["values"])

    def close(self) -> None:
        """Ends the container and its CLI process, whatever state they are in."""
        if self._closed:
            return
        self._closed = True
        with contextlib.suppress(OSError):
            if self._process.stdin is not None:
                self._process.stdin.close()
        try:
            self._process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            with contextlib.suppress(SandboxUnavailableError):
                self._sandbox.docker_command("kill", self.name)
            self._process.kill()
            with contextlib.suppress(subprocess.TimeoutExpired):
                self._process.wait(timeout=5)
        # The CLI may have gone while its container lives on.
        self._sandbox.remove(self.name)
        self._release_watchdog()
        self._drain.join(timeout=2)
        if self._process.stdout is not None:
            self._process.stdout.close()
        # The tail thread owns stderr until it has read to the end.
        if self._process.stderr is not None and not self._drain.is_alive():
            self._process.stderr.close()

    def _release_watchdog(self) -> None:
        """Tells the watchdog that the session closed, so it only checks that
        the container is gone, and waits a bounded time for it."""
        with contextlib.suppress(OSError):
            os.write(self._lifeline, b"closed")
        os.close(self._lifeline)
        with contextlib.suppress(subprocess.TimeoutExpired):
            self._watchdog.wait(timeout=WATCHDOG_WAIT_S)

    def __enter__(self) -> "SandboxSession":
        return self

    def __exit__(self, *exception: object) -> None:
        self.close()
