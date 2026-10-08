"""The ONNX sandbox, run for real: profile refusals inside the container,
host-side output judgement, the container's isolation and limits, timeouts,
and cleanup. Needs Docker and the built image; see conftest."""

import json
import os
import subprocess
import time
from pathlib import Path

import pytest

from chronos_research.onnx_profile import MAX_MODEL_BYTES, MAX_NODES
from chronos_research.onnx_sandbox import (
    BUILD_COMMAND,
    CONTENT_LABEL,
    OWNER_LABEL,
    ROLE_LABEL,
    Sandbox,
    SandboxFailedError,
    SandboxRejectedError,
    SandboxSession,
    SandboxUnavailableError,
    content_digest,
    context_files,
    owner,
)
from chronos_research.registered_inference import InvalidPredictionError, OnnxPredictor

ROW = (20_000_000, 250_000_000, 1_000_000, 10_000_000_000, 20_000_000_000)

# What the container itself reports about where it runs.
PROBE = """
import json, os, resource, socket
facts = {"uid": os.getuid(), "gid": os.getgid(), "environment": sorted(os.environ)}
try:
    socket.create_connection(("1.1.1.1", 53), timeout=2).close()
    facts["network"] = "reachable"
except OSError:
    facts["network"] = "unreachable"
for path in ("/probe", "/tmp/probe"):
    try:
        open(path, "w").close()
        facts[path] = "writable"
    except OSError:
        facts[path] = "read-only"
facts["host_paths"] = [p for p in ("/Users", "/home/runner", "/var/run/docker.sock", "/run/docker.sock")
                       if os.path.exists(p)]
facts["nofile"] = list(resource.getrlimit(resource.RLIMIT_NOFILE))
status = dict(line.split(":", 1) for line in open("/proc/self/status") if ":" in line)
facts["cap_eff"] = status["CapEff"].strip()
facts["no_new_privs"] = status["NoNewPrivs"].strip()
for name in ("memory.max", "pids.max"):
    try:
        facts[name] = open("/sys/fs/cgroup/" + name).read().strip()
    except OSError:
        facts[name] = "unknown"
print(json.dumps(facts))
"""
IMAGE_ENVIRONMENT = {
    "GPG_KEY", "HOME", "HOSTNAME", "LANG", "OMP_NUM_THREADS", "PATH", "PYTHON_SHA256", "PYTHON_VERSION",
}  # fmt: skip


def run_probe(sandbox: Sandbox, *entrypoint: str, timeout: float = 60) -> subprocess.CompletedProcess[bytes]:
    """One container from the sandbox's own command, with another entrypoint."""
    name = f"chronos-onnx-probe-{os.getpid()}-{time.monotonic_ns()}"
    try:
        return subprocess.run(
            sandbox.run_command(name, sandbox.require_available(), entrypoint),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=timeout,
            env=sandbox.environment(),
            check=False,
        )
    finally:
        sandbox.remove(name)


def my_containers(sandbox: Sandbox) -> list[str]:
    listed = sandbox.docker_command(
        "ps", "-a", "--filter", f"label={OWNER_LABEL}={owner()}", "--format", "{{.Names}}"
    )
    return listed.stdout.split()


def test_a_valid_model_predicts_one_row_at_a_time(sandbox: Sandbox) -> None:
    from tests import onnx_models

    predictor = OnnxPredictor(sandbox, onnx_models.valid())
    try:
        first, again = predictor.predict(ROW), predictor.predict(ROW)
        assert predictor.session.report["profile"] == {"name": "chronos.onnx_classifier", "version": "1"}
        assert predictor.session.onnxruntime == "1.30.0"
    finally:
        predictor.close()
    assert first == again
    assert len(first.probabilities) == 3
    assert first.class_index == first.probabilities.index(max(first.probabilities))
    assert my_containers(sandbox) == []


@pytest.mark.parametrize(
    ("build", "reason"),
    [
        ("corrupt", "malformed_model"),
        ("unsupported_operator", "unsupported_operator"),
        ("external_data", "external_data"),
        ("too_many_nodes", "graph_too_large"),
    ],
)
def test_refused_models_are_refused_inside_the_container(sandbox: Sandbox, build: str, reason: str) -> None:
    from tests import onnx_models

    data = {
        "corrupt": b"\x08\x07\x12\xff\xff\xff\xff\x0fnot a model",
        "unsupported_operator": onnx_models.with_operator("Relu"),
        "external_data": onnx_models.with_external_data(),
        "too_many_nodes": onnx_models.with_identity_chain(MAX_NODES),
    }[build]
    with pytest.raises(SandboxRejectedError) as refusal:
        SandboxSession(sandbox, data)
    assert refusal.value.reason == reason
    assert my_containers(sandbox) == []


def test_an_oversized_model_is_refused_before_any_container_starts(sandbox: Sandbox) -> None:
    with pytest.raises(SandboxRejectedError) as refusal:
        SandboxSession(sandbox, b"\x00" * (MAX_MODEL_BYTES + 1))
    assert refusal.value.reason == "model_too_large"
    with pytest.raises(SandboxRejectedError):
        SandboxSession(sandbox, b"")


def test_outputs_that_are_not_probabilities_are_refused_on_the_host(sandbox: Sandbox) -> None:
    from tests import onnx_models

    for data, reasons in (
        (onnx_models.non_finite_output(), {"non_finite_output"}),
        (onnx_models.without_softmax(), {"probability_out_of_range", "probabilities_do_not_sum_to_one"}),
    ):
        predictor = OnnxPredictor(sandbox, data)
        try:
            with pytest.raises(InvalidPredictionError) as refusal:
                predictor.predict(ROW)
            assert refusal.value.reason in reasons
        finally:
            predictor.close()


def test_the_container_has_no_network_host_files_privileges_or_host_environment(
    sandbox: Sandbox, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("CHRONOS_TEST_SECRET", "must-not-reach-the-sandbox")
    completed = run_probe(sandbox, "python", "-I", "-c", PROBE)
    assert completed.returncode == 0, completed.stderr
    facts = json.loads(completed.stdout)
    assert facts["network"] == "unreachable"
    assert facts["/probe"] == facts["/tmp/probe"] == "read-only"
    assert facts["host_paths"] == []
    assert (facts["uid"], facts["gid"]) == (65534, 65534)
    assert facts["cap_eff"] == "0000000000000000"
    assert facts["no_new_privs"] == "1"
    assert facts["nofile"] == [64, 64]
    assert set(facts["environment"]) <= IMAGE_ENVIRONMENT
    if facts["memory.max"] != "unknown":
        assert facts["memory.max"] == str(256 * 1024 * 1024)
        assert facts["pids.max"] == "32"


def test_memory_and_process_limits_stop_a_runaway_worker(sandbox: Sandbox) -> None:
    hungry = run_probe(sandbox, "python", "-I", "-c", "x = bytearray(1 << 30); print('allocated')")
    assert hungry.returncode != 0
    assert b"allocated" not in hungry.stdout
    forks = run_probe(
        sandbox,
        "python",
        "-I",
        "-c",
        "import os, time\n"
        "started = 0\n"
        "for _ in range(64):\n"
        "    try:\n"
        "        if os.fork() == 0:\n"
        "            time.sleep(5); os._exit(0)\n"
        "        started += 1\n"
        "    except OSError:\n"
        "        break\n"
        "print(started)",
    )
    assert forks.returncode == 0, forks.stderr
    assert int(forks.stdout.split()[-1]) < 32


def test_a_worker_that_never_answers_is_killed_and_removed(sandbox: Sandbox) -> None:
    from tests import onnx_models

    started = time.monotonic()
    with pytest.raises(SandboxFailedError) as failure:
        SandboxSession(sandbox, onnx_models.valid(), load_timeout_s=2, entrypoint=("sleep", "600"))
    assert failure.value.reason == "timeout"
    assert time.monotonic() - started < 20
    assert my_containers(sandbox) == []


def test_a_closed_session_refuses_work_and_leaves_no_container(sandbox: Sandbox) -> None:
    from tests import onnx_models

    with SandboxSession(sandbox, onnx_models.valid()) as session:
        assert my_containers(sandbox) == [session.name]
    with pytest.raises(SandboxFailedError):
        session.predict([0.0] * 5)
    assert my_containers(sandbox) == []


def test_only_containers_of_dead_owners_are_reaped(sandbox: Sandbox) -> None:
    image = sandbox.require_available()
    names = {"dead": f"chronos-onnx-dead-{os.getpid()}", "live": f"chronos-onnx-live-{os.getpid()}"}
    owners = {"dead": f"{owner().rpartition(':')[0]}:999999", "live": owner()}
    try:
        for kind, name in names.items():
            created = sandbox.docker_command(
                "create", "--name", name, "--label", f"{ROLE_LABEL}=1",
                "--label", f"{OWNER_LABEL}={owners[kind]}", "--network", "none", "--pull", "never", image,
            )  # fmt: skip
            assert created.returncode == 0, created.stderr
        reaped = sandbox.reap_stale()
        assert names["dead"] in reaped
        assert names["live"] not in reaped
        assert my_containers(sandbox) == [names["live"]]
    finally:
        for name in names.values():
            sandbox.remove(name)


def test_without_docker_the_sandbox_is_unavailable_and_runs_nothing() -> None:
    missing = Sandbox(docker="/nonexistent/docker")
    assert missing.capability()["reason"] == "docker_not_found"
    with pytest.raises(SandboxUnavailableError) as unavailable:
        SandboxSession(missing, b"model")
    assert unavailable.value.reason == "docker_not_found"


def test_an_unreachable_daemon_is_reported_as_such() -> None:
    try:
        Sandbox().executable()
    except SandboxUnavailableError:
        pytest.skip("no Docker CLI here")
    unreachable = Sandbox(docker_host="unix:///nonexistent/docker.sock")
    assert unreachable.capability()["reason"] == "docker_unreachable"


def fake_docker(tmp_path: Path, stdout: str, stderr: str, status: int) -> Sandbox:
    """A Docker CLI that answers every command the same way."""
    script = tmp_path / "docker"
    script.write_text(f"#!/bin/sh\nprintf '%s' '{stdout}'\nprintf '%s' '{stderr}' >&2\nexit {status}\n")
    script.chmod(0o755)
    return Sandbox(docker=str(script))


@pytest.mark.parametrize(
    ("stdout", "stderr", "status", "reason"),
    [
        ("", "Error response from daemon: No such image: chronos-onnx-sandbox:x", 1, "image_not_built"),
        ("", "Cannot connect to the Docker daemon at unix:///var/run/docker.sock.", 1, "docker_unreachable"),
        ("", "permission denied while trying to connect to the docker API", 1, "docker_unreachable"),
        ('{"org.chronos.onnx-sandbox.content":"0000"}', "", 0, "image_mismatch"),
        ("null", "", 0, "image_mismatch"),
    ],
)
def test_the_capability_names_why_the_sandbox_is_unavailable(
    tmp_path: Path, stdout: str, stderr: str, status: int, reason: str
) -> None:
    capability = fake_docker(tmp_path, stdout, stderr, status).capability()
    assert (capability["status"], capability["reason"]) == ("unavailable", reason)
    if reason == "image_not_built":
        assert BUILD_COMMAND in capability["detail"]


def test_the_capability_is_available_only_for_the_image_of_this_source(tmp_path: Path) -> None:
    digest = content_digest(context_files())
    capability = fake_docker(tmp_path, json.dumps({CONTENT_LABEL: digest}), "", 0).capability()
    assert capability["status"] == "available"
    assert capability["image"].endswith(digest[:16])
