"""The inference worker's protocol 2: a registered model version served by
its ID, predicting exactly as its schedules do, refusing what it cannot
serve, and never running an import outside the ONNX sandbox."""

import io
import json
import shutil
from pathlib import Path
from typing import Any

import pytest

from chronos_research import inference_worker, model_import
from chronos_research.inference_worker import REGISTERED_PROTOCOL, run_registered_worker, run_worker
from chronos_research.onnx_sandbox import Sandbox, SandboxFailedError
from chronos_research.registered_inference import (
    InvalidPredictionError,
    RowPrediction,
    judged,
    open_predictor,
)
from chronos_research.registry import (
    RegistryRoots,
    register_experiment_model,
    set_lifecycle,
    verified_model_version,
)
from chronos_research.schedule import ROWS, build_schedule
from tests import pipeline as built
from tests.synthetic import synthetic_events
from tests.test_inference_worker import request_for
from tests.test_model_import import MODEL, FakePredictor, manifest
from tests.test_schedule import Research, research

NO_DOCKER = Sandbox(docker="/nonexistent/docker")


@pytest.fixture(scope="module")
def market(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Research:
    return research(tools, tmp_path_factory.mktemp("registered-worker"), synthetic_events(seed=7))


@pytest.fixture
def data(market: Research, tmp_path: Path) -> Path:
    copy = tmp_path / "data"
    shutil.copytree(market.roots.experiments.parent, copy, symlinks=True)
    return copy


def register(market: Research, data: Path, family: str = "logistic") -> str:
    return register_experiment_model(
        RegistryRoots.under(data), data / "experiments", market.sources, market.experiment_id, "AAPL", family,
        f"aapl-{family}", "1",
    ).model_version_id  # fmt: skip


def converse(
    data: Path, version_id: str, requests: list[Any], sandbox: Sandbox = NO_DOCKER, describe: bool = False
) -> tuple[int, list[dict[str, Any]]]:
    lines = b"".join(json.dumps(request).encode() + b"\n" for request in requests)
    output = io.StringIO()
    code = run_registered_worker(data, version_id, "AAPL", sandbox, describe, io.BytesIO(lines), output)
    return code, [json.loads(line) for line in output.getvalue().splitlines()]


def schedule_requests(market: Research) -> list[dict[str, Any]]:
    schedule = build_schedule(market.sources, market.experiment, "AAPL", "logistic", "test")
    rows = [json.loads(line) for line in schedule.files[ROWS].splitlines()]
    return [
        request_for(row, str(index + 1))
        for index, row in enumerate(row for row in rows if "raw" in row["inputs"])
    ]


@pytest.mark.parametrize("family", ["logistic", "majority"])
def test_a_registered_version_predicts_exactly_as_its_experiment_model(
    market: Research, data: Path, family: str
) -> None:
    version_id = register(market, data, family)
    requests = schedule_requests(market)[:50]
    code, replies = converse(data, version_id, requests)
    ready, predictions = replies[0], replies[1:]
    assert code == 0
    assert ready == {
        "cutoff_basis": "verified",
        "family": family,
        "instrument": "AAPL",
        "model_feature_set": {"name": "top_of_book_baseline", "version": "1"},
        "model_version_id": version_id,
        "payload_sha256": json.loads((data / "models" / version_id / "manifest.json").read_bytes())[
            "payload"
        ]["sha256"],
        "protocol": REGISTERED_PROTOCOL,
        "training_cutoff_ns": str(market.sources.validation_start_at_ns),
        "type": "ready",
    }
    output = io.StringIO()
    lines = b"".join(json.dumps(request).encode() + b"\n" for request in requests)
    run_worker(data / "experiments", market.experiment_id, "AAPL", family, False, io.BytesIO(lines), output)
    assert predictions == [json.loads(line) for line in output.getvalue().splitlines()][1:]


def test_describing_needs_an_active_version_of_this_instrument_with_a_known_cutoff(
    market: Research, data: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    version_id = register(market, data)
    roots = RegistryRoots.under(data)
    set_lifecycle(roots, version_id, "archived", "superseded")
    code, replies = converse(data, version_id, [], describe=True)
    assert code == 1
    assert "archived" in replies[0]["message"]
    # Archiving keeps it from new deployments; one that serves it still starts.
    assert converse(data, version_id, [])[0] == 0
    output = io.StringIO()
    assert run_registered_worker(data, version_id, "SPY", NO_DOCKER, True, io.BytesIO(), output) == 1
    assert "predicts AAPL, not SPY" in output.getvalue()
    assert converse(data, "0" * 64, [], describe=True)[0] == 1
    monkeypatch.setattr(model_import, "OnnxPredictor", FakePredictor)
    undeclared = model_import.import_model(
        roots, manifest(declared__information_cutoff_ns=None, declared__cutoff_covers=None), MODEL, Sandbox()
    ).model_version_id
    code, replies = converse(data, undeclared, [], describe=True)
    assert code == 1
    assert "no information cutoff" in replies[0]["message"]
    # An import with a cutoff, but no sandbox here: it is never run any other way.
    declared = model_import.import_model(roots, manifest(name="declared"), MODEL, Sandbox()).model_version_id
    code, replies = converse(data, declared, [], describe=True)
    assert code == 1
    assert "SandboxUnavailableError" in replies[0]["message"]


@pytest.mark.parametrize("tampered", ["parameters.json", "manifest.json"])
def test_a_version_tampered_with_after_its_deployment_was_created_does_not_start(
    market: Research, data: Path, tampered: str
) -> None:
    version_id = register(market, data)
    assert converse(data, version_id, [], describe=True)[0] == 0
    # Between creation and Start, one byte of the version changes.
    path = data / "models" / version_id / tampered
    path.chmod(0o644)
    content = bytearray(path.read_bytes())
    content[len(content) // 2] ^= 0x01
    path.write_bytes(bytes(content))
    code, replies = converse(data, version_id, [])
    assert code == 1
    assert replies[0]["type"] == "error"
    assert "RegistryError" in replies[0]["message"]


class ScriptedPredictor:
    """A predictor whose first row has no valid output and whose second row
    finds the sandbox gone."""

    def __init__(self) -> None:
        self.calls = 0

    def predict(self, inputs: tuple[int, ...]) -> RowPrediction:
        self.calls += 1
        if self.calls == 1:
            raise InvalidPredictionError("non_finite_output", "a probability is not finite")
        if self.calls == 2:
            return judged((0.25, 0.5, 0.25))
        raise SandboxFailedError("timeout", "the sandbox did not reply in time")

    def runtime_environment(self) -> dict[str, str]:
        return {}

    def close(self) -> None:
        return None


def test_an_invalid_output_is_refused_and_a_failed_sandbox_ends_the_worker(
    market: Research, data: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    version_id = register(market, data)
    monkeypatch.setattr(inference_worker, "open_predictor", lambda version, sandbox: ScriptedPredictor())
    requests = schedule_requests(market)[:4]
    code, replies = converse(data, version_id, requests)
    assert code == 1
    assert replies[1] == {
        "id": "1",
        "message": "the model gave no valid prediction (non_finite_output): a probability is not finite",
        "type": "error",
    }
    assert replies[2]["type"] == "prediction"
    assert replies[3]["id"] is None
    assert "the ONNX sandbox failed (timeout)" in replies[3]["message"]
    assert len(replies) == 4


def test_an_import_is_served_through_the_sandbox(market: Research, data: Path, sandbox: Sandbox) -> None:
    from tests import onnx_models

    roots = RegistryRoots.under(data)
    version_id = model_import.import_model(
        roots, manifest(name="sandboxed"), onnx_models.valid(), sandbox
    ).model_version_id
    code, replies = converse(data, version_id, schedule_requests(market)[:5], sandbox)
    assert code == 0
    assert replies[0]["family"] == "onnx"
    assert replies[0]["cutoff_basis"] == "declared"
    assert [reply["type"] for reply in replies[1:]] == ["prediction"] * 5
    assert all(
        sum(float(p) for p in reply["probabilities"]) == pytest.approx(1.0, abs=1e-12)
        for reply in replies[1:]
    )


def row_inputs(market: Research) -> list[tuple[int, ...]]:
    schedule = build_schedule(market.sources, market.experiment, "AAPL", "logistic", "test")
    rows = [json.loads(line) for line in schedule.files[ROWS].splitlines()]
    return [tuple(int(value) for value in row["inputs"]["raw"]) for row in rows if "raw" in row["inputs"]][
        :30
    ]


def predicted(
    data: Path, version_id: str, rows: list[tuple[int, ...]], sandbox: Sandbox
) -> list[RowPrediction]:
    predictor = open_predictor(verified_model_version(RegistryRoots.under(data), version_id), sandbox)
    try:
        return [predictor.predict(row) for row in rows]
    finally:
        predictor.close()


def require_row_pure(data: Path, version_id: str, rows: list[tuple[int, ...]], sandbox: Sandbox) -> None:
    """One row at a time: a row's prediction is the same in any order, and
    alone in a fresh session, as among the others."""
    forward = predicted(data, version_id, rows, sandbox)
    backward = predicted(data, version_id, rows[::-1], sandbox)
    assert backward[::-1] == forward
    assert [predicted(data, version_id, [row], sandbox)[0] for row in rows[:3]] == forward[:3]


@pytest.mark.parametrize("family", ["logistic", "majority"])
def test_a_baseline_versions_predictions_do_not_depend_on_other_rows(
    market: Research, data: Path, family: str
) -> None:
    require_row_pure(data, register(market, data, family), row_inputs(market), NO_DOCKER)


def test_an_imports_predictions_do_not_depend_on_other_rows(
    market: Research, data: Path, sandbox: Sandbox
) -> None:
    from tests import onnx_models

    version_id = model_import.import_model(
        RegistryRoots.under(data), manifest(name="row-pure"), onnx_models.valid(), sandbox
    ).model_version_id
    require_row_pure(data, version_id, row_inputs(market), sandbox)
