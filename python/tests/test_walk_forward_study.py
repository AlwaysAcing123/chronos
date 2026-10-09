"""Walk-forward studies: every planned fold, model, and scenario cell run
through the C++ engine in independent accounts, outcomes that never vanish,
pooled classification, exact reset-account aggregation, and inspection that
rebuilds and replays everything."""

import json
import shutil
from decimal import Decimal
from pathlib import Path
from typing import Any

import pytest

from chronos_research import cli, walk_forward_study
from chronos_research import pipeline as research_pipeline
from chronos_research.collection import create_collection
from chronos_research.encoding import canonical_json
from chronos_research.pipeline import ArtifactRoots, PipelineError
from chronos_research.scoring import ScoredRows, pooled, scores
from chronos_research.walk_forward import create_plan
from chronos_research.walk_forward_study import (
    StudyError,
    inspect_study,
    money_raw,
    money_text,
    percentage_of,
    run_study,
    studies_root,
    study_identity,
    verified_study,
)
from tests import pipeline as built
from tests.recordings import WEEK, Recording, Recordings, prepare
from tests.test_walk_forward_plan import request

# A threshold at which the fold 2 logistic model trades often, so fills,
# fees, and latency all matter; chosen for coverage, not performance.
THRESHOLD = "400000000"


@pytest.fixture(scope="module")
def week(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Recordings:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("studies"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in WEEK:
        prepare(recordings, recording)
    prepare(recordings, Recording("synthetic-2026-09-19-thin", day=-2, quotes=24, seed=22))
    return recordings


@pytest.fixture
def copy(week: Recordings, tmp_path: Path) -> Recordings:
    root = tmp_path / "artifacts"
    shutil.copytree(week.artifacts, root, symlinks=True)
    return Recordings(artifacts=root, tools=week.tools, prepared=dict(week.prepared))


def plan_of(recordings: Recordings, *streams: str) -> dict[str, Any]:
    created = create_collection(recordings.tools, recordings.artifacts, recordings.label_ids(*streams))
    document = request(created["collection"]["collection_id"])
    document["configuration"]["threshold_ppb"] = THRESHOLD
    plan: dict[str, Any] = create_plan(recordings.tools, recordings.artifacts, document)["plan"]
    return plan


def week_plan(recordings: Recordings) -> dict[str, Any]:
    return plan_of(recordings, *(recording.stream for recording in WEEK))


def cells(study: dict[str, Any]) -> list[tuple[str, str, dict[str, Any]]]:
    return [
        (fold["index"], name, cell)
        for fold in study["folds"]
        for name, model in fold["models"].items()
        for cell in model["cells"]
    ]


def run_manifest(recordings: Recordings, run_id: str) -> dict[str, Any]:
    path = ArtifactRoots.under(recordings.artifacts).strategies / run_id / "manifest.json"
    manifest: dict[str, Any] = json.loads(path.read_text())
    return manifest


def journal(recordings: Recordings, run_id: str) -> list[dict[str, Any]]:
    path = ArtifactRoots.under(recordings.artifacts).strategies / run_id / "journal.ndjson"
    return [json.loads(line) for line in path.read_text().splitlines()]


# --- Exact money, by hand ---------------------------------------------------------------------------------


def test_money_is_the_engines_fixed_point_exactly() -> None:
    assert money_raw("-0.77") == -77_000_000
    assert money_raw("10000") == 1_000_000_000_000
    assert money_raw("0.00000001") == 1
    assert money_text(-27_000_000) == "-0.27"
    assert money_text(0) == "0"
    assert money_text(2_000_000_000_000) == "20000"
    for text in ("0.000000001", "1e3", "+1", "01", "-", "0.10", ""):
        with pytest.raises(StudyError):
            money_raw(text)
    with pytest.raises(StudyError) as overflow:
        money_text(2**63)
    assert overflow.value.reason == "arithmetic_overflow"


def test_a_return_is_rounded_once_half_away_from_zero() -> None:
    capital = money_raw("30000")
    # 1/30000 of capital is 0.003333...%: rounded at 8 places.
    assert percentage_of(money_raw("1"), capital) == "0.00333333"
    assert percentage_of(money_raw("-1"), capital) == "-0.00333333"
    # Exactly half a unit in the last place rounds away from zero.
    assert percentage_of(money_raw("0.00000001"), money_raw("200")) == "0.00000001"
    assert percentage_of(money_raw("-0.00000001"), money_raw("200")) == "-0.00000001"
    assert percentage_of(money_raw("0.00000001"), money_raw("200.00000001")) == "0"
    with pytest.raises(StudyError):
        percentage_of(1, 0)


def account(pnl: str, fees: str, turnover: str, fills: str = "1", valued: bool = True) -> dict[str, Any]:
    return {
        "drawdown": {"maximum": "0", "maximum_percent_of_peak": None, "points_unvalued": "0"},
        "fills": fills,
        "orders": {"expired": "0", "filled": fills, "rejected": "0", "scheduled": fills},
        "turnover": {"notional": turnover, "percent_of_starting_cash": "0"},
        "valuation": (
            {"equity": "0", "fees_paid": fees, "net_total_pnl": pnl, "return_percent": "0"}
            if valued
            else {"unavailable": {"code": "missing_mark", "instruments": [], "message": "no bid"}}
        ),
    }


def cell(strategy: dict[str, Any], state: str = "completed") -> dict[str, Any]:
    if state != "completed":
        return {"reason": "run_failed", "scenario": "base", "state": state}
    return {
        "accounts": {
            "buy_and_hold": account("-0.4", "0.01", "100"),
            "cash": account("0", "0", "0", fills="0"),
            "strategy": strategy,
        },
        "scenario": "base",
        "starting_cash": "10000",
        "state": "completed",
    }


def folds_of(*fold_cells: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        {"index": str(index), "models": {"logistic": {"cells": [value]}}}
        for index, value in enumerate(fold_cells, start=1)
    ]


def test_independent_reset_accounts_sum_exactly_and_derive_one_return() -> None:
    folds = folds_of(
        cell(account("-0.77", "0.01", "99.99")),
        cell(account("0.5", "0.37", "3700.12", fills="37")),
        cell(account("1.04", "0.02", "200.5", fills="2")),
    )
    aggregate = walk_forward_study._trading_aggregate(folds, "logistic", "base")
    complete = aggregate["complete"]
    strategy = complete["accounts"]["strategy"]
    # By hand: -0.77 + 0.5 + 1.04 = 0.77 over 3 x 10000 = 30000 of capital.
    assert complete["starting_capital"] == "30000"
    assert strategy["net_total_pnl"] == "0.77"
    assert strategy["fees_paid"] == "0.4"
    assert strategy["turnover"] == "4000.61"
    assert strategy["fills"] == "40"
    # 0.77 / 30000 = 0.0025666...%, rounded once.
    assert strategy["return_percent"] == "0.00256667"
    # Not the sum of the folds' own returns, nor their average.
    assert complete["deltas"]["buy_and_hold"] == {"net_total_pnl": "1.97", "return_percent": "0.00656667"}
    assert aggregate["folds"] == {"included": ["1", "2", "3"], "planned": "3"}


def test_a_missing_valuation_or_cell_leaves_only_a_labelled_partial_aggregate() -> None:
    folds = folds_of(
        cell(account("-0.77", "0.01", "99.99")),
        cell(account("0.5", "0.37", "3700", valued=False)),
        cell({}, state="failed"),
    )
    aggregate = walk_forward_study._trading_aggregate(folds, "logistic", "base")
    assert aggregate["complete"] == {
        "missing": [
            {"fold": "2", "reason": "valuation_unavailable:strategy:missing_mark"},
            {"fold": "3", "reason": "cell_failed:run_failed"},
        ],
        "unavailable": "folds_missing",
    }
    assert aggregate["folds"] == {"included": ["1"], "planned": "3"}
    assert aggregate["partial"]["starting_capital"] == "10000"
    assert aggregate["partial"]["accounts"]["strategy"]["net_total_pnl"] == "-0.77"
    nothing = walk_forward_study._trading_aggregate(folds_of(cell({}, state="failed")), "logistic", "base")
    assert "partial" not in nothing
    assert nothing["complete"]["unavailable"] == "folds_missing"


def test_a_cells_benchmark_deltas_are_exact_and_say_why_they_are_missing() -> None:
    accounts = cell(account("-0.77", "0.01", "99.99"))["accounts"]
    deltas = walk_forward_study.cell_deltas(accounts, "10000")
    # -0.77 - (-0.4) = -0.37, and -0.37 / 10000 = -0.0037 %.
    assert deltas == {
        "buy_and_hold": {"net_total_pnl": "-0.37", "return_percent": "-0.0037"},
        "cash": {"net_total_pnl": "-0.77", "return_percent": "-0.0077"},
    }
    accounts["buy_and_hold"] = account("0", "0", "0", valued=False)
    assert walk_forward_study.cell_deltas(accounts, "10000")["buy_and_hold"] == {
        "unavailable": "valuation_unavailable:buy_and_hold:missing_mark"
    }


def test_classification_is_pooled_over_rows_never_averaged_over_folds() -> None:
    def rows(truth: list[int], predicted: list[int]) -> ScoredRows:
        probabilities = [[1.0 if index == p else 0.0 for index in range(3)] for p in predicted]
        return ScoredRows(truth, predicted, probabilities, len(truth) + 1, {"label_unavailable:x": 1})

    # Fold 1: 1 of 1 correct; fold 2: 1 of 4 correct.
    one, two = rows([2], [2]), rows([0, 0, 1, 2], [0, 2, 2, 0])
    result = scores(pooled([one, two]))
    assert result["metrics"]["accuracy"] == {"value": str(2 / 5)}
    assert (1.0 + 0.25) / 2 != 2 / 5
    assert result["rows"] == {"not_scored": {"label_unavailable:x": "2"}, "scheduled": "7", "scored": "5"}
    assert result["metrics"]["confusion_matrix"] == {
        "down": {"down": "1", "flat": "0", "up": "1"},
        "flat": {"down": "0", "flat": "0", "up": "1"},
        "up": {"down": "1", "flat": "0", "up": "1"},
    }


# --- Whole studies, through the engine --------------------------------------------------------------------


def test_every_cell_runs_in_its_own_accounts_through_the_engine(copy: Recordings) -> None:
    plan = week_plan(copy)
    result = run_study(copy.tools, copy.artifacts, plan["plan_id"])
    study = result["study"]
    assert result["outcome"] == "published"
    assert study["status"] == "complete"
    assert study["missing"] == []
    assert study["outcomes"] == {"cells": {"completed": "8"}, "models": {"completed": "4"}}
    assert study["collection"]["evidence"] == "synthetic"
    every = cells(study)
    assert len({value["run"]["artifact_id"] for _, _, value in every}) == 8
    for fold, name, value in every:
        manifest = run_manifest(copy, value["run"]["artifact_id"])
        schedule_id = study["folds"][int(fold) - 1]["models"][name]["schedule"]["artifact_id"]
        assert manifest["source"]["schedule_id"] == schedule_id
        assert manifest["source"]["fold"] == fold
        assert manifest["configuration"]["execution"]["fee_per_fill"] == value["fee_per_fill"]
        assert manifest["configuration"]["execution"]["latency_ns"] == value["latency_ns"]
        assert value["run"]["replay"].startswith("ran the schedule again")
        entries = journal(copy, value["run"]["artifact_id"])
        # Independent accounts: each starts with the configured cash.
        first = next(entry for entry in entries if entry["type"] == "valuation")
        assert {name: state["cash"] for name, state in first["accounts"].items()} == {
            "buy_and_hold": "10000",
            "cash": "10000",
            "strategy": "10000",
        }
        # Each cell's deltas are its own accounts' exact differences.
        for benchmark in ("buy_and_hold", "cash"):
            difference = Decimal(value["accounts"]["strategy"]["valuation"]["net_total_pnl"]) - Decimal(
                value["accounts"][benchmark]["valuation"]["net_total_pnl"]
            )
            assert Decimal(value["deltas"][benchmark]["net_total_pnl"]) == difference
        # Costs and fills reconcile with the journal.
        fills = [
            e["fill"]
            for e in entries
            if e["type"] == "execution" and e["account"] == "strategy" and e["fill"]
        ]
        strategy = value["accounts"]["strategy"]
        assert strategy["fills"] == str(len(fills))
        assert all(fill["fee"] == value["fee_per_fill"] for fill in fills)
        assert Decimal(strategy["valuation"]["fees_paid"]) == sum(
            (Decimal(fill["fee"]) for fill in fills), Decimal(0)
        )
        # Latency: no fill on the signal's quote.
        for entry in entries:
            if entry["type"] == "execution" and entry["fill"]:
                assert int(entry["executed_at_ns"]) > int(entry["signal_at_ns"])
                assert int(entry["arrival_at_ns"]) - int(entry["signal_at_ns"]) == int(value["latency_ns"])
    # The stress cell ran the same schedule, and was executed, not derived.
    fold_two = {cell["scenario"]: cell for cell in study["folds"][1]["models"]["logistic"]["cells"]}
    assert fold_two["base"]["accounts"]["strategy"]["fills"] != "0"
    assert fold_two["base"]["run"]["artifact_id"] != fold_two["stress"]["run"]["artifact_id"]
    base_pnl = Decimal(fold_two["base"]["accounts"]["strategy"]["valuation"]["net_total_pnl"])
    stress_pnl = Decimal(fold_two["stress"]["accounts"]["strategy"]["valuation"]["net_total_pnl"])
    traded = int(fold_two["base"]["accounts"]["strategy"]["fills"])
    # Latency moved fills, so the stress PnL is not the base less extra fees.
    assert stress_pnl != base_pnl - traded * (Decimal("0.05") - Decimal("0.01"))


def test_aggregates_are_the_folds_own_numbers_summed_and_pooled(copy: Recordings) -> None:
    study = run_study(copy.tools, copy.artifacts, week_plan(copy)["plan_id"])["study"]
    for name in ("logistic", "majority"):
        for scenario in ("base", "stress"):
            folded = [
                next(c for c in fold["models"][name]["cells"] if c["scenario"] == scenario)
                for fold in study["folds"]
            ]
            complete = study["aggregates"]["trading"][name][scenario]["complete"]
            pnl = sum(
                (Decimal(c["accounts"]["strategy"]["valuation"]["net_total_pnl"]) for c in folded), Decimal(0)
            )
            assert Decimal(complete["accounts"]["strategy"]["net_total_pnl"]) == pnl
            assert complete["starting_capital"] == "20000"
            expected = (pnl / Decimal(20000) * 100).quantize(Decimal("0.00000001"))
            assert Decimal(complete["accounts"]["strategy"]["return_percent"]) == expected
        predictive = study["aggregates"]["predictive"][name]
        assert predictive["status"] == "complete"
        per_fold = [fold["models"][name]["predictive"] for fold in study["folds"]]
        pooled_matrix = predictive["pooled"]["metrics"]["confusion_matrix"]
        for truth in ("down", "flat", "up"):
            for predicted in ("down", "flat", "up"):
                assert int(pooled_matrix[truth][predicted]) == sum(
                    int(fold["metrics"]["confusion_matrix"][truth][predicted]) for fold in per_fold
                )
        correct = sum(int(pooled_matrix[c][c]) for c in ("down", "flat", "up"))
        assert predictive["pooled"]["metrics"]["accuracy"]["value"] == str(
            correct / int(predictive["pooled"]["rows"]["scored"])
        )


def test_a_study_reruns_to_the_same_bytes_and_inspection_reproduces_it(copy: Recordings) -> None:
    plan = week_plan(copy)
    first = run_study(copy.tools, copy.artifacts, plan["plan_id"])
    again = run_study(copy.tools, copy.artifacts, plan["plan_id"])
    assert again["outcome"] == "already_present"
    assert again["study"] == first["study"]
    inspected = inspect_study(copy.tools, copy.artifacts, first["study"]["study_id"])
    assert inspected["study"] == first["study"]
    assert inspected["reproduced"].endswith("identical study")


def rewrite(root: Path, study: dict[str, Any], edit: Any) -> str:
    """A tampered copy of the study, published under its own new ID."""
    changed = json.loads(canonical_json(study))
    edit(changed)
    del changed["study_id"]
    changed["study_id"] = study_identity(changed)
    directory = root / changed["study_id"]
    directory.mkdir()
    (directory / "manifest.json").write_bytes(canonical_json(changed))
    return str(changed["study_id"])


@pytest.mark.parametrize(
    "edit",
    [
        lambda s: s["aggregates"]["trading"]["logistic"]["base"]["complete"].update(starting_capital="10000"),
        lambda s: s["folds"][0]["models"]["logistic"]["cells"][0]["accounts"]["strategy"].update(fills="9"),
        lambda s: s["folds"][1]["models"]["majority"]["predictive"]["rows"].update(scored="1"),
        lambda s: (
            s.update(status="partial") or s["missing"].append({"fold": "1", "item": "x", "reason": "y"})
        ),
        lambda s: s["folds"][0]["models"]["logistic"]["cells"].reverse(),
    ],
)
def test_inspection_refuses_a_study_its_sources_do_not_give(copy: Recordings, edit: Any) -> None:
    study = run_study(copy.tools, copy.artifacts, week_plan(copy)["plan_id"])["study"]
    tampered = rewrite(studies_root(copy.artifacts), study, edit)
    assert tampered != study["study_id"]
    with pytest.raises(StudyError) as refused:
        inspect_study(copy.tools, copy.artifacts, tampered)
    assert refused.value.reason == "not_reproduced"


def test_a_tampered_study_file_is_refused(copy: Recordings) -> None:
    study = run_study(copy.tools, copy.artifacts, week_plan(copy)["plan_id"])["study"]
    path = studies_root(copy.artifacts) / study["study_id"] / "manifest.json"
    path.parent.chmod(0o755)
    path.chmod(0o644)
    path.write_text(path.read_text().replace('"complete"', '"partial"', 1))
    with pytest.raises(StudyError) as refused:
        verified_study(studies_root(copy.artifacts), study["study_id"])
    assert refused.value.reason == "artifact_invalid"


def test_an_unfit_model_leaves_its_cells_unavailable_and_the_study_partial(copy: Recordings) -> None:
    plan = plan_of(copy, "synthetic-2026-09-19-thin", WEEK[0].stream, WEEK[1].stream)
    study = run_study(copy.tools, copy.artifacts, plan["plan_id"])["study"]
    assert study["status"] == "partial"
    first = study["folds"][0]["models"]
    assert first["logistic"]["state"] == "unevaluable"
    assert first["logistic"]["reason"] == "insufficient_class_support"
    assert {c["state"] for c in first["logistic"]["cells"]} == {"unavailable"}
    assert first["majority"]["state"] == "completed"
    assert {"fold": "1", "item": "model:logistic", "reason": "insufficient_class_support"} in study["missing"]
    logistic = study["aggregates"]["predictive"]["logistic"]
    assert logistic["status"] == "partial"
    assert logistic["folds"] == {"included": ["2"], "planned": "2"}
    trading = study["aggregates"]["trading"]["logistic"]["base"]
    assert trading["complete"]["unavailable"] == "folds_missing"
    assert trading["complete"]["missing"] == [{"fold": "1", "reason": "cell_unavailable:model_unevaluable"}]
    assert trading["partial"]["starting_capital"] == "10000"
    assert "unavailable" not in study["aggregates"]["trading"]["majority"]["base"]["complete"]
    assert inspect_study(copy.tools, copy.artifacts, study["study_id"])["study"] == study


def test_a_failed_cell_is_kept_with_its_reason_and_not_executed_again_by_inspection(
    copy: Recordings, monkeypatch: pytest.MonkeyPatch
) -> None:
    plan = week_plan(copy)
    original = research_pipeline.run_strategy

    def failing(tools: Any, roots: Any, schedule_id: str, options: Any) -> Any:
        if options.latency_ns != 1000:
            raise PipelineError("chronos_strategy failed: the engine refused the run")
        return original(tools, roots, schedule_id, options)

    monkeypatch.setattr(walk_forward_study, "run_strategy", failing)
    study = run_study(copy.tools, copy.artifacts, plan["plan_id"])["study"]
    monkeypatch.undo()
    assert study["status"] == "partial"
    assert study["outcomes"]["cells"] == {"completed": "4", "failed": "4"}
    stress = study["aggregates"]["trading"]["logistic"]["stress"]
    assert stress["complete"]["unavailable"] == "folds_missing"
    assert "partial" not in stress
    assert "unavailable" not in study["aggregates"]["trading"]["logistic"]["base"]["complete"]
    assert inspect_study(copy.tools, copy.artifacts, study["study_id"])["study"] == study


def test_a_source_changed_after_planning_stops_the_study_before_anything_runs(copy: Recordings) -> None:
    plan = week_plan(copy)
    labels = copy.artifacts / "labels" / copy.prepared[WEEK[2].stream].label_id
    labels.chmod(0o755)
    rows = labels / "labels.ndjson"
    rows.chmod(0o644)
    rows.write_bytes(rows.read_bytes().replace(b'"up"', b'"down"', 1))
    with pytest.raises(StudyError) as refused:
        run_study(copy.tools, copy.artifacts, plan["plan_id"])
    assert refused.value.reason in ("source_invalid", "artifact_invalid")
    assert not studies_root(copy.artifacts).exists()
    assert not (ArtifactRoots.under(copy.artifacts).strategies).exists()


def test_the_cli_runs_lists_and_inspects_a_study_with_progress(
    copy: Recordings, capsys: pytest.CaptureFixture[str]
) -> None:
    plan = week_plan(copy)
    roots = ["--tools", str(copy.tools.directory), "--artifacts", str(copy.artifacts)]
    assert cli.main(["run-walk-forward", *roots, "--plan", plan["plan_id"], "--progress"]) == 0
    output = capsys.readouterr()
    study = json.loads(output.out)["study"]
    progress = [json.loads(line)["progress"] for line in output.err.splitlines()]
    assert progress[0] == {"stage": "verify_sources"}
    assert progress[-1] == {"stage": "publish"}
    assert {
        "fold": "2",
        "folds": "2",
        "model": "logistic",
        "scenario": "stress",
        "stage": "fold",
        "step": "run",
    } in progress
    assert cli.main(["list-walk-forward-studies", "--artifacts", str(copy.artifacts)]) == 0
    listed = json.loads(capsys.readouterr().out)
    assert listed["studies"] == [walk_forward_study.listing(study)]
    assert cli.main(["inspect-walk-forward-study", *roots, "--study", study["study_id"]]) == 0
    assert json.loads(capsys.readouterr().out)["study"] == study
    assert cli.main(["inspect-walk-forward-study", *roots, "--study", "f" * 64]) == 1
    assert json.loads(capsys.readouterr().err)["error"]["reason"] == "artifact_invalid"
