"""Strategy diagnostics end to end (ADR 0024): synthetic studies run through
the C++ engine, then explained without refitting or rerunning anything,
published, reused, inspected, and refused when anything is tampered with."""

import json
import os
import shutil
from collections import Counter
from collections.abc import Iterator
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

import pytest

from chronos_research import cli, strategy_diagnostics, walk_forward_study
from chronos_research import decision_diagnostics as cohorts
from chronos_research import pipeline as research_pipeline
from chronos_research.collection import create_collection
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.pipeline import ArtifactRoots, PipelineError
from chronos_research.strategy_attribution import DiagnosticsError
from chronos_research.strategy_diagnostics import (
    build_diagnostics,
    diagnostics_root,
    generate_diagnostics,
    inspect_diagnostics,
    request_for,
    validated_request,
    verified_diagnostics,
)
from chronos_research.walk_forward import create_plan
from chronos_research.walk_forward_study import run_study
from tests import pipeline as built
from tests.recordings import WEEK, Recording, Recordings, prepare
from tests.test_walk_forward_plan import request

THIN = Recording("synthetic-2026-09-19-thin", day=-2, quotes=24, seed=22)
# Sub-cent prices, so notionals and marks round, and one third of a share
# displayed at the ask, so a one-share buy fills in part.
ROUNDING = tuple(
    Recording(f"synthetic-rounding-2026-10-0{day}", day=12 + day, quotes=300, seed=40 + day, up_bias=0.45,
              price_nanos=5, ask_size="0.33333333")
    for day in range(3)
)  # fmt: skip
# AAPL stops quoting six seconds before SPY does, so an open AAPL position
# cannot be marked at the window's end.
STALE = tuple(
    Recording(f"synthetic-stale-2026-10-1{day}", day=20 + day, quotes=200, seed=50 + day, up_bias=0.5,
              instruments=("SPY", "AAPL"), tail_quotes=60)
    for day in range(3)
)  # fmt: skip


@dataclass
class Studies:
    recordings: Recordings
    ids: dict[str, str]

    @property
    def artifacts(self) -> Path:
        return self.recordings.artifacts

    @property
    def tools(self) -> research_pipeline.Tools:
        return self.recordings.tools


def _study(recordings: Recordings, streams: list[str], threshold: str) -> str:
    created = create_collection(recordings.tools, recordings.artifacts, recordings.label_ids(*streams))
    document = request(created["collection"]["collection_id"])
    document["configuration"]["threshold_ppb"] = threshold
    plan = create_plan(recordings.tools, recordings.artifacts, document)["plan"]
    study: dict[str, Any] = run_study(recordings.tools, recordings.artifacts, plan["plan_id"])["study"]
    return str(study["study_id"])


@pytest.fixture(scope="module")
def studies(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Studies:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("diagnostics"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in (*WEEK, THIN, *ROUNDING, *STALE):
        prepare(recordings, recording)
    week = [recording.stream for recording in WEEK]
    ids = {
        "week": _study(recordings, week, "400000000"),
        "partial": _study(recordings, [THIN.stream, *week[:2]], "400000000"),
        "rounding": _study(recordings, [recording.stream for recording in ROUNDING], "0"),
        "stale": _study(recordings, [recording.stream for recording in STALE], "0"),
    }
    # The same plan as "week", with every stress run failing to start.
    original = research_pipeline.run_strategy

    def failing(tools: Any, roots: Any, schedule_id: str, options: Any) -> Any:
        if options.latency_ns != 1000:
            raise PipelineError("chronos_strategy failed: the engine refused the run")
        return original(tools, roots, schedule_id, options)

    with pytest.MonkeyPatch.context() as patch:
        patch.setattr(walk_forward_study, "run_strategy", failing)
        ids["failed"] = _study(recordings, week, "400000000")
    return Studies(recordings, ids)


@pytest.fixture
def copy(studies: Studies, tmp_path: Path) -> Studies:
    root = tmp_path / "artifacts"
    shutil.copytree(studies.artifacts, root, symlinks=True)
    return Studies(
        Recordings(artifacts=root, tools=studies.tools, prepared=dict(studies.recordings.prepared)),
        studies.ids,
    )


def study_manifest(studies: Studies, name: str) -> dict[str, Any]:
    return walk_forward_study.verified_study(
        walk_forward_study.studies_root(studies.artifacts), studies.ids[name]
    )


def runs(manifest: dict[str, Any]) -> Iterator[tuple[dict[str, Any], dict[str, Any]]]:
    for study in manifest["studies"]:
        for fold in study["folds"]:
            for run in fold["runs"]:
                yield study, run


def value(figure: dict[str, str]) -> Fraction:
    return Fraction(figure["value"])


def test_every_account_reconciles_with_the_engine_and_the_study(studies: Studies) -> None:
    built_once = build_diagnostics(studies.tools, studies.artifacts, request_for([studies.ids["week"]]))
    manifest = built_once.manifest
    study = study_manifest(studies, "week")
    cells = {
        (fold["index"], name, cell["scenario"]): cell
        for fold in study["folds"]
        for name, model in fold["models"].items()
        for cell in model["cells"]
    }
    fills = [json.loads(line) for line in built_once.files["fills.ndjson"].splitlines()]
    attributed = 0
    for _, run in runs(manifest):
        cell = cells[(run["fold"], run["model"], run["scenario"])]
        assert run["state"] == cell["state"] == "completed"
        assert run["run_id"] == cell["run"]["artifact_id"]
        for account, document in run["accounts"].items():
            assert document["reconciliation"] == {
                "booked_net_pnl": {"state": "reconciled"},
                "signal_reference": {"state": "reconciled"},
            }
            components = {name: value(figure) for name, figure in document["components"].items()}
            assert components["booked_net_pnl"] == Fraction(
                cell["accounts"][account]["valuation"]["net_total_pnl"]
            )
            assert components["booked_net_pnl"] == (
                components["midpoint_inventory_pnl"]
                - components["execution_spread_cost"]
                - components["fees"]
                - components["terminal_bid_markdown"]
                + components["rounding_adjustment"]
            )
            assert components["fees"] == Fraction(cell["accounts"][account]["valuation"]["fees_paid"])
            mine = [row for row in fills if (row["run_id"], row["account"]) == (run["run_id"], account)]
            assert len(mine) == int(document["fills"]) == int(cell["accounts"][account]["fills"])
            assert (
                sum(value(row["contributions"]["execution_spread_cost"]) for row in mine)
                == components["execution_spread_cost"]
            )
            attributed += 1
        # The strategy minus a benchmark, component by component.
        strategy, cash = run["accounts"]["strategy"]["components"], run["accounts"]["cash"]["components"]
        assert value(run["deltas"]["cash"]["fees"]) == value(strategy["fees"]) - value(cash["fees"])
    assert attributed == 8 * 3
    assert manifest["status"] == "complete"
    assert manifest["counts"]["fills"] == str(len(fills))


def test_scorable_predictions_are_exactly_the_studys_scores(studies: Studies) -> None:
    manifest = build_diagnostics(
        studies.tools, studies.artifacts, request_for([studies.ids["week"]])
    ).manifest
    study = study_manifest(studies, "week")
    section = manifest["studies"][0]
    for fold, stored in zip(section["folds"], study["folds"], strict=True):
        for name in ("majority", "logistic"):
            mine = fold["models"][name]
            assert mine["study_scores"] == "agree"
            cohort = mine["predictions"]["cohort"]
            predictive = stored["models"][name]["predictive"]
            assert cohort["scored"] == predictive["rows"]["scored"]
            assert cohort["confusion"] == predictive["metrics"]["confusion_matrix"]
            accuracy = cohort["accuracy"]
            assert float(Fraction(int(accuracy["numerator"]), int(accuracy["denominator"]))) == float(
                predictive["metrics"]["accuracy"]["value"]
            )
    # Pooled, never averaged: the pooled cohort is the folds' counts summed.
    pooled = section["pooled"]["predictions"]["logistic"]["cohort"]
    folds = [fold["models"]["logistic"]["predictions"]["cohort"] for fold in section["folds"]]
    assert int(pooled["scored"]) == sum(int(fold["scored"]) for fold in folds)
    assert int(pooled["correct"]) == sum(int(fold["correct"]) for fold in folds)


def test_decision_cohorts_nest_and_agree_with_the_journal(studies: Studies) -> None:
    manifest = build_diagnostics(
        studies.tools, studies.artifacts, request_for([studies.ids["week"]])
    ).manifest
    for _, run in runs(manifest):
        decisions = run["decisions"]
        sampled = decisions["cohorts"]["sampled_decisions"]
        scheduling = decisions["cohorts"]["order_scheduling_decisions"]
        filled = decisions["cohorts"]["filled_orders"]
        assert sampled["items"] == decisions["evaluated"]
        assert sampled["reasons"] == {
            key: count for key, count in decisions["journaled"].items() if count != "0"
        }
        assert scheduling["items"] == decisions["journaled"]["order_scheduled"]
        assert int(filled["items"]) <= int(scheduling["items"]) <= int(sampled["items"])
        orders = run["accounts"]["strategy"]["orders"]["outcomes"]
        assert int(filled["items"]) == int(orders["filled"]) + int(orders["partially_filled_and_cancelled"])
        assert sum(int(bucket["items"]) for bucket in sampled["confidence_bins"]) == int(sampled["items"])


def test_labels_never_reach_the_attribution(studies: Studies, monkeypatch: pytest.MonkeyPatch) -> None:
    """Every label made unavailable changes the cohorts' label statuses and
    nothing about any account, fill, or episode."""
    request_document = request_for([studies.ids["week"]])
    honest = build_diagnostics(studies.tools, studies.artifacts, request_document)
    original = cohorts.label_statuses

    def unlabelled(rows: Any, window: Any) -> Any:
        return {
            identity: status
            if status.status.startswith("feature_unavailable:")
            else cohorts.LabelStatus("label_unavailable:hidden")
            for identity, status in original(rows, window).items()
        }

    monkeypatch.setattr(cohorts, "label_statuses", unlabelled)
    monkeypatch.setattr(cohorts, "study_scores_agree", lambda *_: None)
    blind = build_diagnostics(studies.tools, studies.artifacts, request_document)
    assert blind.files["fills.ndjson"] == honest.files["fills.ndjson"]
    assert blind.files["episodes.ndjson"] == honest.files["episodes.ndjson"]
    for (_, mine), (_, theirs) in zip(runs(honest.manifest), runs(blind.manifest), strict=True):
        assert mine["accounts"] == theirs["accounts"]
        assert mine["deltas"] == theirs["deltas"]
        assert (
            mine["decisions"]["cohorts"] != theirs["decisions"]["cohorts"]
            or mine["decisions"]["evaluated"] == "0"
        )


def test_the_engines_partial_fills_and_rounding_are_attributed_exactly(studies: Studies) -> None:
    built_once = build_diagnostics(studies.tools, studies.artifacts, request_for([studies.ids["rounding"]]))
    fills = [json.loads(line) for line in built_once.files["fills.ndjson"].splitlines()]
    outcomes = Counter(row["outcome"] for row in fills)
    assert outcomes["partially_filled_and_cancelled"] > 0
    assert any(value(row["contributions"]["notional_rounding"]) != 0 for row in fills)
    rounding = [
        value(run["accounts"][account]["components"]["rounding_adjustment"])
        for _, run in runs(built_once.manifest)
        for account in ("strategy", "buy_and_hold")
        if "value" in run["accounts"][account]["components"]["rounding_adjustment"]
    ]
    assert any(item != 0 for item in rounding)
    for _, run in runs(built_once.manifest):
        for document in run["accounts"].values():
            assert document["reconciliation"]["booked_net_pnl"] == {"state": "reconciled"}
    # Every figure is a multiple of 5 x 10^-18, written in at most 18 places.
    for row in fills:
        for figure in row["contributions"].values():
            assert (Fraction(figure["value"]) * 2 * 10**17).denominator == 1


def test_an_unmarkable_final_position_leaves_its_figures_unavailable(studies: Studies) -> None:
    built_once = build_diagnostics(studies.tools, studies.artifacts, request_for([studies.ids["stale"]]))
    unavailable = 0
    for _, run in runs(built_once.manifest):
        holder = run["accounts"]["buy_and_hold"]
        if "unavailable" not in holder["booked"]["valuation"]:
            continue
        unavailable += 1
        reason = "final_valuation_unavailable:position_mark_unavailable"
        assert holder["components"]["booked_net_pnl"] == {"unavailable": reason}
        assert holder["components"]["terminal_bid_markdown"] == {"unavailable": reason}
        assert "value" in holder["components"]["execution_spread_cost"]
        assert holder["reconciliation"]["booked_net_pnl"] == {"unavailable": reason}
        assert run["deltas"]["buy_and_hold"]["booked_net_pnl"] == {"unavailable": reason}
    assert unavailable > 0
    section = built_once.manifest["studies"][0]
    pooled = section["pooled"]["trading"]["majority"]["base"]["accounts"]["buy_and_hold"]["booked_net_pnl"]
    assert pooled["unavailable"].startswith("fold_")


def test_a_partial_study_lists_every_missing_cell(studies: Studies) -> None:
    manifest = build_diagnostics(
        studies.tools, studies.artifacts, request_for([studies.ids["partial"], studies.ids["failed"]])
    ).manifest
    by_id = {section["study_id"]: section for section in manifest["studies"]}
    partial = by_id[studies.ids["partial"]]
    assert partial["status"] == "partial"
    assert partial["missing"] == [
        {"fold": "1", "item": "model:logistic", "reason": "model_unevaluable:insufficient_class_support"}
    ]
    unfit = [run for run in partial["folds"][0]["runs"] if run["model"] == "logistic"]
    assert [run["state"] for run in unfit] == ["unavailable", "unavailable"]
    assert partial["folds"][0]["models"]["logistic"] == {
        "reason": "model_unevaluable:insufficient_class_support",
        "state": "unavailable",
    }
    logistic = partial["pooled"]["trading"]["logistic"]["base"]
    assert logistic["status"] == "partial"
    assert logistic["folds"]["missing"] == ["1"]
    failed = by_id[studies.ids["failed"]]
    stress = failed["pooled"]["trading"]["logistic"]["stress"]
    assert stress["status"] == "unavailable"
    assert {item["reason"] for item in failed["missing"]} == {"cell_failed:run_failed"}
    assert manifest["status"] == "partial"
    across = manifest["across_studies"]
    assert across["trading"]["logistic"]["base"]["status"] == "partial"


def test_studies_pool_across_instruments_only_for_identical_scenarios(studies: Studies) -> None:
    manifest = build_diagnostics(
        studies.tools, studies.artifacts, request_for([studies.ids["week"], studies.ids["rounding"]])
    ).manifest
    across = manifest["across_studies"]
    assert [scenario["name"] for scenario in across["scenarios"]] == ["base", "stress"]
    pooled = across["trading"]["logistic"]["base"]["accounts"]["strategy"]["booked_net_pnl"]
    parts = [
        section["pooled"]["trading"]["logistic"]["base"]["accounts"]["strategy"]["booked_net_pnl"]
        for section in manifest["studies"]
    ]
    assert value(pooled) == sum(value(part) for part in parts)


def test_missing_evaluations_are_unavailable_never_zero(studies: Studies) -> None:
    manifest = build_diagnostics(
        studies.tools, studies.artifacts, request_for([studies.ids["partial"], studies.ids["failed"]])
    ).manifest
    by_id = {section["study_id"]: section for section in manifest["studies"]}
    # No stress run of the failed study completed: nothing was evaluated, so
    # no financial figure exists, though its counts are genuinely zero.
    stress = by_id[studies.ids["failed"]]["pooled"]["trading"]["logistic"]["stress"]
    assert (stress["status"], stress["folds"]["included"]) == ("unavailable", [])
    for account in stress["accounts"].values():
        assert set(map(json.dumps, account.values())) == {json.dumps({"unavailable": "no_completed_runs"})}
    for benchmark in stress["deltas"].values():
        assert set(map(json.dumps, benchmark.values())) == {json.dumps({"unavailable": "no_completed_runs"})}
    assert stress["cohorts"]["sampled_decisions"]["items"] == "0"
    # Partial coverage: the partial study's stress pool has some folds and
    # real figures; across studies the failed study is missing, not a zero.
    partial = by_id[studies.ids["partial"]]["pooled"]["trading"]["logistic"]["stress"]
    assert partial["status"] == "partial"
    assert partial["folds"]["included"]
    across = manifest["across_studies"]["trading"]["logistic"]["stress"]
    assert across["status"] == "partial"
    assert across["studies"] == {"included": [studies.ids["partial"]], "missing": [studies.ids["failed"]]}
    assert across["accounts"] == partial["accounts"]
    assert across["deltas"] == partial["deltas"]
    # A genuinely evaluated account that never trades is zero, not missing.
    base = by_id[studies.ids["failed"]]["pooled"]["trading"]["logistic"]["base"]
    assert base["status"] == "complete"
    assert base["accounts"]["cash"]["booked_net_pnl"] == {"value": "0"}
    assert base["accounts"]["cash"]["fees"] == {"value": "0"}
    predictions = manifest["across_studies"]["predictions"]["logistic"]
    assert predictions["status"] == "partial"
    assert predictions["studies"]["missing"] == []
    # An unavailable final valuation is unavailable for its own reason.
    stale = build_diagnostics(studies.tools, studies.artifacts, request_for([studies.ids["stale"]])).manifest
    held = stale["studies"][0]["pooled"]["trading"]["majority"]["base"]["accounts"]["buy_and_hold"]
    assert held["booked_net_pnl"]["unavailable"].endswith(
        ":final_valuation_unavailable:position_mark_unavailable"
    )


def test_future_returns_summarize_scored_labels_in_their_unit(studies: Studies) -> None:
    manifest = build_diagnostics(
        studies.tools, studies.artifacts, request_for([studies.ids["week"]])
    ).manifest
    section = manifest["studies"][0]
    cohorts_seen = 0
    for fold in section["folds"]:
        model = fold["models"]["logistic"]
        documents = [model["predictions"]["cohort"]]
        documents += [
            cohort
            for run in fold["runs"]
            if "decisions" in run
            for cohort in run["decisions"]["cohorts"].values()
        ]
        for cohort in documents:
            cohorts_seen += 1
            returns = cohort["future_returns"]
            assert returns["all"]["rows"] == cohort["scored"]
            assert returns["not_scored"] == str(int(cohort["items"]) - int(cohort["scored"]))
            for groups in (list(returns["by_predicted_class"].values()), returns["by_confidence_bin"]):
                assert sum(int(group["rows"]) for group in groups) == int(returns["all"]["rows"])
                assert sum(int(group["sum_raw"]) for group in groups) == int(returns["all"]["sum_raw"])
            if returns["all"]["rows"] != "0":
                mean = returns["all"]["mean_raw"]
                assert Fraction(int(mean["numerator"]), int(mean["denominator"])) == Fraction(
                    int(returns["all"]["sum_raw"]), int(returns["all"]["rows"])
                )
    assert cohorts_seen > 4
    pooled_returns = section["pooled"]["predictions"]["logistic"]["cohort"]["future_returns"]["all"]
    assert int(pooled_returns["sum_raw"]) == sum(
        int(fold["models"]["logistic"]["predictions"]["cohort"]["future_returns"]["all"]["sum_raw"])
        for fold in section["folds"]
    )
    assert manifest["definitions"]["future_returns"].startswith(
        "each cohort's scored items' label return_raw"
    )


def test_publication_is_deterministic_and_inspection_rebuilds_identical_bytes(copy: Studies) -> None:
    document = request_for([copy.ids["week"]])
    first = generate_diagnostics(copy.tools, copy.artifacts, document)
    again = generate_diagnostics(copy.tools, copy.artifacts, document)
    assert (first["outcome"], again["outcome"]) == ("published", "already_present")
    identifier = first["diagnostics"]["diagnostic_id"]
    assert again["diagnostics"]["diagnostic_id"] == identifier
    inspected = inspect_diagnostics(copy.tools, copy.artifacts, identifier)
    assert inspected["reproduced"].endswith("identical bytes")
    directory = diagnostics_root(copy.artifacts) / identifier
    assert sorted(os.listdir(directory)) == ["episodes.ndjson", "fills.ndjson", "manifest.json"]
    assert all(
        not os.access(directory / name, os.W_OK) or os.geteuid() == 0 for name in os.listdir(directory)
    )
    # Generating published one directory and wrote nothing else.
    assert sorted(os.listdir(diagnostics_root(copy.artifacts))) == [identifier]


def test_tampering_after_publication_symlinks_and_corruption_are_refused(copy: Studies) -> None:
    document = request_for([copy.ids["week"]])
    identifier = generate_diagnostics(copy.tools, copy.artifacts, document)["diagnostics"]["diagnostic_id"]
    root = diagnostics_root(copy.artifacts)
    directory = root / identifier
    directory.chmod(0o755)
    fills = directory / "fills.ndjson"
    fills.chmod(0o644)
    original = fills.read_bytes()
    fills.write_bytes(original.replace(b'"buy"', b'"sell"', 1))
    with pytest.raises(DiagnosticsError, match="checksum"):
        verified_diagnostics(root, identifier)
    with pytest.raises(DiagnosticsError) as republished:
        generate_diagnostics(copy.tools, copy.artifacts, document)
    assert republished.value.reason == "publication_failed"
    with pytest.raises(DiagnosticsError):
        inspect_diagnostics(copy.tools, copy.artifacts, identifier)
    fills.write_bytes(original)
    (directory / "extra.json").write_text("{}")
    with pytest.raises(DiagnosticsError, match="exactly"):
        verified_diagnostics(root, identifier)
    (directory / "extra.json").unlink()
    manifest = directory / "manifest.json"
    manifest.chmod(0o644)
    text = manifest.read_bytes()
    manifest.write_bytes(text.replace(b'"complete"', b'"partial"', 1))
    with pytest.raises(DiagnosticsError, match="identity"):
        verified_diagnostics(root, identifier)
    manifest.write_bytes(b" " + text)
    with pytest.raises(DiagnosticsError, match="canonical"):
        verified_diagnostics(root, identifier)
    manifest.write_bytes(text)
    verified_diagnostics(root, identifier)
    moved = copy.artifacts / "elsewhere"
    directory.rename(moved)
    directory.symlink_to(moved, target_is_directory=True)
    with pytest.raises(DiagnosticsError):
        verified_diagnostics(root, identifier)
    directory.unlink()
    moved.rename(directory)
    (moved_file := directory / "episodes.ndjson").chmod(0o644)
    target = copy.artifacts / "episodes-copy.ndjson"
    shutil.copyfile(moved_file, target)
    moved_file.unlink()
    moved_file.symlink_to(target)
    with pytest.raises(DiagnosticsError):
        verified_diagnostics(root, identifier)


def test_an_artifact_of_another_schema_version_is_refused_by_name(copy: Studies) -> None:
    published = generate_diagnostics(copy.tools, copy.artifacts, request_for([copy.ids["week"]]))
    identifier = published["diagnostics"]["diagnostic_id"]
    root = diagnostics_root(copy.artifacts)
    manifest = json.loads((root / identifier / "manifest.json").read_bytes())
    # The same artifact under version 1, with an identity that matches it.
    manifest["schema"] = {"name": "chronos.strategy_diagnostics", "version": "1"}
    body = {key: value for key, value in manifest.items() if key != "diagnostic_id"}
    manifest["diagnostic_id"] = sha256_hex(canonical_json(body))
    target = root / manifest["diagnostic_id"]
    target.mkdir()
    for name in ("fills.ndjson", "episodes.ndjson"):
        (target / name).write_bytes((root / identifier / name).read_bytes())
    (target / "manifest.json").write_bytes(canonical_json(manifest))
    with pytest.raises(DiagnosticsError, match="version '1', which this version does not read"):
        verified_diagnostics(root, manifest["diagnostic_id"])


@pytest.mark.parametrize("target", ["journal", "schedule", "events", "cell"])
def test_sources_that_no_longer_verify_stop_the_diagnostics(copy: Studies, target: str) -> None:
    roots = ArtifactRoots.under(copy.artifacts)
    study = study_manifest(copy, "week")
    cell = study["folds"][1]["models"]["logistic"]["cells"][0]
    if target == "journal":
        path = roots.strategies / cell["run"]["artifact_id"] / "journal.ndjson"
    elif target == "schedule":
        path = (
            roots.schedules
            / study["folds"][1]["models"]["logistic"]["schedule"]["artifact_id"]
            / "schedule.ndjson"
        )
    elif target == "events":
        path = roots.catalog / study["folds"][1]["evaluation"]["dataset_id"] / "events.ndjson"
    if target == "cell":
        # A study naming another cell's run, consistent in itself.
        other = study["folds"][1]["models"]["logistic"]["cells"][1]["run"]["artifact_id"]
        cell["run"]["artifact_id"] = other
        stored = {key: value for key, value in study.items() if key != "study_id"}
        rewritten = {**stored, "study_id": walk_forward_study.study_identity(stored)}
        root = walk_forward_study.studies_root(copy.artifacts)
        (root / rewritten["study_id"]).mkdir()
        (root / rewritten["study_id"] / "manifest.json").write_bytes(
            json.dumps(rewritten, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode() + b"\n"
        )
        identifier = rewritten["study_id"]
    else:
        path.parent.chmod(0o755)
        path.chmod(0o644)
        path.write_bytes(path.read_bytes().replace(b"1", b"2", 1))
        identifier = copy.ids["week"]
    with pytest.raises(DiagnosticsError) as refused:
        build_diagnostics(copy.tools, copy.artifacts, request_for([identifier]))
    assert refused.value.reason == "source_invalid"
    assert not diagnostics_root(copy.artifacts).exists()


def test_requests_are_strict() -> None:
    good = "a" * 64
    assert validated_request({"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": [good]})[
        "study_ids"
    ] == [good]
    for document in (
        {"study_ids": [good]},
        {"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": []},
        {"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": [good, good]},
        {"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": ["b" * 64, good]},
        {"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": ["c" * 64] * 5},
        {"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": ["../etc"]},
        {"schema": strategy_diagnostics.REQUEST_SCHEMA, "study_ids": [good], "extra": "1"},
    ):
        with pytest.raises(DiagnosticsError) as refused:
            validated_request(document)
        assert refused.value.reason == "invalid_request"


def test_the_cli_generates_lists_shows_and_inspects(
    copy: Studies, capsys: pytest.CaptureFixture[str]
) -> None:
    roots = ["--tools", str(copy.tools.directory), "--artifacts", str(copy.artifacts)]
    assert cli.main(["diagnose-strategies", *roots, "--study", copy.ids["week"], "--progress"]) == 0
    captured = capsys.readouterr()
    generated = json.loads(captured.out)
    identifier = generated["diagnostics"]["diagnostic_id"]
    progress = [json.loads(line)["progress"] for line in captured.err.splitlines()]
    assert progress[0] == {"stage": "verify_sources"}
    assert {
        "fold": "2",
        "folds": "2",
        "model": "logistic",
        "scenario": "stress",
        "stage": "attribute",
    } in progress
    assert progress[-1] == {"stage": "publish"}
    assert cli.main(["list-strategy-diagnostics", "--artifacts", str(copy.artifacts)]) == 0
    listed = json.loads(capsys.readouterr().out)
    assert [entry["diagnostic_id"] for entry in listed["diagnostics"]] == [identifier]
    assert (
        cli.main(
            ["show-strategy-diagnostics", "--artifacts", str(copy.artifacts), "--diagnostics", identifier]
        )
        == 0
    )
    assert json.loads(capsys.readouterr().out)["diagnostics"]["diagnostic_id"] == identifier
    assert cli.main(["inspect-strategy-diagnostics", *roots, "--diagnostics", identifier]) == 0
    assert "identical bytes" in json.loads(capsys.readouterr().out)["reproduced"]
    assert cli.main(["diagnose-strategies", *roots, "--study", "f" * 64]) == 1
    error = json.loads(capsys.readouterr().err)["error"]
    assert (error["kind"], error["reason"]) == ("DiagnosticsError", "source_invalid")
    assert (
        cli.main(["diagnose-strategies", *roots, "--study", copy.ids["week"], "--study", copy.ids["week"]])
        == 1
    )
    assert json.loads(capsys.readouterr().err)["error"]["reason"] == "invalid_request"
