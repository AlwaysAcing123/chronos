"""The research service over HTTP, against artifacts the real C++ tools built
from synthetic events, with real child jobs."""

import hashlib
import json
import os
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Iterator
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any

import pytest

from chronos_research.demo import DemoSource, run_demo
from chronos_research.pipeline import ArtifactRoots, Tools
from chronos_research.service import (
    DEFAULT_CONFIGURATION,
    ArtifactUnavailableError,
    JobStore,
    ServiceConfiguration,
    Workers,
    artifact_directory,
    decode_cursor,
    encode_cursor,
    rfc3339,
    start,
    verified_file,
)
from tests import pipeline as built
from tests.synthetic import synthetic_events

API = "/api/v1/research"


@dataclass(frozen=True)
class Running:
    origin: str
    configuration: ServiceConfiguration

    def call(
        self,
        method: str,
        path: str,
        body: Any = None,
        *,
        raw: bytes | None = None,
        content_type: str = "application/json",
        host: str | None = None,
    ) -> tuple[int, dict[str, Any], dict[str, str]]:
        data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
        request = urllib.request.Request(self.origin + path, data=data, method=method)
        if data is not None:
            request.add_header("Content-Type", content_type)
        if host is not None:
            request.add_header("Host", host)
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return response.status, json.loads(response.read()), dict(response.headers)
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read()), dict(error.headers)

    def get(self, path: str) -> dict[str, Any]:
        status, document, _ = self.call("GET", path)
        assert status == 200, document
        return document

    def wait(self, job_id: str, timeout_s: float = 120) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            job: dict[str, Any] = self.get(f"{API}/runs/{job_id}")["run"]
            if job["state"] in ("succeeded", "failed", "interrupted"):
                return job
            time.sleep(0.1)
        raise AssertionError(f"job {job_id} did not finish")


@pytest.fixture(scope="module")
def artifacts(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> tuple[Tools, Path]:
    base = tmp_path_factory.mktemp("service")
    directory = Tools(tools.chronos_dataset.parent)
    (base / "events.csv").write_text(synthetic_events(seed=7))
    run_demo(
        directory,
        ArtifactRoots.under(base / "data"),
        DemoSource("synthetic-stream", synthetic_events=base / "events.csv"),
        150_000,
    )
    return directory, base / "data"


def serving(configuration: ServiceConfiguration) -> Iterator[Running]:
    server, service = start(configuration)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield Running(f"http://127.0.0.1:{server.server_port}", configuration)
    finally:
        server.shutdown()
        service.workers.stop()
        server.server_close()


@pytest.fixture
def service(artifacts: tuple[Tools, Path], tmp_path: Path) -> Iterator[Running]:
    tools, data = artifacts
    yield from serving(ServiceConfiguration(tools=tools, artifacts=data, state=tmp_path / "state", port=0))


def experiment_id(service: Running) -> str:
    experiments = service.get(f"{API}/experiments")["experiments"]
    assert len(experiments) == 1
    return str(experiments[0]["experiment_id"])


def configuration(**changes: str) -> dict[str, str]:
    return {**DEFAULT_CONFIGURATION, **changes}


def request(experiment: str, **changes: Any) -> dict[str, Any]:
    body: dict[str, Any] = {
        "configuration": dict(DEFAULT_CONFIGURATION),
        "experiment_id": experiment,
        "instrument": "AAPL",
        "model": "logistic",
        "window": "test",
    }
    body.update(changes)
    return body


def test_the_catalog_shows_models_windows_and_limitations(service: Running) -> None:
    status = service.get(f"{API}/status")
    assert status["tools"] == {"chronos_dataset": "available", "chronos_strategy": "available"}
    assert status["defaults"]["fee_per_fill"] == "0.01"
    experiment = service.get(f"{API}/experiments/{experiment_id(service)}")["experiment"]
    assert [entry["instrument"] for entry in experiment["instruments"]] == ["AAPL", "SPY"]
    aapl = experiment["instruments"][0]
    assert aapl["models"]["logistic"]["status"] == "fitted"
    assert set(aapl["prediction_metrics"]) == {"validation", "test"}
    windows = experiment["windows"]
    assert windows["training"]["labels_available_through"] < windows["validation"]["start_at"]
    assert windows["test"]["end_at"] is None
    assert any("No result here is evidence" in text for text in experiment["limitations"])
    assert experiment["dataset"]["receive_time"]["earliest"].endswith("Z")


def test_an_evaluation_runs_as_a_job_and_its_result_pages(service: Running) -> None:
    experiment = experiment_id(service)
    body = request(
        experiment,
        configuration={**DEFAULT_CONFIGURATION, "decision_interval_ns": "1", "fee_per_fill": "0.0100"},
    )
    status, started, _ = service.call("POST", f"{API}/runs", body)
    assert status == 202, started
    assert started["job"]["state"] == "queued"
    # The same request again is the same job, not new work.
    status, again, _ = service.call("POST", f"{API}/runs", body)
    assert (status, again["deduplicated"], again["job"]["job_id"]) == (200, True, started["job"]["job_id"])
    job = service.wait(started["job"]["job_id"])
    assert job["state"] == "succeeded", job
    assert job["request"]["configuration"]["fee_per_fill"] == "0.01"
    assert job["result"]["reproduced"].startswith("ran the schedule again")
    run_id = job["result"]["run_id"]
    result = service.get(f"{API}/results/{run_id}")["result"]
    assert result["source"]["instrument"] == "AAPL"
    assert result["window"]["name"] == "test"
    assert result["configuration"]["policy"]["decision_interval_ns"] == "1"
    assert set(result["accounts"]) == {"buy_and_hold", "cash", "strategy"}
    assert result["accounts"]["cash"]["final"]["valuation"]["equity"] == "10000"
    # Trades page with an opaque cursor that never repeats an item.
    seen: list[str] = []
    path = f"{API}/results/{run_id}/trades?limit=2"
    while True:
        page = service.get(path)
        seen += [trade["client_order_id"] for trade in page["trades"]]
        if page["next_cursor"] is None:
            break
        path = f"{API}/results/{run_id}/trades?limit=2&cursor={page['next_cursor']}"
    assert len(seen) == len(set(seen)) == int(result["accounts"]["strategy"]["orders"]["scheduled"])
    benchmark = service.get(f"{API}/results/{run_id}/trades?account=buy_and_hold")["trades"]
    assert [trade["side"] for trade in benchmark] == ["buy"]
    equity = service.get(f"{API}/results/{run_id}/equity?limit=200")
    assert equity["points"][-1]["trigger"] == "window_end"
    assert equity["points"][0]["at"].endswith("Z")
    decisions = service.get(f"{API}/results/{run_id}/decisions?limit=1")
    assert len(decisions["decisions"]) == 1
    assert decisions["next_cursor"] is not None
    listed = service.get(f"{API}/results")["results"]
    assert run_id in {item["run_id"] for item in listed}
    assert service.get(f"{API}/runs")["runs"][0]["job_id"] == started["job"]["job_id"]


@pytest.mark.parametrize(
    ("change", "status", "code", "reason"),
    [
        ({"extra": "1"}, 400, "malformed_request", "unexpected_field"),
        ({"model": "oracle"}, 400, "malformed_request", "unknown_enum_value"),
        ({"window": "train"}, 400, "malformed_request", "unknown_enum_value"),
        ({"instrument": "../AAPL"}, 400, "malformed_request", "invalid_identifier"),
        ({"experiment_id": "../" + "0" * 61}, 400, "malformed_request", "invalid_identifier"),
        ({"instrument": "MSFT"}, 422, "unknown_instrument", None),
        ({"experiment_id": "0" * 64}, 404, "experiment_not_found", None),
        ({"configuration": configuration(fee_per_fill="-0.01")}, 422, "invalid_configuration",
         "out_of_domain"),
        ({"configuration": configuration(latency_ns="0")}, 422, "invalid_configuration", "out_of_domain"),
        ({"configuration": configuration(starting_cash="1e4")}, 400, "malformed_request", "invalid_decimal"),
        ({"configuration": configuration(fee_per_fill="0.000000001")}, 400, "malformed_request",
         "too_many_fractional_digits"),
        ({"configuration": configuration(threshold_ppb="1000000001")}, 422, "invalid_configuration",
         "out_of_domain"),
        ({"configuration": {k: v for k, v in DEFAULT_CONFIGURATION.items() if k != "fee_per_fill"}}, 400,
         "malformed_request", "missing_field"),
    ],
)  # fmt: skip
def test_invalid_requests_follow_the_api_conventions(
    service: Running, change: dict[str, Any], status: int, code: str, reason: str | None
) -> None:
    returned, document, _ = service.call("POST", f"{API}/runs", request(experiment_id(service), **change))
    assert returned == status, document
    assert document["error"]["code"] == code
    assert document["error"]["reason"] == reason


def test_bodies_queries_methods_and_hosts_are_strict(service: Running) -> None:
    experiment = experiment_id(service)
    body = json.dumps(request(experiment)).encode()
    cases = [
        service.call("POST", f"{API}/runs", raw=body, content_type="text/plain"),
        service.call("POST", f"{API}/runs", raw=b'{"model":"a","model":"b"}'),
        service.call("POST", f"{API}/runs", raw=body.replace(b'"1000000"', b"1000000")),
        service.call("POST", f"{API}/runs", raw=b"[]"),
        service.call("POST", f"{API}/runs", raw=b" " * (16 * 1024 + 1)),
        service.call("GET", f"{API}/experiments?limit=0"),
        service.call("GET", f"{API}/experiments?limit=201"),
        service.call("GET", f"{API}/experiments?verbose=1"),
        service.call("GET", f"{API}/experiments?limit=1&limit=2"),
        service.call("GET", f"{API}/experiments?cursor=bm90LWEtY3Vyc29y"),
        service.call("GET", f"{API}/results?cursor={encode_cursor('experiments', '0')}"),
        service.call("GET", f"{API}/status?x=1"),
        service.call("PUT", f"{API}/runs", raw=body),
        service.call("DELETE", f"{API}/experiments"),
        service.call("GET", f"{API}/nothing"),
        service.call("GET", "/api/v1/accounts"),
        service.call("GET", f"{API}/status", host="example.com"),
        service.call("GET", f"{API}/results/{'..%2F' * 3}etc"),
        service.call("GET", f"{API}/runs/{'f' * 31}"),
    ]
    expected = [
        (415, "unsupported_media_type"), (400, "duplicate_field"), (400, "invalid_type"),
        (400, "invalid_type"), (413, "payload_too_large"), (400, "invalid_limit"), (400, "invalid_limit"),
        (400, "invalid_query"), (400, "invalid_query"), (400, "invalid_cursor"), (400, "invalid_cursor"),
        (400, "invalid_query"), (405, "method_not_allowed"), (405, "method_not_allowed"),
        (404, "route_not_found"), (404, "route_not_found"), (421, "misdirected_request"),
        (400, "invalid_identifier"), (400, "invalid_identifier"),
    ]  # fmt: skip
    for (status, document, _), (expected_status, code) in zip(cases, expected, strict=True):
        assert status == expected_status, document
        assert code in (document["error"]["code"], document["error"]["reason"]), document
    assert cases[12][2]["Allow"] == "GET, POST"


def test_an_unevaluable_model_cannot_be_started(tools: built.Tools, tmp_path: Path) -> None:
    directory = Tools(tools.chronos_dataset.parent)
    (tmp_path / "events.csv").write_text(synthetic_events(seed=7, quotes_per_instrument=30))
    run_demo(
        directory,
        ArtifactRoots.under(tmp_path / "data"),
        DemoSource("synthetic-stream", synthetic_events=tmp_path / "events.csv"),
        150_000,
    )
    for running in serving(
        ServiceConfiguration(tools=directory, artifacts=tmp_path / "data", state=tmp_path / "state", port=0)
    ):
        experiment = experiment_id(running)
        logistic = running.get(f"{API}/experiments/{experiment}")["experiment"]["instruments"][0]["models"][
            "logistic"
        ]
        assert logistic == {"reason": "insufficient_class_support", "status": "unevaluable"}
        status, document, _ = running.call("POST", f"{API}/runs", request(experiment))
        assert (status, document["error"]["code"], document["error"]["reason"]) == (
            422, "unevaluable_model", "insufficient_class_support",
        )  # fmt: skip


def writable_copy(source: Path, destination: Path) -> None:
    shutil.copytree(source, destination)
    for path in (destination, *destination.iterdir()):
        os.chmod(path, stat.S_IRWXU)


def test_symlinked_and_corrupt_artifacts_are_refused(artifacts: tuple[Tools, Path], tmp_path: Path) -> None:
    tools, data = artifacts
    copied = tmp_path / "data"
    shutil.copytree(data, copied, symlinks=True)
    for path in copied.rglob("*"):
        os.chmod(path, stat.S_IRWXU)
    strategies = copied / "strategies"
    run_id = sorted(os.listdir(strategies))[0]
    journal = strategies / run_id / "journal.ndjson"
    journal.write_bytes(journal.read_bytes().replace(b'"buy"', b'"sell"', 1))
    # A symlink with a valid ID that points at another run's directory.
    other = sorted(os.listdir(strategies))[1]
    os.symlink(strategies / other, strategies / ("f" * 64))
    for running in serving(
        ServiceConfiguration(tools=tools, artifacts=copied, state=tmp_path / "state", port=0)
    ):
        status, document, _ = running.call("GET", f"{API}/results/{run_id}/trades")
        assert (status, document["error"]["code"]) == (422, "artifact_invalid")
        status, document, _ = running.call("GET", f"{API}/results/{'f' * 64}")
        assert (status, document["error"]["code"]) == (422, "artifact_invalid")
        listed = running.get(f"{API}/results")
        assert "f" * 64 in {entry["run_id"] for entry in listed["invalid_entries"]}


def test_unfinished_jobs_are_interrupted_on_restart_and_finished_ones_kept(tmp_path: Path) -> None:
    store = JobStore(tmp_path / "jobs", retain=10)
    finished, _ = store.create({"n": "1"}, "a")
    finished.state = "succeeded"
    store.save(finished)
    running, _ = store.create({"n": "2"}, "b")
    running.state = "running"
    store.save(running)
    queued, _ = store.create({"n": "3"}, "c")
    recovered = JobStore(tmp_path / "jobs", retain=10)
    assert recovered.get(finished.job_id).state == "succeeded"  # type: ignore[union-attr]
    for job_id in (running.job_id, queued.job_id):
        job = recovered.get(job_id)
        assert job is not None
        assert job.state == "interrupted"
        assert job.error is not None
        assert job.error["code"] == "interrupted"
    # A retry after an interruption is new work.
    retried, created = recovered.create({"n": "2"}, "b")
    assert created
    assert retried.job_id != running.job_id


def test_failed_timed_out_and_queue_full_jobs_are_reported(
    artifacts: tuple[Tools, Path], tmp_path: Path
) -> None:
    tools, data = artifacts
    sleeper = tmp_path / "sleeper"
    # Like a job's C++ tool: a grandchild the time limit must stop too.
    sleeper.write_text(f"#!/bin/sh\nsleep 30 &\necho $! > {tmp_path / 'grandchild'}\nwait\n")
    sleeper.chmod(0o755)
    failing = tmp_path / "failing"
    failing.write_text(
        '#!/bin/sh\necho \'{"error": {"kind": "ArtifactError", "message": "refused"}}\' >&2\nexit 1\n'
    )
    failing.chmod(0o755)
    base = ServiceConfiguration(
        tools=tools, artifacts=data, state=tmp_path / "failing-state", port=0, python=str(failing)
    )
    for running in serving(base):
        experiment = experiment_id(running)
        _, started, _ = running.call("POST", f"{API}/runs", request(experiment))
        job = running.wait(started["job"]["job_id"])
        assert job["state"] == "failed"
        assert job["error"] == {"code": "evaluation_failed", "kind": "ArtifactError", "message": "refused"}
    slow = replace(base, state=tmp_path / "slow-state", python=str(sleeper), job_timeout_s=1, max_queued=1)
    for running in serving(slow):
        experiment = experiment_id(running)
        first = running.call("POST", f"{API}/runs", request(experiment))[1]["job"]
        time.sleep(0.5)
        second = running.call("POST", f"{API}/runs", request(experiment, window="validation"))
        third = running.call("POST", f"{API}/runs", request(experiment, instrument="SPY"))
        assert second[0] == 202
        assert (third[0], third[1]["error"]["code"]) == (503, "queue_full")
        assert running.wait(first["job_id"])["error"]["code"] == "timed_out"
        grandchild = int((tmp_path / "grandchild").read_text())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and _alive(grandchild):
            time.sleep(0.05)
        assert not _alive(grandchild)


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie still answers; it has exited unless ps shows it running.
    state = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True, check=False)
    return state.stdout.strip() != "" and not state.stdout.strip().startswith("Z")


def test_cursors_and_timestamps_are_exact() -> None:
    assert rfc3339(1_790_343_599_969_574_414) == "2026-09-25T13:39:59.969574414Z"
    cursor = encode_cursor("trades", "a" * 64, "strategy", "4")
    assert decode_cursor(cursor, "trades", "a" * 64, "strategy") == 4
    with pytest.raises(Exception, match="cursor"):
        decode_cursor(cursor, "trades", "a" * 64, "cash")
    assert sys.executable


@pytest.mark.parametrize("filename", ["../outside.json", "nested/../outside.json"])
def test_artifact_file_names_cannot_escape_the_directory(tmp_path: Path, filename: str) -> None:
    directory = tmp_path / "artifact"
    directory.mkdir()
    (directory / "nested").mkdir()
    contents = b"{}\n"
    (tmp_path / "outside.json").write_bytes(contents)
    (directory / "outside.json").write_bytes(contents)
    digest = {
        "name": filename,
        "sha256": hashlib.sha256(contents).hexdigest(),
        "size_bytes": str(len(contents)),
    }
    with pytest.raises(ArtifactUnavailableError):
        verified_file(directory, digest)


def test_artifact_catalog_roots_cannot_be_symlinks(tmp_path: Path) -> None:
    outside = tmp_path / "outside"
    outside.mkdir()
    (outside / ("a" * 64)).mkdir()
    linked = tmp_path / "strategies"
    linked.symlink_to(outside, target_is_directory=True)
    with pytest.raises(ArtifactUnavailableError):
        artifact_directory(linked, "a" * 64)


def test_shutdown_kills_workers_that_ignore_termination(tmp_path: Path) -> None:
    ready = tmp_path / "worker-pid"
    executable = tmp_path / "ignore-termination"
    executable.write_text(f"#!/bin/sh\ntrap '' TERM\necho $$ > '{ready}'\nwhile :; do sleep 30; done\n")
    executable.chmod(0o755)
    store = JobStore(tmp_path / "jobs", retain=10)
    workers = Workers(
        ServiceConfiguration(
            tools=Tools(tmp_path),
            artifacts=tmp_path / "data",
            state=tmp_path,
            python=str(executable),
            job_timeout_s=60,
        ),
        store,
    )
    job, _ = store.create(request("a" * 64), "test-request")
    workers.submit(job)
    pid: int | None = None
    try:
        deadline = time.monotonic() + 5
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert ready.exists()
        pid = int(ready.read_text())
        started = time.monotonic()
        workers.stop()
        assert time.monotonic() - started < 3
        assert not _alive(pid)
        assert all(not thread.is_alive() for thread in workers.threads)
        assert store.get(job.job_id).state == "interrupted"  # type: ignore[union-attr]
    finally:
        if pid is not None and _alive(pid):
            os.killpg(pid, signal.SIGKILL)
        for thread in workers.threads:
            thread.join(timeout=3)


@pytest.mark.parametrize("query", ["cursor=%FF", "cursor=%ZZ", "limit=%"])
def test_malformed_query_encoding_is_a_client_error(service: Running, query: str) -> None:
    status, document, _ = service.call("GET", f"{API}/runs?{query}")
    assert (status, document["error"]["code"], document["error"]["reason"]) == (
        400,
        "malformed_request",
        "invalid_query",
    )


@pytest.mark.parametrize("stream", ["stdout", "stderr"])
def test_worker_output_is_bounded_while_the_process_is_running(tmp_path: Path, stream: str) -> None:
    executable = tmp_path / "oversized-output"
    redirect = " >&2" if stream == "stderr" else ""
    executable.write_text(f"#!/bin/sh\nhead -c 1100000 /dev/zero{redirect}\nsleep 30\n")
    executable.chmod(0o755)
    store = JobStore(tmp_path / "jobs", retain=10)
    workers = Workers(
        ServiceConfiguration(
            tools=Tools(tmp_path),
            artifacts=tmp_path / "data",
            state=tmp_path,
            python=str(executable),
            job_timeout_s=60,
        ),
        store,
    )
    job, _ = store.create(request("a" * 64), "oversized-output")
    try:
        workers.submit(job)
        deadline = time.monotonic() + 3
        while job.state not in ("succeeded", "failed", "interrupted") and time.monotonic() < deadline:
            time.sleep(0.01)
        assert job.state == "failed"
        assert job.error == {"code": "worker_failed", "message": "the job's output was too large"}
        assert len(job.log.encode()) <= 4096
    finally:
        workers.stop()
