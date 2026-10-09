"""Fold models and their version 3 schedules: fresh training-only fits per
fold, no evaluation label or later recording reaching a fold's fit,
prediction, or execution, explicit unevaluable outcomes, immutable artifacts,
and label-free schedules of every evaluation row run by the C++ engine."""

import dataclasses
import json
import shutil
from collections.abc import Callable
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from chronos_research import pipeline as research_pipeline
from chronos_research import walk_forward
from chronos_research.artifacts import LabelRow, VerifiedArtifact, read_label_rows
from chronos_research.collection import create_collection
from chronos_research.encoding import canonical_json, decode_float, sha256_hex
from chronos_research.fold_models import (
    MODEL,
    FoldModelError,
    build_fold_model,
    fold_models_root,
    publish_fold_model,
    verified_fold_model,
)
from chronos_research.models import LogisticModel, MajorityModel, Unevaluable
from chronos_research.pipeline import ArtifactRoots, StrategyOptions, inspect_strategy, run_strategy
from chronos_research.schedule import ROWS, ScheduleError, Window, schedule_identity, verify_schedule
from chronos_research.scoring import score_schedule
from chronos_research.walk_forward import PlanSources, plan_sources
from chronos_research.walk_forward_schedule import (
    build_fold_schedule,
    fold_labels,
    fold_window,
    publish_fold_schedule,
)
from tests import pipeline as built
from tests.recordings import WEEK, Recording, Recordings, prepare
from tests.test_walk_forward_plan import raw_rows, request

INPUT_SCALES = (10**9, 10**9, 10**9, 10**8, 10**8)
OPTIONS = StrategyOptions(
    threshold_ppb=500_000_000, target_quantity="1", latency_ns=1000, decision_interval_ns=1_000_000,
    starting_cash="10000", fee_per_fill="0.01", maximum_quote_age_ns=5_000_000_000,
    maximum_order_notional="1000000",
)  # fmt: skip


@pytest.fixture(scope="module")
def week(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Recordings:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("folds"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in WEEK:
        prepare(recordings, recording)
    # The day before: so few quotes that no label is ever known.
    prepare(recordings, Recording("synthetic-2026-09-20-empty", day=-1, quotes=3, seed=21))
    # Two days before: too few rows of some class for a logistic fit.
    prepare(recordings, Recording("synthetic-2026-09-19-thin", day=-2, quotes=24, seed=22))
    return recordings


@pytest.fixture
def copy(week: Recordings, tmp_path: Path) -> Recordings:
    root = tmp_path / "artifacts"
    shutil.copytree(week.artifacts, root, symlinks=True)
    return Recordings(artifacts=root, tools=week.tools, prepared=dict(week.prepared))


def sources_of(recordings: Recordings, *streams: str) -> PlanSources:
    created = create_collection(recordings.tools, recordings.artifacts, recordings.label_ids(*streams))
    return plan_sources(
        recordings.tools, recordings.artifacts, request(created["collection"]["collection_id"])
    )


def week_sources(recordings: Recordings) -> PlanSources:
    return sources_of(recordings, *(recording.stream for recording in WEEK))


def fitted(sources: PlanSources, index: int) -> Any:
    return build_fold_model(sources.plan, sources.fold(index), sources.training(index))


def schedule_of(sources: PlanSources, index: int, name: str = "logistic") -> Any:
    model = fitted(sources, index)
    recording, _ = sources.evaluation(index)
    return build_fold_schedule(sources.plan, sources.fold(index), model, name, recording)


def training_inputs(artifacts: Path, prepared: Any) -> list[list[float]]:
    """The eligible AAPL rows' model inputs, from the raw files: inputs
    available and a usable label (all earlier than any later recording)."""
    features = {
        row["ingest_sequence"]: row["features"]
        for row in raw_rows(artifacts, "features", prepared.feature_id)
        if row["instrument"] == "AAPL"
    }
    inputs = []
    for label in raw_rows(artifacts, "labels", prepared.label_id):
        values = features.get(label["ingest_sequence"]) if label["instrument"] == "AAPL" else None
        names = ("spread", "imbalance", "microprice", "midpoint", "bid_size", "ask_size")
        if values is None or "direction" not in label["label"] or any("raw" not in values[n] for n in names):
            continue
        raw = [int(values[name]["raw"]) for name in names]
        exact = [raw[0], raw[1], raw[2] - raw[3], raw[4], raw[5]]
        inputs.append([value / scale for value, scale in zip(exact, INPUT_SCALES, strict=True)])
    return inputs


def test_each_fold_fits_fresh_models_on_its_own_training_rows_only(copy: Recordings) -> None:
    sources = week_sources(copy)
    scalers = []
    for index in (1, 2):
        model = fitted(sources, index)
        majority, logistic = model.models["majority"], model.models["logistic"]
        assert isinstance(majority, MajorityModel)
        assert isinstance(logistic, LogisticModel)
        support = sources.fold(index)["training"]["class_support"]
        assert majority.as_json()["class_counts"] == support
        members = [copy.prepared[recording.stream] for recording in WEEK[:index]]
        inputs = [row for member in members for row in training_inputs(copy.artifacts, member)]
        assert len(inputs) == int(sources.fold(index)["training"]["eligible_rows"])
        np.testing.assert_allclose(logistic.mean, np.mean(np.array(inputs), axis=0), rtol=1e-12, atol=0)
        scalers.append(logistic.mean)
        assert model.manifest["training"]["rows"] == str(len(inputs))
        assert int(model.manifest["training"]["max_label_available_at_ns"]) < int(
            model.manifest["training"]["information_boundary_ns"]
        )
    # A fresh scaler per fold: fold 2 also learned from fold 1's evaluation day.
    assert not np.array_equal(scalers[0], scalers[1])


def flipping(label_id: str) -> Callable[[VerifiedArtifact], list[LabelRow]]:
    """A label reader that turns every up into down, and down into up, in
    one label artifact's rows, after they were verified."""
    swap = {"up": "down", "down": "up", "flat": "flat"}

    def read(artifact: VerifiedArtifact) -> list[LabelRow]:
        rows = read_label_rows(artifact)
        if artifact.artifact_id != label_id:
            return rows
        return [
            dataclasses.replace(row, direction=swap[row.direction]) if row.direction is not None else row
            for row in rows
        ]

    return read


def run_results(copy: Recordings, built_schedule: Any) -> dict[str, Any]:
    roots = ArtifactRoots.under(copy.artifacts)
    publish_fold_schedule(roots.schedules, built_schedule)
    _, manifest = run_strategy(copy.tools, roots, built_schedule.schedule_id, OPTIONS)
    results: dict[str, Any] = manifest["results"]
    return results


def test_a_folds_evaluation_labels_reach_nothing_it_fits_predicts_or_executes(
    copy: Recordings, monkeypatch: pytest.MonkeyPatch
) -> None:
    before = week_sources(copy)
    fold_one, fold_two = fitted(before, 1), fitted(before, 2)
    schedules = {name: schedule_of(before, 1, name) for name in ("logistic", "majority")}
    executed = {name: run_results(copy, schedule) for name, schedule in schedules.items()}
    scores = {name: scored(before, schedule) for name, schedule in schedules.items()}
    # Fold 1's evaluation day is synthetic-2026-09-22; its labels flip.
    monkeypatch.setattr(walk_forward, "read_label_rows", flipping(copy.prepared[WEEK[1].stream].label_id))
    after = week_sources(copy)
    assert fitted(after, 1).files[MODEL] == fold_one.files[MODEL]
    for name, schedule in schedules.items():
        rebuilt = schedule_of(after, 1, name)
        assert rebuilt.files[ROWS] == schedule.files[ROWS]
        assert run_results(copy, rebuilt) == executed[name]
        # Only the scores, read from the labels after the schedule, move.
        rescored = scored(after, rebuilt)
        assert rescored["rows"] == scores[name]["rows"]
        assert rescored["metrics"]["confusion_matrix"] != scores[name]["metrics"]["confusion_matrix"]
    # Fold 2 trains on that day, now history: its fit legitimately changes,
    # and so does the plan's claim about its training.
    assert fitted(after, 2).files[MODEL] != fold_two.files[MODEL]
    assert after.plan["plan_id"] != before.plan["plan_id"]


def scored(sources: PlanSources, schedule: Any) -> dict[str, Any]:
    recording, _ = sources.evaluation(1)
    return score_schedule(schedule.files[ROWS], fold_labels(recording, "AAPL", fold_window(sources.fold(1))))


def test_a_fold_is_scored_on_every_scheduled_row_with_a_label_known_by_its_end(copy: Recordings) -> None:
    sources = week_sources(copy)
    schedule = schedule_of(sources, 1)
    result = scored(sources, schedule)
    rows = result["rows"]
    labels = {
        row["ingest_sequence"]: row["label"]
        for row in raw_rows(copy.artifacts, "labels", copy.prepared[WEEK[1].stream].label_id)
        if row["instrument"] == "AAPL"
    }
    usable = sum("direction" in label for label in labels.values())
    assert rows["scheduled"] == str(len(labels)) == sources.fold(1)["evaluation"]["feature_rows"]
    assert rows["scored"] == str(usable)
    assert rows["not_scored"] == {"label_unavailable:no_future_observation": str(len(labels) - usable)}
    # The cutoff, one nanosecond either side of a label's availability.
    recording, joined = sources.evaluation(1)
    row = next(r for r in joined if r.label.direction is not None)
    known = row.label.label_available_at_ns
    assert known is not None
    start = row.feature.available_at_ns
    at = fold_labels(recording, "AAPL", Window("cutoff", start, known))
    after = fold_labels(recording, "AAPL", Window("cutoff", start, known + 1))
    assert row.feature.identity not in at.truth
    assert at.not_scored["label_after_scoring_cutoff"] >= 1
    assert after.truth[row.feature.identity] == ("down", "flat", "up").index(row.label.direction)


def test_a_changed_training_label_changes_its_folds_fit(
    copy: Recordings, monkeypatch: pytest.MonkeyPatch
) -> None:
    before = fitted(week_sources(copy), 1)
    monkeypatch.setattr(walk_forward, "read_label_rows", flipping(copy.prepared[WEEK[0].stream].label_id))
    after = fitted(week_sources(copy), 1)
    assert after.files[MODEL] != before.files[MODEL]


def test_a_later_recording_changes_no_earlier_folds_numbers(copy: Recordings) -> None:
    three = week_sources(copy)
    two = sources_of(copy, WEEK[0].stream, WEEK[1].stream)
    assert fitted(three, 1).files[MODEL] == fitted(two, 1).files[MODEL]
    assert schedule_of(three, 1).files[ROWS] == schedule_of(two, 1).files[ROWS]
    # Only identities differ: another collection, so another plan.
    assert fitted(three, 1).fold_model_id != fitted(two, 1).fold_model_id


def test_unfit_models_say_why_and_schedule_nothing(copy: Recordings) -> None:
    empty = sources_of(copy, "synthetic-2026-09-20-empty", WEEK[0].stream)
    model = fitted(empty, 1)
    assert model.models["majority"] == Unevaluable("no_training_rows", {})
    assert model.models["logistic"] == Unevaluable("no_training_rows", {})
    assert model.manifest["models"]["majority"] == {"reason": "no_training_rows", "status": "unevaluable"}
    assert model.manifest["training"]["members"][0]["excluded"] == {
        "label_unavailable:no_future_observation": "3"
    }
    assert model.manifest["training"]["max_label_available_at_ns"] is None
    recording, _ = empty.evaluation(1)
    with pytest.raises(ScheduleError, match="unevaluable"):
        build_fold_schedule(empty.plan, empty.fold(1), model, "majority", recording)
    thin = fitted(sources_of(copy, "synthetic-2026-09-19-thin", WEEK[0].stream), 1)
    assert isinstance(thin.models["majority"], MajorityModel)
    logistic = thin.models["logistic"]
    assert isinstance(logistic, Unevaluable)
    assert logistic.reason == "insufficient_class_support"


def test_a_fold_model_is_published_once_and_tampering_is_refused(copy: Recordings) -> None:
    sources = week_sources(copy)
    model = fitted(sources, 1)
    assert fitted(sources, 1).files == model.files
    assert publish_fold_model(copy.artifacts, model) == "published"
    assert publish_fold_model(copy.artifacts, model) == "already_present"
    root = fold_models_root(copy.artifacts)
    stored = verified_fold_model(root, model.fold_model_id)
    assert {name: fit.as_json() for name, fit in stored.models.items()} == {
        name: fit.as_json() for name, fit in model.models.items()
    }
    assert stored.manifest == model.manifest
    directory = root / model.fold_model_id
    directory.chmod(0o755)
    model_file = directory / MODEL
    model_file.chmod(0o644)
    model_file.write_text(model_file.read_text().replace('"iterations":"', '"iterations":"1', 1))
    with pytest.raises(FoldModelError) as tampered:
        verified_fold_model(root, model.fold_model_id)
    assert tampered.value.reason == "artifact_invalid"


def test_a_fold_schedule_predicts_every_evaluation_row_without_labels(copy: Recordings) -> None:
    sources = week_sources(copy)
    model = fitted(sources, 1)
    schedule = build_fold_schedule(sources.plan, sources.fold(1), model, "logistic", sources.evaluation(1)[0])
    rows = [json.loads(line) for line in schedule.files[ROWS].splitlines()]
    evaluation = copy.prepared[WEEK[1].stream]
    features = raw_rows(copy.artifacts, "features", evaluation.feature_id)
    aapl = [row for row in features if row["instrument"] == "AAPL"]
    assert [row["identity"]["ingest_sequence"] for row in rows] == [row["ingest_sequence"] for row in aapl]
    labels = {row["ingest_sequence"]: row for row in raw_rows(copy.artifacts, "labels", evaluation.label_id)}
    assert any("unavailable" in labels[row["identity"]["ingest_sequence"]]["label"] for row in rows)
    assert all(set(row) == {"available_at_ns", "identity", "inputs", "prediction", "record"} for row in rows)
    logistic = model.models["logistic"]
    assert isinstance(logistic, LogisticModel)
    raw = [row["inputs"]["raw"] for row in rows]
    inputs = np.array(
        [[int(value) / scale for value, scale in zip(r, INPUT_SCALES, strict=True)] for r in raw]
    )
    _, expected = logistic.predict(inputs)
    stored = np.array([[decode_float(p) for p in row["prediction"]["probabilities"]] for row in rows])
    np.testing.assert_allclose(stored, expected, rtol=0, atol=1e-12)
    manifest = json.loads(schedule.files["manifest.json"])
    assert manifest["schema"] == {"name": "chronos.inference_schedule", "version": "3"}
    assert "label_artifact_id" not in manifest["source"]
    assert "split_artifact_id" not in manifest["source"]
    roots = ArtifactRoots.under(copy.artifacts)
    publish_fold_schedule(roots.schedules, schedule)
    assert verify_schedule(roots.schedules / schedule.schedule_id)["artifact_id"] == schedule.schedule_id
    step, run = run_strategy(copy.tools, roots, schedule.schedule_id, OPTIONS)
    assert run["schema"]["version"] == "3"
    assert {key: run["source"][key] for key in ("fold", "fold_model_id", "model", "plan_id")} == {
        "fold": "1",
        "fold_model_id": model.fold_model_id,
        "model": "logistic",
        "plan_id": sources.plan["plan_id"],
    }
    inspected = inspect_strategy(copy.tools, roots, step.artifact_id)
    assert inspected["reproduced"].startswith("ran the schedule again")


def tampered_schedule(schedule: Any, edit: Callable[[dict[str, Any]], None]) -> dict[str, bytes]:
    manifest = json.loads(schedule.files["manifest.json"])
    del manifest["artifact_id"]
    edit(manifest)
    manifest["artifact_id"] = schedule_identity(manifest)
    return {ROWS: schedule.files[ROWS], "manifest.json": canonical_json(manifest)}


@pytest.mark.parametrize(
    "edit",
    [
        lambda m: m["window"].update(end_at_ns=None),
        lambda m: m["training"].update(label_available_through_ns=m["training"]["information_boundary_ns"]),
        lambda m: m["training"].update(prediction_through_ns=m["training"]["information_boundary_ns"]),
        lambda m: m["training"].update(information_boundary_ns=str(int(m["window"]["start_at_ns"]) + 1)),
        lambda m: m["source"].update(split_artifact_id="a" * 64),
        lambda m: m["model"].update(experiment_id="e" * 64),
        lambda m: m["model"].update(name="onnx"),
    ],
)
def test_a_fold_schedule_that_could_have_seen_its_window_or_claims_a_split_is_refused(
    copy: Recordings, edit: Callable[[dict[str, Any]], None]
) -> None:
    sources = week_sources(copy)
    schedule = schedule_of(sources, 1)
    files = tampered_schedule(schedule, edit)
    identifier = json.loads(files["manifest.json"])["artifact_id"]
    directory = copy.artifacts / "tampered" / identifier
    directory.mkdir(parents=True)
    for name, contents in files.items():
        (directory / name).write_bytes(contents)
    with pytest.raises(ScheduleError):
        verify_schedule(directory)
    # The C++ runner refuses it too.
    roots = ArtifactRoots.under(copy.artifacts)
    shutil.copytree(directory, roots.schedules / identifier)
    with pytest.raises(research_pipeline.PipelineError):
        run_strategy(copy.tools, roots, identifier, OPTIONS)
    assert sha256_hex(files[ROWS]) == sha256_hex(schedule.files[ROWS])


def drop_last(rows: list[dict[str, Any]], counts: dict[str, Any]) -> None:
    dropped = rows.pop()
    counts["rows"] = str(int(counts["rows"]) - 1)
    counts["predicted"] = str(int(counts["predicted"]) - 1)
    name = dropped["prediction"]["class"]
    counts["classes"][name] = str(int(counts["classes"][name]) - 1)


@pytest.mark.parametrize(
    "edit",
    [
        lambda rows, _: rows[3]["inputs"]["raw"].__setitem__(0, str(int(rows[3]["inputs"]["raw"][0]) + 1)),
        lambda rows, _: rows[3].update(available_at_ns=str(int(rows[3]["available_at_ns"]) + 1)),
        lambda rows, _: rows[3]["identity"].update(
            ingest_sequence=str(int(rows[3]["identity"]["ingest_sequence"]) + 1)
        ),
        lambda rows, _: rows.__setitem__(slice(3, 5), [rows[4], rows[3]]),
        drop_last,
    ],
    ids=["inputs", "time", "identity", "order", "missing-row"],
)
def test_the_engine_refuses_fold_schedule_rows_its_dataset_does_not_give(
    copy: Recordings, edit: Callable[[list[dict[str, Any]], dict[str, Any]], None]
) -> None:
    """Rows rewritten with consistent checksums, counts, and ID: only their
    content disagrees with the dataset, and the C++ runner recomputes it."""
    sources = week_sources(copy)
    schedule = schedule_of(sources, 1)
    rows = [json.loads(line) for line in schedule.files[ROWS].splitlines()]
    manifest = json.loads(schedule.files["manifest.json"])
    edit(rows, manifest["counts"])
    contents = b"".join(canonical_json(row) for row in rows)
    manifest["files"]["schedule"].update(sha256=sha256_hex(contents), size_bytes=str(len(contents)))
    del manifest["artifact_id"]
    manifest["artifact_id"] = schedule_identity(manifest)
    roots = ArtifactRoots.under(copy.artifacts)
    directory = roots.schedules / manifest["artifact_id"]
    directory.mkdir(parents=True)
    (directory / ROWS).write_bytes(contents)
    (directory / "manifest.json").write_bytes(canonical_json(manifest))
    with pytest.raises(research_pipeline.PipelineError):
        run_strategy(copy.tools, roots, manifest["artifact_id"], OPTIONS)


def test_the_rewriting_itself_gives_a_schedule_the_engine_runs(copy: Recordings) -> None:
    """The control for the refusals above: rows rewritten unchanged run."""
    sources = week_sources(copy)
    schedule = schedule_of(sources, 1)
    rows = [json.loads(line) for line in schedule.files[ROWS].splitlines()]
    contents = b"".join(canonical_json(row) for row in rows)
    assert contents == schedule.files[ROWS]
    roots = ArtifactRoots.under(copy.artifacts)
    publish_fold_schedule(roots.schedules, schedule)
    _, run = run_strategy(copy.tools, roots, schedule.schedule_id, OPTIONS)
    assert run["source"]["schedule_id"] == schedule.schedule_id
