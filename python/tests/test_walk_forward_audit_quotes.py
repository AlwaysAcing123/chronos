"""The independent auditor's execution quotes are the engine's: a pending
order executes on the first top-of-book event of its instrument received at
or after its arrival and inside the window (strategy_simulation.hpp). A
trade print has no bid or ask and never executes an order.

The synthetic dataset tool writes quotes only, so these tests build a real
study whose recordings interleave AAPL's quotes with other instruments'
quotes, then show the auditor some of those other instruments' events as AAPL
trade prints. For an AAPL strategy the engine ignores both alike (it executes
only on AAPL top-of-book events, and AAPL's features and rows come only from
them), so its journal is the one it would have written had they been AAPL
trades. Only what the auditor is shown differs.
"""

import json
import runpy
import shutil
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from chronos_research import pipeline as research_pipeline
from tests import pipeline as built
from tests.recordings import Recording, Recordings, prepare
from tests.test_walk_forward_end_to_end import AUDIT, audited, study_of

type Event = dict[str, Any]
# AAPL quotes last at each step, after the other three; every eleventh step
# all four share one receive time. A tail of SPY quotes outlasts AAPL's by
# three seconds, so an order arriving after AAPL's last quote expires there.
SESSIONS = tuple(
    Recording(f"quotes-2026-09-2{day + 1}", day=day, quotes=quotes, seed=seed, up_bias=bias,
              instruments=("SPY", "QQQ", "IWM", "AAPL"), tail_quotes=30)
    for day, quotes, seed, bias in ((0, 360, 41, 0.45), (1, 320, 42, 0.30), (2, 400, 43, 0.40))
)  # fmt: skip


@pytest.fixture(scope="module")
def study(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> tuple[Recordings, dict[str, Any]]:
    """Two folds; base orders arrive 1 µs after their signal, stress orders
    one second later, after AAPL's last quote, so every stress order expires."""
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("audit-quotes"),
        tools=research_pipeline.Tools(tools.chronos_dataset.parent),
    )
    for session in SESSIONS:
        prepare(recordings, session)
    made = study_of(recordings, *(session.stream for session in SESSIONS), stress_latency_ns="1000000000")
    assert made["status"] == "complete"
    return recordings, made


def as_trade(event: Event) -> Event:
    """The event as an AAPL trade print at the same sequence and time."""
    return {
        "aggressor_side": "unknown",
        "event_timestamp_ns": event["event_timestamp_ns"],
        "ingest_sequence": event["ingest_sequence"],
        "instrument": "AAPL",
        "price_raw": event["bid"]["price_raw"],
        "quality": list(event["quality"]),
        "quantity_raw": event["bid"]["quantity_raw"],
        "receive_timestamp_ns": event["receive_timestamp_ns"],
        "stream": event["stream"],
        "type": "trade",
        "venue": None,
        "venue_sequence": None,
        "version": "1",
    }


def next_aapl_quote_time(events: list[Event]) -> dict[str, int | None]:
    """For each event, the receive time of the first AAPL quote at or after
    it in ingest order; None after AAPL's last quote."""
    following: dict[str, int | None] = {}
    upcoming: int | None = None
    for event in sorted(events, key=lambda item: -int(item["ingest_sequence"])):
        if event["instrument"] == "AAPL" and event["type"] == "top_of_book":
            upcoming = int(event["receive_timestamp_ns"])
        following[event["ingest_sequence"]] = upcoming
    return following


def shown_with_trades(events: list[Event], becomes_trade: Callable[[Event, int | None], bool]) -> list[Event]:
    """The events with every other instrument's quote that `becomes_trade`
    selects shown as an AAPL trade print."""
    following = next_aapl_quote_time(events)
    return [
        as_trade(event)
        if event["instrument"] != "AAPL" and becomes_trade(event, following[event["ingest_sequence"]])
        else event
        for event in events
    ]


def time_of(event: Event) -> int:
    return int(event["receive_timestamp_ns"])


# Which other-instrument quotes the auditor is shown as AAPL trade prints.
VIEWS: dict[str, Callable[[Event, int | None], bool]] = {
    # One trade, earlier than the AAPL quote that follows it.
    "a trade before the quote": lambda event, quote: (
        event["instrument"] == "SPY" and quote is not None and time_of(event) < quote
    ),
    # Trades at the very receive time of the AAPL quote after them: the real failure.
    "a trade at the quote's receive time": lambda event, quote: quote is not None and time_of(event) == quote,
    # Three trades before the quote.
    "several trades before the quote": lambda event, quote: quote is not None and time_of(event) < quote,
    # After AAPL's last quote, only trades remain until the window ends.
    "only trades after the last quote": lambda event, quote: quote is None,
}


def audit_shown(artifacts: Path, study_id: str, view: Callable[[list[Event]], list[Event]]) -> dict[str, Any]:
    """The standard-library auditor, run in this process, with each run's
    events replaced by `view` of them. Nothing on disk changes."""
    namespace = runpy.run_path(str(AUDIT))
    scope = namespace["run_audit"].__globals__
    audit_run = scope["audit_run"]

    def audit_run_shown(audit: Any, root: Any, cell: Any, schedule: Any, events: Any, *rest: Any) -> Any:
        return audit_run(audit, root, cell, schedule, view(events), *rest)

    scope["audit_run"] = audit_run_shown
    report: dict[str, Any] = namespace["run_audit"](artifacts, study_id)
    return report


def journals(artifacts: Path, study: dict[str, Any]) -> list[list[Event]]:
    runs = [
        cell["run"]["artifact_id"]
        for fold in study["folds"]
        for model in fold["models"].values()
        for cell in model["cells"]
        if "run" in cell
    ]
    return [
        [
            json.loads(line)
            for line in (artifacts / "strategies" / run / "journal.ndjson").read_text().splitlines()
        ]
        for run in runs
    ]


def events_of(artifacts: Path, study: dict[str, Any], fold: int) -> list[Event]:
    dataset = study["folds"][fold]["evaluation"]["dataset_id"]
    path = artifacts / "normalized" / dataset / "events.ndjson"
    return [json.loads(line) for line in path.read_text().splitlines()]


def test_a_quote_only_study_passes(study: tuple[Recordings, dict[str, Any]]) -> None:
    """The control: other instruments' quotes between an order's arrival and
    AAPL's next quote never execute it."""
    recordings, made = study
    returned, report = audited(recordings.artifacts, made["study_id"])
    assert (returned, report["verdict"], report["problems"], report["not_audited"]) == (0, "pass", [], [])
    assert report["checks"]["fill"] > 0
    assert report["checks"]["order"] > 0
    others = [
        event
        for fold in range(len(made["folds"]))
        for event in events_of(recordings.artifacts, made, fold)
        if event["instrument"] != "AAPL"
    ]
    passed_over = sum(
        any(
            time_of(event) >= int(entry["arrival_at_ns"])
            and int(event["ingest_sequence"]) < int(entry["observation"]["ingest_sequence"])
            for event in others
        )
        for journal in journals(recordings.artifacts, made)
        for entry in journal
        if entry["type"] == "execution"
    )
    assert passed_over > 0


@pytest.mark.parametrize("name", VIEWS)
def test_trade_prints_never_execute_an_order(study: tuple[Recordings, dict[str, Any]], name: str) -> None:
    recordings, made = study
    becomes_trade = VIEWS[name]
    control = audit_shown(recordings.artifacts, made["study_id"], trades_shown_by(no_trades))
    report = audit_shown(recordings.artifacts, made["study_id"], trades_shown_by(becomes_trade))
    assert (report["verdict"], report["problems"], report["not_audited"]) == ("pass", [], [])
    assert report["checks"] == control["checks"]
    assert exercised(recordings.artifacts, made, becomes_trade, name) > 0


def exercised(
    artifacts: Path, made: dict[str, Any], becomes_trade: Callable[[Event, int | None], bool], name: str
) -> int:
    """How many resolved orders had a shown trade print at or after their
    arrival and before their outcome: the case this view is about."""
    trades = []
    for fold in range(len(made["folds"])):
        shown = shown_with_trades(events_of(artifacts, made, fold), becomes_trade)
        trades += [event for event in shown if event["type"] == "trade"]
    count = 0
    for journal in journals(artifacts, made):
        for entry in journal:
            if entry["type"] == "execution":
                quote = int(entry["observation"]["ingest_sequence"])
                before = [
                    trade for trade in trades
                    if time_of(trade) >= int(entry["arrival_at_ns"]) and int(trade["ingest_sequence"]) < quote
                ]  # fmt: skip
                if name == "a trade at the quote's receive time":
                    before = [trade for trade in before if time_of(trade) == int(entry["executed_at_ns"])]
                if name == "several trades before the quote":
                    count += len(before) >= 2
                else:
                    count += bool(before)
            if entry["type"] == "order_expired" and name == "only trades after the last quote":
                count += any(time_of(trade) >= int(entry["arrival_at_ns"]) for trade in trades)
    return count


def test_a_quote_at_the_window_end_never_executes_an_order(study: tuple[Recordings, dict[str, Any]]) -> None:
    """The window is [start, end): an AAPL quote at `end` is not eligible, so
    every expired order still expires."""
    recordings, made = study
    ends = {fold["evaluation"]["window"]["end_at_ns"] for fold in made["folds"]}

    def with_quote_at_the_end(events: list[Event]) -> list[Event]:
        last = max(
            (event for event in events if event["instrument"] == "AAPL" and event["type"] == "top_of_book"),
            key=lambda event: int(event["ingest_sequence"]),
        )
        # The window is [the recording's first receive time, its last + 1).
        end = str(max(time_of(event) for event in events) + 1)
        assert end in ends
        sequence = str(max(int(event["ingest_sequence"]) for event in events) + 1)
        at_end = {**last, "event_timestamp_ns": end, "ingest_sequence": sequence, "receive_timestamp_ns": end}
        return [*events, at_end]

    report = audit_shown(recordings.artifacts, made["study_id"], with_quote_at_the_end)
    assert (report["verdict"], report["problems"]) == ("pass", [])
    assert any(
        entry["type"] == "order_expired"
        for journal in journals(recordings.artifacts, made)
        for entry in journal
    )


def test_an_execution_on_a_later_quote_still_fails(
    study: tuple[Recordings, dict[str, Any]], tmp_path: Path
) -> None:
    """A journal moved to the next AAPL quote is refused, however the
    auditor is shown the trades between them."""
    recordings, made = study
    root = tmp_path / "forged"
    shutil.copytree(recordings.artifacts, root, symlinks=True)
    events = events_of(root, made, 1)
    quotes = sorted(
        (event for event in events if event["instrument"] == "AAPL" and event["type"] == "top_of_book"),
        key=lambda event: int(event["ingest_sequence"]),
    )
    base = next(
        cell for cell in made["folds"][1]["models"]["logistic"]["cells"] if cell["scenario"] == "base"
    )
    path = root / "strategies" / base["run"]["artifact_id"] / "journal.ndjson"
    entries = [json.loads(line) for line in path.read_text().splitlines()]
    execution = next(
        entry for entry in entries if entry["type"] == "execution" and entry["account"] == "strategy"
    )
    first = next(
        index
        for index, quote in enumerate(quotes)
        if quote["ingest_sequence"] == execution["observation"]["ingest_sequence"]
    )
    later = quotes[first + 1]
    execution["observation"] = {**execution["observation"], "ingest_sequence": later["ingest_sequence"],
                                "receive_timestamp_ns": later["receive_timestamp_ns"]}  # fmt: skip
    execution["executed_at_ns"] = later["receive_timestamp_ns"]
    path.parent.chmod(0o755)
    path.chmod(0o644)
    path.write_text(
        "".join(json.dumps(entry, separators=(",", ":"), sort_keys=True) + "\n" for entry in entries)
    )
    expected = (
        f"order: {base['run']['artifact_id'][:12]} {execution['client_order_id']} "
        "executes on the first quote at or after its arrival"
    )
    for becomes_trade in (no_trades, VIEWS["several trades before the quote"]):
        report = audit_shown(root, made["study_id"], trades_shown_by(becomes_trade))
        assert report["verdict"] == "fail"
        assert expected in report["problems"]


def no_trades(event: Event, quote: int | None) -> bool:
    return False


def trades_shown_by(
    becomes_trade: Callable[[Event, int | None], bool],
) -> Callable[[list[Event]], list[Event]]:
    return lambda events: shown_with_trades(events, becomes_trade)
