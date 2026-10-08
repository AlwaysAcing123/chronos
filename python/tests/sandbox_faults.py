"""Faults for the sandbox supervision tests, run as a child process.

`python -m tests.sandbox_faults -m chronos_research <command> ...` is a
research job's child, with the fault named by CHRONOS_TEST_SANDBOX_FAULT;
`python -m tests.sandbox_faults session <docker> <model>` owns one session
directly. Every sandbox session gets a runtime that never answers (the
entrypoint `sleep 600`, through the session's own test hook). Once its
container runs, the child writes `<CHRONOS_TEST_SANDBOX_MARKERS>/<pid>.json`
naming it, then:

    hang         keeps waiting
    ignore_term  keeps waiting, ignoring SIGTERM
    exit         exits at once, without closing the session
    flood        floods stdout past the research service's output limit
"""

import json
import os
import runpy
import signal
import sys
import threading
import time
from pathlib import Path
from typing import Any

from chronos_research import onnx_sandbox
from chronos_research.service import MAX_WORKER_OUTPUT_BYTES

NEVER_ANSWERS = ("sleep", "600")


def _running_container(sandbox: onnx_sandbox.Sandbox) -> str:
    """This process's sandbox container, once Docker lists it running."""
    owned = f"label={onnx_sandbox.OWNER_LABEL}={onnx_sandbox.owner()}"
    while True:
        listed = sandbox.docker_command("ps", "--filter", owned, "--format", "{{.Names}}")
        if listed.stdout.split():
            return listed.stdout.split()[0]
        time.sleep(0.1)


def _after_start(sandbox: onnx_sandbox.Sandbox, fault: str, markers: Path) -> None:
    name = _running_container(sandbox)
    (markers / f"{os.getpid()}.json").write_text(json.dumps({"container": name}))
    if fault == "exit":
        os._exit(3)
    if fault == "flood":
        chunk = b"x" * 65536
        for _ in range(MAX_WORKER_OUTPUT_BYTES // len(chunk) + 2):
            os.write(1, chunk)


def install(fault: str, markers: Path) -> None:
    original = onnx_sandbox.SandboxSession.__init__

    def never_answering(
        self: onnx_sandbox.SandboxSession, sandbox: onnx_sandbox.Sandbox, model: bytes, **_: Any
    ) -> None:
        threading.Thread(target=_after_start, args=(sandbox, fault, markers), daemon=True).start()
        original(self, sandbox, model, load_timeout_s=600.0, entrypoint=NEVER_ANSWERS)

    onnx_sandbox.SandboxSession.__init__ = never_answering  # type: ignore[method-assign]
    if fault == "ignore_term":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)


def main() -> None:
    markers = Path(os.environ["CHRONOS_TEST_SANDBOX_MARKERS"])
    if sys.argv[1] == "session":
        install("hang", markers)
        onnx_sandbox.SandboxSession(onnx_sandbox.Sandbox(docker=sys.argv[2]), Path(sys.argv[3]).read_bytes())
        return
    if sys.argv[1:3] != ["-m", "chronos_research"]:
        raise SystemExit("usage: python -m tests.sandbox_faults -m chronos_research <command> ...")
    install(os.environ["CHRONOS_TEST_SANDBOX_FAULT"], markers)
    sys.argv = ["chronos_research", *sys.argv[3:]]
    runpy.run_module("chronos_research", run_name="__main__", alter_sys=True)


if __name__ == "__main__":
    main()
