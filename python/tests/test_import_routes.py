"""ONNX imports over HTTP: a staged manifest, frozen model bytes, a
validation job in the real sandbox, refusals that publish nothing, and
persistence across a restart."""

import hashlib
import http.client
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import pytest

from chronos_research.onnx_profile import MAX_MODEL_BYTES
from chronos_research.onnx_sandbox import Sandbox
from chronos_research.pipeline import Tools
from chronos_research.service import ServiceConfiguration
from tests.test_model_import import manifest
from tests.test_model_routes import finish
from tests.test_service import API, Running, serving

MODEL_BYTES = b"\x08\x08 staged bytes; nothing parses them before validation"


def configuration(tmp_path: Path, sandbox: Sandbox | None = None) -> ServiceConfiguration:
    return ServiceConfiguration(
        tools=Tools(tmp_path / "no-tools"),
        artifacts=tmp_path / "data",
        state=tmp_path / "state",
        port=0,
        sandbox=sandbox or Sandbox(docker="/nonexistent/docker"),
    )


@pytest.fixture
def unsandboxed(tmp_path: Path) -> Iterator[Running]:
    yield from serving(configuration(tmp_path))


def staged(service: Running, **changes: Any) -> dict[str, Any]:
    status, document, _ = service.call("POST", f"{API}/imports", manifest(**changes))
    assert status == 201, document
    assert document["job"] is None
    return dict(document["import"])


def upload(service: Running, import_id: str, data: bytes) -> tuple[int, dict[str, Any]]:
    status, document, _ = service.call(
        "PUT", f"{API}/imports/{import_id}/model", raw=data, content_type="application/octet-stream"
    )
    return status, document


def validate(service: Running, import_id: str, digest: str) -> tuple[int, dict[str, Any]]:
    status, document, _ = service.call(
        "POST", f"{API}/imports/{import_id}/validation", {"model_sha256": digest}
    )
    return status, document


def announced_length(service: Running, path: str, length: int) -> int:
    """The status for a body announced as `length` bytes but never sent."""
    origin = urlsplit(service.origin)
    connection = http.client.HTTPConnection(origin.hostname or "127.0.0.1", origin.port, timeout=10)
    try:
        connection.putrequest("PUT", path)
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(length))
        connection.endheaders()
        return connection.getresponse().status
    finally:
        connection.close()


def wait_job(service: Running, job_id: str) -> dict[str, Any]:
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        job: dict[str, Any] = service.get(f"{API}/jobs/{job_id}")["job"]
        if job["state"] in ("succeeded", "failed", "interrupted"):
            return job
        time.sleep(0.1)
    raise AssertionError(f"job {job_id} did not finish")


def test_a_manifest_is_checked_when_it_is_staged(unsandboxed: Running) -> None:
    for changes, code in (
        ({"inputs": {"name": "top_of_book_baseline", "version": "2"}}, "incompatible_contract"),
        ({"declared__cutoff_covers": "training only"}, "invalid_manifest"),
    ):
        status, document, _ = unsandboxed.call("POST", f"{API}/imports", manifest(**changes))
        assert (status, document["error"]["code"]) == (422, code)
    status, document, _ = unsandboxed.call(
        "POST", f"{API}/imports", raw=b'{"schema": 1}', content_type="application/json"
    )
    assert (status, document["error"]["reason"]) == (400, "invalid_type")
    created = staged(unsandboxed)
    assert created["model"] is None
    assert created["manifest"] == manifest()
    assert unsandboxed.get(f"{API}/imports/{created['import_id']}")["import"] == created
    assert unsandboxed.call("GET", f"{API}/imports/{'0' * 32}")[0] == 404
    assert unsandboxed.call("GET", f"{API}/imports/not-an-id")[0] == 400


def test_the_model_upload_is_bounded_and_frozen(unsandboxed: Running) -> None:
    import_id = staged(unsandboxed)["import_id"]
    path = f"{API}/imports/{import_id}/model"
    status, document, _ = unsandboxed.call("PUT", path, raw=MODEL_BYTES, content_type="application/json")
    assert (status, document["error"]["code"]) == (415, "unsupported_media_type")
    assert announced_length(unsandboxed, path, MAX_MODEL_BYTES + 1) == 413
    status, document = upload(unsandboxed, import_id, b"")
    assert (status, document["error"]["code"]) == (400, "payload_too_large")
    status, document, _ = unsandboxed.call(
        "PUT", path + "?replace=1", raw=MODEL_BYTES, content_type="application/octet-stream"
    )
    assert status == 400
    status, document = upload(unsandboxed, import_id, MODEL_BYTES)
    digest = hashlib.sha256(MODEL_BYTES).hexdigest()
    assert status == 201
    assert document["import"]["model"]["sha256"] == digest
    assert document["import"]["model"]["size_bytes"] == str(len(MODEL_BYTES))
    assert upload(unsandboxed, import_id, MODEL_BYTES)[0] == 200
    status, document = upload(unsandboxed, import_id, MODEL_BYTES + b"!")
    assert (status, document["error"]["code"]) == (409, "model_already_uploaded")
    assert unsandboxed.get(f"{API}/imports/{import_id}")["import"]["model"]["sha256"] == digest


def test_validation_needs_the_reviewed_bytes_and_the_sandbox(unsandboxed: Running) -> None:
    import_id = staged(unsandboxed)["import_id"]
    digest = hashlib.sha256(MODEL_BYTES).hexdigest()
    status, document = validate(unsandboxed, import_id, digest)
    assert (status, document["error"]["code"]) == (409, "model_not_uploaded")
    upload(unsandboxed, import_id, MODEL_BYTES)
    status, document = validate(unsandboxed, import_id, "f" * 64)
    assert (status, document["error"]["code"]) == (409, "model_mismatch")
    status, document = validate(unsandboxed, import_id, digest)
    # Fail closed: without the sandbox nothing is validated any other way.
    assert (status, document["error"]["code"], document["error"]["reason"]) == (
        503,
        "sandbox_unavailable",
        "docker_not_found",
    )
    assert unsandboxed.get(f"{API}/jobs?kind=import")["jobs"] == []
    imports = unsandboxed.get(f"{API}/status")["imports"]
    assert imports["sandbox"]["status"] == "unavailable"
    assert imports["profile"]["max_model_bytes"] == str(MAX_MODEL_BYTES)


def test_a_staged_import_survives_a_restart(tmp_path: Path) -> None:
    first = serving(configuration(tmp_path))
    running = next(first)
    try:
        import_id = staged(running)["import_id"]
        upload(running, import_id, MODEL_BYTES)
    finally:
        finish(first)
    second = serving(configuration(tmp_path))
    running = next(second)
    try:
        model = running.get(f"{API}/imports/{import_id}")["import"]["model"]
        assert model["sha256"] == hashlib.sha256(MODEL_BYTES).hexdigest()
    finally:
        finish(second)


def test_an_import_job_publishes_exactly_the_reviewed_bytes(tmp_path: Path, sandbox: Sandbox) -> None:
    from tests import onnx_models

    data = onnx_models.valid()
    digest = hashlib.sha256(data).hexdigest()
    first = serving(configuration(tmp_path, sandbox))
    running = next(first)
    try:
        import_id = staged(running)["import_id"]
        assert upload(running, import_id, data)[0] == 201
        status, started = validate(running, import_id, digest)
        assert status == 202, started
        job = wait_job(running, started["job"]["job_id"])
        assert job["state"] == "succeeded", job
        assert job["kind"] == "import"
        version_id = job["result"]["model_version_id"]
        assert running.get(f"{API}/imports/{import_id}")["job"]["job_id"] == job["job_id"]
        assert validate(running, import_id, digest)[1]["deduplicated"] is True
    finally:
        finish(first)
    second = serving(configuration(tmp_path, sandbox))
    running = next(second)
    try:
        (listed,) = running.get(f"{API}/models")["models"]
        assert (listed["model_version_id"], listed["family"], listed["origin"]) == (
            version_id,
            "onnx",
            "import",
        )
        detail = running.get(f"{API}/models/{version_id}")["model"]
        assert detail["payload"]["sha256"] == digest
        assert detail["training"]["information_cutoff"]["basis"] == "declared"
        assert detail["source_check"]["state"] == "not_applicable"
        assert detail["compatibility"]["evaluation"]["status"] == "compatible"
        assert (tmp_path / "data" / "models" / version_id / "model.onnx").read_bytes() == data
    finally:
        finish(second)


@pytest.mark.parametrize(
    ("build", "reason"),
    [("unsupported_operator", "unsupported_operator"), ("non_finite", "non_finite_output")],
)
def test_a_refused_import_fails_its_job_and_publishes_nothing(
    tmp_path: Path, sandbox: Sandbox, build: str, reason: str
) -> None:
    from tests import onnx_models

    data = {
        "unsupported_operator": onnx_models.with_operator("Relu"),
        "non_finite": onnx_models.non_finite_output(),
    }
    model = data[build]
    running_service = serving(configuration(tmp_path, sandbox))
    running = next(running_service)
    try:
        import_id = staged(running)["import_id"]
        upload(running, import_id, model)
        status, started = validate(running, import_id, hashlib.sha256(model).hexdigest())
        assert status == 202
        job = wait_job(running, started["job"]["job_id"])
        assert job["state"] == "failed"
        assert (job["error"]["code"], job["error"]["reason"]) == ("import_refused", reason)
        assert running.get(f"{API}/models?state=all")["models"] == []
        assert not (tmp_path / "data" / "models").exists() or not any(
            (tmp_path / "data" / "models").iterdir()
        )
    finally:
        finish(running_service)
