"""Exact PnL attribution of one run (ADR 0024), on hand-built journals in
the engine's format. Every expected figure is computed by hand in the test."""

import copy
from fractions import Fraction
from typing import Any

import pytest

from chronos_research.strategy_attribution import (
    DiagnosticsError,
    Missing,
    RunAttribution,
    _reconciliation,
    attribute_run,
    diagnostic_text,
    engine_decimal,
)
from tests.attribution_runs import (
    Act,
    Script,
    inputs,
    market_events,
    quote,
    rounded_money,
    row,
    text,
    tie,
    ties_toward_zero,
    trade,
)

T = 1_000_000  # 1 ms, so every event is well apart


def figures(result: RunAttribution, account: str = "strategy") -> dict[str, str]:
    components = result.accounts[account].document["components"]
    return {
        name: value.get("value", f"unavailable:{value.get('unavailable')}")
        for name, value in components.items()
    }


def reconciliation(result: RunAttribution, account: str = "strategy") -> dict[str, Any]:
    reconciled: dict[str, Any] = result.accounts[account].document["reconciliation"]
    return reconciled


def round_trip() -> Script:
    """Buy on a signal at mid 100.00, filled at the 100.02 ask (mid 100.01);
    sell on a signal at mid 100.07, filled at the 100.05 bid (mid 100.06)."""
    events = [
        quote(0, 1 * T, "99.99", "100.01"),
        quote(1, 2 * T, "100.00", "100.02"),
        quote(2, 3 * T, "100.06", "100.08"),
        quote(3, 4 * T, "100.05", "100.07"),
    ]
    rows = [row(0, 1 * T, "up"), row(1, 2 * T, "up"), row(2, 3 * T, "down"), row(3, 4 * T, "down")]
    # The latency is exactly the gap to the next quote: arrival is at a quote.
    return Script(events, rows, [Act(0), Act(2, side="sell")], latency_ns=T)


def test_a_hand_computed_round_trip_reconciles_exactly() -> None:
    result = attribute_run(inputs(round_trip()))
    assert figures(result) == {
        # 10000 - 100.02 - 0.01 + 100.05 - 0.01 = 10000.01.
        "booked_net_pnl": "0.01",
        # Q*M - sum q*m = 0 - (100.01 - 100.06).
        "midpoint_inventory_pnl": "0.05",
        # (100.02 - 100.01) + -1 * (100.05 - 100.06).
        "execution_spread_cost": "0.02",
        "fees": "0.02",
        "terminal_bid_markdown": "0",
        "rounding_adjustment": "0",
        "market_value_rounding": "0",
        "notional_rounding": "0",
        # (100.01 - 100.00) + -1 * (100.06 - 100.07): both adverse.
        "observed_midpoint_movement": "0.02",
        # 0 - (100.00 - 100.07).
        "signal_reference_pnl": "0.07",
        "inventory_pnl_at_bid": "0.05",
    }
    assert reconciliation(result) == {
        "booked_net_pnl": {"state": "reconciled"},
        "signal_reference": {"state": "reconciled"},
    }
    # Buy and hold bought at the same signal and holds: marked at the 100.05
    # bid of the last quote, whose midpoint is 100.06.
    assert figures(result, "buy_and_hold") == {
        "booked_net_pnl": "0.02",
        "midpoint_inventory_pnl": "0.05",
        "execution_spread_cost": "0.01",
        "fees": "0.01",
        "terminal_bid_markdown": "0.01",
        "rounding_adjustment": "0",
        "market_value_rounding": "0",
        "notional_rounding": "0",
        "observed_midpoint_movement": "0.01",
        "signal_reference_pnl": "0.06",
        "inventory_pnl_at_bid": "0.04",
    }
    document = result.accounts["buy_and_hold"].document
    assert document["final_mark"] == {
        "ask": "100.07",
        "bid": "100.05",
        "ingest_sequence": "3",
        "midpoint": {"value": "100.06"},
        "receive_at_ns": str(4 * T),
    }
    # The deltas are computed by the study layer; here every fill is listed.
    assert [fill["side"] for fill in result.fills] == ["buy", "sell", "buy"]
    first = result.fills[0]
    assert first["contributions"] == {
        "execution_spread_cost": {"value": "0.01"},
        "notional_rounding": {"value": "0"},
        "observed_midpoint_movement": {"value": "0.01"},
    }
    assert first["timing"] == {
        "arrival_to_execution_duration_ns": "0",
        "configured_latency_ns": str(T),
        "signal_to_arrival_duration_ns": str(T),
        "signal_to_execution_duration_ns": str(T),
    }
    assert first["signal"]["quoted_spread"] == {"value": "0.02"}


def test_no_trades_are_genuine_zeros_not_missing_values() -> None:
    script = round_trip()
    script.acts = [Act(0, reason="hold_below_threshold", side=None)]
    result = attribute_run(inputs(script))
    assert set(figures(result).values()) == {"0"}
    assert set(figures(result, "cash").values()) == {"0"}
    assert reconciliation(result, "cash")["booked_net_pnl"] == {"state": "reconciled"}
    timing = result.accounts["strategy"].document["timing"]
    assert timing["executions"] == "0"
    assert timing["signal_to_execution_duration_ns"] == {"unavailable": "no_executions"}
    assert result.accounts["strategy"].document["episodes"]["completed_pnl"] == {"value": "0"}
    assert result.accounts["strategy"].document["final_mark"] is None


def test_an_open_position_is_marked_down_from_midpoint_to_bid() -> None:
    events = [
        quote(0, 1 * T, "99.99", "100.01"),
        quote(1, 2 * T, "100.00", "100.02"),
        quote(2, 3 * T, "100.10", "100.14"),
    ]
    script = Script(events, [row(0, T), row(1, 2 * T), row(2, 3 * T)], [Act(0)], latency_ns=T)
    result = attribute_run(inputs(script))
    values = figures(result)
    # Bought at 100.02; marked at the 100.10 bid of a quote whose midpoint is 100.12.
    assert values["booked_net_pnl"] == "0.07"
    assert values["midpoint_inventory_pnl"] == "0.11"
    assert values["terminal_bid_markdown"] == "0.02"
    assert values["inventory_pnl_at_bid"] == "0.09"
    episodes = [episode for episode in result.episodes if episode["account"] == "strategy"]
    assert [episode["state"] for episode in episodes] == ["censored"]
    assert episodes[0]["pnl"] == {"value": "0.07"}
    assert episodes[0]["basis"] == "marked_at_final_valuation"
    assert episodes[0]["censored_at_ns"] == str(3 * T)


def test_favourable_movement_is_negative() -> None:
    # The midpoint falls from 100.00 to 99.95 before the buy fills.
    events = [
        quote(0, T, "99.99", "100.01"),
        quote(1, 2 * T, "99.94", "99.96"),
        quote(2, 3 * T, "99.94", "99.96"),
    ]
    script = Script(events, [row(0, T), row(1, 2 * T), row(2, 3 * T)], [Act(0)], latency_ns=T)
    values = figures(attribute_run(inputs(script)))
    assert values["observed_midpoint_movement"] == "-0.05"
    assert values["signal_reference_pnl"] == "-0.05"
    assert values["midpoint_inventory_pnl"] == "0"


def test_partial_and_multiple_fills_with_fractional_quantities() -> None:
    events = [
        quote(0, T, "99.99", "100.01"),
        quote(1, 2 * T, "100.00", "100.02", ask_size="0.4"),
        quote(2, 3 * T, "100.00", "100.02"),
        quote(3, 4 * T, "100.01", "100.03"),
        quote(4, 5 * T, "100.01", "100.03"),
    ]
    rows = [row(index, (index + 1) * T) for index in range(5)]
    # The first order fills 0.4 of 1; the next decision orders the 0.6 left.
    script = Script(events, rows, [Act(0), Act(2, quantity="0.6")], latency_ns=T)
    result = attribute_run(inputs(script))
    strategy = result.accounts["strategy"].document
    assert strategy["orders"]["outcomes"]["partially_filled_and_cancelled"] == "1"
    assert strategy["orders"]["cancellation_reasons"] == {"insufficient_liquidity": "1"}
    assert strategy["fills"] == "2"
    values = figures(result)
    # 0.4 at 100.02 and 0.6 at 100.03, both one cent over their midpoints.
    assert values["execution_spread_cost"] == "0.01"
    assert values["fees"] == "0.02"
    # Marked at the 100.01 bid (midpoint 100.02): 1 x 0.01 below.
    assert values["terminal_bid_markdown"] == "0.01"
    # Cash 10000 - 40.008 - 60.018 - 0.02; market value 100.01.
    assert values["booked_net_pnl"] == "-0.036"


def test_fees_are_charged_on_booked_fills_only() -> None:
    events = [
        quote(0, T, "99.99", "100.01"),
        quote(1, 2 * T, "100.00", None),  # no ask: the buy is rejected
        quote(2, 3 * T, "100.00", "100.02"),
        quote(3, 4 * T, "100.00", "100.02", ask_size="0"),  # nothing displayed: cancelled
        quote(4, 5 * T, "100.00", "100.02"),
        quote(5, 6 * T, "100.00", "100.02"),
    ]
    rows = [row(index, (index + 1) * T) for index in range(6)]
    # Arrives after the last quote of the window: expires.
    script = Script(events, rows, [Act(0), Act(2), Act(5)], latency_ns=T)
    result = attribute_run(inputs(script))
    strategy = result.accounts["strategy"].document
    assert strategy["orders"]["outcomes"] == {
        "cancelled": "1",
        "expired": "1",
        "filled": "0",
        "partially_filled_and_cancelled": "0",
        "rejected": "1",
        "scheduled": "3",
    }
    assert strategy["orders"]["rejections"] == {"pre_trade_risk:missing_top_of_book_side": "1"}
    assert figures(result)["fees"] == "0"
    assert strategy["timing"]["expired"] == "1"
    assert strategy["timing"]["executions"] == "2"


def test_a_halt_rejects_by_the_runner() -> None:
    script = round_trip()
    script.halted_from_ns = 2 * T
    result = attribute_run(inputs(script))
    assert result.accounts["strategy"].document["orders"]["rejections"] == {"runner:trading_halted": "2"}
    assert figures(result)["booked_net_pnl"] == "0"


def test_a_missing_signal_midpoint_leaves_only_signal_figures_unavailable() -> None:
    events = [
        quote(0, T, "99.99", None),
        quote(1, 2 * T, "100.00", "100.02"),
        quote(2, 3 * T, "100.00", "100.02"),
    ]
    script = Script(events, [row(0, T), row(1, 2 * T), row(2, 3 * T)], [Act(0)], latency_ns=T)
    result = attribute_run(inputs(script))
    values = figures(result)
    reason = "unavailable:signal_midpoint_unavailable:ask_missing"
    assert values["observed_midpoint_movement"] == reason
    assert values["signal_reference_pnl"] == reason
    assert values["execution_spread_cost"] == "0.01"
    assert reconciliation(result) == {
        "booked_net_pnl": {"state": "reconciled"},
        "signal_reference": {"unavailable": "signal_midpoint_unavailable:ask_missing"},
    }
    assert result.fills[0]["signal"]["midpoint"] == {"unavailable": "ask_missing"}
    assert result.fills[0]["signal"]["ask"] is None


def test_a_missing_final_ask_reconciles_at_the_bid_and_never_borrows_an_ask() -> None:
    events = [
        quote(0, T, "99.99", "100.01"),
        quote(1, 2 * T, "100.00", "100.02"),
        quote(2, 3 * T, "100.03", None),
    ]
    script = Script(events, [row(0, T), row(1, 2 * T), row(2, 3 * T)], [Act(0)], latency_ns=T)
    result = attribute_run(inputs(script))
    values = figures(result)
    assert values["midpoint_inventory_pnl"] == "unavailable:final_midpoint_unavailable:ask_missing"
    assert values["terminal_bid_markdown"] == "unavailable:final_midpoint_unavailable:ask_missing"
    assert values["inventory_pnl_at_bid"] == "0.02"
    assert values["booked_net_pnl"] == "0"
    assert reconciliation(result)["booked_net_pnl"] == {"state": "reconciled_at_bid"}
    assert result.accounts["strategy"].document["final_mark"]["ask"] is None


@pytest.mark.parametrize("stale", [False, True])
def test_an_unavailable_final_valuation_is_never_zero(stale: bool) -> None:
    last = (
        quote(3, 30_000 * T, "100.00", "100.02", instrument="SPY")
        if stale
        else quote(3, 4 * T, "100.00", "100.02", quality=("replay", "potentially_invalid_book"))
    )
    events = [
        quote(0, T, "99.99", "100.01"),
        quote(1, 2 * T, "100.00", "100.02"),
        quote(2, 3 * T, "100.0", "100.02"),
        last,
    ]
    script = Script(
        events,
        [row(0, T), row(1, 2 * T), row(2, 3 * T)] + ([] if stale else [row(3, 4 * T)]),
        [Act(0)],
        latency_ns=T,
    )
    result = attribute_run(inputs(script))
    values = figures(result)
    reason = "unavailable:final_valuation_unavailable:position_mark_unavailable"
    for name in ("booked_net_pnl", "midpoint_inventory_pnl", "terminal_bid_markdown", "rounding_adjustment"):
        assert values[name] == reason
    # What the fills alone determine stays.
    assert values["execution_spread_cost"] == "0.01"
    assert values["fees"] == "0.01"
    assert values["observed_midpoint_movement"] == "0.01"
    assert reconciliation(result)["booked_net_pnl"] == {
        "unavailable": "final_valuation_unavailable:position_mark_unavailable"
    }
    # The flat cash account is still valued.
    assert figures(result, "cash")["booked_net_pnl"] == "0"
    censored = [episode for episode in result.episodes if episode["account"] == "strategy"]
    assert censored[0]["pnl"] == {"unavailable": "final_valuation_unavailable:position_mark_unavailable"}


def test_rounding_ties_go_away_from_zero_in_both_directions() -> None:
    # 0.5 x 100.00000001 = 50.000000005, booked 50.00000001 (+0.000000005);
    # selling 0.5 at a 100.00000001 bid books -50.00000001 (-0.000000005).
    events = [
        quote(0, T, "100.00000001", "100.00000003"),
        quote(1, 2 * T, "100.00000001", "100.00000002"),
        quote(2, 3 * T, "100.00000001", "100.00000003"),
        quote(3, 4 * T, "100.00000001", "100.00000003"),
    ]
    rows = [row(index, (index + 1) * T) for index in range(4)]
    script = Script(
        events,
        rows,
        [Act(0, quantity="0.5"), Act(2, side="sell", quantity="0.5")],
        latency_ns=T,
        target="0.5",
    )
    values = figures(attribute_run(inputs(script)))
    assert values["notional_rounding"] == "-0.000000005"
    assert values["rounding_adjustment"] == "0.000000005"
    # The buy was exact (0.5 x 100.00000002 = 50.00000001), the sell rounded:
    # cash 10000 - 50.00000001 - 0.01 + 50.00000001 - 0.01.
    assert values["booked_net_pnl"] == "-0.02"
    # The buy's quote has a half-unit midpoint, 100.000000015: 0.5 x 0.000000005;
    # the sell's midpoint is 100.00000002: -0.5 x -0.00000001.
    assert values["execution_spread_cost"] == "0.0000000075"

    events[1] = quote(1, 2 * T, "100.00000001", "100.00000001")
    events[3] = quote(3, 4 * T, "100.00000002", "100.00000003")
    hold = Script(events, rows, [Act(0, quantity="0.5")], latency_ns=T, target="0.5")
    held = figures(attribute_run(inputs(hold)))
    # Bought 0.5 at 100.00000001: booked +0.000000005 over exact. Marked at
    # 100.00000002: 50.00000001 exactly, so only the purchase rounded.
    assert held["notional_rounding"] == "0.000000005"
    assert held["market_value_rounding"] == "0"
    assert held["rounding_adjustment"] == "-0.000000005"
    # Q*(M - B) = 0.5 x 0.000000005 needs 18 places, and keeps them.
    assert held["terminal_bid_markdown"] == "0.0000000025"


@pytest.mark.parametrize("mode", ["buy", "sell", "market_value"])
def test_a_tie_rounded_toward_zero_is_refused_although_the_ledger_balances(mode: str) -> None:
    script = tie(mode)
    # The engine's rounding, half away from zero, in both directions: the
    # buy and the mark round 2.000000005 up to 2.00000001, and the sell's
    # -50.000000005 to -50.00000001.
    attribute_run(inputs(script))
    if mode == "market_value":
        script.value_rounding = ties_toward_zero
    else:
        script.notional_rounding = ties_toward_zero
    # Every cash, position, and valuation figure agrees with the wrong
    # rounding, so only the execution rules can refuse it.
    with pytest.raises(DiagnosticsError) as refused:
        attribute_run(inputs(script))
    assert refused.value.reason == "ledger_invalid"
    assert ("market value" if mode == "market_value" else "notional") in str(refused.value)


def test_a_notional_one_unit_off_is_refused() -> None:
    script = round_trip()
    script.notional_rounding = lambda value: rounded_money(value) + Fraction(1, 10**8)
    with pytest.raises(DiagnosticsError) as refused:
        attribute_run(inputs(script))
    assert refused.value.reason == "ledger_invalid"


@pytest.mark.parametrize(("configured", "booked"), [("0.01", "0.02"), ("0.01", "0"), ("0", "0.01")])
def test_a_fee_other_than_the_runs_fee_per_fill_is_refused(configured: str, booked: str) -> None:
    script = round_trip()
    script.fee, script.booked_fee = configured, booked
    with pytest.raises(DiagnosticsError) as refused:
        attribute_run(inputs(script))
    assert refused.value.reason == "ledger_invalid"
    assert "fee_per_fill" in str(refused.value)
    script.booked_fee = None
    assert figures(attribute_run(inputs(script)))["fees"] == text(2 * Fraction(configured), 8)


def test_a_half_unit_midpoint_is_exact() -> None:
    events = [
        quote(0, T, "100.000000001", "100.000000002"),
        quote(1, 2 * T, "100.000000001", "100.000000002"),
        quote(2, 3 * T, "100.000000001", "100.000000002"),
    ]
    script = Script(
        events,
        [row(0, T), row(1, 2 * T), row(2, 3 * T)],
        [Act(0, quantity="0.5")],
        latency_ns=T,
        target="0.5",
    )
    result = attribute_run(inputs(script))
    assert result.fills[0]["execution"]["midpoint"] == {"value": "100.0000000015"}
    values = figures(result)
    # 0.5 x (100.000000002 - 100.0000000015).
    assert values["execution_spread_cost"] == "0.00000000025"
    assert values["terminal_bid_markdown"] == "0.00000000025"


def test_trade_prints_same_time_quotes_and_other_instruments_never_execute() -> None:
    events = [
        quote(0, T, "99.99", "100.01"),
        trade(1, 2 * T, "105.00"),
        quote(2, 2 * T, "100.00", "100.02", instrument="SPY"),
        quote(3, 3 * T, "100.00", "100.02"),
        quote(4, 3 * T, "100.50", "100.52"),
        quote(5, 4 * T, "100.00", "100.02"),
    ]
    script = Script(events, [row(0, T), row(3, 3 * T), row(4, 3 * T), row(5, 4 * T)], [Act(0)], latency_ns=T)
    result = attribute_run(inputs(script))
    # The first of the two quotes at 3 ms, after the trade print and SPY's quote.
    assert result.fills[0]["execution"]["ingest_sequence"] == "3"
    _, journal = script.run()
    for sequence, why in (("1", "a trade event"), ("2", "a top_of_book event of SPY"), ("4", "first quote")):
        tampered = copy.deepcopy(journal)
        execution = next(entry for entry in tampered if entry["type"] == "execution")
        execution["observation"]["ingest_sequence"] = sequence
        if sequence == "4":
            execution["observation"] |= {
                "ask": {"price": "100.52", "quantity": "10"},
                "bid": {"price": "100.5", "quantity": "10"},
            }
        with pytest.raises(DiagnosticsError) as refused:
            attribute_run(inputs(script, journal=tampered))
        assert refused.value.reason == "join_failed"
        assert why in str(refused.value)


def test_a_quote_at_the_windows_exclusive_end_never_executes() -> None:
    events = [quote(0, T, "99.99", "100.01"), quote(1, 2 * T, "100.00", "100.02")]
    script = Script(events, [row(0, T)], [Act(0)], latency_ns=T, window_end_ns=2 * T)
    result = attribute_run(inputs(script))
    assert result.accounts["strategy"].document["orders"]["outcomes"]["expired"] == "1"
    ended, _ = script.run()
    # The same order claimed as executed on the quote at the window's end is refused.
    script.window_end_ns = None
    _, executed = script.run()
    with pytest.raises(DiagnosticsError, match="first quote"):
        attribute_run(inputs(script, manifest=ended, journal=executed))


def test_events_join_only_within_their_own_dataset_and_stream() -> None:
    script = round_trip()
    built = inputs(script)
    # Another recording with the same ingest sequences.
    other = market_events(script.events, dataset="f" * 64)
    with pytest.raises(DiagnosticsError, match="not of its own dataset"):
        attribute_run(type(built)(built.run_id, built.manifest, built.journal, other, built.schedule))
    wrong_stream = [dict(event, stream="another-stream") for event in script.events]
    with pytest.raises(DiagnosticsError, match="not on the dataset's stream"):
        market_events(wrong_stream)
    repeated = [*script.events, quote(3, 5 * T, "1", "2")]
    with pytest.raises(DiagnosticsError, match="appears twice"):
        market_events(repeated)
    manifest = copy.deepcopy(dict(built.manifest))
    manifest["source"]["instrument"] = "SPY"
    with pytest.raises(DiagnosticsError, match="not of its own dataset, stream, and instrument"):
        attribute_run(inputs(script, manifest=manifest))


def _tampered(script: Script, change: Any) -> DiagnosticsError:
    _, journal = script.run()
    tampered = copy.deepcopy(journal)
    change(tampered)
    with pytest.raises(DiagnosticsError) as refused:
        attribute_run(inputs(script, journal=tampered))
    return refused.value


def _first(journal: list[dict[str, Any]], kind: str, account: str = "strategy") -> dict[str, Any]:
    return next(entry for entry in journal if entry["type"] == kind and entry.get("account") == account)


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda j: _first(j, "decision").__setitem__("account", "paper-1"), "not one of this run's"),
        (
            lambda j: _first(j, "decision")["signal"].__setitem__("ingest_sequence", "1"),
            "differs from its schedule row",
        ),
        (
            lambda j: _first(j, "decision")["signal"].__setitem__("ingest_sequence", "99"),
            "not a row of the run's schedule",
        ),
        (
            lambda j: _first(j, "decision")["signal"].__setitem__("class", "down"),
            "differs from its schedule row",
        ),
        (lambda j: _first(j, "order").__setitem__("decision", "7"), "did not make"),
        (
            lambda j: _first(j, "order", "buy_and_hold").__setitem__("client_order_id", "strategy-1"),
            "resolves",
        ),
        (lambda j: j.insert(3, copy.deepcopy(_first(j, "order"))), "did not schedule exactly this one order"),
        (lambda j: j.insert(1, copy.deepcopy(_first(j, "decision"))), "appears twice"),
        (lambda j: j.insert(-1, copy.deepcopy(_first(j, "execution"))), "resolved twice"),
        (lambda j: j.remove(_first(j, "execution")), "has no execution or expiry"),
        (
            lambda j: _first(j, "execution").__setitem__("client_order_id", "strategy-9"),
            "not an order of strategy",
        ),
        (lambda j: _first(j, "execution").__setitem__("outcome", "faulted"), "unknown outcome"),
        (lambda j: _first(j, "execution")["fill"].pop("fee"), "malformed fill"),
        (lambda j: _first(j, "execution").__setitem__("cash_after", "1"), "cash or position after it"),
        (
            lambda j: _first(j, "execution")["observation"]["bid"].__setitem__("price", "99"),
            "differs from the dataset",
        ),
        (lambda j: _first(j, "order").__setitem__("arrival_at_ns", "1"), "configured latency"),
        (lambda j: j.append(copy.deepcopy(_first(j, "decision"))), "follows the window_end valuation"),
        (lambda j: _first(j, "decision").__setitem__("type", "note"), "unknown type"),
    ],
)
def test_wrong_duplicate_missing_and_malformed_links_are_refused(change: Any, message: str) -> None:
    refused = _tampered(round_trip(), change)
    assert message in str(refused)
    assert refused.reason in ("join_failed", "artifact_invalid")


def test_the_manifest_must_agree_with_its_journal() -> None:
    script = round_trip()
    manifest, _ = script.run()
    changed = copy.deepcopy(manifest)
    changed["results"]["accounts"]["strategy"]["orders"]["filled"] = "1"
    with pytest.raises(DiagnosticsError, match="order counts"):
        attribute_run(inputs(script, manifest=changed))
    changed = copy.deepcopy(manifest)
    changed["results"]["decisions"]["orders"] = "3"
    with pytest.raises(DiagnosticsError, match="decision counts"):
        attribute_run(inputs(script, manifest=changed))
    changed = copy.deepcopy(manifest)
    changed["results"]["accounts"]["strategy"]["final"]["ledger"]["fees_paid"] = "0.03"
    with pytest.raises(DiagnosticsError, match="final ledger"):
        attribute_run(inputs(script, manifest=changed))


def test_a_reconciliation_that_fails_is_refused_not_published() -> None:
    values: dict[str, Any] = {
        "booked_net_pnl": Fraction(1),
        "midpoint_inventory_pnl": Fraction(1),
        "execution_spread_cost": Fraction(0),
        "fees": Fraction(0),
        "terminal_bid_markdown": Fraction(0),
        "rounding_adjustment": Fraction(0),
        "observed_midpoint_movement": Fraction(0),
        "signal_reference_pnl": Fraction(1),
        "inventory_pnl_at_bid": Fraction(1),
    }
    assert _reconciliation(values, "r", "strategy")["booked_net_pnl"] == {"state": "reconciled"}
    with pytest.raises(DiagnosticsError) as refused:
        _reconciliation({**values, "fees": Fraction(1, 10**8)}, "r", "strategy")
    assert refused.value.reason == "reconciliation_failed"
    with pytest.raises(DiagnosticsError):
        _reconciliation({**values, "observed_midpoint_movement": Fraction(1)}, "r", "strategy")
    missing = _reconciliation({**values, "terminal_bid_markdown": Missing("x")}, "r", "strategy")
    assert missing["booked_net_pnl"] == {"state": "reconciled_at_bid"}


def test_engine_decimals_and_diagnostic_decimals_are_strict_and_exact() -> None:
    assert engine_decimal("-0.77", 8, "x") == Fraction(-77, 100)
    assert engine_decimal("92233720368.54775807", 8, "x") == Fraction(2**63 - 1, 10**8)
    for invalid in ("1e3", "+1", "01", "-0", "0.10", "", "1.", ".5", "0.000000001", None, 1):
        with pytest.raises(DiagnosticsError):
            engine_decimal(invalid, 8, "x")
    assert diagnostic_text(Fraction(0)) == "0"
    assert diagnostic_text(Fraction(-1, 10**18)) == "-0.000000000000000001"
    assert diagnostic_text(Fraction(5, 10**18) * 3) == "0.000000000000000015"
    assert diagnostic_text(Fraction(10**30)) == "1" + "0" * 30
    for inexact in (Fraction(1, 3), Fraction(1, 10**19)):
        with pytest.raises(DiagnosticsError) as refused:
            diagnostic_text(inexact)
        assert refused.value.reason == "arithmetic_invalid"
