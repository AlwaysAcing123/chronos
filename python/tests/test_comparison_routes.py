"""Comparisons over HTTP: the request conventions, the comparison job and
its dedup, the list and detail with re-verified member artifacts, a
restart, and evaluation results that list only experiment runs."""

import os
import stat
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from chronos_research.onnx_sandbox import Sandbox
from chronos_research.pipeline import ArtifactRoots, Tools, experiment_split
from chronos_research.service import ServiceConfiguration
from tests.test_comparison import CONFIGURATION
from tests.test_model_routes import finish, registered, wait_job
from tests.test_service import API, Running, serving


def configuration(artifacts: tuple[Tools, Path], data: Path, tmp_path: Path) -> ServiceConfiguration:
    return ServiceConfiguration(
        tools=artifacts[0],
        artifacts=data,
        state=tmp_path / "state",
        port=0,
        sandbox=Sandbox(docker="/nonexistent/docker"),
    )


@pytest.fixture
def service(demo_artifacts: tuple[Tools, Path], demo_data: Path, tmp_path: Path) -> Iterator[Running]:
    yield from serving(configuration(demo_artifacts, demo_data, tmp_path))


def request(service: Running, models: list[str], **changes: Any) -> dict[str, Any]:
    roots = ArtifactRoots.under(service.configuration.artifacts)
    (experiment,) = os.listdir(roots.experiments)
    return {
        "configuration": dict(CONFIGURATION),
        "instrument": "AAPL",
        "model_version_ids": models,
        "split_artifact_id": experiment_split(roots.experiments, experiment),
        "window": "test",
        **changes,
    }


def two_models(service: Running) -> list[str]:
    logistic = registered(service)["result"]["model_version_id"]
    majority = registered(service, model="majority", name="aapl-majority")["result"]["model_version_id"]
    return [str(logistic), str(majority)]


def compared(service: Running, models: list[str]) -> dict[str, Any]:
    status, started, _ = service.call("POST", f"{API}/comparisons", request(service, models))
    assert status == 202, started
    job = wait_job(service, started["job"]["job_id"])
    assert job["state"] == "succeeded", job
    return job


def test_a_comparison_request_follows_the_conventions(service: Running) -> None:
    models = two_models(service)
    for changes, status, code in (
        ({"model_version_ids": models[:1]}, 422, "invalid_configuration"),
        ({"model_version_ids": [models[0], models[0]]}, 422, "invalid_configuration"),
        ({"model_version_ids": ["not-an-id", models[0]]}, 400, "malformed_request"),
        ({"model_version_ids": [models[0], "e" * 64]}, 422, "model_not_found"),
        ({"split_artifact_id": "0" * 64}, 422, "split_not_found"),
        ({"window": "training"}, 400, "malformed_request"),
        ({"configuration": {**CONFIGURATION, "fee_per_fill": "-1"}}, 422, "invalid_configuration"),
        ({"extra": "field"}, 400, "malformed_request"),
    ):
        answered, document, _ = service.call(
            "POST", f"{API}/comparisons", request(service, models, **changes)
        )
        assert (answered, document["error"]["code"]) == (status, code), changes
    assert service.get(f"{API}/jobs?kind=comparison")["jobs"] == []


def test_a_comparison_job_publishes_one_verified_artifact(service: Running) -> None:
    models = two_models(service)
    job = compared(service, models)
    assert job["kind"] == "comparison"
    assert job["result"]["status"] == {"completed": "2", "failed": "0", "refused": "0"}
    comparison_id = job["result"]["comparison_id"]
    # An equal request is the same job.
    _, again, _ = service.call("POST", f"{API}/comparisons", request(service, models))
    assert (again["deduplicated"], again["job"]["job_id"]) == (True, job["job_id"])
    (listed,) = service.get(f"{API}/comparisons")["comparisons"]
    assert listed["comparison_id"] == comparison_id
    assert [member["state"] for member in listed["members"]] == ["completed", "completed"]
    detail = service.get(f"{API}/comparisons/{comparison_id}")
    assert [check["schedule"] for check in detail["artifacts"]] == ["verified", "verified"]
    assert [check["run"] for check in detail["artifacts"]] == ["verified", "verified"]
    comparison = detail["comparison"]
    assert comparison["window"]["start_at"].endswith("Z")
    first = comparison["members"][0]
    assert first["information_cutoff"]["at"].endswith("Z")
    # A member's run is readable as a result, but evaluation results list
    # only experiment runs.
    run_id = first["run"]["artifact_id"]
    assert service.get(f"{API}/results/{run_id}")["result"]["source"]["model_version_id"] == models[0]
    assert all(entry["run_id"] != run_id for entry in service.get(f"{API}/results")["results"])
    assert service.get(f"{API}/results")["invalid_entries"] == []


def test_comparisons_survive_a_restart_and_corruption_is_reported(
    demo_artifacts: tuple[Tools, Path], demo_data: Path, tmp_path: Path
) -> None:
    data = demo_data
    first = serving(configuration(demo_artifacts, data, tmp_path))
    running = next(first)
    try:
        comparison_id = compared(running, two_models(running))["result"]["comparison_id"]
    finally:
        finish(first)
    second = serving(configuration(demo_artifacts, data, tmp_path))
    running = next(second)
    try:
        assert (
            running.get(f"{API}/comparisons/{comparison_id}")["comparison"]["comparison_id"] == comparison_id
        )
        # A member's run journal edited after the fact is reported per member.
        detail = running.get(f"{API}/comparisons/{comparison_id}")
        run_dir = data / "strategies" / detail["comparison"]["members"][1]["run"]["artifact_id"]
        os.chmod(run_dir, stat.S_IRWXU)
        journal = run_dir / "journal.ndjson"
        os.chmod(journal, stat.S_IRUSR | stat.S_IWUSR)
        journal.write_bytes(journal.read_bytes().replace(b'"buy"', b'"sel"', 1))
        checks = running.get(f"{API}/comparisons/{comparison_id}")["artifacts"]
        assert checks[0]["run"] == "verified"
        assert checks[1]["run"].startswith("invalid")
        # The comparison itself edited is refused.
        directory = data / "comparisons" / comparison_id
        os.chmod(directory, stat.S_IRWXU)
        manifest = directory / "manifest.json"
        os.chmod(manifest, stat.S_IRUSR | stat.S_IWUSR)
        manifest.write_bytes(manifest.read_bytes().replace(b'"completed"', b'"failed"', 1))
        status, document, _ = running.call("GET", f"{API}/comparisons/{comparison_id}")
        assert (status, document["error"]["code"]) == (422, "artifact_invalid")
        listing = running.get(f"{API}/comparisons")
        assert listing["comparisons"] == []
        assert [entry["comparison_id"] for entry in listing["invalid_entries"]] == [comparison_id]
    finally:
        finish(second)


def test_a_comparison_running_at_a_restart_is_interrupted_and_runs_again_only_when_asked(
    tmp_path: Path,
) -> None:
    from chronos_research import comparison
    from chronos_research.service import JobStore

    spec = {
        "configuration": dict(CONFIGURATION),
        "instrument": "AAPL",
        "model_version_ids": ["a" * 64, "b" * 64],
        "schema": comparison.SPEC_SCHEMA,
        "split_artifact_id": "c" * 64,
        "window": "test",
    }
    store = JobStore(tmp_path / "jobs", retain=10)
    job, created = store.create(spec, comparison.spec_digest(spec), "comparison")
    assert created
    job.state = "running"
    store.save(job)
    reopened = JobStore(tmp_path / "jobs", retain=10)
    interrupted = reopened.get(job.job_id)
    assert interrupted is not None
    assert (interrupted.kind, interrupted.state, interrupted.error["code"]) == (  # type: ignore[index]
        "comparison",
        "interrupted",
        "interrupted",
    )
    # Nothing resumes it; the same spec submitted again is new work.
    again, created_again = reopened.create(spec, comparison.spec_digest(spec), "comparison")
    assert created_again
    assert again.job_id != job.job_id
