"""Gate C: the offline chain from prepared recordings to frozen studies and
independent audits, on deterministic synthetic sessions (labelled synthetic
throughout), and its honesty about what is missing.

The real-recording check is opt-in (CHRONOS_REAL_RECORDING, the licensed
recording's path, and CHRONOS_DATABENTO_TOOLS, a Databento-enabled build):
CI has neither, and it must never be faked with synthetic bytes.
"""

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

from chronos_research import pipeline as research_pipeline
from chronos_research.recording_pilot import (
    LIMITATIONS,
    PreparedRecording,
    evaluate_collection,
    prepare_plan,
    render_markdown,
)
from chronos_research.recording_plan import instant_ns, validated_evaluation
from tests import fake_databento
from tests import pipeline as built
from tests.recording_plans import plan, recording
from tests.recordings import HORIZON_NS, WEEK, Recording, Recordings, prepare
from tests.test_walk_forward_plan import CONFIGURATION

PYTHON_ROOT = Path(__file__).resolve().parents[1]
SESSIONS = (
    Recording("synthetic-2026-09-18-thin", day=-3, quotes=24, seed=31),
    *WEEK,
    Recording("synthetic-2026-09-24", day=3, quotes=440, seed=14, up_bias=0.35),
    Recording("synthetic-2026-09-25", day=4, quotes=380, seed=15, up_bias=0.42),
)
EVALUATION = validated_evaluation(
    {
        "configuration": dict(CONFIGURATION),
        "horizon_ns": str(HORIZON_NS),
        "instruments": ["AAPL", "SPY"],
        "minimum_training_sources": "1",
        "scenarios": [
            {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
            {"fee_per_fill": "0.01", "latency_ns": "200000", "name": "slow"},
            {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"},
        ],
    },
    ["AAPL", "SPY"],
    len(SESSIONS),
)


def prepared_sessions(recordings: Recordings) -> list[PreparedRecording]:
    prepared = []
    for session in SESSIONS:
        made = prepare(recordings, session, HORIZON_NS)
        label = f"{session.stream} (synthetic, declared)"
        prepared.append(
            PreparedRecording(session.stream, label, made.dataset_id, made.feature_id, made.label_id)
        )
    return prepared


@pytest.fixture(scope="module")
def pilot(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> tuple[Recordings, dict[str, Any]]:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("pilot"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    report = evaluate_collection(
        recordings.tools, recordings.artifacts, prepared_sessions(recordings), EVALUATION
    )
    return recordings, report


def test_synthetic_sessions_pass_the_whole_chain_and_the_independent_audit(
    pilot: tuple[Recordings, dict[str, Any]],
) -> None:
    _, report = pilot
    assert report["collection"]["evidence"] == "synthetic"
    assert report["collection"]["members"] == str(len(SESSIONS))
    assert [study["instrument"] for study in report["studies"]] == ["AAPL", "SPY"]
    for study in report["studies"]:
        assert study["audit"]["verdict"] == "pass", study["audit"]
        assert int(study["audit"]["checks_total"]) > 1_000
        assert len(study["folds"]) == len(SESSIONS) - 1
        assert all(
            len(model["cells"]) == 3 for fold in study["folds"][1:] for model in fold["models"].values()
        )


def test_what_could_not_be_evaluated_keeps_its_reason_everywhere(
    pilot: tuple[Recordings, dict[str, Any]],
) -> None:
    _, report = pilot
    for study in report["studies"]:
        # The thin first session cannot train a logistic model for fold 1.
        assert study["status"] == "partial"
        assert {"fold": "1", "item": "model:logistic", "reason": "insufficient_class_support"} in study[
            "missing"
        ]
        logistic = study["folds"][0]["models"]["logistic"]
        assert (logistic["state"], logistic["reason"]) == ("unevaluable", "insufficient_class_support")
        for scenario in ("base", "slow", "stress"):
            aggregate = study["aggregates"]["trading"]["logistic"][scenario]
            assert aggregate["complete"]["unavailable"] == "folds_missing"
            assert aggregate["complete"]["missing"] == [
                {"fold": "1", "reason": "cell_unavailable:model_unevaluable"}
            ]
            assert aggregate["folds"]["included"] == ["2", "3", "4", "5"]
            assert "net_total_pnl" in aggregate["partial"]["accounts"]["strategy"]
        majority = study["aggregates"]["trading"]["majority"]["base"]["complete"]
        assert "starting_capital" in majority
        rows = study["folds"][1]["models"]["majority"]["predictive"]["rows"]
        assert int(rows["scored"]) + sum(int(count) for count in rows["not_scored"].values()) == int(
            rows["scheduled"]
        )


def test_running_the_chain_again_reuses_every_artifact(pilot: tuple[Recordings, dict[str, Any]]) -> None:
    recordings, report = pilot
    again = evaluate_collection(
        recordings.tools, recordings.artifacts, prepared_sessions(recordings), EVALUATION
    )
    assert again == report


def test_the_readers_report_shows_evidence_and_reasons(pilot: tuple[Recordings, dict[str, Any]]) -> None:
    recordings, report = pilot
    prepared = prepared_sessions(recordings)
    document = {
        **report,
        "limitations": LIMITATIONS,
        "pending": [{"reason": "not acquired: an example", "stream_id": "later-session"}],
        "plan_id": None,
        "recordings": [{**session.as_json(), "origin": "synthetic generator"} for session in prepared],
    }
    markdown = render_markdown(document)
    assert "Evidence: **synthetic**" in markdown
    assert "unevaluable: insufficient_class_support" in markdown
    assert "complete total unavailable (fold 1: cell_unavailable:model_unevaluable)" in markdown
    assert "**pending**: not acquired: an example" in markdown
    assert "independent audit **pass**" in markdown
    assert "not SIP, not the NBBO" in markdown
    assert "# Recording pilot report: synthetic fixture, no request plan" in markdown
    assert "| none: synthetic_recording |" in markdown


def test_recordings_not_yet_acquired_are_pending_and_nothing_is_evaluated(tmp_path: Path) -> None:
    tools = fake_databento.install(tmp_path / "tools")
    report = prepare_plan(research_pipeline.Tools(tools), plan(), tmp_path / "artifacts", None)
    assert [entry["reason"] for entry in report["pending"]] == ["not acquired: no state root was given"] * 3
    assert report["collection"] == {"unavailable": "no_recordings"}
    assert report["studies"] == []
    state = tmp_path / "state"
    state.mkdir()
    report = prepare_plan(research_pipeline.Tools(tools), plan(), tmp_path / "artifacts", state)
    assert {entry["reason"] for entry in report["pending"]} == {
        "not acquired: the state root holds no authorization for this plan"
    }
    assert fake_databento.invocations(tools, "download") == []


def test_a_recording_that_fails_verification_is_pending_with_the_refusal(tmp_path: Path) -> None:
    tools = fake_databento.install(tmp_path / "tools", {"replay": {"mode": "mismatch"}})
    document = plan()
    entry = document["recordings"][0]
    path = tmp_path / "existing.dbn.zst"
    window = instant_ns(entry["start"]), instant_ns(entry["end"])
    sha256 = fake_databento.write_recording(path, *window, entry["stream_id"], ["AAPL", "SPY"])
    del entry["destination"]
    entry["existing"] = {"path": str(path), "sha256": sha256, "size_bytes": str(path.stat().st_size)}
    report = prepare_plan(research_pipeline.Tools(tools), document, tmp_path / "artifacts", None)
    assert report["pending"][0]["reason"].startswith("refused (recording_mismatch): ")
    assert report["recordings"] == []


def test_the_report_never_replaces_a_file(tmp_path: Path) -> None:
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan()))
    existing = tmp_path / "report.md"
    existing.write_text("keep me")
    completed = subprocess.run(
        [sys.executable, "-m", "chronos_research", "prepare-recordings", "--plan", str(plan_file), "--tools",
         str(tmp_path), "--artifacts", str(tmp_path / "artifacts"), "--markdown", str(existing)],
        cwd=PYTHON_ROOT, capture_output=True, text=True, check=False,
    )  # fmt: skip
    assert completed.returncode == 1
    assert "never replaces a file" in completed.stderr
    assert existing.read_text() == "keep me"


REAL_RECORDING = os.environ.get("CHRONOS_REAL_RECORDING")
DATABENTO_TOOLS = os.environ.get("CHRONOS_DATABENTO_TOOLS")


@pytest.mark.skipif(
    not (REAL_RECORDING and DATABENTO_TOOLS),
    reason="opt-in: set CHRONOS_REAL_RECORDING and CHRONOS_DATABENTO_TOOLS to check the licensed recording",
)
def test_the_real_recording_is_verified_offline_and_refused_as_one_session(tmp_path: Path) -> None:
    assert REAL_RECORDING is not None
    assert DATABENTO_TOOLS is not None
    path = Path(REAL_RECORDING)
    real = recording(
        25,
        stream_id="databento-equs-mini-mbp1-20260925-1330-1340",
        start="2026-09-25T13:30:00Z",
        end="2026-09-25T13:40:00Z",
    )
    del real["destination"]
    real["existing"] = {
        "path": str(path),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "size_bytes": str(path.stat().st_size),
    }
    document = plan(recordings=[recording(24), real])
    report = prepare_plan(
        research_pipeline.Tools(Path(DATABENTO_TOOLS)), document, tmp_path / "artifacts", None
    )
    assert [entry["origin"] for entry in report["recordings"]] == ["existing"]
    assert report["pending"][0]["stream_id"] == recording(24)["stream_id"]
    assert report["collection"]["evidence"] == "observed"
    assert [study["refused"]["reason"] for study in report["studies"]] == ["insufficient_coverage"] * 2
    dataset = report["recordings"][0]["dataset_id"]
    manifest = json.loads((tmp_path / "artifacts" / "normalized" / dataset / "manifest.json").read_bytes())
    assert "ts_recv" in manifest["source"]["coverage"]["semantics"]
