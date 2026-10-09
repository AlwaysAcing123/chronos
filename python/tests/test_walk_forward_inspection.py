"""Inspection against rewritten studies: an author who edits a stored study
and renews every derived figure and its identity, so that only the edit is
wrong. Inspection must refuse a completed cell with no run, a run-less
failure that is not exactly a failure, and any run that is not of the
plan's schedule under all eight of its frozen settings."""

import json
import shutil
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from chronos_research import pipeline as research_pipeline
from chronos_research import walk_forward_study
from chronos_research.comparison import account_summary
from chronos_research.encoding import canonical_json
from chronos_research.pipeline import ArtifactRoots, run_strategy
from chronos_research.strategy_configuration import strategy_options
from chronos_research.walk_forward_study import (
    StudyError,
    cell_deltas,
    inspect_study,
    run_study,
    studies_root,
    study_identity,
)
from tests import pipeline as built
from tests.recordings import WEEK, Recordings, prepare
from tests.test_walk_forward_study import week_plan

REPLAYED = "ran the schedule again over its verified dataset: identical journal and results"
type Edit = Callable[[dict[str, Any]], None]


@pytest.fixture(scope="module")
def studied(
    tools: built.Tools, tmp_path_factory: pytest.TempPathFactory
) -> tuple[Recordings, dict[str, Any]]:
    """The synthetic week and its complete study, published once."""
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("inspection"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in WEEK:
        prepare(recordings, recording)
    study = run_study(recordings.tools, recordings.artifacts, week_plan(recordings)["plan_id"])["study"]
    assert study["status"] == "complete"
    return recordings, study


@pytest.fixture
def copy(studied: tuple[Recordings, dict[str, Any]], tmp_path: Path) -> tuple[Recordings, dict[str, Any]]:
    recordings, study = studied
    root = tmp_path / "artifacts"
    shutil.copytree(recordings.artifacts, root, symlinks=True)
    return Recordings(artifacts=root, tools=recordings.tools, prepared=dict(recordings.prepared)), study


def forge(study: dict[str, Any], edit: Edit) -> dict[str, Any]:
    """The study edited, then every figure derived from its cells recomputed
    and its identity renewed: only the edit itself is wrong."""
    forged: dict[str, Any] = json.loads(canonical_json(study))
    del forged["study_id"]
    edit(forged)
    for name in forged["models"]:
        for scenario in forged["scenarios"]:
            forged["aggregates"]["trading"][name][scenario["name"]] = walk_forward_study._trading_aggregate(
                forged["folds"], name, scenario["name"]
            )
    forged["missing"] = walk_forward_study._missing(forged["folds"])
    forged["outcomes"] = walk_forward_study._outcomes(forged["folds"])
    forged["status"] = "partial" if forged["missing"] else "complete"
    forged["study_id"] = study_identity(forged)
    return forged


def publish_forged(artifacts: Path, forged: dict[str, Any]) -> str:
    directory = studies_root(artifacts) / forged["study_id"]
    directory.mkdir()
    (directory / "manifest.json").write_bytes(canonical_json(forged))
    return str(forged["study_id"])


def cell_of(study: dict[str, Any], fold: int, name: str, scenario: str) -> dict[str, Any]:
    found: dict[str, Any] = next(
        cell for cell in study["folds"][fold - 1]["models"][name]["cells"] if cell["scenario"] == scenario
    )
    return found


def substituted(
    recordings: Recordings,
    study: dict[str, Any],
    *,
    schedule: tuple[int, str] = (2, "logistic"),
    changes: dict[str, str] | None = None,
) -> dict[str, Any]:
    """The study with fold 2's logistic base cell replaced by a genuine,
    replayable run of a schedule under some settings, and its figures and
    totals rewritten to match that run."""
    base = cell_of(study, 2, "logistic", "base")
    configuration = {
        **study["configuration"],
        "fee_per_fill": base["fee_per_fill"],
        "latency_ns": base["latency_ns"],
        **(changes or {}),
    }
    fold, name = schedule
    schedule_id = study["folds"][fold - 1]["models"][name]["schedule"]["artifact_id"]
    step, run = run_strategy(
        recordings.tools,
        ArtifactRoots.under(recordings.artifacts),
        schedule_id,
        strategy_options(configuration),
    )

    def edit(forged: dict[str, Any]) -> None:
        cell = cell_of(forged, 2, "logistic", "base")
        accounts = {
            account: account_summary(run["results"]["accounts"][account])
            for account in ("strategy", "buy_and_hold", "cash")
        }
        cell.update(
            accounts=accounts,
            deltas=cell_deltas(accounts, run["configuration"]["starting_cash"]),
            run={"artifact_id": step.artifact_id, "replay": REPLAYED},
            starting_cash=run["configuration"]["starting_cash"],
        )

    return forge(study, edit)


def refused(recordings: Recordings, forged: dict[str, Any]) -> StudyError:
    study_id = publish_forged(recordings.artifacts, forged)
    with pytest.raises(StudyError) as refusal:
        inspect_study(recordings.tools, recordings.artifacts, study_id)
    return refusal.value


def test_a_completed_cell_without_a_run_is_refused_however_consistent(
    copy: tuple[Recordings, dict[str, Any]],
) -> None:
    recordings, study = copy

    def without_run(forged: dict[str, Any]) -> None:
        cell = cell_of(forged, 2, "logistic", "base")
        del cell["run"]
        cell["accounts"]["strategy"]["valuation"]["net_total_pnl"] = "1234"

    forged = forge(study, without_run)
    assert forged["status"] == "complete"
    assert refused(recordings, forged).reason == "artifact_invalid"


def failure_with_figures(cell: dict[str, Any]) -> None:
    del cell["run"]
    cell.update(detail="refused", reason="run_failed", state="failed")


def runless(cell: dict[str, Any], **fields: str) -> None:
    cell.clear()
    cell.update(
        {"detail": "refused", "fee_per_fill": "0.01", "latency_ns": "1000", "scenario": "base", **fields}
    )


@pytest.mark.parametrize(
    "shape",
    [
        failure_with_figures,
        lambda cell: runless(cell, reason="run_failed", state="failed", fee_per_fill="9"),
        lambda cell: runless(cell, reason="replay_failed", state="failed"),
        # Not run, as if its model were unevaluable, though the model was fitted.
        lambda cell: runless(cell, reason="model_unevaluable", state="unavailable"),
    ],
    ids=["failure-with-figures", "failure-under-another-cost", "failure-that-needs-a-run", "not-a-failure"],
)
def test_a_runless_cell_is_kept_only_as_exactly_a_run_failure(
    copy: tuple[Recordings, dict[str, Any]], shape: Callable[[dict[str, Any]], None]
) -> None:
    recordings, study = copy
    forged = forge(study, lambda forged: shape(cell_of(forged, 2, "logistic", "base")))
    assert refused(recordings, forged).reason == "artifact_invalid"


@pytest.mark.parametrize(
    "changes",
    [
        {"threshold_ppb": "1000000000"},
        {"target_quantity": "2"},
        {"decision_interval_ns": "2000000"},
        {"starting_cash": "20000"},
        {"maximum_quote_age_ns": "4000000000"},
        {"maximum_order_notional": "999999"},
        {"fee_per_fill": "0.02"},
        {"latency_ns": "2000"},
    ],
    ids=lambda changes: next(iter(changes)),
)
def test_a_run_under_any_setting_but_the_plans_is_refused(
    copy: tuple[Recordings, dict[str, Any]], changes: dict[str, str]
) -> None:
    recordings, study = copy
    assert refused(recordings, substituted(recordings, study, changes=changes)).reason == "not_reproduced"


@pytest.mark.parametrize("schedule", [(2, "majority"), (1, "logistic")], ids=["other-model", "other-fold"])
def test_a_run_of_another_schedule_is_refused(
    copy: tuple[Recordings, dict[str, Any]], schedule: tuple[int, str]
) -> None:
    recordings, study = copy
    assert refused(recordings, substituted(recordings, study, schedule=schedule)).reason == "not_reproduced"


RUN_FIELDS = [
    *(("source", key) for key in (
        "collection_id", "dataset_id", "events_sha256", "feature_artifact_id", "fold", "fold_model_id",
        "information_boundary_ns", "instrument", "model", "parameters_sha256", "plan_id", "schedule_id",
        "schedule_sha256", "stream", "training_labels_available_through_ns",
        "training_predictions_through_ns",
    )),
    *(("window", key) for key in ("end_at_ns", "name", "start_at_ns")),
    *(("schema", key) for key in ("name", "version")),
]  # fmt: skip


@pytest.mark.parametrize(("part", "key"), RUN_FIELDS, ids=lambda value: str(value))
def test_a_run_naming_anything_but_its_cells_source_window_and_schema_is_refused(
    studied: tuple[Recordings, dict[str, Any]], monkeypatch: pytest.MonkeyPatch, part: str, key: str
) -> None:
    """A run the engine replays, but whose manifest names another source,
    window, or schema in one field, cannot stand for its cell."""
    recordings, study = studied
    target = cell_of(study, 2, "logistic", "base")["run"]["artifact_id"]
    replayed = research_pipeline.inspect_strategy

    def replay(*arguments: Any) -> dict[str, Any]:
        result = replayed(*arguments)
        if arguments[2] == target:
            result["artifact"][part][key] = f"{result['artifact'][part][key]}0"
        return result

    monkeypatch.setattr(walk_forward_study, "inspect_strategy", replay)
    with pytest.raises(StudyError) as refusal:
        inspect_study(recordings.tools, recordings.artifacts, study["study_id"])
    assert refusal.value.reason == "not_reproduced"


def test_the_genuine_run_rewritten_the_same_way_is_the_study_itself(
    copy: tuple[Recordings, dict[str, Any]],
) -> None:
    """The control for the refusals above: substituting the plan's own run
    under the plan's own settings rewrites nothing."""
    recordings, study = copy
    assert substituted(recordings, study) == study
    assert inspect_study(recordings.tools, recordings.artifacts, study["study_id"])["study"] == study
