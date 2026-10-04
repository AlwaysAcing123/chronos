"""The deployment inference worker: verified identity, exact parity with the
historical schedules, and failing closed on anything malformed."""

import io
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from chronos_research.inference_worker import MAXIMUM_LINE_BYTES, PROTOCOL, run_worker
from chronos_research.schedule import ROWS, build_schedule
from tests import pipeline as built
from tests.synthetic import synthetic_events
from tests.test_schedule import Research, research


@pytest.fixture(scope="module")
def market(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Research:
    return research(tools, tmp_path_factory.mktemp("worker"), synthetic_events(seed=7))


def converse(
    market: Research, model: str, requests: list[Any], instrument: str = "AAPL"
) -> list[dict[str, Any]]:
    lines = b"".join(
        (json.dumps(request) if not isinstance(request, bytes) else request.decode()).encode() + b"\n"
        for request in requests
    )
    output = io.StringIO()
    run_worker(
        market.roots.experiments, market.experiment_id, instrument, model, False, io.BytesIO(lines), output
    )
    return [json.loads(line) for line in output.getvalue().splitlines()]


def request_for(row: dict[str, Any], identifier: str) -> dict[str, Any]:
    return {
        "id": identifier,
        "ingest_sequence": row["identity"]["ingest_sequence"],
        "inputs": row["inputs"]["raw"],
        "instrument": row["identity"]["instrument"],
        "stream": row["identity"]["stream"],
    }


@pytest.mark.parametrize("model", ["logistic", "majority"])
def test_predictions_and_identity_equal_the_historical_schedule(market: Research, model: str) -> None:
    schedule = build_schedule(market.sources, market.experiment, "AAPL", model, "test")
    manifest = json.loads(schedule.files["manifest.json"])
    rows = [json.loads(line) for line in schedule.files[ROWS].splitlines()]
    available = [row for row in rows if "raw" in row["inputs"]]
    replies = converse(
        market, model, [request_for(row, str(index + 1)) for index, row in enumerate(available)]
    )
    ready, predictions = replies[0], replies[1:]
    assert ready["type"] == "ready"
    assert ready["protocol"] == PROTOCOL
    assert ready["parameters_sha256"] == manifest["model"]["parameters_sha256"]
    assert ready["experiment_id"] == market.experiment_id
    assert int(ready["training_cutoff_ns"]) == market.sources.validation_start_at_ns
    assert len(predictions) == len(available)
    for row, prediction, index in zip(available, predictions, range(1, len(available) + 1), strict=True):
        assert prediction["id"] == str(index)
        assert prediction["ingest_sequence"] == row["identity"]["ingest_sequence"]
        assert (prediction["class"], prediction["probabilities"], prediction["confidence_ppb"]) == (
            row["prediction"]["class"],
            row["prediction"]["probabilities"],
            row["prediction"]["confidence_ppb"],
        )


def test_malformed_requests_are_errors_and_the_worker_continues(market: Research) -> None:
    good = {
        "id": "9",
        "ingest_sequence": "3",
        "inputs": ["1", "0", "0", "100000000", "100000000"],
        "instrument": "AAPL",
        "stream": "s",
    }
    replies = converse(
        market,
        "logistic",
        [
            b"not json",
            {**good, "id": "01"},
            {**good, "inputs": ["1", "0", "0", "1"]},
            {**good, "inputs": ["1.5", "0", "0", "1", "1"]},
            {**good, "inputs": ["9223372036854775808", "0", "0", "1", "1"]},
            {**good, "instrument": "SPY"},
            {**good, "extra": "1"},
            good,
        ],
    )
    assert [reply["type"] for reply in replies[1:]] == ["error"] * 7 + ["prediction"]
    assert replies[3]["id"] == "9"
    assert replies[-1]["id"] == "9"


def test_an_oversized_line_ends_the_worker(market: Research) -> None:
    output = io.StringIO()
    code = run_worker(
        market.roots.experiments, market.experiment_id, "AAPL", "logistic", False,
        io.BytesIO(b"x" * (MAXIMUM_LINE_BYTES + 10) + b"\n"), output,
    )  # fmt: skip
    assert code == 1
    assert json.loads(output.getvalue().splitlines()[-1])["message"] == "a request line is too long"


def test_unevaluable_unknown_and_corrupt_models_fail_closed(
    market: Research, tools: built.Tools, tmp_path: Path
) -> None:
    (tmp_path / "thin").mkdir()
    thin = research(tools, tmp_path / "thin", synthetic_events(seed=7, quotes_per_instrument=30))
    output = io.StringIO()
    assert (
        run_worker(thin.roots.experiments, thin.experiment_id, "AAPL", "logistic", True, io.BytesIO(), output)
        == 1
    )
    assert "unevaluable (insufficient_class_support)" in output.getvalue()
    output = io.StringIO()
    assert (
        run_worker(
            market.roots.experiments, market.experiment_id, "MSFT", "logistic", True, io.BytesIO(), output
        )
        == 1
    )
    # A copy whose stored parameters were edited no longer verifies.
    corrupt = tmp_path / "corrupt"
    shutil.copytree(market.roots.experiments, corrupt)
    for path in corrupt.rglob("*"):
        os.chmod(path, stat.S_IRWXU)
    model = corrupt / market.experiment_id / "model.json"
    model.write_bytes(model.read_bytes().replace(b'"iterations":"', b'"iterations":"1', 1))
    output = io.StringIO()
    assert run_worker(corrupt, market.experiment_id, "AAPL", "logistic", True, io.BytesIO(), output) == 1
    assert json.loads(output.getvalue())["type"] == "error"


def test_the_command_line_worker_answers_line_by_line(market: Research) -> None:
    command = [
        sys.executable, "-m", "chronos_research", "inference-worker",
        "--artifacts", str(market.roots.catalog.parent), "--experiment", market.experiment_id,
        "--instrument", "AAPL", "--model", "logistic",
    ]  # fmt: skip
    with subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
        cwd=Path(__file__).resolve().parent.parent,
    ) as child:
        assert child.stdin is not None
        assert child.stdout is not None
        assert json.loads(child.stdout.readline())["type"] == "ready"
        request = {
            "id": "1",
            "ingest_sequence": "5",
            "inputs": ["2", "0", "0", "100000000", "100000000"],
            "instrument": "AAPL",
            "stream": "s",
        }
        child.stdin.write(json.dumps(request) + "\n")
        child.stdin.flush()
        reply = json.loads(child.stdout.readline())
        assert reply["type"] == "prediction"
        assert reply["id"] == "1"
        child.stdin.close()
        assert child.wait(timeout=30) == 0
