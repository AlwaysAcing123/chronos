"""Request plans and money: strict validation, exact identities, exact sums,
and a cost limit that cannot round above what was authorized. Offline."""

import copy
import json
from collections.abc import Callable
from decimal import Decimal
from fractions import Fraction
from pathlib import Path
from typing import Any

import pytest

from chronos_research.acquisition_budget import (
    BudgetError,
    cap_argument,
    format_cents,
    parse_cents,
    parse_estimate,
    proposed_cap_cents,
)
from chronos_research.acquisition_cli import read_plan_file
from chronos_research.recording_plan import RecordingPlanError, plan_identity, validated_plan
from tests.recording_plans import plan, recording

type Edit = Callable[[dict[str, Any]], None]


def edited(edit: Edit) -> dict[str, Any]:
    document = plan()
    edit(document)
    return document


def test_a_valid_plan_is_canonical_and_its_id_names_every_binding() -> None:
    document = plan()
    checked = validated_plan(document)
    assert checked == document
    assert plan_identity(checked) == plan_identity(validated_plan(copy.deepcopy(document)))
    changes: list[Edit] = [
        lambda d: d["recordings"][0].update(end="2026-09-21T13:41:00Z"),
        lambda d: d["recordings"][1].update(stream_id="another-stream"),
        lambda d: d["recordings"][2].update(destination="databento/other.dbn.zst"),
        lambda d: d["recordings"][0].update(session_label="another label"),
        lambda d: d.update(symbols=["AAPL", "MSFT", "SPY"]),
        lambda d: d["evaluation"].update(horizon_ns="2000000000"),
        lambda d: d["evaluation"]["configuration"].update(threshold_ppb="400000000"),
        lambda d: d["evaluation"]["scenarios"].pop(1),
    ]
    identities = {plan_identity(validated_plan(edited(change))) for change in changes}
    assert len(identities) == len(changes)
    assert plan_identity(checked) not in identities


REFUSALS: dict[str, Edit] = {
    "unknown key": lambda d: d.update(note="x"),
    "missing key": lambda d: d.pop("symbols"),
    "another schema": lambda d: d.update(schema={"name": "chronos.recording_request_plan", "version": "2"}),
    "unsorted symbols": lambda d: d.update(symbols=["SPY", "AAPL"]),
    "duplicate symbols": lambda d: d.update(symbols=["AAPL", "AAPL"]),
    "lowercase symbol": lambda d: d.update(symbols=["aapl"]),
    "seconds in a time": lambda d: d["recordings"][0].update(start="2026-09-21T13:30:05Z"),
    "no time zone": lambda d: d["recordings"][0].update(start="2026-09-21T13:30:00"),
    "impossible date": lambda d: d["recordings"][0].update(start="2026-02-30T13:30:00Z"),
    "end before start": lambda d: d["recordings"][0].update(end="2026-09-21T13:20:00Z"),
    "empty window": lambda d: d["recordings"][0].update(end="2026-09-21T13:30:00Z"),
    "window over an hour": lambda d: d["recordings"][0].update(end="2026-09-21T14:31:00Z"),
    "overlapping windows": lambda d: d["recordings"][1].update(
        start="2026-09-21T13:35:00Z", end="2026-09-21T13:45:00Z"
    ),
    "out of order": lambda d: d["recordings"].reverse(),
    "duplicate stream": lambda d: d["recordings"][1].update(stream_id=d["recordings"][0]["stream_id"]),
    "duplicate destination": lambda d: d["recordings"][1].update(
        destination=d["recordings"][0]["destination"]
    ),
    "parent directory": lambda d: d["recordings"][0].update(destination="../escape.dbn.zst"),
    "absolute destination": lambda d: d["recordings"][0].update(destination="/tmp/x.dbn.zst"),
    "dot component": lambda d: d["recordings"][0].update(destination="a/./b.dbn.zst"),
    "not a recording name": lambda d: d["recordings"][0].update(destination="a/b.csv"),
    "both sources": lambda d: d["recordings"][0].update(
        existing={"path": "/data/a.dbn.zst", "sha256": "0" * 64, "size_bytes": "1"}
    ),
    "relative existing path": lambda d: _existing(d, path="data/a.dbn.zst"),
    "unnormalized existing path": lambda d: _existing(d, path="/data/../a.dbn.zst"),
    "bad existing checksum": lambda d: _existing(d, sha256="ABC"),
    "zero existing size": lambda d: _existing(d, size_bytes="0"),
    "bad stream": lambda d: d["recordings"][0].update(stream_id="Bad_Stream"),
    "control character in a label": lambda d: d["recordings"][0].update(session_label="a\nb"),
    "another provider": lambda d: d["provider"].update(dataset="XNAS.ITCH"),
    "another schema of data": lambda d: d["provider"].update(schema="mbp-10"),
    "instrument not requested": lambda d: d["evaluation"].update(instruments=["MSFT"]),
    "zero horizon": lambda d: d["evaluation"].update(horizon_ns="0"),
    "engine-invalid threshold": lambda d: d["evaluation"]["configuration"].update(threshold_ppb="1000000001"),
    "non-canonical cash": lambda d: d["evaluation"]["configuration"].update(starting_cash="10000.0"),
    "too many scenarios": lambda d: d["evaluation"]["scenarios"].extend(
        {"fee_per_fill": "0.1", "latency_ns": str(10**8 + n), "name": f"extra{n}"} for n in range(2)
    ),
    "no fold left": lambda d: d["evaluation"].update(minimum_training_sources="3"),
    "one recording": lambda d: d.update(recordings=d["recordings"][:1]),
    "thirteen recordings": lambda d: d.update(recordings=[recording(day) for day in range(1, 14)]),
}


def _existing(document: dict[str, Any], **fields: str) -> None:
    entry = document["recordings"][0]
    del entry["destination"]
    entry["existing"] = {"path": "/data/a.dbn.zst", "sha256": "0" * 64, "size_bytes": "1", **fields}


@pytest.mark.parametrize("edit", REFUSALS.values(), ids=REFUSALS.keys())
def test_a_plan_breaking_any_rule_is_refused(edit: Edit) -> None:
    with pytest.raises(RecordingPlanError):
        validated_plan(edited(edit))


def test_a_plan_file_with_numbers_or_duplicate_keys_is_refused(tmp_path: Path) -> None:
    good = tmp_path / "plan.json"
    good.write_text(json.dumps(plan()))
    assert read_plan_file(good) == plan()
    for text in (
        json.dumps(plan()).replace('"1000000000"', "1000000000", 1),
        json.dumps(plan())[:-1] + ', "schema": {}}',
        json.dumps(plan()).replace('"0.01"', "NaN", 1),
    ):
        bad = tmp_path / "bad.json"
        bad.write_text(text)
        with pytest.raises(RecordingPlanError):
            read_plan_file(bad)


@pytest.mark.parametrize("text", ["0.30", "1.00", "12.34", "0.00"])
def test_cents_are_read_exactly(text: str) -> None:
    assert format_cents(parse_cents(text, "amount")) == text


@pytest.mark.parametrize("text", ["0.3", "1", "-0.10", "0.001", "1e2", "NaN", " 1.00", "01.00", "", "1,00"])
def test_anything_but_dollars_and_two_decimals_is_refused(text: str) -> None:
    with pytest.raises(BudgetError):
        parse_cents(text, "amount")


def test_estimates_add_exactly_and_caps_round_up_to_the_cent() -> None:
    parsed = json.loads('{"a": 0.1, "b": 0.2, "c": 1.2e-05}', parse_float=Decimal)
    estimates = [parse_estimate(value) for value in parsed.values()]
    assert sum(estimates, Decimal(0)) == Decimal("0.300012")
    assert 0.1 + 0.2 != 0.3  # the binary sum the exact one avoids
    assert [proposed_cap_cents(estimate) for estimate in estimates] == [13, 25, 1]
    assert proposed_cap_cents(Decimal("0.08")) == 10
    for bad in (Decimal("NaN"), Decimal("-0.01"), Decimal("Infinity"), True, "0.1"):
        with pytest.raises(BudgetError):
            parse_estimate(bad)


def test_the_cost_limit_the_tool_reads_never_exceeds_the_authorized_cents() -> None:
    for cents in range(1, 10_001):
        text = cap_argument(cents)
        value = float(text)  # correctly rounded, like strtod
        assert Fraction(value) <= Fraction(cents, 100)
        assert repr(value) == text
    # 0.10's nearest double is above 0.10, so the next one down is used.
    assert Fraction(0.1) > Fraction(1, 10)
    assert cap_argument(10) == "0.09999999999999999"
    # 0.30's nearest double is already below 0.30.
    assert cap_argument(30) == "0.3"
    for cents in (0, -1, 10_001):
        with pytest.raises(BudgetError):
            cap_argument(cents)
