"""Comparisons of registered model versions over the synthetic market: one
frozen spec, independent accounts per member, refused and failed members
kept with their reasons, exact deltas, and an artifact that verifies."""

import json
import os
import shutil
from decimal import Decimal
from pathlib import Path
from typing import Any

import pytest

from chronos_research import comparison, model_import
from chronos_research.comparison import ComparisonError, compare, exact_difference, verified_comparison
from chronos_research.demo import DemoSource, run_demo
from chronos_research.encoding import canonical_json
from chronos_research.onnx_sandbox import Sandbox
from chronos_research.pipeline import ArtifactRoots, Tools, experiment_split, verified_sources
from chronos_research.registry import RegistryRoots, register_experiment_model, set_lifecycle
from tests import accounting
from tests import pipeline as built
from tests.synthetic import synthetic_events
from tests.test_model_import import MODEL, FakePredictor, manifest

CONFIGURATION = {
    "decision_interval_ns": "1000000000",
    "fee_per_fill": "0.01",
    "latency_ns": "1000000",
    "maximum_order_notional": "1000000",
    "maximum_quote_age_ns": "5000000000",
    "starting_cash": "10000",
    "target_quantity": "1",
    "threshold_ppb": "500000000",
}


@pytest.fixture(scope="module")
def demo(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> tuple[Tools, Path, str]:
    base = tmp_path_factory.mktemp("comparison")
    directory = Tools(tools.chronos_dataset.parent)
    (base / "events.csv").write_text(synthetic_events(seed=7))
    run_demo(
        directory,
        ArtifactRoots.under(base / "data"),
        DemoSource("synthetic-stream", synthetic_events=base / "events.csv"),
        150_000,
    )
    (experiment,) = os.listdir(base / "data" / "experiments")
    return directory, base / "data", experiment


@pytest.fixture
def data(demo: tuple[Tools, Path, str], tmp_path: Path) -> Path:
    copy = tmp_path / "data"
    shutil.copytree(demo[1], copy, symlinks=True)
    return copy


def register(demo: tuple[Tools, Path, str], data: Path, family: str, instrument: str = "AAPL") -> str:
    tools, _, experiment = demo
    roots = ArtifactRoots.under(data)
    sources = verified_sources(tools, roots, experiment_split(roots.experiments, experiment), True)
    name = f"{instrument.lower()}-{family}"
    return register_experiment_model(
        RegistryRoots.under(data), roots.experiments, sources, experiment, instrument, family, name, "1"
    ).model_version_id


def spec(demo: tuple[Tools, Path, str], models: list[str], **changes: Any) -> dict[str, Any]:
    roots = ArtifactRoots.under(demo[1])
    return {
        "configuration": dict(CONFIGURATION),
        "instrument": "AAPL",
        "model_version_ids": models,
        "schema": comparison.SPEC_SCHEMA,
        "split_artifact_id": experiment_split(roots.experiments, demo[2]),
        "window": "test",
        **changes,
    }


def journal(data: Path, run_id: str) -> list[dict[str, Any]]:
    lines = (data / "strategies" / run_id / "journal.ndjson").read_text().splitlines()
    return [json.loads(line) for line in lines]


def run_manifest(data: Path, run_id: str) -> dict[str, Any]:
    document: dict[str, Any] = json.loads((data / "strategies" / run_id / "manifest.json").read_bytes())
    return document


def by_id(result: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {member["model_version_id"]: member for member in result["comparison"]["members"]}


def test_members_run_in_their_own_accounts_with_exact_deltas_and_verify(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    logistic, majority = register(demo, data, "logistic"), register(demo, data, "majority")
    result = compare(demo[0], data, spec(demo, [logistic, majority]), Sandbox(docker="/nonexistent/docker"))
    document = result["comparison"]
    assert result["outcome"] == "published"
    assert document["status"] == {"completed": "2", "failed": "0", "refused": "0"}
    assert document["reference_model_version_id"] == logistic
    assert document["benchmarks"]["identical_across_members"] is True
    members = by_id(result)
    for member in members.values():
        assert member["schedule"]["reproduced"] is True
        assert member["run"]["replayed"] is True
        assert member["conditional"] is False
        run = run_manifest(data, member["run"]["artifact_id"])
        assert run["schema"]["version"] == "2"
        assert run["source"]["model_version_id"] == member["model_version_id"]
        assert "experiment_id" not in run["source"]
        # Independent of the engine: every account recomputed from the journal.
        accounting.check_run(run, journal(data, member["run"]["artifact_id"]))
    # Exact deltas, as Decimal recomputes them.
    strategy = {key: m["accounts"]["strategy"]["valuation"] for key, m in members.items()}
    hold = document["benchmarks"]["accounts"]["buy_and_hold"]["valuation"]
    for key, member in members.items():
        expected = Decimal(strategy[key]["net_total_pnl"]) - Decimal(hold["net_total_pnl"])
        assert Decimal(member["deltas"]["buy_and_hold"]["net_total_pnl"]["value"]) == expected
    reference = members[majority]["deltas"]["reference"]["net_total_pnl"]["value"]
    assert Decimal(reference) == Decimal(strategy[majority]["net_total_pnl"]) - Decimal(
        strategy[logistic]["net_total_pnl"]
    )
    assert "reference" not in members[logistic]["deltas"]
    # The artifact verifies, and the same spec gives the same comparison.
    root = comparison.comparisons_root(data)
    assert verified_comparison(root, document["comparison_id"]) == document
    again = compare(demo[0], data, spec(demo, [logistic, majority]), Sandbox(docker="/nonexistent/docker"))
    assert (again["outcome"], again["comparison"]["comparison_id"]) == (
        "already_present",
        document["comparison_id"],
    )


def test_members_that_cannot_run_are_kept_with_their_reasons(
    demo: tuple[Tools, Path, str], data: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    logistic = register(demo, data, "logistic")
    spy = register(demo, data, "logistic", "SPY")
    archived = register(demo, data, "majority")
    registry_roots = RegistryRoots.under(data)
    set_lifecycle(registry_roots, archived, "archived", "superseded")
    monkeypatch.setattr(model_import, "OnnxPredictor", FakePredictor)
    window_start = int(
        verified_sources(
            demo[0], ArtifactRoots.under(data), spec(demo, [])["split_artifact_id"], False
        ).test_start_at_ns
    )
    late = model_import.import_model(
        registry_roots,
        manifest(name="late", declared__information_cutoff_ns=str(window_start + 1)),
        MODEL,
        Sandbox(),
    ).model_version_id
    undeclared = model_import.import_model(
        registry_roots,
        manifest(name="undeclared", declared__information_cutoff_ns=None, declared__cutoff_covers=None),
        MODEL,
        Sandbox(),
    ).model_version_id
    declared = model_import.import_model(
        registry_roots, manifest(name="declared", declared__information_cutoff_ns="1000"), MODEL, Sandbox()
    ).model_version_id
    models = [logistic, spy, archived, late, undeclared, declared]
    result = compare(demo[0], data, spec(demo, models), Sandbox(docker="/nonexistent/docker"))
    members = by_id(result)
    assert [member["model_version_id"] for member in result["comparison"]["members"]] == models
    assert members[logistic]["state"] == "completed"
    assert (members[spy]["state"], members[spy]["reason"]) == ("refused", "instrument_mismatch")
    assert (members[archived]["state"], members[archived]["reason"]) == ("refused", "archived")
    assert (members[late]["state"], members[late]["reason"]) == ("refused", "cutoff_after_window_start")
    assert (members[undeclared]["state"], members[undeclared]["reason"]) == ("refused", "cutoff_unavailable")
    # An import that may run, but whose sandbox is unavailable here.
    assert (members[declared]["state"], members[declared]["reason"]) == (
        "refused",
        "sandbox_unavailable:docker_not_found",
    )
    assert members[declared]["conditional"] is True
    assert "accounts" not in members[spy]
    assert result["comparison"]["status"] == {"completed": "1", "failed": "0", "refused": "5"}


def test_a_comparison_without_a_completed_member_is_still_published(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    spy, majority = register(demo, data, "logistic", "SPY"), register(demo, data, "majority", "SPY")
    result = compare(demo[0], data, spec(demo, [spy, majority]), Sandbox(docker="/nonexistent/docker"))
    assert result["comparison"]["benchmarks"] == {"unavailable": "no_member_completed"}
    assert result["comparison"]["reference_model_version_id"] is None
    assert verified_comparison(comparison.comparisons_root(data), result["comparison"]["comparison_id"])


def test_a_spec_must_be_one_frozen_comparison(demo: tuple[Tools, Path, str]) -> None:
    ids = ["a" * 64, "b" * 64]
    for changes in (
        {"model_version_ids": ["a" * 64]},
        {"model_version_ids": ["a" * 64] * 2},
        {"model_version_ids": [c * 64 for c in "abcdef0"]},
        {"window": "training"},
        {"instrument": "../AAPL"},
        {"configuration": {**CONFIGURATION, "latency_ns": "1e6"}},
        {"configuration": {key: value for key, value in CONFIGURATION.items() if key != "fee_per_fill"}},
        {"schema": {"name": "chronos.model_comparison_spec", "version": "2"}},
    ):
        with pytest.raises(ComparisonError):
            comparison.validated_spec(spec(demo, ids, **changes))
    assert comparison.validated_spec(spec(demo, ids))["model_version_ids"] == ids


def test_a_tampered_comparison_is_refused(demo: tuple[Tools, Path, str], data: Path) -> None:
    logistic, majority = register(demo, data, "logistic"), register(demo, data, "majority")
    result = compare(demo[0], data, spec(demo, [logistic, majority]), Sandbox(docker="/nonexistent/docker"))
    root = comparison.comparisons_root(data)
    directory = root / result["comparison"]["comparison_id"]
    os.chmod(directory, 0o700)
    path = directory / "manifest.json"
    os.chmod(path, 0o600)
    document = json.loads(path.read_bytes())
    document["members"][0]["accounts"]["strategy"]["valuation"]["net_total_pnl"] = "1000000"
    path.write_bytes(canonical_json(document))
    with pytest.raises(ComparisonError):
        verified_comparison(root, directory.name)


@pytest.mark.parametrize(
    ("left", "right", "difference"),
    [
        ("10000.13", "10000", "0.13"),
        ("0.1", "0.3", "-0.2"),
        ("-0.04", "-0.04", "0"),
        ("-0.04", "0", "-0.04"),
        ("123456789012345678.123456789", "0.000000001", "123456789012345678.123456788"),
        ("0.0004", "-0.0003", "0.0007"),
    ],
)
def test_deltas_are_exact_decimal_differences(left: str, right: str, difference: str) -> None:
    assert exact_difference(left, right) == difference


def test_an_import_is_compared_through_the_sandbox(
    demo: tuple[Tools, Path, str], data: Path, sandbox: Sandbox, tmp_path: Path
) -> None:
    from chronos_research.onnx_export import export_logistic
    from chronos_research.registry import verified_model_version

    logistic = register(demo, data, "logistic")
    registry_roots = RegistryRoots.under(data)
    exported = export_logistic(
        demo[0],
        data,
        verified_model_version(registry_roots, logistic),
        tmp_path / "export",
        "onnx-copy",
        "1",
        sandbox,
    )
    assert exported["parity"]["status"] == "agrees"
    imported = model_import.import_model(
        registry_roots, exported["manifest"], (tmp_path / "export" / "model.onnx").read_bytes(), sandbox
    ).model_version_id
    result = compare(demo[0], data, spec(demo, [logistic, imported]), sandbox)
    members = by_id(result)
    assert result["comparison"]["status"]["completed"] == "2"
    assert members[imported]["conditional"] is True
    assert members[imported]["family"] == "onnx"
    run = run_manifest(data, members[imported]["run"]["artifact_id"])
    assert (
        "onnxruntime"
        in json.loads((data / "schedules" / run["source"]["schedule_id"] / "manifest.json").read_bytes())[
            "environment"
        ]
    )
    accounting.check_run(run, journal(data, members[imported]["run"]["artifact_id"]))
    # The exported model trades exactly as the model it came from.
    assert members[imported]["accounts"] == members[logistic]["accounts"]


def test_a_member_whose_schedule_cannot_be_published_fails_alone(
    demo: tuple[Tools, Path, str], data: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from chronos_research.publication import PublicationError

    logistic, majority = register(demo, data, "logistic"), register(demo, data, "majority")
    original = comparison._publish_schedule

    def refuse_logistic(roots: ArtifactRoots, built: Any) -> None:
        if json.loads(built.files["manifest.json"])["model"]["family"] == "logistic":
            raise PublicationError("an entry with this ID exists and does not verify")
        original(roots, built)

    monkeypatch.setattr(comparison, "_publish_schedule", refuse_logistic)
    result = compare(demo[0], data, spec(demo, [logistic, majority]), Sandbox(docker="/nonexistent/docker"))
    members = by_id(result)
    assert (members[logistic]["state"], members[logistic]["reason"]) == ("failed", "schedule_not_published")
    assert members[majority]["state"] == "completed"
    assert result["comparison"]["reference_model_version_id"] == majority


class InputsOnly:
    """A predictor that records that it ran and checks that only a row's
    exact inputs reach the model."""

    def __init__(self, predictor: Any, events: list[str]) -> None:
        self.predictor, self.events = predictor, events

    def predict(self, inputs: tuple[int, ...]) -> Any:
        assert isinstance(inputs, tuple)
        assert [type(value) for value in inputs] == [int] * 5
        self.events.append("predict")
        return self.predictor.predict(inputs)

    def runtime_environment(self) -> dict[str, str]:
        return dict(self.predictor.runtime_environment())

    def close(self) -> None:
        self.predictor.close()


def test_completed_members_are_scored_against_the_windows_labels_only_after_inference(
    demo: tuple[Tools, Path, str], data: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from chronos_research.registered_inference import open_predictor
    from chronos_research.scoring import window_labels

    logistic, majority = register(demo, data, "logistic"), register(demo, data, "majority")
    archived = register(demo, data, "logistic", instrument="SPY")
    events: list[str] = []

    def reading_labels(*arguments: Any) -> Any:
        events.append("labels")
        return window_labels(*arguments)

    monkeypatch.setattr(
        "chronos_research.registered_schedule.open_predictor",
        lambda version, sandbox: InputsOnly(open_predictor(version, sandbox), events),
    )
    monkeypatch.setattr("chronos_research.comparison.window_labels", reading_labels)
    result = compare(
        demo[0], data, spec(demo, [logistic, majority, archived]), Sandbox(docker="/nonexistent/docker")
    )
    # Every schedule, and its rebuild, was predicted before any label was read.
    assert events.count("labels") == 1
    assert "predict" not in events[events.index("labels") :]
    assert result["comparison"]["scoring"] == list(comparison.SCORING)
    members = by_id(result)
    assert "predictive" not in members[archived]
    stored = json.loads((data / "experiments" / demo[2] / "metrics.json").read_bytes())
    for version_id, family in ((logistic, "logistic"), (majority, "majority")):
        predictive = members[version_id]["predictive"]
        rows = predictive["rows"]
        assert rows["scheduled"] == members[version_id]["schedule"]["counts"]["rows"]
        assert int(rows["scored"]) + sum(int(n) for n in rows["not_scored"].values()) == int(
            rows["scheduled"]
        )
        assert predictive["metrics"]["rows"] == rows["scored"]
        # The experiment scored the same rows with the same model. Its
        # probabilities came from one batch, which a BLAS may round apart
        # from the schedule's row-pure arithmetic in the last bits, so only
        # the two probability-weighted metrics are compared within 1e-12.
        expected = stored["splits"]["test"]["instruments"]["AAPL"][family]
        weighted = ("brier_score", "log_loss")
        for name in weighted:
            assert float(predictive["metrics"][name]["value"]) == pytest.approx(
                float(expected[name]["value"]), rel=0, abs=1e-12
            )
        assert {k: v for k, v in predictive["metrics"].items() if k not in weighted} == {
            k: v for k, v in expected.items() if k not in weighted
        }
        assert rows["not_scored"] == stored["excluded"]["test"]["AAPL"]
