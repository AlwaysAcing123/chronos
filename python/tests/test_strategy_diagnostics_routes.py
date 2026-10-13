"""Strategy diagnostics through the research service (ADR 0024): generated
only by a POST, as a deduplicated child job off the request thread; read
verified by GETs that never start work; every verification level reported
apart; rows paged per run; and the bounded worker's timeout, restart,
progress, and cleanup for the new job kind."""

import json
import os
import shutil
import signal
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest

from chronos_research.pipeline import Tools
from chronos_research.service import JobStore, ServiceConfiguration, Workers, job_arguments
from chronos_research.strategy_diagnostics import diagnostics_root
from tests.test_service import API, Running, serving
from tests.test_strategy_diagnostics import Studies, studies  # noqa: F401  (the module fixture)

TERMINAL = ("succeeded", "failed", "interrupted")


@pytest.fixture
def copy(studies: Studies, tmp_path: Path) -> Studies:  # noqa: F811
    root = tmp_path / "artifacts"
    shutil.copytree(studies.artifacts, root, symlinks=True)
    return Studies(type(studies.recordings)(artifacts=root, tools=studies.tools), studies.ids)


@pytest.fixture
def service(copy: Studies, tmp_path: Path) -> Iterator[Running]:
    yield from serving(
        ServiceConfiguration(tools=copy.tools, artifacts=copy.artifacts, state=tmp_path / "state", port=0)
    )


def wait(service: Running, job_id: str, timeout_s: float = 240) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        job: dict[str, Any] = service.get(f"{API}/jobs/{job_id}")["job"]
        if job["state"] in TERMINAL:
            return job
        time.sleep(0.1)
    raise AssertionError(f"job {job_id} did not finish")


def generate(service: Running, *study_ids: str) -> dict[str, Any]:
    status, started, _ = service.call("POST", f"{API}/strategy-diagnostics", {"study_ids": list(study_ids)})
    assert status == 202, started
    assert started["job"]["kind"] == "strategy_diagnostics"
    return wait(service, started["job"]["job_id"])


def test_a_post_generates_once_and_gets_only_read(copy: Studies, service: Running) -> None:
    week, rounding = copy.ids["week"], copy.ids["rounding"]
    # Reads before anything exists start nothing.
    assert service.get(f"{API}/strategy-diagnostics") == {
        "diagnostics": [],
        "invalid_entries": [],
        "next_cursor": None,
    }
    assert service.call("GET", f"{API}/strategy-diagnostics/{'a' * 64}")[0] == 404
    assert service.get(f"{API}/jobs?kind=strategy_diagnostics")["jobs"] == []
    assert not diagnostics_root(copy.artifacts).exists()
    job = generate(service, rounding, week)
    assert job["state"] == "succeeded", job
    identifier = job["result"]["diagnostic_id"]
    assert job["result"] == {
        "diagnostic_id": identifier,
        "outcome": "published",
        "status": "complete",
        "study_ids": sorted([week, rounding]),
    }
    assert job["request"] == {"study_ids": sorted([week, rounding])}
    assert job["progress"] == {"stage": "publish"}
    # The same studies in another order are the same job.
    status, again, _ = service.call("POST", f"{API}/strategy-diagnostics", {"study_ids": [week, rounding]})
    assert (status, again["deduplicated"], again["job"]["job_id"]) == (200, True, job["job_id"])
    detail = service.get(f"{API}/strategy-diagnostics/{identifier}")
    diagnostics = detail["diagnostics"]
    assert diagnostics["diagnostic_id"] == identifier
    window = diagnostics["studies"][0]["folds"][0]["window"]
    assert set(window) == {"end_at", "name", "start_at"}
    assert window["start_at"].endswith("Z")
    verification = detail["verification"]
    assert verification["artifact"]["state"] == "verified"
    now = verification["sources"]["now"]
    assert now["state"] == "verified"
    assert set(now["sources"]) == {
        "collections",
        "datasets",
        "features",
        "labels",
        "plans",
        "runs",
        "schedules",
        "studies",
    }
    assert {state for states in now["sources"].values() for state in states.values()} == {"verified"}
    assert {trust["state"] for trust in now["figures"]["runs"].values()} == {"verified"}
    assert verification["recomputed_inspection"]["state"] == "not_run_by_this_read"
    assert verification["independent_audit"]["state"] == "not_run_by_this_read"
    assert verification["engine_replay"]["state"] == "recorded_by_study"
    # No path of this machine appears in what a read returns.
    assert str(copy.artifacts) not in json.dumps(detail)
    listed = service.get(f"{API}/strategy-diagnostics")
    assert [entry["diagnostic_id"] for entry in listed["diagnostics"]] == [identifier]
    assert service.get(f"{API}/strategy-diagnostics?study={week}")["diagnostics"] == listed["diagnostics"]
    assert service.get(f"{API}/strategy-diagnostics?study={copy.ids['stale']}")["diagnostics"] == []
    # Reads started no job and wrote no artifact.
    assert len(service.get(f"{API}/jobs?kind=strategy_diagnostics")["jobs"]) == 1
    assert os.listdir(diagnostics_root(copy.artifacts)) == [identifier]


def test_rows_page_per_run_and_account(copy: Studies, service: Running) -> None:
    job = generate(service, copy.ids["week"])
    identifier = job["result"]["diagnostic_id"]
    diagnostics = service.get(f"{API}/strategy-diagnostics/{identifier}")["diagnostics"]
    run = next(
        entry
        for fold in diagnostics["studies"][0]["folds"]
        for entry in fold["runs"]
        if entry["model"] == "logistic" and int(entry["accounts"]["strategy"]["fills"]) > 3
    )
    base = f"{API}/strategy-diagnostics/{identifier}/fills?run={run['run_id']}&account=strategy"
    first = service.get(f"{base}&limit=2")
    assert len(first["fills"]) == 2
    assert first["next_cursor"] is not None
    assert {row["account"] for row in first["fills"]} == {"strategy"}
    assert "executed_at" in first["fills"][0]["execution"]
    assert first["fills"][0]["timing"]["configured_latency_ns"] == run["latency_ns"]
    collected = list(first["fills"])
    cursor = first["next_cursor"]
    while cursor is not None:
        page = service.get(f"{base}&limit=2&cursor={cursor}")
        collected.extend(page["fills"])
        cursor = page["next_cursor"]
    assert len(collected) == int(run["accounts"]["strategy"]["fills"])
    everyone = service.get(f"{API}/strategy-diagnostics/{identifier}/fills?run={run['run_id']}&limit=200")
    assert len(everyone["fills"]) == sum(int(account["fills"]) for account in run["accounts"].values())
    episodes = service.get(f"{API}/strategy-diagnostics/{identifier}/episodes?run={run['run_id']}&limit=200")
    assert {episode["state"] for episode in episodes["episodes"]} <= {"completed", "censored"}
    # A cursor of one run's list is not another's.
    other = f"{API}/strategy-diagnostics/{identifier}/fills?run={run['run_id']}&account=buy_and_hold"
    other += f"&cursor={first['next_cursor']}"
    assert service.call("GET", other)[0] == 400
    for query, status in (
        ("", 400),
        (f"run={'f' * 64}", 404),
        ("run=nope", 400),
        (f"run={run['run_id']}&account=paper", 400),
        (f"run={run['run_id']}&path=/etc", 400),
    ):
        assert service.call("GET", f"{API}/strategy-diagnostics/{identifier}/fills?{query}")[0] == status


@pytest.mark.parametrize(
    ("document", "status", "reason"),
    [
        ({}, 400, "missing_field"),
        ({"study_ids": []}, 422, "out_of_domain"),
        ({"study_ids": "a"}, 400, "invalid_type"),
        ({"study_ids": ["a" * 64] * 2}, 422, "out_of_domain"),
        ({"study_ids": [f"{n}" * 64 for n in range(5)]}, 422, "out_of_domain"),
        ({"study_ids": ["../../etc"]}, 400, "invalid_identifier"),
        ({"study_ids": ["a" * 64], "path": "/tmp"}, 400, "unexpected_field"),
        ({"study_ids": ["a" * 64]}, 422, None),
    ],
)
def test_requests_are_strict_and_refused_before_any_job(
    service: Running, document: dict[str, Any], status: int, reason: str | None
) -> None:
    code, refused, _ = service.call("POST", f"{API}/strategy-diagnostics", document)
    assert code == status, refused
    if reason is not None:
        assert refused["error"]["reason"] == reason
    else:
        assert refused["error"]["code"] == "study_not_found"
    assert service.get(f"{API}/jobs?kind=strategy_diagnostics")["jobs"] == []


def test_identifiers_never_name_paths(service: Running) -> None:
    for identifier in ("..", "%2e%2e", "nope", "A" * 64):
        assert service.call("GET", f"{API}/strategy-diagnostics/{identifier}")[0] in (400, 404)
    assert service.call("GET", f"{API}/strategy-diagnostics?study=../x")[0] == 400
    assert service.call("DELETE", f"{API}/strategy-diagnostics")[0] == 405


def test_a_changed_source_is_reported_on_read_and_refused_by_a_new_job(
    copy: Studies, service: Running
) -> None:
    identifier = generate(service, copy.ids["week"])["result"]["diagnostic_id"]
    diagnostics = service.get(f"{API}/strategy-diagnostics/{identifier}")["diagnostics"]
    run_id = diagnostics["studies"][0]["folds"][0]["runs"][0]["run_id"]
    journal = copy.artifacts / "strategies" / run_id / "journal.ndjson"
    journal.parent.chmod(0o755)
    journal.chmod(0o644)
    journal.write_bytes(journal.read_bytes() + b"\n")
    verification = service.get(f"{API}/strategy-diagnostics/{identifier}")["verification"]
    assert verification["artifact"]["state"] == "verified"
    assert verification["sources"]["now"]["state"] == "invalid"
    assert verification["sources"]["now"]["sources"]["runs"][run_id].startswith("invalid:")
    assert verification["sources"]["now"]["figures"]["runs"][run_id] == {
        "state": "invalid",
        "sources": [f"runs:{run_id}"],
    }
    failed = generate(service, copy.ids["week"], copy.ids["stale"])
    assert failed["state"] == "failed"
    assert (failed["error"]["code"], failed["error"]["reason"]) == ("diagnostics_refused", "source_invalid")
    # A tampered artifact cannot be read at all.
    fills = diagnostics_root(copy.artifacts) / identifier / "fills.ndjson"
    fills.parent.chmod(0o755)
    fills.chmod(0o644)
    fills.write_bytes(fills.read_bytes() + b"\n")
    code, refused, _ = service.call("GET", f"{API}/strategy-diagnostics/{identifier}")
    assert (code, refused["error"]["code"]) == (422, "artifact_invalid")
    listed = service.get(f"{API}/strategy-diagnostics")
    assert listed["diagnostics"] == []
    assert listed["invalid_entries"][0]["diagnostic_id"] == identifier


def _same_size_change(path: Path) -> None:
    """One digit changed in place, and the modification time put back: only
    the content and the change time differ."""
    status = path.stat()
    data = bytearray(path.read_bytes())
    position = next(index for index in range(len(data) - 1, 0, -1) if data[index] in b"12345678")
    data[position] += 1
    path.write_bytes(bytes(data))
    os.utime(path, ns=(status.st_atime_ns, status.st_mtime_ns))


def _appended(path: Path) -> None:
    path.write_bytes(path.read_bytes() + b"\n")


def _removed(path: Path) -> None:
    path.unlink()


CHANGES: dict[str, tuple[str, str, str, Callable[[Path], None]]] = {
    "events appended": (
        "datasets",
        "dataset_id",
        "events.ndjson",
        _appended,
    ),
    "labels appended": (
        "labels",
        "label_artifact_id",
        "labels.ndjson",
        _appended,
    ),
    "features changed in place": ("features", "feature_artifact_id", "features.ndjson", _same_size_change),
    "labels missing": ("labels", "label_artifact_id", "labels.ndjson", _removed),
}
DIRECTORIES = {"datasets": "normalized", "features": "features", "labels": "labels"}


@pytest.mark.parametrize("change", sorted(CHANGES))
def test_every_dependency_is_checked_on_read_and_downgrades_only_what_depends_on_it(
    copy: Studies, service: Running, change: str
) -> None:
    week, rounding = copy.ids["week"], copy.ids["rounding"]
    identifier = generate(service, week, rounding)["result"]["diagnostic_id"]
    path = f"{API}/strategy-diagnostics/{identifier}"
    first = service.get(path)
    now = first["verification"]["sources"]["now"]
    assert now["state"] == "verified"
    assert int(now["content"]["hashed_now"]) > 0
    # The cached report's next read reuses every digest of an unchanged file.
    again = service.get(path)["verification"]["sources"]["now"]
    assert (again["state"], again["content"]["hashed_now"]) == ("verified", "0")
    assert again["content"]["reused_unchanged"] == now["content"]["hashed_now"]

    kind, key, name, mutate = CHANGES[change]
    sections = {section["study_id"]: section for section in first["diagnostics"]["studies"]}
    fold = sections[week]["folds"][0]
    source = copy.artifacts / DIRECTORIES[kind] / fold[key] / name
    source.parent.chmod(0o755)
    source.chmod(0o644)
    mutate(source)
    after = service.get(path)["verification"]["sources"]["now"]
    assert after["state"] == "invalid"
    assert after["sources"][kind][fold[key]].startswith("invalid:")
    figures = after["figures"]
    changed = f"{kind}:{fold[key]}"
    for other in sections[week]["folds"]:
        expected = "invalid" if other["index"] == fold["index"] else "verified"
        for run in other["runs"]:
            if run["state"] == "completed":
                assert figures["runs"][run["run_id"]]["state"] == expected, (other["index"], run["run_id"])
        assert {trust["state"] for trust in figures["studies"][week]["folds"][other["index"]].values()} == {
            expected
        }
    assert figures["runs"][fold["runs"][0]["run_id"]]["sources"] == [changed]
    week_pools = figures["studies"][week]["pooled"]
    assert week_pools["trading"]["logistic"]["base"] == {"state": "invalid", "sources": [changed]}
    assert week_pools["predictions"]["logistic"]["state"] == "invalid"
    # The other study depends on none of it, so nothing of it is downgraded.
    rounding_pools = figures["studies"][rounding]["pooled"]
    assert {
        trust["state"] for scenarios in rounding_pools["trading"].values() for trust in scenarios.values()
    } == {"verified"}
    across = figures["across_studies"]
    assert across["trading"]["logistic"]["base"]["state"] == "invalid"
    assert across["predictions"]["majority"]["state"] == "invalid"


def test_the_child_command_is_a_fixed_argument_vector(tmp_path: Path) -> None:
    configuration = ServiceConfiguration(
        tools=Tools(tmp_path / "tools"), artifacts=tmp_path / "data", state=tmp_path
    )
    arguments = job_arguments(configuration, {"study_ids": ["a" * 64, "b" * 64]}, "strategy_diagnostics")
    assert arguments[1:] == [
        "-m", "chronos_research", "diagnose-strategies",
        "--tools", str(tmp_path / "tools"), "--artifacts", str(tmp_path / "data"),
        "--study", "a" * 64, "--study", "b" * 64, "--progress",
    ]  # fmt: skip


def _workers(tmp_path: Path, child: Path, timeout_s: int) -> tuple[JobStore, Workers]:
    store = JobStore(tmp_path / "jobs", retain=10)
    configuration = ServiceConfiguration(
        tools=Tools(tmp_path),
        artifacts=tmp_path / "data",
        state=tmp_path,
        python=str(child),
        job_timeout_s=timeout_s,
    )
    return store, Workers(configuration, store)


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def test_a_diagnostics_job_times_out_and_its_whole_process_group_is_killed(tmp_path: Path) -> None:
    started = tmp_path / "grandchild-pid"
    child = tmp_path / "slow-diagnostics"
    child.write_text(
        "#!/bin/sh\n"
        'echo \'{"progress": {"stage": "verify_sources"}}\' >&2\n'
        f"sleep 60 & echo $! > '{started}'\n"
        "wait\n"
    )
    child.chmod(0o755)
    store, workers = _workers(tmp_path, child, timeout_s=1)
    job, _ = store.create({"study_ids": ["a" * 64]}, "slow", "strategy_diagnostics")
    try:
        workers.submit(job)
        deadline = time.monotonic() + 10
        while job.state not in TERMINAL and time.monotonic() < deadline:
            time.sleep(0.02)
        assert job.state == "failed"
        assert job.error is not None
        assert job.error["code"] == "timed_out"
        assert job.progress == {"stage": "verify_sources"}
        grandchild = int(started.read_text())
        deadline = time.monotonic() + 3
        while _alive(grandchild) and time.monotonic() < deadline:
            time.sleep(0.02)
        assert not _alive(grandchild)
    finally:
        workers.stop()


def test_a_diagnostics_job_is_interrupted_on_shutdown_and_restart_never_resumes_it(tmp_path: Path) -> None:
    ready = tmp_path / "pid"
    child = tmp_path / "stubborn-diagnostics"
    child.write_text(f"#!/bin/sh\ntrap '' TERM\necho $$ > '{ready}'\nwhile :; do sleep 30; done\n")
    child.chmod(0o755)
    store, workers = _workers(tmp_path, child, timeout_s=60)
    job, _ = store.create({"study_ids": ["a" * 64]}, "stubborn", "strategy_diagnostics")
    workers.submit(job)
    pid = None
    try:
        deadline = time.monotonic() + 5
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        pid = int(ready.read_text())
        workers.stop()
        assert not _alive(pid)
        assert store.get(job.job_id).state == "interrupted"  # type: ignore[union-attr]
    finally:
        if pid is not None and _alive(pid):
            os.killpg(pid, signal.SIGKILL)
    # A record left running by a crash is interrupted on the next start.
    queued, _ = store.create({"study_ids": ["b" * 64]}, "crashed", "strategy_diagnostics")
    queued.state = "running"
    store.save(queued)
    recovered = JobStore(tmp_path / "jobs", retain=10).get(queued.job_id)
    assert recovered is not None
    assert recovered.state == "interrupted"
    assert recovered.error == {
        "code": "interrupted",
        "message": "the research service stopped before this job finished; submit it again",
    }


def test_a_refused_child_is_reported_with_its_reason(tmp_path: Path) -> None:
    child = tmp_path / "refusing-diagnostics"
    child.write_text(
        "#!/bin/sh\n"
        'echo \'{"error": {"kind": "DiagnosticsError", "message": "study x: missing", '
        '"reason": "source_invalid"}}\' >&2\n'
        "exit 1\n"
    )
    child.chmod(0o755)
    store, workers = _workers(tmp_path, child, timeout_s=30)
    job, _ = store.create({"study_ids": ["a" * 64]}, "refused", "strategy_diagnostics")
    try:
        workers.submit(job)
        deadline = time.monotonic() + 10
        while job.state not in TERMINAL and time.monotonic() < deadline:
            time.sleep(0.02)
    finally:
        workers.stop()
    assert job.state == "failed"
    assert job.error == {
        "code": "diagnostics_refused",
        "kind": "DiagnosticsError",
        "message": "study x: missing",
        "reason": "source_invalid",
    }
