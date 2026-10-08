"""Sandbox containers never outlive whoever owns them (ADR 0021).

The research service removes a job's containers, and verifies that they are
gone, whenever the job ends: by its deadline, by flooding its output, by
exiting without closing its session, or at shutdown, including the forced
kill. It does this before the job's slot is free, and after a restart for
the jobs a crash interrupted. A session's watchdog removes its container
when the owner dies with no supervisor left. Cleanup never touches a
container it does not own. Every case runs the real image with a runtime
that never answers (`tests.sandbox_faults`)."""

import contextlib
import hashlib
import json
import os
import secrets
import shlex
import signal
import subprocess
import sys
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path

import pytest

from chronos_research.onnx_sandbox import Sandbox, SandboxSession
from chronos_research.pipeline import Tools
from chronos_research.service import Job, JobStore, ServiceConfiguration, Workers
from tests.test_import_routes import staged, upload, validate, wait_job
from tests.test_service import Running, serving


def valid_model() -> bytes:
    # Needs the onnx extra, as the sandbox fixture does, which skips without it.
    from tests import onnx_models

    return onnx_models.valid()


@dataclass(frozen=True)
class Faults:
    sandbox: Sandbox
    markers: Path
    python: Path
    base: Path

    def configuration(self, job_timeout_s: int = 1800) -> ServiceConfiguration:
        return ServiceConfiguration(
            tools=Tools(self.base / "no-tools"),
            artifacts=self.base / "data",
            state=self.base / "state",
            port=0,
            sandbox=self.sandbox,
            python=str(self.python),
            job_timeout_s=job_timeout_s,
        )

    def started(self, count: int = 1) -> list[str]:
        """The containers the faulty children started, once there are `count`."""
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            names = [
                json.loads(path.read_text())["container"] for path in sorted(self.markers.glob("*.json"))
            ]
            if len(names) >= count:
                return names
            time.sleep(0.1)
        raise AssertionError("the sandbox container never started")

    def remaining(self, names: list[str]) -> list[str]:
        return [name for name in names if exists(self.sandbox, name)]

    def remove_leftovers(self) -> None:
        for path in self.markers.glob("*.json"):
            self.sandbox.remove(json.loads(path.read_text())["container"])


def exists(sandbox: Sandbox, name: str) -> bool:
    listed = sandbox.docker_command("ps", "-a", "--filter", f"name={name}", "--format", "{{.Names}}")
    assert listed.returncode == 0, listed.stderr
    return name in listed.stdout.split()


@pytest.fixture
def faults(tmp_path: Path, sandbox: Sandbox, monkeypatch: pytest.MonkeyPatch) -> Iterator[Faults]:
    markers = tmp_path / "markers"
    markers.mkdir()
    monkeypatch.setenv("CHRONOS_TEST_SANDBOX_MARKERS", str(markers))
    python = tmp_path / "faulty-python"
    python.write_text(f'#!/bin/sh\nexec {shlex.quote(sys.executable)} -m tests.sandbox_faults "$@"\n')
    python.chmod(0o755)
    configured = Faults(sandbox, markers, python, tmp_path)
    try:
        yield configured
    finally:
        configured.remove_leftovers()


def start_import(service: Running) -> str:
    model = valid_model()
    import_id = staged(service, name=f"supervised-{secrets.token_hex(4)}")["import_id"]
    assert upload(service, import_id, model)[0] == 201
    status, started = validate(service, import_id, hashlib.sha256(model).hexdigest())
    assert status == 202, started
    return str(started["job"]["job_id"])


@contextlib.contextmanager
def running(configuration: ServiceConfiguration) -> Iterator[Running]:
    yield from serving(configuration)


def test_a_job_past_its_deadline_leaves_no_container(faults: Faults, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("CHRONOS_TEST_SANDBOX_FAULT", "hang")
    with running(faults.configuration(job_timeout_s=6)) as service:
        job = wait_job(service, start_import(service))
        # The job is over only once its container is gone.
        names = faults.started()
        assert faults.remaining(names) == []
    assert (job["state"], job["error"]["code"]) == ("failed", "timed_out")


@pytest.mark.parametrize(("fault", "code"), [("exit", "import_failed"), ("flood", "worker_failed")])
def test_a_job_that_dies_or_floods_leaves_no_container(
    faults: Faults, monkeypatch: pytest.MonkeyPatch, fault: str, code: str
) -> None:
    monkeypatch.setenv("CHRONOS_TEST_SANDBOX_FAULT", fault)
    with running(faults.configuration()) as service:
        job = wait_job(service, start_import(service))
        assert faults.remaining(faults.started()) == []
    assert (job["state"], job["error"]["code"]) == ("failed", code)


@pytest.mark.parametrize("fault", ["hang", "ignore_term"])
def test_shutdown_leaves_no_container_even_when_the_job_must_be_killed(
    faults: Faults, monkeypatch: pytest.MonkeyPatch, fault: str
) -> None:
    monkeypatch.setenv("CHRONOS_TEST_SANDBOX_FAULT", fault)
    with running(faults.configuration()) as service:
        job_id = start_import(service)
        names = faults.started()
    # Shutdown has returned: the job was stopped and its container removed.
    assert faults.remaining(names) == []
    job = JobStore(faults.base / "state" / "jobs", retain=10).get(job_id)
    assert job is not None
    assert job.state == "interrupted"
    assert job.sandbox_released


def test_cleanup_leaves_another_sessions_container_running(
    faults: Faults, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("CHRONOS_TEST_SANDBOX_FAULT", "hang")
    other = Sandbox(docker=faults.sandbox.docker, supervisor=secrets.token_hex(16))
    with SandboxSession(other, valid_model()) as live:
        with running(faults.configuration(job_timeout_s=6)) as service:
            assert wait_job(service, start_import(service))["error"]["code"] == "timed_out"
        assert faults.remaining(faults.started()) == []
        assert exists(faults.sandbox, live.name)
        assert len(live.predict([0.01, 0.0, 0.0, 100.0, 100.0])) == 3


def test_a_session_whose_owner_is_killed_is_removed_by_its_watchdog(faults: Faults) -> None:
    model = faults.base / "model.onnx"
    model.write_bytes(valid_model())
    owner = subprocess.Popen(
        [sys.executable, "-m", "tests.sandbox_faults", "session", faults.sandbox.executable(), str(model)],
        cwd=Path(__file__).resolve().parent.parent,
        start_new_session=True,
    )
    try:
        names = faults.started()
    finally:
        # The owner and everything in its process group, at once.
        with contextlib.suppress(ProcessLookupError):
            os.killpg(owner.pid, signal.SIGKILL)
        owner.wait()
    deadline = time.monotonic() + 30
    while faults.remaining(names) and time.monotonic() < deadline:
        time.sleep(0.2)
    assert faults.remaining(names) == []


def test_a_restart_reclaims_the_containers_of_jobs_a_crash_interrupted(faults: Faults) -> None:
    supervisor = secrets.token_hex(16)
    jobs = faults.base / "state" / "jobs"
    jobs.mkdir(parents=True)
    abandoned = Job(
        job_id=secrets.token_hex(16),
        sequence=1,
        request={"import_id": "a" * 32, "model_sha256": "b" * 64},
        request_key="k",
        state="running",
        created_at="2026-10-07T00:00:00Z",
        started_at="2026-10-07T00:00:01Z",
        kind="import",
        sandbox_supervisor=supervisor,
        sandbox_released=False,
    )
    (jobs / f"{abandoned.job_id}.json").write_text(json.dumps(abandoned.as_json()))
    # Its container is still running when the service comes back.
    session = SandboxSession(Sandbox(docker=faults.sandbox.docker, supervisor=supervisor), valid_model())
    try:
        store = JobStore(jobs, retain=10)
        workers = Workers(faults.configuration(), store)
        try:
            assert not exists(faults.sandbox, session.name)
            recovered = store.get(abandoned.job_id)
            assert recovered is not None
            assert (recovered.state, recovered.sandbox_released) == ("interrupted", True)
        finally:
            workers.stop()
    finally:
        session.close()


def test_a_job_whose_containers_are_not_yet_released_is_never_pruned(tmp_path: Path) -> None:
    store = JobStore(tmp_path / "jobs", retain=1)
    pending, _ = store.create({"n": "1"}, "one", "import")
    pending.state, pending.sandbox_supervisor, pending.sandbox_released = "failed", "c" * 32, False
    store.save(pending)
    released, _ = store.create({"n": "2"}, "two", "import")
    released.state = "failed"
    store.save(released)
    store.create({"n": "3"}, "three", "import")
    kept = {job.job_id for job in store.newest_first()}
    assert pending.job_id in kept
    assert released.job_id not in kept
