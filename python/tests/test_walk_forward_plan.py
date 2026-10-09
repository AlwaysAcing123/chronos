"""Walk-forward plans: expanding folds over later independent recordings, an
exclusive information boundary, counts recomputed from the raw files, strict
requests, finite workloads, and inspection that rebuilds rather than hashes."""

import copy as copying
import json
import shutil
from pathlib import Path
from typing import Any

import pytest

from chronos_research import pipeline as research_pipeline
from chronos_research import walk_forward
from chronos_research.artifacts import FeatureRow, LabelRow, RowIdentity, Unavailable
from chronos_research.collection import create_collection
from chronos_research.encoding import canonical_json
from chronos_research.walk_forward import (
    REQUEST_SCHEMA,
    LabelledRow,
    PlanError,
    build_plan,
    create_plan,
    inspect_plan,
    plan_identity,
    plans_root,
    training_row,
    verified_plan,
)
from tests import pipeline as built
from tests.recordings import WEEK, Recording, Recordings, prepare

CONFIGURATION = {
    "decision_interval_ns": "1000000",
    "fee_per_fill": "0.01",
    "latency_ns": "1000",
    "maximum_order_notional": "1000000",
    "maximum_quote_age_ns": "5000000000",
    "starting_cash": "10000",
    "target_quantity": "1",
    "threshold_ppb": "500000000",
}
SCENARIOS = [
    {"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base"},
    {"fee_per_fill": "0.05", "latency_ns": "200000", "name": "stress"},
]


@pytest.fixture(scope="module")
def week(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Recordings:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("plans"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for recording in WEEK:
        prepare(recordings, recording)
    # A day on which only SPY quoted.
    prepare(
        recordings, Recording("synthetic-2026-09-24-spy", day=3, quotes=300, seed=14, instruments=("SPY",))
    )
    return recordings


@pytest.fixture
def copy(week: Recordings, tmp_path: Path) -> Recordings:
    root = tmp_path / "artifacts"
    shutil.copytree(week.artifacts, root, symlinks=True)
    return Recordings(artifacts=root, tools=week.tools, prepared=dict(week.prepared))


def collection_of(recordings: Recordings, *streams: str) -> str:
    result = create_collection(recordings.tools, recordings.artifacts, recordings.label_ids(*streams))
    return str(result["collection"]["collection_id"])


def request(collection: str, **changes: Any) -> dict[str, Any]:
    return {
        "collection_id": collection,
        "configuration": dict(CONFIGURATION),
        "instrument": "AAPL",
        "minimum_training_sources": "1",
        "scenarios": copying.deepcopy(SCENARIOS),
        "schema": REQUEST_SCHEMA,
        **changes,
    }


def raw_rows(artifacts: Path, kind: str, artifact_id: str) -> list[dict[str, Any]]:
    path = artifacts / kind / artifact_id / f"{kind}.ndjson"
    return [json.loads(line) for line in path.read_text().splitlines()]


def receive_range(artifacts: Path, dataset_id: str) -> tuple[int, int]:
    times = [
        int(json.loads(line)["receive_timestamp_ns"])
        for line in (artifacts / "normalized" / dataset_id / "events.ndjson").read_text().splitlines()
    ]
    return min(times), max(times)


def hand_eligible(artifacts: Path, prepared: Any, boundary: int) -> tuple[int, dict[str, int]]:
    """The training rows of one recording, counted from its raw files."""
    features = {
        (row["stream"], row["ingest_sequence"]): row
        for row in raw_rows(artifacts, "features", prepared.feature_id)
        if row["instrument"] == "AAPL"
    }
    eligible, classes = 0, {"down": 0, "flat": 0, "up": 0}
    for label in raw_rows(artifacts, "labels", prepared.label_id):
        if label["instrument"] != "AAPL":
            continue
        values = features[(label["stream"], label["ingest_sequence"])]["features"]
        if any(
            "raw" not in values[name]
            for name in ("spread", "imbalance", "microprice", "midpoint", "bid_size", "ask_size")
        ):
            continue
        if "direction" not in label["label"]:
            continue
        if int(label["prediction_at_ns"]) < boundary and int(label["label_available_at_ns"]) < boundary:
            eligible += 1
            classes[label["label"]["direction"]] += 1
    return eligible, classes


def test_folds_expand_over_later_independent_recordings(copy: Recordings) -> None:
    streams = [recording.stream for recording in WEEK] + ["synthetic-2026-09-24-spy"]
    collection_id = collection_of(copy, *streams)
    plan = build_plan(copy.tools, copy.artifacts, request(collection_id))
    assert [fold["index"] for fold in plan["folds"]] == ["1", "2"]
    assert plan["collection"]["skipped_members"] == [
        {"dataset_id": copy.prepared["synthetic-2026-09-24-spy"].dataset_id, "reason": "instrument_absent"}
    ]
    for index, fold in enumerate(plan["folds"], start=1):
        evaluated = copy.prepared[WEEK[index].stream]
        start, end = receive_range(copy.artifacts, evaluated.dataset_id)
        assert fold["evaluation"]["dataset_id"] == evaluated.dataset_id
        assert fold["evaluation"]["window"] == {
            "end_at_ns": str(end + 1),
            "name": f"fold_{index}",
            "start_at_ns": str(start),
        }
        assert fold["information_boundary_ns"] == str(start)
        aapl = [
            row
            for row in raw_rows(copy.artifacts, "features", evaluated.feature_id)
            if row["instrument"] == "AAPL"
        ]
        assert fold["evaluation"]["feature_rows"] == str(len(aapl))
        trained = [copy.prepared[recording.stream] for recording in WEEK[:index]]
        assert [member["dataset_id"] for member in fold["training"]["members"]] == [
            p.dataset_id for p in trained
        ]
        total, support = 0, {"down": 0, "flat": 0, "up": 0}
        for member, prepared in zip(fold["training"]["members"], trained, strict=True):
            eligible, classes = hand_eligible(copy.artifacts, prepared, start)
            first, last = receive_range(copy.artifacts, prepared.dataset_id)
            assert member["interval"] == {"end_at_ns": str(last + 1), "start_at_ns": str(first)}
            assert member["eligible_rows"] == str(eligible)
            total += eligible
            support = {name: support[name] + classes[name] for name in support}
        assert fold["training"]["eligible_rows"] == str(total)
        assert fold["training"]["class_support"] == {name: str(count) for name, count in support.items()}
        assert int(fold["training"]["max_label_available_at_ns"]) < start
    assert plan["workload"]["folds"] == "2"
    assert plan["workload"]["schedules"] == "4"
    assert plan["workload"]["strategy_runs"] == "8"
    # A later recording changes nothing an earlier fold was planned on.
    shorter = build_plan(copy.tools, copy.artifacts, request(collection_of(copy, *streams[:2])))
    assert shorter["folds"][0] == {**plan["folds"][0]}
    # Training on at least two earlier recordings leaves one fold.
    assert (
        len(
            build_plan(copy.tools, copy.artifacts, request(collection_id, minimum_training_sources="2"))[
                "folds"
            ]
        )
        == 1
    )


def labelled(
    prediction_at: int, label_at: int | None, direction: str | None = "up", inputs: bool = True
) -> LabelledRow:
    identity = RowIdentity("s", "AAPL", 1)
    names = ("spread", "imbalance", "microprice", "midpoint", "bid_size", "ask_size")
    values: dict[str, int | Unavailable] = dict.fromkeys(names, 5)
    if not inputs:
        values["spread"] = Unavailable("missing_bid")
    return LabelledRow(
        FeatureRow(identity, prediction_at, values),
        LabelRow(
            identity,
            prediction_at,
            label_at,
            direction,
            None if direction is None else 7,
            None if direction is not None else "no_future_observation",
        ),
    )


@pytest.mark.parametrize(
    ("prediction_at", "label_at", "outcome"),
    [
        (99, 99, None),
        (98, 99, None),
        (99, 100, "label_available_at_not_before_boundary"),
        (99, 101, "label_available_at_not_before_boundary"),
        (100, 100, "prediction_at_not_before_boundary"),
        (101, 102, "prediction_at_not_before_boundary"),
        # A label delayed far beyond its horizon, but still before the boundary.
        (10, 99, None),
    ],
)
def test_the_information_boundary_is_exclusive_for_prediction_and_label_times(
    prediction_at: int, label_at: int, outcome: str | None
) -> None:
    trained = training_row(labelled(prediction_at, label_at), 100)
    assert (trained if isinstance(trained, str) else None) == outcome


def test_rows_without_a_label_or_inputs_never_train() -> None:
    assert training_row(labelled(1, None, direction=None), 100) == "label_unavailable:no_future_observation"
    assert training_row(labelled(1, 2, inputs=False), 100) == "feature_unavailable:missing_bid"


def test_too_few_recordings_holding_the_instrument_is_insufficient_coverage(copy: Recordings) -> None:
    single = collection_of(copy, WEEK[0].stream)
    with pytest.raises(PlanError) as one:
        build_plan(copy.tools, copy.artifacts, request(single))
    assert one.value.reason == "insufficient_coverage"
    assert "1 recording(s)" in str(one.value)
    pair = collection_of(copy, WEEK[0].stream, "synthetic-2026-09-24-spy")
    with pytest.raises(PlanError) as absent:
        build_plan(copy.tools, copy.artifacts, request(pair))
    assert absent.value.reason == "insufficient_coverage"
    with pytest.raises(PlanError) as more:
        build_plan(
            copy.tools,
            copy.artifacts,
            request(collection_of(copy, *(r.stream for r in WEEK)), minimum_training_sources="3"),
        )
    assert more.value.reason == "insufficient_coverage"


def scenarios(*cells: tuple[str, str, str]) -> list[dict[str, str]]:
    return [{"fee_per_fill": fee, "latency_ns": latency, "name": name} for name, fee, latency in cells]


@pytest.mark.parametrize(
    "changes",
    [
        {"extra": "field"},
        {"schema": {"name": "chronos.walk_forward_request", "version": "2"}},
        {"collection_id": "not-an-id"},
        {"instrument": ""},
        {"minimum_training_sources": "0"},
        {"minimum_training_sources": "01"},
        {"minimum_training_sources": 1},
        {"minimum_training_sources": "12"},
        {"configuration": {**CONFIGURATION, "latency_ns": "1.5"}},
        {"configuration": {**CONFIGURATION, "fee_per_fill": "0.010"}},
        {"configuration": {key: value for key, value in CONFIGURATION.items() if key != "fee_per_fill"}},
        {"scenarios": []},
        {
            "scenarios": scenarios(
                *[("base", "0.01", "1000")] + [(f"s{i}", "0.02", str(1000 + i)) for i in range(4)]
            )
        },
        {"scenarios": scenarios(("base", "0.01", "1000"), ("base", "0.02", "2000"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.01", "1000"))},
        {"scenarios": scenarios(("base", "0.02", "1000"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("other", "0.02", "2000"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.005", "5000"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.02", "500"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.020", "5000"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.02", "0"))},
        {"scenarios": [{"fee_per_fill": "0.01", "latency_ns": "1000", "name": "base", "spread": "0"}]},
        {"scenarios": scenarios(("Base", "0.01", "1000"))},
        # Values the engine's 64-bit fixed point cannot hold exactly.
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.000000001", "5000"))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "0.02", "1" + "0" * 18))},
        {"scenarios": scenarios(("base", "0.01", "1000"), ("stress", "1" + "0" * 11, "5000"))},
        {"configuration": {**CONFIGURATION, "starting_cash": "0"}},
        {"configuration": {**CONFIGURATION, "starting_cash": "10000.000000001"}},
        {"configuration": {**CONFIGURATION, "starting_cash": "-1"}},
    ],
)
def test_malformed_requests_are_refused(changes: dict[str, Any]) -> None:
    with pytest.raises(PlanError) as refused:
        walk_forward.validated_request(request("a" * 64, **changes))
    assert refused.value.reason == "invalid_request"


INT64_MAX = str(2**63 - 1)
# Money at 8 decimal places in a signed 64-bit integer, as the engine holds it.
MONEY_MAX = "92233720368.54775807"


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("threshold_ppb", "1000000001"),
        ("threshold_ppb", "-1"),
        ("threshold_ppb", "01"),
        ("target_quantity", "0"),
        ("target_quantity", "-1"),
        ("target_quantity", "0.000000001"),
        ("target_quantity", "1.0"),
        ("target_quantity", "92233720368.54775808"),
        ("latency_ns", "0"),
        ("latency_ns", str(2**63)),
        ("decision_interval_ns", "0"),
        ("decision_interval_ns", str(2**63)),
        ("starting_cash", "99999999999"),
        ("starting_cash", "92233720368.54775808"),
        ("starting_cash", "0"),
        ("fee_per_fill", "-0.01"),
        ("fee_per_fill", "92233720368.54775808"),
        ("maximum_quote_age_ns", "0"),
        ("maximum_quote_age_ns", str(2**63)),
        ("maximum_order_notional", "0"),
        ("maximum_order_notional", "92233720368.54775808"),
    ],
)
def test_every_setting_the_engine_cannot_hold_is_refused_at_the_plan_boundary(field: str, value: str) -> None:
    configuration = {**CONFIGURATION, field: value}
    cells = [dict(cell) for cell in SCENARIOS]
    if field in ("fee_per_fill", "latency_ns"):
        cells[0][field] = value
    with pytest.raises(PlanError) as refused:
        walk_forward.validated_request(request("a" * 64, configuration=configuration, scenarios=cells))
    assert refused.value.reason == "invalid_request"


def test_settings_just_inside_the_engines_limits_are_accepted() -> None:
    configuration = {
        "decision_interval_ns": INT64_MAX,
        "fee_per_fill": "0",
        "latency_ns": "1",
        "maximum_order_notional": MONEY_MAX,
        "maximum_quote_age_ns": INT64_MAX,
        "starting_cash": MONEY_MAX,
        "target_quantity": "0.00000001",
        "threshold_ppb": "1000000000",
    }
    cells = [{"fee_per_fill": "0", "latency_ns": "1", "name": "base"}]
    accepted = walk_forward.validated_request(request("a" * 64, configuration=configuration, scenarios=cells))
    assert accepted["configuration"] == configuration
    zero = {
        **configuration,
        "threshold_ppb": "0",
        "starting_cash": "0.00000001",
        "latency_ns": "999999999999999999",
    }
    cells = [{"fee_per_fill": "0", "latency_ns": "999999999999999999", "name": "base"}]
    assert walk_forward.validated_request(request("a" * 64, configuration=zero, scenarios=cells))


def test_the_total_capital_of_every_fold_must_fit_the_engines_money(copy: Recordings) -> None:
    collection_id = collection_of(copy, *(recording.stream for recording in WEEK))
    # Two folds: each fold's account gets the starting cash, summed exactly.
    half = "46116860184.27387903"
    for cash, fits in ((half, True), ("46116860184.27387904", False), (MONEY_MAX, False)):
        configuration = {**CONFIGURATION, "starting_cash": cash}
        document = request(collection_id, configuration=configuration)
        if fits:
            assert build_plan(copy.tools, copy.artifacts, document)["configuration"]["starting_cash"] == half
            continue
        with pytest.raises(PlanError) as refused:
            build_plan(copy.tools, copy.artifacts, document)
        assert refused.value.reason == "workload_exceeded"


def test_a_zero_fee_is_only_ever_an_explicit_scenario() -> None:
    cells = scenarios(("base", "0.01", "1000"), ("stress", "0.05", "200000"), ("no_fee", "0", "1000"))
    checked = walk_forward.validated_request(request("a" * 64, scenarios=cells))
    assert [cell["fee_per_fill"] for cell in checked["scenarios"]] == ["0.01", "0.05", "0"]
    assert walk_forward.validated_request(request("a" * 64))["configuration"]["fee_per_fill"] == "0.01"


def test_workloads_over_the_caps_are_refused_before_anything_runs(
    copy: Recordings, monkeypatch: pytest.MonkeyPatch
) -> None:
    collection_id = collection_of(copy, *(recording.stream for recording in WEEK))
    monkeypatch.setattr(walk_forward, "MAX_FOLDS", 1)
    with pytest.raises(PlanError) as folds:
        build_plan(copy.tools, copy.artifacts, request(collection_id))
    assert folds.value.reason == "workload_exceeded"
    monkeypatch.setattr(walk_forward, "MAX_FOLDS", 10)
    monkeypatch.setattr(walk_forward, "MAX_TRAINING_ROWS_PER_FOLD", 10)
    with pytest.raises(PlanError) as rows:
        build_plan(copy.tools, copy.artifacts, request(collection_id))
    assert rows.value.reason == "workload_exceeded"


def test_a_published_plan_is_rebuilt_from_its_request_on_inspection(copy: Recordings) -> None:
    collection_id = collection_of(copy, *(recording.stream for recording in WEEK))
    preview = build_plan(copy.tools, copy.artifacts, request(collection_id))
    created = create_plan(copy.tools, copy.artifacts, request(collection_id))
    assert created["outcome"] == "published"
    assert created["plan"] == preview
    assert create_plan(copy.tools, copy.artifacts, request(collection_id))["outcome"] == "already_present"
    plan_id = preview["plan_id"]
    assert inspect_plan(copy.tools, copy.artifacts, plan_id) == preview
    other = build_plan(copy.tools, copy.artifacts, request(collection_id, scenarios=SCENARIOS[:1]))
    assert other["plan_id"] != plan_id
    # A plan whose identity verifies but whose counts were edited is refused
    # on inspection, which recomputes them.
    edited = copying.deepcopy(preview)
    edited["folds"][0]["training"]["eligible_rows"] = "1"
    del edited["plan_id"]
    edited["plan_id"] = plan_identity(edited)
    directory = plans_root(copy.artifacts) / edited["plan_id"]
    directory.mkdir()
    (directory / "manifest.json").write_bytes(canonical_json(edited))
    assert (
        verified_plan(plans_root(copy.artifacts), edited["plan_id"])["folds"][0]["training"]["eligible_rows"]
        == "1"
    )
    with pytest.raises(PlanError) as differs:
        inspect_plan(copy.tools, copy.artifacts, edited["plan_id"])
    assert differs.value.reason == "artifact_invalid"


def test_the_cli_creates_lists_previews_plans_and_inspects(
    copy: Recordings, capsys: pytest.CaptureFixture[str]
) -> None:
    from chronos_research.cli import main

    roots = ["--tools", str(copy.tools.directory), "--artifacts", str(copy.artifacts)]
    labels = [
        part
        for stream in (r.stream for r in WEEK)
        for part in ("--label-artifact", copy.prepared[stream].label_id)
    ]
    first = copy.prepared[WEEK[0].stream].label_id
    assert main(["create-collection", *roots, *labels, "--session-label", f"{first}=Monday, declared"]) == 0
    created = json.loads(capsys.readouterr().out)
    collection_id = created["collection"]["collection_id"]
    assert created["collection"]["members"][0]["session"] == {
        "basis": "declared",
        "label": "Monday, declared",
    }
    assert main(["list-collections", "--artifacts", str(copy.artifacts)]) == 0
    assert [entry["collection_id"] for entry in json.loads(capsys.readouterr().out)["collections"]] == [
        collection_id
    ]
    assert main(["inspect-collection", *roots, "--collection", collection_id]) == 0
    assert json.loads(capsys.readouterr().out)["collection"]["collection_id"] == collection_id
    document = json.dumps(request(collection_id))
    assert main(["plan-walk-forward", *roots, "--request", document, "--preview"]) == 0
    preview = json.loads(capsys.readouterr().out)
    assert preview["outcome"] == "preview"
    assert not plans_root(copy.artifacts).exists()
    assert main(["plan-walk-forward", *roots, "--request", document]) == 0
    published = json.loads(capsys.readouterr().out)
    assert (published["outcome"], published["plan"]) == ("published", preview["plan"])
    plan_id = published["plan"]["plan_id"]
    assert main(["list-walk-forward-plans", "--artifacts", str(copy.artifacts)]) == 0
    assert [entry["plan_id"] for entry in json.loads(capsys.readouterr().out)["plans"]] == [plan_id]
    assert main(["inspect-walk-forward-plan", *roots, "--plan", plan_id]) == 0
    assert json.loads(capsys.readouterr().out)["plan"]["plan_id"] == plan_id
    # One recording: refused, with the reason, exit 1.
    assert main(["create-collection", *roots, "--label-artifact", first]) == 0
    single = json.loads(capsys.readouterr().out)["collection"]["collection_id"]
    assert main(["plan-walk-forward", *roots, "--request", json.dumps(request(single)), "--preview"]) == 1
    error = json.loads(capsys.readouterr().err)["error"]
    assert (error["kind"], error["reason"]) == ("PlanError", "insufficient_coverage")
