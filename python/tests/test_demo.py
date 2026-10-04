"""The saved demonstration, on synthetic events, through the real C++ tools."""

import json
from decimal import Decimal
from pathlib import Path
from typing import Any

import pytest

from chronos_research.cli import main
from chronos_research.demo import DemoSource, run_demo
from chronos_research.pipeline import ArtifactRoots, PipelineError, Tools, inspect_strategy
from tests import pipeline as built
from tests.synthetic import synthetic_events


def tool_directory(tools: built.Tools) -> Tools:
    directory = tools.chronos_dataset.parent
    assert tools.synthetic_dataset.parent == directory, "the demo reads every tool from one directory"
    return Tools(directory)


def demo(tools: built.Tools, base: Path, events: str) -> dict[str, Any]:
    (base / "events.csv").write_text(events)
    return run_demo(
        tool_directory(tools),
        ArtifactRoots.under(base / "data"),
        DemoSource(stream_id="synthetic-stream", synthetic_events=base / "events.csv"),
        horizon_ns=150_000,
    )


def test_the_demonstration_runs_verifies_and_reuses_its_artifacts(tools: built.Tools, tmp_path: Path) -> None:
    first = demo(tools, tmp_path, synthetic_events(seed=7))
    kinds = [step["kind"] for step in first["artifacts"]]
    # Then, per instrument with a fitted model, a schedule and a run for
    # each window.
    assert kinds == ["dataset", "features", "labels", "split", "experiment", *(["schedule", "strategy"] * 4)]
    assert {step["outcome"] for step in first["artifacts"]} == {"published"}
    assert first["models"]["AAPL"]["logistic"]["status"] == "fitted"
    assert "not trading profit" in str(first["limitations"])
    # A repeat verifies and reuses every artifact, and finds the same IDs.
    second = demo(tools, tmp_path, synthetic_events(seed=7))
    assert {step["outcome"] for step in second["artifacts"]} == {"already_present"}
    assert [step["artifact_id"] for step in second["artifacts"]] == [
        step["artifact_id"] for step in first["artifacts"]
    ]
    for step in first["artifacts"]:
        assert Path(step["directory"]).is_dir()
    runs = first["strategy_runs"]
    assert [(run["instrument"], run["window"]) for run in runs] == [
        ("AAPL", "validation"), ("AAPL", "test"), ("SPY", "validation"), ("SPY", "test"),
    ]  # fmt: skip
    for run in runs:
        assert set(run["accounts"]) == {"buy_and_hold", "cash", "strategy"}
        assert run["accounts"]["cash"]["net_total_pnl"] == "0"
        assert run["accounts"]["buy_and_hold"]["fills"] in ("0", "1")
    assert second["strategy_runs"] == runs


def test_a_strategy_run_from_a_python_schedule_reproduces_in_cpp(tools: built.Tools, tmp_path: Path) -> None:
    result = demo(tools, tmp_path, synthetic_events(seed=7))
    run = result["strategy_runs"][1]
    inspected = inspect_strategy(tool_directory(tools), ArtifactRoots.under(tmp_path / "data"), run["run_id"])
    assert inspected["reproduced"].startswith("ran the schedule again")
    manifest = inspected["artifact"]
    assert manifest["source"]["schedule_id"] == run["schedule_id"]
    assert manifest["window"]["name"] == "test"
    assert manifest["window"]["end_at_ns"] is None
    assert manifest["configuration"]["execution"]["fee_per_fill"] == "0.01"
    assert manifest["configuration"]["execution"]["latency_ns"] == "1000000"
    assert manifest["configuration"]["policy"]["threshold_ppb"] == "500000000"
    # Accounting identity: equity = cash + market value, net PnL = equity - starting cash.
    for account in manifest["results"]["accounts"].values():
        valuation = account["final"]["valuation"]
        if "unavailable" in valuation:
            continue
        assert Decimal(valuation["equity"]) == Decimal(valuation["cash"]) + Decimal(valuation["market_value"])
        assert Decimal(valuation["net_total_pnl"]) == Decimal(valuation["equity"]) - Decimal("10000")


def test_an_unevaluable_model_is_reported_with_its_reason(tools: built.Tools, tmp_path: Path) -> None:
    result = demo(tools, tmp_path, synthetic_events(seed=7, quotes_per_instrument=30))
    for instrument in ("AAPL", "SPY"):
        logistic = result["models"][instrument]["logistic"]
        assert logistic["status"] == "unevaluable"
        assert logistic["reason"] == "insufficient_class_support"
        assert set(logistic["detail"]) == {"down", "flat", "up"}
    # Nothing trades on a model that predicts nothing, and that is said.
    assert result["strategy_runs"] == [
        {
            "instrument": name,
            "model": "logistic",
            "not_run": "the logistic model is unevaluable (insufficient_class_support)",
        }
        for name in ("AAPL", "SPY")
    ]


def test_the_demo_command_prints_one_json_document(
    tools: built.Tools, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    (tmp_path / "events.csv").write_text(synthetic_events(seed=7))
    arguments = [
        "demo", "--tools", str(tool_directory(tools).directory), "--artifacts", str(tmp_path / "data"),
        "--synthetic-events", str(tmp_path / "events.csv"), "--stream-id", "synthetic-stream",
        "--horizon-ns", "150000",
    ]  # fmt: skip
    assert main(arguments) == 0
    printed = json.loads(capsys.readouterr().out)
    assert printed["artifacts"][-1]["kind"] == "strategy"
    # A missing tool directory is a refusal, not a crash.
    assert main([*arguments[:2], str(tmp_path / "no-tools"), *arguments[3:]]) == 1
    assert "is not built" in json.loads(capsys.readouterr().err)["error"]["message"]


def test_a_tool_outside_the_pipeline_cannot_be_named(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="not a Chronos pipeline tool"):
        Tools(tmp_path).path("sh")
    with pytest.raises(PipelineError, match="is not built"):
        Tools(tmp_path).path("chronos_dataset")
