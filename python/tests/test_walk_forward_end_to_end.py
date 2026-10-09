"""The synthetic walk-forward path end to end, as CI runs it: independent
synthetic recordings collected, planned, run through the C++ engine, and
inspected, all through the CLI; then audited by tools/walk_forward_audit,
which uses the standard library alone (`python -I -S`) and nothing from
Chronos. No licensed data, credentials, or network."""

import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from chronos_research import pipeline as research_pipeline
from tests import pipeline as built
from tests.recordings import WEEK, Recording, Recordings, prepare
from tests.test_walk_forward_inspection import cell_of, forge, publish_forged, substituted
from tests.test_walk_forward_plan import request

AUDIT = Path(__file__).resolve().parents[2] / "tools" / "walk_forward_audit" / "audit.py"


@pytest.fixture(scope="module")
def week(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Recordings:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("end-to-end"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in WEEK:
        prepare(recordings, recording)
    prepare(recordings, Recording("synthetic-2026-09-19-thin", day=-2, quotes=24, seed=22))
    return recordings


def cli(recordings: Recordings, command: str, *arguments: str) -> dict[str, Any]:
    completed = subprocess.run(
        [sys.executable, "-m", "chronos_research", command, "--tools", str(recordings.tools.directory),
         "--artifacts", str(recordings.artifacts), *arguments],
        capture_output=True, text=True, check=False, cwd=Path(__file__).resolve().parents[1],
    )  # fmt: skip
    assert completed.returncode == 0, completed.stderr
    document: dict[str, Any] = json.loads(completed.stdout)
    return document


def audited(artifacts: Path, study_id: str) -> tuple[int, dict[str, Any]]:
    completed = subprocess.run(
        [sys.executable, "-I", "-S", str(AUDIT), str(artifacts), study_id],
        capture_output=True, text=True, check=False, cwd=artifacts,
    )  # fmt: skip
    report: dict[str, Any] = json.loads(completed.stdout)
    return completed.returncode, report


def study_of(recordings: Recordings, *streams: str, stress_latency_ns: str = "200000") -> dict[str, Any]:
    labels = [
        part for stream in streams for part in ("--label-artifact", recordings.prepared[stream].label_id)
    ]
    collection = cli(recordings, "create-collection", *labels)["collection"]["collection_id"]
    document = request(collection)
    document["configuration"]["threshold_ppb"] = "400000000"
    document["scenarios"][1]["latency_ns"] = stress_latency_ns
    plan = cli(recordings, "plan-walk-forward", "--request", json.dumps(document))["plan"]["plan_id"]
    study: dict[str, Any] = cli(recordings, "run-walk-forward", "--plan", plan)["study"]
    inspected = cli(recordings, "inspect-walk-forward-study", "--study", study["study_id"])
    assert inspected["study"] == study
    return study


def test_a_synthetic_study_passes_the_independent_audit(week: Recordings, tmp_path: Path) -> None:
    study = study_of(week, *(recording.stream for recording in WEEK))
    assert study["status"] == "complete"
    returned, report = audited(week.artifacts, study["study_id"])
    assert (returned, report["verdict"], report["problems"]) == (0, "pass", [])
    assert report["not_audited"] == []
    assert report["checks"]["fill"] > 0
    assert report["checks"]["aggregate"] > 0
    # A planted error is found: the auditor recomputes, it does not trust.
    root = tmp_path / "planted"
    shutil.copytree(week.artifacts, root, symlinks=True)
    path = root / "walk_forward_studies" / study["study_id"] / "manifest.json"
    path.parent.chmod(0o755)
    path.chmod(0o644)
    planted = json.loads(path.read_text())
    cell = planted["folds"][1]["models"]["logistic"]["cells"][1]
    cell["deltas"]["buy_and_hold"]["net_total_pnl"] = "0.01"
    path.write_text(json.dumps(planted))
    returned, report = audited(root, study["study_id"])
    assert (returned, report["verdict"]) == (1, "fail")
    assert any("delta vs buy_and_hold pnl" in problem for problem in report["problems"])


def test_a_partial_study_is_audited_with_what_it_could_not_audit(week: Recordings) -> None:
    study = study_of(week, "synthetic-2026-09-19-thin", WEEK[0].stream, WEEK[1].stream)
    assert study["status"] == "partial"
    returned, report = audited(week.artifacts, study["study_id"])
    assert (returned, report["verdict"], report["problems"]) == (0, "pass", [])
    assert report["not_audited"] == ["fold 1: logistic unevaluable (insufficient_class_support)"]


def test_orders_a_latency_outlasts_expire_at_the_window_end_and_the_audit_agrees(week: Recordings) -> None:
    # Five seconds is longer than any of these recordings: no stress order
    # can reach an eligible quote before its window ends.
    study = study_of(week, *(recording.stream for recording in WEEK), stress_latency_ns="5000000000")
    for fold in study["folds"]:
        for model in fold["models"].values():
            stress = next(cell for cell in model["cells"] if cell["scenario"] == "stress")
            for account in ("strategy", "buy_and_hold"):
                figures = stress["accounts"][account]
                assert figures["fills"] == "0"
                assert figures["orders"]["expired"] == figures["orders"]["scheduled"]
            assert stress["accounts"]["buy_and_hold"]["orders"]["expired"] == "1"
            assert stress["accounts"]["strategy"]["valuation"]["net_total_pnl"] == "0"
    returned, report = audited(week.artifacts, study["study_id"])
    assert (returned, report["verdict"], report["problems"]) == (0, "pass", [])
    assert report["checks"]["order"] > 0


def test_the_auditor_holds_every_run_to_the_plans_settings_and_reports_a_missing_run(
    week: Recordings,
) -> None:
    """Rewritten studies whose figures and identity are consistent: the
    auditor must derive its expectations from the plan, not from the runs."""
    study = study_of(week, *(recording.stream for recording in WEEK))
    forged = substituted(week, study, changes={"threshold_ppb": "1000000000"})
    returned, report = audited(week.artifacts, publish_forged(week.artifacts, forged))
    assert (returned, report["verdict"]) == (1, "fail")
    assert any("threshold_ppb" in problem for problem in report["problems"])

    def without_run(rewritten: dict[str, Any]) -> None:
        del cell_of(rewritten, 2, "logistic", "base")["run"]

    returned, report = audited(week.artifacts, publish_forged(week.artifacts, forge(study, without_run)))
    assert (returned, report["verdict"]) == (1, "fail")
    assert "completed cell has no run" in " ".join(report["problems"])


@pytest.mark.parametrize(
    "key",
    [
        "events_sha256", "information_boundary_ns", "parameters_sha256", "schedule_sha256",
        "training_labels_available_through_ns", "training_predictions_through_ns",
    ],
)  # fmt: skip
def test_the_auditor_derives_every_source_field_of_a_run_itself(
    week: Recordings, tmp_path: Path, key: str
) -> None:
    """A run manifest naming another events file, schedule file, fold model
    parameters, boundary, or training cutoff is found by the auditor from the
    raw files and the plan, not taken from the run."""
    study = study_of(week, *(recording.stream for recording in WEEK))
    root = tmp_path / "artifacts"
    shutil.copytree(week.artifacts, root, symlinks=True)
    manifest = (
        root / "strategies" / cell_of(study, 2, "logistic", "base")["run"]["artifact_id"] / "manifest.json"
    )
    run = json.loads(manifest.read_bytes())
    run["source"][key] = f"{run['source'][key]}0"
    manifest.chmod(0o644)
    manifest.write_text(json.dumps(run))
    returned, report = audited(root, study["study_id"])
    assert (returned, report["verdict"]) == (1, "fail")
    assert any(f"source.{key}" in problem for problem in report["problems"])
