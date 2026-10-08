"""The model library over HTTP: registration as a job, listing, detail,
lifecycle, and persistence across a service restart."""

import os
import stat
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from chronos_research.pipeline import Tools
from chronos_research.service import ServiceConfiguration
from tests.test_service import API, Running, serving


@pytest.fixture
def service(demo_artifacts: tuple[Tools, Path], demo_data: Path, tmp_path: Path) -> Iterator[Running]:
    yield from serving(
        ServiceConfiguration(tools=demo_artifacts[0], artifacts=demo_data, state=tmp_path / "state", port=0)
    )


def experiment_id(service: Running) -> str:
    (experiment,) = service.get(f"{API}/experiments")["experiments"]
    return str(experiment["experiment_id"])


def registration(service: Running, **changes: str) -> dict[str, str]:
    return {
        "experiment_id": experiment_id(service),
        "instrument": "AAPL",
        "model": "logistic",
        "name": "aapl-logistic",
        "version": "1",
        **changes,
    }


def wait_job(service: Running, job_id: str) -> dict[str, Any]:
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        job: dict[str, Any] = service.get(f"{API}/jobs/{job_id}")["job"]
        if job["state"] in ("succeeded", "failed", "interrupted"):
            return job
        time.sleep(0.1)
    raise AssertionError(f"job {job_id} did not finish")


def registered(service: Running, **changes: str) -> dict[str, Any]:
    status, started, _ = service.call("POST", f"{API}/models", registration(service, **changes))
    assert status == 202, started
    job = wait_job(service, started["job"]["job_id"])
    assert job["state"] == "succeeded", job
    return job


def test_a_registration_runs_as_a_job_and_the_library_lists_it(service: Running) -> None:
    job = registered(service)
    assert job["kind"] == "registration"
    version_id = job["result"]["model_version_id"]
    assert (job["result"]["outcome"], job["result"]["binding_outcome"]) == ("published", "published")
    # An identical request is the same job.
    status, again, _ = service.call("POST", f"{API}/models", registration(service))
    assert status == 200
    assert again["deduplicated"] is True
    assert again["job"]["job_id"] == job["job_id"]
    # It is not an evaluation run, and the runs list keeps its shape.
    assert service.get(f"{API}/runs")["runs"] == []
    assert service.call("GET", f"{API}/runs/{job['job_id']}")[0] == 404
    assert [entry["job_id"] for entry in service.get(f"{API}/jobs?kind=registration")["jobs"]] == [
        job["job_id"]
    ]

    library = service.get(f"{API}/models")
    assert library["invalid_entries"] == []
    (entry,) = library["models"]
    assert entry["model_version_id"] == version_id
    assert entry["names"] == [{"name": "aapl-logistic", "version": "1"}]
    assert entry["origin"] == "experiment"
    assert entry["lifecycle"]["state"] == "active"
    cutoff = entry["training"]["information_cutoff"]
    assert cutoff["basis"] == "verified"
    assert cutoff["at"].endswith("Z")

    detail = service.get(f"{API}/models/{version_id}")["model"]
    assert detail["source_check"] == {"experiment_id": experiment_id(service), "state": "matches"}
    assert detail["compatibility"]["evaluation"]["status"] == "compatible"
    # An active version with a verified cutoff can be deployed to paper.
    assert detail["compatibility"]["deployment"]["status"] == "compatible"
    assert detail["inputs"]["name"] == "top_of_book_baseline"
    assert [field["name"] for field in detail["inputs"]["fields"]] == [
        "spread",
        "imbalance",
        "microprice_minus_midpoint",
        "bid_size",
        "ask_size",
    ]
    assert detail["output"]["class_order"] == ["down", "flat", "up"]
    assert detail["limitations"]


def test_a_taken_name_fails_the_job_with_its_reason(service: Running) -> None:
    first = registered(service)
    status, started, _ = service.call("POST", f"{API}/models", registration(service, model="majority"))
    assert status == 202
    job = wait_job(service, started["job"]["job_id"])
    assert job["state"] == "failed"
    assert job["error"]["code"] == "name_version_taken"
    assert first["result"]["model_version_id"] in job["error"]["message"]
    # The binding still names the first model only.
    names = {
        entry["model_version_id"]: entry["names"]
        for entry in service.get(f"{API}/models?state=all")["models"]
    }
    assert names[first["result"]["model_version_id"]] == [{"name": "aapl-logistic", "version": "1"}]


def test_registration_requests_follow_the_conventions(service: Running) -> None:
    for body, status, reason in [
        ({**registration(service), "extra": "1"}, 400, "unexpected_field"),
        ({key: value for key, value in registration(service).items() if key != "name"}, 400, "missing_field"),
        (registration(service, name="Upper Case"), 400, "invalid_identifier"),
        (registration(service, version="1.02"), 400, "invalid_identifier"),
        (registration(service, model="onnx"), 400, "unknown_enum_value"),
        (registration(service, experiment_id="0" * 64), 404, None),
        (registration(service, instrument="MSFT"), 422, None),
    ]:
        code, document, headers = service.call("POST", f"{API}/models", body)
        assert code == status, document
        assert headers["X-Chronos-Service"] == "research"
        if reason is not None:
            assert document["error"]["reason"] == reason
    assert service.call("GET", f"{API}/models?state=champion")[0] == 400
    assert service.call("GET", f"{API}/jobs?kind=upload")[0] == 400
    assert service.call("GET", f"{API}/models/not-an-id")[0] == 400
    assert service.call("GET", f"{API}/models/{'a' * 64}")[1]["error"]["code"] == "model_not_found"
    assert service.call("DELETE", f"{API}/models")[2]["Allow"] == "GET, POST"
    assert service.call("GET", f"{API}/models/{'a' * 64}/lifecycle")[2]["Allow"] == "POST"


def test_lifecycle_archives_and_filters_without_touching_the_version(service: Running) -> None:
    version_id = registered(service)["result"]["model_version_id"]
    before = service.get(f"{API}/models/{version_id}")["model"]["payload"]
    status, archived, _ = service.call(
        "POST", f"{API}/models/{version_id}/lifecycle", {"reason": "superseded", "state": "archived"}
    )
    assert status == 200
    assert archived["lifecycle"]["state"] == "archived"
    assert service.get(f"{API}/models")["models"] == []
    assert [entry["model_version_id"] for entry in service.get(f"{API}/models?state=archived")["models"]] == [
        version_id
    ]
    archived_detail = service.get(f"{API}/models/{version_id}")["model"]
    assert archived_detail["payload"] == before
    # Archived: no new comparison or deployment may choose it.
    assert archived_detail["compatibility"] == {
        "deployment": {
            "detail": "an archived version cannot be chosen for a new deployment",
            "reason": "archived",
            "status": "unavailable",
        },
        "evaluation": {
            "detail": "an archived version cannot be chosen for a new comparison",
            "reason": "archived",
            "status": "unavailable",
        },
    }
    for body, reason in [
        ({"reason": "x", "state": "champion"}, "unknown_enum_value"),
        ({"state": "archived"}, "missing_field"),
    ]:
        code, document, _ = service.call("POST", f"{API}/models/{version_id}/lifecycle", body)
        assert code == 400
        assert document["error"]["reason"] == reason
    code, document, _ = service.call(
        "POST", f"{API}/models/{version_id}/lifecycle", {"reason": "line\nbreak", "state": "active"}
    )
    assert code == 422


def finish(running: Iterator[Running]) -> None:
    """Resumes a serving() generator after its yield, so it shuts down."""
    for _ in running:
        pass


def test_models_survive_a_restart_and_a_corrupt_version_is_reported(
    demo_artifacts: tuple[Tools, Path], demo_data: Path, tmp_path: Path
) -> None:
    configuration = ServiceConfiguration(
        tools=demo_artifacts[0], artifacts=demo_data, state=tmp_path / "state", port=0
    )
    first = serving(configuration)
    running = next(first)
    try:
        version_id = registered(running)["result"]["model_version_id"]
        majority_id = registered(running, model="majority", name="aapl-majority")["result"][
            "model_version_id"
        ]
    finally:
        finish(first)
    second = serving(configuration)
    running = next(second)
    try:
        listed = {entry["model_version_id"] for entry in running.get(f"{API}/models")["models"]}
        assert listed == {version_id, majority_id}
        assert running.get(f"{API}/models/{version_id}")["model"]["source_check"]["state"] == "matches"
        # Corrupt one version's payload: it is reported, never served.
        directory = demo_data / "models" / majority_id
        os.chmod(directory, stat.S_IRWXU)
        payload = directory / "parameters.json"
        os.chmod(payload, stat.S_IRUSR | stat.S_IWUSR)
        payload.write_bytes(payload.read_bytes().replace(b'"', b"'", 1))
        listing = running.get(f"{API}/models")
        assert [entry["model_version_id"] for entry in listing["models"]] == [version_id]
        assert [entry["model_version_id"] for entry in listing["invalid_entries"]] == [majority_id]
        code, document, _ = running.call("GET", f"{API}/models/{majority_id}")
        assert code == 422
        assert document["error"]["code"] == "artifact_invalid"
    finally:
        finish(second)


def test_a_version_without_an_information_cutoff_cannot_be_evaluated_or_deployed(
    service: Running, monkeypatch: pytest.MonkeyPatch
) -> None:
    from chronos_research import model_import
    from chronos_research.onnx_sandbox import Sandbox
    from chronos_research.registry import RegistryRoots
    from tests.test_model_import import MODEL, FakePredictor, manifest

    monkeypatch.setattr(model_import, "OnnxPredictor", FakePredictor)
    undeclared = model_import.import_model(
        RegistryRoots.under(service.configuration.artifacts),
        manifest(declared__information_cutoff_ns=None, declared__cutoff_covers=None),
        MODEL,
        Sandbox(),
    ).model_version_id
    compatibility = service.get(f"{API}/models/{undeclared}")["model"]["compatibility"]
    # Before the sandbox's own availability: no window could follow an unknown cutoff.
    assert compatibility["evaluation"] == {
        "detail": "without an information cutoff no comparison window can be shown to follow it",
        "reason": "cutoff_unavailable",
        "status": "unavailable",
    }
    assert compatibility["deployment"]["reason"] == "cutoff_unavailable"
