"""Walk-forward evaluation through the research service: collections and
plans read verified, Preview and Start as deduplicated child jobs off the
request thread, bounded progress, sources checked again at execution, and
studies whose fold runs open like any result."""

import json
import shutil
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from chronos_research import pipeline as research_pipeline
from chronos_research import walk_forward
from chronos_research.collection import create_collection
from chronos_research.pipeline import Tools
from chronos_research.service import (
    ApiError,
    JobStore,
    ServiceConfiguration,
    Workers,
    progress_report,
    validate_walk_forward_request,
)
from tests import pipeline as built
from tests.recordings import WEEK, Recordings, prepare
from tests.test_service import API, Running, serving
from tests.test_walk_forward_plan import CONFIGURATION, SCENARIOS

TERMINAL = ("succeeded", "failed", "interrupted")


@pytest.fixture(scope="module")
def week(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Recordings:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("routes"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in WEEK:
        prepare(recordings, recording)
    return recordings


@pytest.fixture
def copy(week: Recordings, tmp_path: Path) -> Recordings:
    root = tmp_path / "artifacts"
    shutil.copytree(week.artifacts, root, symlinks=True)
    return Recordings(artifacts=root, tools=week.tools, prepared=dict(week.prepared))


@pytest.fixture
def service(copy: Recordings, tmp_path: Path) -> Iterator[Running]:
    yield from serving(
        ServiceConfiguration(tools=copy.tools, artifacts=copy.artifacts, state=tmp_path / "state", port=0)
    )


def collection_id(recordings: Recordings, *streams: str) -> str:
    created = create_collection(recordings.tools, recordings.artifacts, recordings.label_ids(*streams))
    return str(created["collection"]["collection_id"])


def week_collection(recordings: Recordings) -> str:
    return collection_id(recordings, *(recording.stream for recording in WEEK))


def body(collection: str, **changes: Any) -> dict[str, Any]:
    document: dict[str, Any] = {
        "collection_id": collection,
        "configuration": {**CONFIGURATION, "threshold_ppb": "400000000"},
        "instrument": "AAPL",
        "minimum_training_sources": "1",
        "scenarios": [dict(cell) for cell in SCENARIOS],
    }
    document.update(changes)
    return document


def wait(service: Running, job_id: str, timeout_s: float = 240) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        job: dict[str, Any] = service.get(f"{API}/jobs/{job_id}")["job"]
        if job["state"] in TERMINAL:
            return job
        time.sleep(0.1)
    raise AssertionError(f"job {job_id} did not finish")


def preview(service: Running, document: dict[str, Any]) -> dict[str, Any]:
    status, started, _ = service.call("POST", f"{API}/walk-forward/plans", document)
    assert status == 202, started
    assert started["job"]["kind"] == "walk_forward_plan"
    return wait(service, started["job"]["job_id"])


def test_collections_are_listed_verified_with_their_coverage(copy: Recordings, service: Running) -> None:
    identifier = week_collection(copy)
    listed = service.get(f"{API}/collections")
    assert listed["invalid_entries"] == []
    (entry,) = listed["collections"]
    assert entry["collection_id"] == identifier
    assert entry["evidence"] == "synthetic"
    assert entry["instruments"] == {"AAPL": "3", "SPY": "3"}
    assert [member["stream"] for member in entry["members"]] == [recording.stream for recording in WEEK]
    first = entry["members"][0]
    assert first["coverage"]["completeness"] == "partial"
    assert first["receive_time"]["earliest"].startswith("2026-09-21T13:30:00.")
    assert first["instruments"]["AAPL"]["first_available_at"].startswith("2026-09-21T")
    assert first["session"] == {"unavailable": "not_declared"}
    assert service.get(f"{API}/collections/{identifier}")["collection"] == entry
    assert service.call("GET", f"{API}/collections/nope")[0] == 400
    assert service.call("GET", f"{API}/collections/{'f' * 64}")[0] == 404


def test_preview_publishes_the_plan_of_exactly_the_reviewed_request(
    copy: Recordings, service: Running
) -> None:
    collection = week_collection(copy)
    job = preview(service, body(collection))
    assert job["state"] == "succeeded", job
    assert job["result"]["folds"] == "2"
    plan_id = job["result"]["plan_id"]
    plan = service.get(f"{API}/walk-forward/plans/{plan_id}")["plan"]
    assert plan["plan_id"] == plan_id
    assert plan["collection"]["evidence"] == "synthetic"
    assert [fold["evaluation"]["stream"] for fold in plan["folds"]] == [WEEK[1].stream, WEEK[2].stream]
    fold = plan["folds"][0]
    assert fold["information_boundary"] == fold["evaluation"]["window"]["start_at"]
    assert fold["training"]["max_label_available_at"] < fold["information_boundary"]
    assert plan["request"]["configuration"]["threshold_ppb"] == "400000000"
    # The same request, written differently, is the same job.
    again = body(collection)
    again["configuration"]["fee_per_fill"] = "0.0100"
    again["scenarios"][0]["fee_per_fill"] = "0.010"
    status, deduplicated, _ = service.call("POST", f"{API}/walk-forward/plans", again)
    assert (status, deduplicated["deduplicated"], deduplicated["job"]["job_id"]) == (200, True, job["job_id"])
    listed = service.get(f"{API}/walk-forward/plans")
    assert [entry["plan_id"] for entry in listed["plans"]] == [plan_id]
    assert service.get(f"{API}/jobs?kind=walk_forward_plan")["jobs"][0]["job_id"] == job["job_id"]


def test_one_recording_cannot_be_planned_and_says_what_is_needed(copy: Recordings, service: Running) -> None:
    job = preview(service, body(collection_id(copy, WEEK[0].stream)))
    assert job["state"] == "failed"
    assert job["error"]["code"] == "plan_refused"
    assert job["error"]["reason"] == "insufficient_coverage"
    assert "needs at least 2 independent recordings" in job["error"]["message"]


@pytest.mark.parametrize(
    ("changes", "status", "reason"),
    [
        ({"extra": "1"}, 400, "unexpected_field"),
        ({"collection_id": "nope"}, 400, "invalid_identifier"),
        ({"minimum_training_sources": "0"}, 422, "out_of_domain"),
        ({"minimum_training_sources": 1}, 400, "invalid_type"),
        ({"scenarios": []}, 422, "out_of_domain"),
        ({"scenarios": [{"fee_per_fill": "0.01", "latency_ns": "1000"}]}, 400, "missing_field"),
        ({"scenarios": [{"fee_per_fill": "0.01", "latency_ns": "0", "name": "base"}]}, 422, "out_of_domain"),
        ({"scenarios": [{"fee_per_fill": "-1", "latency_ns": "1000", "name": "base"}]}, 422, "out_of_domain"),
        (
            {
                "scenarios": [
                    {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
                    {"fee_per_fill": "0.005", "latency_ns": "5000", "name": "stress"},
                ]
            },
            422,
            "invalid_request",
        ),
    ],
)
def test_preview_requests_are_strict(
    copy: Recordings, service: Running, changes: dict[str, Any], status: int, reason: str
) -> None:
    returned, document, _ = service.call("POST", f"{API}/walk-forward/plans", body("a" * 64, **changes))
    assert (returned, document["error"]["reason"]) == (status, reason), document


def test_an_unknown_collection_or_plan_is_refused_before_any_job(service: Running) -> None:
    status, document, _ = service.call("POST", f"{API}/walk-forward/plans", body("a" * 64))
    assert (status, document["error"]["code"]) == (422, "collection_not_found")
    status, document, _ = service.call("POST", f"{API}/walk-forward/studies", {"plan_id": "b" * 64})
    assert (status, document["error"]["code"]) == (422, "plan_not_found")
    status, document, _ = service.call("POST", f"{API}/walk-forward/studies", {"plan_id": "b" * 64, "x": "1"})
    assert (status, document["error"]["reason"]) == (400, "unexpected_field")
    assert service.get(f"{API}/jobs")["jobs"] == []


def test_a_study_runs_as_a_job_and_its_folds_runs_open_as_results(copy: Recordings, service: Running) -> None:
    plan_id = preview(service, body(week_collection(copy)))["result"]["plan_id"]
    status, started, _ = service.call("POST", f"{API}/walk-forward/studies", {"plan_id": plan_id})
    assert status == 202, started
    assert started["job"]["kind"] == "walk_forward"
    assert started["job"]["progress"] is None
    job = wait(service, started["job"]["job_id"])
    assert job["state"] == "succeeded", job
    assert job["progress"] == {"stage": "publish"}
    assert "progress" not in job["log"]
    assert job["result"]["status"] == "complete"
    assert job["result"]["missing"] == "0"
    study_id = job["result"]["study_id"]
    status, again, _ = service.call("POST", f"{API}/walk-forward/studies", {"plan_id": plan_id})
    assert (status, again["job"]["job_id"]) == (200, job["job_id"])
    detail = service.get(f"{API}/walk-forward/studies/{study_id}")
    study = detail["study"]
    assert study["study_id"] == study_id
    assert study["collection"]["evidence"] == "synthetic"
    assert study["folds"][0]["information_boundary"].startswith("2026-09-22T")
    for fold in detail["artifacts"]:
        assert fold["fold_model"] == "verified"
        for model in fold["models"].values():
            assert model["schedule"] == "verified"
            assert set(model["runs"].values()) == {"verified"}
    listed = service.get(f"{API}/walk-forward/studies")["studies"]
    assert [entry["study_id"] for entry in listed] == [study_id]
    cell = study["folds"][1]["models"]["logistic"]["cells"][0]
    run_id = cell["run"]["artifact_id"]
    result = service.get(f"{API}/results/{run_id}")["result"]
    assert result["source"]["fold"] == "2"
    assert result["source"]["plan_id"] == plan_id
    assert result["window"]["name"] == "fold_2"
    trades = service.get(f"{API}/results/{run_id}/trades?account=strategy")["trades"]
    assert len(trades) == int(cell["accounts"]["strategy"]["fills"])
    assert service.get(f"{API}/results/{run_id}/decisions")["decisions"]
    # Fold runs belong to their study, not the evaluation results list.
    assert run_id not in {entry["run_id"] for entry in service.get(f"{API}/results")["results"]}


def test_a_source_changed_after_review_fails_the_study_before_it_runs(
    copy: Recordings, service: Running
) -> None:
    plan_id = preview(service, body(week_collection(copy)))["result"]["plan_id"]
    labels = copy.artifacts / "labels" / copy.prepared[WEEK[2].stream].label_id
    labels.chmod(0o755)
    rows = labels / "labels.ndjson"
    rows.chmod(0o644)
    rows.write_bytes(rows.read_bytes().replace(b'"up"', b'"down"', 1))
    _, started, _ = service.call("POST", f"{API}/walk-forward/studies", {"plan_id": plan_id})
    job = wait(service, started["job"]["job_id"])
    assert job["state"] == "failed"
    assert (job["error"]["code"], job["error"]["reason"]) == ("study_refused", "source_invalid")
    assert job["progress"] == {"stage": "verify_sources"}
    assert service.get(f"{API}/walk-forward/studies")["studies"] == []


@pytest.mark.parametrize(
    "line",
    [
        b'{"progress": {"stage": "fold", "fold": "1", "folds": "2", "step": "run", "model": "logistic"}}',
        b'{"progress": {"stage": "publish"}}',
    ],
)
def test_progress_reports_are_small_known_tokens(line: bytes) -> None:
    report = progress_report(line)
    assert report is not None
    assert list(report) == sorted(report)


@pytest.mark.parametrize(
    "line",
    [
        b"not json",
        b'{"progress": {}}',
        b'{"progress": {"stage": "fold"}, "other": 1}',
        b'{"progress": {"path": "/etc/passwd"}}',
        b'{"progress": {"stage": "Fold 1"}}',
        b'{"progress": {"stage": "' + b"x" * 33 + b'"}}',
        b'{"progress": {"stage": 1}}',
        b'{"error": {"kind": "StudyError", "message": "refused"}}',
    ],
)
def test_anything_else_on_stderr_is_only_log(line: bytes) -> None:
    assert progress_report(line) is None


def test_a_childs_progress_is_recorded_apart_from_its_log_and_error(tmp_path: Path) -> None:
    child = tmp_path / "study-child"
    child.write_text(
        "#!/bin/sh\n"
        'echo \'{"progress": {"stage": "verify_sources"}}\' >&2\n'
        "echo 'a warning' >&2\n"
        'echo \'{"progress": {"fold": "1", "folds": "3", "stage": "fold", "step": "fit"}}\' >&2\n'
        'printf \'{"progress": {"stage": "fold"\' >&2\n'
        "echo '' >&2\n"
        'echo \'{"error": {"kind": "StudyError", "message": "fold 2 failed", "reason": "x"}}\' >&2\n'
        "exit 1\n"
    )
    child.chmod(0o755)
    store = JobStore(tmp_path / "jobs", retain=10)
    workers = Workers(
        ServiceConfiguration(
            tools=Tools(tmp_path),
            artifacts=tmp_path / "data",
            state=tmp_path,
            python=str(child),
            job_timeout_s=30,
        ),
        store,
    )
    job, _ = store.create({"plan_id": "a" * 64}, "study", "walk_forward")
    try:
        workers.submit(job)
        deadline = time.monotonic() + 10
        while job.state not in TERMINAL and time.monotonic() < deadline:
            time.sleep(0.01)
    finally:
        workers.stop()
    assert job.state == "failed"
    assert job.progress == {"fold": "1", "folds": "3", "stage": "fold", "step": "fit"}
    assert "a warning" in job.log
    assert '"progress": {"stage": "verify' not in job.log
    # An incomplete report is log, not progress.
    assert '{"progress": {"stage": "fold"' in job.log
    assert job.error is not None
    assert job.error["code"] == "study_failed"
    # A study that stops before it finishes keeps its last progress when interrupted.
    running, _ = store.create({"plan_id": "b" * 64}, "other", "walk_forward")
    running.state, running.progress = "running", {"fold": "2", "folds": "3", "stage": "fold", "step": "run"}
    store.save(running)
    recovered = JobStore(tmp_path / "jobs", retain=10).get(running.job_id)
    assert recovered is not None
    assert recovered.state == "interrupted"
    assert recovered.progress == {"fold": "2", "folds": "3", "stage": "fold", "step": "run"}
    # Only walk-forward jobs show progress.
    evaluation, _ = store.create({"n": "1"}, "evaluation")
    assert "progress" not in evaluation.public()
    assert json.loads(json.dumps(running.public()))["progress"]["fold"] == "2"


def test_collections_plans_and_studies_page_with_keyset_cursors(copy: Recordings, service: Running) -> None:
    first = collection_id(copy, WEEK[0].stream, WEEK[1].stream)
    second = week_collection(copy)
    page = service.get(f"{API}/collections?limit=1")
    assert len(page["collections"]) == 1
    assert page["next_cursor"] is not None
    rest = service.get(f"{API}/collections?limit=1&cursor={page['next_cursor']}")
    assert rest["next_cursor"] is None
    listed = {entry["collection_id"] for entry in page["collections"] + rest["collections"]}
    assert listed == {first, second}
    status, refused, _ = service.call("GET", f"{API}/walk-forward/studies?cursor={page['next_cursor']}")
    assert (status, refused["error"]["reason"]) == (400, "invalid_cursor")
    for path in ("walk-forward/plans", "walk-forward/studies"):
        assert service.get(f"{API}/{path}?limit=1")["next_cursor"] is None


@pytest.mark.parametrize(
    ("field", "value", "status"),
    [
        ("threshold_ppb", "1000000001", 422),
        ("target_quantity", "-1", 422),
        ("target_quantity", "0.000000001", 400),
        ("decision_interval_ns", "0", 422),
        ("starting_cash", "99999999999", 400),
        ("maximum_quote_age_ns", "0", 422),
        ("maximum_order_notional", "0", 422),
    ],
)
def test_the_cli_and_http_refuse_the_same_settings(field: str, value: str, status: int) -> None:
    document = body("a" * 64)
    document["configuration"][field] = value
    with pytest.raises(ApiError) as http:
        validate_walk_forward_request(document)
    assert (http.value.status, http.value.field) == (status, f"configuration.{field}")
    with pytest.raises(walk_forward.PlanError) as cli:
        walk_forward.validated_request({**document, "schema": walk_forward.REQUEST_SCHEMA})
    assert cli.value.reason == "invalid_request"
    assert field in str(cli.value)
