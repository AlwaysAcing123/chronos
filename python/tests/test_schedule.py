"""Inference schedules: label-free, causal, complete over their window, and
verifiable, built through the real C++ tools from synthetic events."""

import json
import math
import os
import shutil
import stat
from dataclasses import dataclass, replace
from fractions import Fraction
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from chronos_research.artifacts import (
    CLASS_ORDER,
    Unavailable,
    VerifiedSources,
    read_feature_rows,
    read_label_rows,
)
from chronos_research.cli import main
from chronos_research.experiment import PREDICTIONS
from chronos_research.inspection import verified_experiment
from chronos_research.model_data import exact_inputs, to_floats
from chronos_research.models import INFERENCE_TOLERANCE, LogisticModel
from chronos_research.pipeline import (
    ArtifactRoots,
    StrategyOptions,
    Tools,
    create_split,
    evaluate_baselines,
    extract_features,
    extract_labels,
    infer_schedule,
    run_strategy,
    synthesize_dataset,
    verified_sources,
)
from chronos_research.schedule import (
    ROWS,
    ScheduleError,
    Window,
    build_schedule,
    evaluation_window,
    logistic_predictions,
    reproduce_schedule,
    require_training_precedes,
    verify_schedule,
)
from tests import pipeline as built
from tests.synthetic import synthetic_events


@dataclass(frozen=True)
class Research:
    tools: Tools
    roots: ArtifactRoots
    sources: VerifiedSources
    experiment_id: str

    @property
    def experiment(self) -> Path:
        return self.roots.experiments / self.experiment_id


def research(tools: built.Tools, base: Path, events: str) -> Research:
    directory = Tools(tools.chronos_dataset.parent)
    roots = ArtifactRoots.under(base / "data")
    (base / "events.csv").write_text(events)
    dataset = synthesize_dataset(directory, base / "events.csv", "synthetic-stream", roots.catalog)
    for root in (roots.features, roots.labels, roots.splits):
        root.mkdir(parents=True, exist_ok=True)
    features = extract_features(directory, roots, dataset.artifact_id)
    labels = extract_labels(directory, roots, features.artifact_id, 150_000)
    split = create_split(directory, roots, labels.artifact_id)
    sources = verified_sources(directory, roots, split.artifact_id, reproduce_features=True)
    experiment, _ = evaluate_baselines(sources, roots.experiments)
    return Research(directory, roots, sources, experiment.artifact_id)


@pytest.fixture(scope="module")
def market(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Research:
    return research(tools, tmp_path_factory.mktemp("schedule"), synthetic_events(seed=7))


def rows_of(contents: bytes) -> list[dict[str, Any]]:
    return [json.loads(line) for line in contents.splitlines()]


def schedule_rows(
    market: Research, window: str, instrument: str = "AAPL", model: str = "logistic"
) -> list[dict[str, Any]]:
    schedule = build_schedule(market.sources, market.experiment, instrument, model, window)
    return rows_of(schedule.files[ROWS])


def identities(rows: list[dict[str, Any]]) -> set[tuple[str, str, int]]:
    return {
        (row["identity"]["stream"], row["identity"]["instrument"], int(row["identity"]["ingest_sequence"]))
        for row in rows
    }


def test_a_schedule_covers_every_feature_row_of_its_window_including_the_unlabelled_tail(
    market: Research,
) -> None:
    features = read_feature_rows(market.sources.features)
    labels = {row.identity: row for row in read_label_rows(market.sources.labels)}
    evaluated = rows_of((market.experiment / PREDICTIONS).read_bytes())
    for name in ("validation", "test"):
        window = evaluation_window(market.sources, name)
        expected = [
            row
            for row in features
            if row.identity.instrument == "AAPL" and window.contains(row.available_at_ns)
        ]
        scheduled = schedule_rows(market, name)
        assert [int(row["identity"]["ingest_sequence"]) for row in scheduled] == [
            row.identity.ingest_sequence for row in expected
        ]
        # More rows than the experiment evaluated: rows the split excluded,
        # and in the test window the tail whose labels never became known.
        predicted = identities(
            [row for row in evaluated if row["split"] == name and row["identity"]["instrument"] == "AAPL"]
        )
        unevaluated = identities(scheduled) - predicted
        assert unevaluated
        if name == "test":
            assert any(
                labels[row.identity].direction is None
                for row in expected
                if tuple(row.identity) in unevaluated
            )


def test_schedule_rows_carry_no_label_information(market: Research) -> None:
    schedule = build_schedule(market.sources, market.experiment, "AAPL", "logistic", "test")
    for row in rows_of(schedule.files[ROWS]):
        assert set(row) == {"available_at_ns", "identity", "inputs", "prediction", "record"}
    text = schedule.files[ROWS].decode()
    for forbidden in ("true_class", "return", "label", "future", "split", "direction"):
        assert forbidden not in text


def test_inference_never_reads_the_label_or_split_rows(market: Research, tmp_path: Path) -> None:
    missing = tmp_path / "absent.ndjson"
    blind = replace(
        market.sources,
        labels=replace(market.sources.labels, rows_path=missing),
        splits=replace(market.sources.splits, rows_path=missing),
    )
    for window in ("validation", "test"):
        assert (
            build_schedule(blind, market.experiment, "AAPL", "logistic", window).files
            == build_schedule(market.sources, market.experiment, "AAPL", "logistic", window).files
        )


@pytest.mark.parametrize("model_name", ["majority", "logistic"])
def test_schedule_does_not_reopen_model_parameters_after_verification(
    market: Research, monkeypatch: pytest.MonkeyPatch, model_name: str
) -> None:
    expected = build_schedule(market.sources, market.experiment, "AAPL", model_name, "test")
    model_path = market.experiment / "model.json"
    original_read = Path.read_bytes
    reads = 0

    def checked_read(path: Path) -> bytes:
        nonlocal reads
        if path == model_path:
            reads += 1
            assert reads == 1, "model parameters were reread after verification"
        return original_read(path)

    monkeypatch.setattr(Path, "read_bytes", checked_read)
    actual = build_schedule(market.sources, market.experiment, "AAPL", model_name, "test")
    assert actual.files == expected.files
    assert reads == 1


def test_each_prediction_is_the_stored_model_on_that_row_alone(market: Research) -> None:
    _, models = verified_experiment(market.experiment)
    model = models["AAPL"]["logistic"]
    assert isinstance(model, LogisticModel)
    features = {
        row.identity.ingest_sequence: row
        for row in read_feature_rows(market.sources.features)
        if row.identity.instrument == "AAPL"
    }
    for row in schedule_rows(market, "test"):
        inputs = exact_inputs(features[int(row["identity"]["ingest_sequence"])])
        prediction = row["prediction"]
        if isinstance(inputs, Unavailable):
            assert prediction == {"unavailable": f"feature_unavailable:{inputs.reason}"}
            continue
        assert row["inputs"] == {"raw": [str(value) for value in inputs]}
        # The same bits whether the row is predicted alone or in its window.
        classes, probabilities = logistic_predictions(model, [inputs])
        assert prediction["class"] == CLASS_ORDER[int(classes[0])]
        assert prediction["probabilities"] == [repr(float(p)) for p in probabilities[0]]
        chosen = float(probabilities[0][int(classes[0])])
        assert int(prediction["confidence_ppb"]) == math.floor(Fraction(chosen) * 1_000_000_000)
        # And the experiment's batch inference, within its tolerance.
        batch_classes, batch_probabilities = model.predict(to_floats([inputs]))
        assert int(batch_classes[0]) == int(classes[0])
        assert float(np.max(np.abs(batch_probabilities - probabilities))) <= INFERENCE_TOLERANCE


def test_the_majority_model_schedules_its_constant_prediction(market: Research) -> None:
    rows = schedule_rows(market, "validation", model="majority")
    predicted = {row["prediction"]["class"] for row in rows if "class" in row["prediction"]}
    assert len(predicted) == 1


def test_unevaluable_models_unknown_instruments_and_other_windows_are_refused(
    market: Research, tools: built.Tools, tmp_path: Path
) -> None:
    with pytest.raises(ScheduleError, match="no models for 'MSFT'"):
        build_schedule(market.sources, market.experiment, "MSFT", "logistic", "test")
    with pytest.raises(ScheduleError, match="one evaluation window"):
        build_schedule(market.sources, market.experiment, "AAPL", "logistic", "train")
    with pytest.raises(ScheduleError, match="the model is one of"):
        build_schedule(market.sources, market.experiment, "AAPL", "oracle", "test")
    thin = research(tools, tmp_path, synthetic_events(seed=7, quotes_per_instrument=30))
    with pytest.raises(ScheduleError, match=r"unevaluable \(insufficient_class_support\)"):
        build_schedule(thin.sources, thin.experiment, "AAPL", "logistic", "test")
    # An experiment trained on other sources is not this split's model.
    with pytest.raises(ScheduleError, match="not trained on these verified sources"):
        build_schedule(thin.sources, market.experiment, "AAPL", "logistic", "test")


def test_training_labels_must_be_known_before_the_window_begins(market: Research) -> None:
    require_training_precedes(Window("test", 100, None), 99)
    with pytest.raises(ScheduleError, match="not before the test window"):
        require_training_precedes(Window("test", 100, None), 100)
    manifest = json.loads(
        build_schedule(market.sources, market.experiment, "AAPL", "logistic", "validation").files[
            "manifest.json"
        ]
    )
    assert int(manifest["training"]["label_available_through_ns"]) < int(manifest["window"]["start_at_ns"])


def writable_copy(source: Path, destination: Path) -> Path:
    shutil.copytree(source, destination)
    for path in (destination, *destination.iterdir()):
        os.chmod(path, stat.S_IRWXU)
    return destination


def test_a_published_schedule_verifies_reproduces_and_refuses_tampering(
    market: Research, tmp_path: Path
) -> None:
    first = infer_schedule(market.sources, market.roots, market.experiment_id, "AAPL", "logistic", "test")
    assert first.outcome in ("published", "already_present")
    again = infer_schedule(market.sources, market.roots, market.experiment_id, "AAPL", "logistic", "test")
    assert (again.outcome, again.artifact_id) == ("already_present", first.artifact_id)
    verify_schedule(first.directory)
    reproduce_schedule(first.directory, market.sources, market.roots.experiments)

    edited = writable_copy(first.directory, tmp_path / "edited" / first.artifact_id)
    lines = (edited / ROWS).read_bytes().splitlines(keepends=True)
    record = json.loads(lines[0])
    record["available_at_ns"] = str(int(record["available_at_ns"]) + 1)
    lines[0] = json.dumps(record, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    (edited / ROWS).write_bytes(b"".join(lines))
    with pytest.raises(ScheduleError):
        verify_schedule(edited)

    renamed = writable_copy(first.directory, tmp_path / "renamed" / ("0" * 64))
    with pytest.raises(ScheduleError, match="does not match"):
        verify_schedule(renamed)


def test_the_cli_infers_and_inspects_a_schedule(market: Research, capsys: pytest.CaptureFixture[str]) -> None:
    common = ["--tools", str(market.tools.directory), "--artifacts", str(market.roots.catalog.parent)]
    model = ["--experiment", market.experiment_id, "--instrument", "SPY", "--model", "logistic"]
    assert main(["infer", *common, *model, "--window", "validation"]) == 0
    inferred = json.loads(capsys.readouterr().out)
    assert inferred["kind"] == "schedule"
    assert inferred["window"]["name"] == "validation"
    assert main(["inspect-schedule", *common, "--schedule", inferred["artifact_id"]]) == 0
    inspected = json.loads(capsys.readouterr().out)
    assert "rebuilt from its verified sources" in inspected["verified"]
    with pytest.raises(SystemExit) as refused:
        main(["infer", *common, *model, "--window", "train"])
    assert refused.value.code == 2


def test_the_strategy_decides_on_unlabelled_tail_rows(market: Research) -> None:
    schedule = infer_schedule(market.sources, market.roots, market.experiment_id, "AAPL", "logistic", "test")
    step, manifest = run_strategy(
        market.tools, market.roots, schedule.artifact_id, StrategyOptions(decision_interval_ns=1)
    )
    journal = [json.loads(line) for line in (step.directory / "journal.ndjson").read_text().splitlines()]
    signalled = {
        int(entry["signal"]["ingest_sequence"])
        for entry in journal
        if entry["type"] == "decision" and entry["account"] == "strategy"
    }
    tail = {
        row.identity.ingest_sequence
        for row in read_label_rows(market.sources.labels)
        if row.identity.instrument == "AAPL" and row.direction is None
    }
    assert tail
    assert tail <= signalled
    assert manifest["results"]["decisions"]["evaluated"] == str(len(signalled))
