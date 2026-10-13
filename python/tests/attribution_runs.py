"""Hand-built strategy runs in the engine's journal and manifest format, for
the attribution tests: no engine, no licensed data.

`Market` holds one dataset's events and one schedule; `Script` replays it
the way chronos_strategy does for a long/flat policy whose decisions the
test names: an order arrives `latency_ns` after its signal and executes on
the first top-of-book event of the instrument received at or after that,
whatever its quality; a buy fills at the ask and a sell at the bid, up to
the displayed size; the notional is price x quantity rounded half away
from zero to 10^-8; every fill pays the fee; an order with no quote left in
the window expires; and the window's end values every account at the
instrument's latest quote's bid, unless that quote is suspect, stale, or
has no bid. Buy and hold buys the target once, at the first decision.

Tests compute their expected figures by hand; this only writes records. A
test can book a ledger the engine could not: another rounding of the
notional or the final market value, or a fee other than the configured one,
applied consistently to every cash and position figure, so that only the
execution rules can refuse it.
"""

from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from typing import Any

from chronos_research.strategy_attribution import MarketEvents, RunInputs, ScheduleRow, schedule_row

STREAM = "synthetic-diagnostics-2026-10-12"
DATASET = "d" * 64
RUN = "e" * 64
INSTRUMENT = "AAPL"
RECORD = {"name": "chronos.strategy_journal_entry", "version": "1"}
SUSPECT = ("replay", "potentially_invalid_book")


def text(value: Fraction | int | str, places: int) -> str:
    """A value as the engine writes it: canonical, at most `places` places."""
    value = Fraction(value)
    scaled = value * 10**places
    assert scaled.denominator == 1, f"{value} needs more than {places} places"
    whole, fraction = divmod(abs(scaled.numerator), 10**places)
    written = str(whole) + ("." + str(fraction).rjust(places, "0").rstrip("0") if fraction else "")
    return f"-{written}" if scaled.numerator < 0 else written


def rounded_money(value: Fraction) -> Fraction:
    """Half away from zero to 10^-8, as the engine's calculate_notional."""
    scaled = abs(value) * 10**8
    whole = scaled.numerator // scaled.denominator
    if scaled - whole >= Fraction(1, 2):
        whole += 1
    return Fraction(whole if value >= 0 else -whole, 10**8)


def ties_toward_zero(value: Fraction) -> Fraction:
    """A wrong rounding: to 10^-8, but a tie goes toward zero."""
    scaled = abs(value) * 10**8
    whole = scaled.numerator // scaled.denominator
    if scaled - whole > Fraction(1, 2):
        whole += 1
    return Fraction(whole if value >= 0 else -whole, 10**8)


def raw(value: str | None, scale: int) -> str | None:
    return None if value is None else str(int(Fraction(value) * scale))


def quote(
    sequence: int,
    at: int,
    bid: str | None,
    ask: str | None,
    *,
    bid_size: str = "10",
    ask_size: str = "10",
    instrument: str = INSTRUMENT,
    quality: Sequence[str] = ("replay",),
    stream: str = STREAM,
) -> dict[str, Any]:
    def level(price: str | None, size: str) -> dict[str, str] | None:
        if price is None:
            return None
        return {"price_raw": str(raw(price, 10**9)), "quantity_raw": str(raw(size, 10**8))}

    return {
        "ask": level(ask, ask_size),
        "bid": level(bid, bid_size),
        "event_timestamp_ns": str(at),
        "ingest_sequence": str(sequence),
        "instrument": instrument,
        "quality": list(quality),
        "receive_timestamp_ns": str(at),
        "stream": stream,
        "type": "top_of_book",
        "venue": None,
        "venue_sequence": None,
        "version": "1",
    }


def trade(sequence: int, at: int, price: str, *, instrument: str = INSTRUMENT) -> dict[str, Any]:
    return {
        "aggressor_side": "buy",
        "event_timestamp_ns": str(at),
        "ingest_sequence": str(sequence),
        "instrument": instrument,
        "price_raw": str(raw(price, 10**9)),
        "quality": ["replay"],
        "quantity_raw": "100000000",
        "receive_timestamp_ns": str(at),
        "stream": STREAM,
        "type": "trade",
        "venue": None,
        "venue_sequence": None,
        "version": "1",
    }


def row(
    sequence: int, at: int, predicted: str | None = "up", confidence: int = 600_000_000
) -> dict[str, Any]:
    prediction: dict[str, Any] = (
        {"unavailable": "feature_unavailable:missing_ask"}
        if predicted is None
        else {"class": predicted, "confidence_ppb": str(confidence), "probabilities": ["0.2", "0.2", "0.6"]}
    )
    return {
        "available_at_ns": str(at),
        "identity": {"ingest_sequence": str(sequence), "instrument": INSTRUMENT, "stream": STREAM},
        "inputs": {"raw": ["0"]} if predicted is not None else {"unavailable": "missing_ask"},
        "prediction": prediction,
        "record": {"name": "chronos.inference_row", "version": "1"},
    }


@dataclass(frozen=True)
class Act:
    """One strategy decision at a schedule row: its reason, and the order it
    schedules, if any."""

    sequence: int
    reason: str = "order_scheduled"
    side: str | None = "buy"
    quantity: str = "1"


@dataclass
class _Account:
    name: str
    cash: Fraction
    position: Fraction = Fraction(0)
    fees: Fraction = Fraction(0)
    fills: int = 0
    orders: int = 0
    pending: dict[str, Any] | None = None
    outcomes: dict[str, int] = field(default_factory=dict)
    rejections: dict[str, int] = field(default_factory=dict)


@dataclass
class Script:
    events: list[dict[str, Any]]
    rows: list[dict[str, Any]]
    acts: list[Act]
    latency_ns: int = 1_000
    fee: str = "0.01"
    starting_cash: str = "10000"
    target: str = "1"
    maximum_quote_age_ns: int = 5_000_000_000
    window_end_ns: int | None = None
    # The trading status from this receive time on, if halted.
    halted_from_ns: int | None = None
    # What the ledger books instead of the engine's rules, when a test needs
    # a ledger the engine could not write: the notional's and the market
    # value's rounding, and the fee each fill pays (the manifest keeps `fee`).
    notional_rounding: Callable[[Fraction], Fraction] | None = None
    value_rounding: Callable[[Fraction], Fraction] | None = None
    booked_fee: str | None = None

    def run(self) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        acts = {act.sequence: act for act in self.acts}
        accounts = {
            name: _Account(name, Fraction(self.starting_cash))
            for name in ("strategy", "buy_and_hold", "cash")
        }
        journal: list[dict[str, Any]] = []
        window_end = (
            self.window_end_ns
            if self.window_end_ns is not None
            else max(int(event["receive_timestamp_ns"]) for event in self.events) + 1
        )
        latest: dict[str, Any] | None = None
        decisions = 0
        last_at = 0
        counts = dict.fromkeys(
            ("already_at_target", "hold_below_threshold", "hold_flat_prediction", "order_pending", "orders"),
            0,
        )
        rows = {int(item["identity"]["ingest_sequence"]): item for item in self.rows}
        for event in self.events:
            at = int(event["receive_timestamp_ns"])
            if at >= window_end:
                break
            last_at = at
            is_quote = event["type"] == "top_of_book" and event["instrument"] == INSTRUMENT
            if not is_quote:
                continue
            latest = event
            for name in ("strategy", "buy_and_hold"):
                account = accounts[name]
                if account.pending is not None and at >= int(account.pending["arrival_at_ns"]):
                    journal.append(self._execute(account, event))
            sequence = int(event["ingest_sequence"])
            act = acts.get(sequence)
            if act is None:
                continue
            decisions += 1
            scheduled = rows[sequence]
            signal = {
                "available_at_ns": scheduled["available_at_ns"],
                "class": scheduled["prediction"]["class"],
                "confidence_ppb": scheduled["prediction"]["confidence_ppb"],
                "ingest_sequence": str(sequence),
            }
            counts["orders" if act.reason == "order_scheduled" else act.reason] += 1
            strategy = accounts["strategy"]
            journal.append(
                {
                    "account": "strategy",
                    "decision": str(decisions),
                    "position_before": text(strategy.position, 8),
                    "reason": act.reason,
                    "record": RECORD,
                    "signal": signal,
                    "target": None if act.side is None else (self.target if act.side == "buy" else "0"),
                    "type": "decision",
                }
            )
            if act.reason == "order_scheduled":
                assert act.side is not None
                journal.append(self._schedule(strategy, act.side, act.quantity, at, decisions))
            if decisions == 1:
                journal.append(
                    {
                        "account": "buy_and_hold",
                        "decision": "1",
                        "position_before": "0",
                        "reason": "benchmark_entry",
                        "record": RECORD,
                        "signal": signal,
                        "target": self.target,
                        "type": "decision",
                    }
                )
                journal.append(self._schedule(accounts["buy_and_hold"], "buy", self.target, at, decisions))
        for account in accounts.values():
            if account.pending is not None:
                account.outcomes["expired"] = account.outcomes.get("expired", 0) + 1
                journal.append(
                    {
                        "account": account.name,
                        "arrival_at_ns": account.pending["arrival_at_ns"],
                        "client_order_id": account.pending["client_order_id"],
                        "reason": "window_ended_before_an_eligible_observation",
                        "record": RECORD,
                        "type": "order_expired",
                    }
                )
                account.pending = None
        valuations = {name: self._valuation(account, latest, last_at) for name, account in accounts.items()}
        journal.append(
            {
                "accounts": valuations,
                "at_ns": str(last_at),
                "record": RECORD,
                "trigger": "window_end",
                "type": "valuation",
            }
        )
        manifest = {
            "artifact_id": RUN,
            "configuration": {
                "execution": {
                    "fee_per_fill": self.fee,
                    "latency_ns": str(self.latency_ns),
                    "maximum_order_notional": "1000000",
                    "maximum_quote_age_ns": str(self.maximum_quote_age_ns),
                    "order_type": "market",
                    "time_in_force": "immediate_or_cancel",
                },
                "starting_cash": self.starting_cash,
            },
            "results": {
                "accounts": {
                    name: self._results(account, valuations[name]) for name, account in accounts.items()
                },
                "decisions": {
                    **{key: str(value) for key, value in counts.items()},
                    "evaluated": str(decisions),
                    "skipped_decision_interval": "0",
                    "skipped_prediction_unavailable": "0",
                },
                "valued_at_ns": str(last_at),
            },
            "source": {"dataset_id": DATASET, "instrument": INSTRUMENT, "stream": STREAM},
            "window": {
                "end_at_ns": str(window_end),
                "name": "fold_1",
                "start_at_ns": self.events[0]["receive_timestamp_ns"],
            },
        }
        return manifest, journal

    def _schedule(
        self, account: _Account, side: str, quantity: str, at: int, decision: int
    ) -> dict[str, Any]:
        account.orders += 1
        account.pending = {
            "account": account.name,
            "arrival_at_ns": str(at + self.latency_ns),
            "client_order_id": f"{account.name}-{account.orders}",
            "decision": str(decision),
            "quantity": quantity,
            "record": RECORD,
            "side": side,
            "signal_at_ns": str(at),
            "type": "order",
        }
        return dict(account.pending)

    def _execute(self, account: _Account, event: dict[str, Any]) -> dict[str, Any]:
        order = account.pending
        assert order is not None
        account.pending = None
        side = order["side"]
        level = event["ask"] if side == "buy" else event["bid"]
        fill = None
        rejection = None
        cancellation = None
        at = int(event["receive_timestamp_ns"])
        if self.halted_from_ns is not None and at >= self.halted_from_ns:
            outcome = "rejected"
            rejection = {"by": "runner", "code": "trading_halted", "message": "halted"}
            account.rejections["trading_halted"] = account.rejections.get("trading_halted", 0) + 1
        elif "potentially_invalid_book" in event["quality"] or event["bid"] is None or event["ask"] is None:
            outcome = "rejected"
            code = "suspect_market_data" if event["bid"] and event["ask"] else "missing_top_of_book_side"
            rejection = {"by": "pre_trade_risk", "code": code, "message": "refused"}
            account.rejections[code] = account.rejections.get(code, 0) + 1
        else:
            price = Fraction(int(level["price_raw"]), 10**9)
            displayed = Fraction(int(level["quantity_raw"]), 10**8)
            wanted = Fraction(order["quantity"])
            quantity = min(wanted, displayed)
            if quantity == 0:
                outcome, cancellation = "cancelled", "insufficient_liquidity"
            else:
                outcome = "filled" if quantity == wanted else "partially_filled_and_cancelled"
                if outcome != "filled":
                    cancellation = "insufficient_liquidity"
                notional = (self.notional_rounding or rounded_money)(price * quantity)
                fee = Fraction(self.fee if self.booked_fee is None else self.booked_fee)
                sign = 1 if side == "buy" else -1
                account.cash -= sign * notional + fee
                account.position += sign * quantity
                account.fees += fee
                account.fills += 1
                fill = {
                    "fee": text(fee, 8),
                    "fill_id": f"{order['client_order_id']}-fill",
                    "notional": text(notional, 8),
                    "price": text(price, 9),
                    "quantity": text(quantity, 8),
                }
        account.outcomes[outcome] = account.outcomes.get(outcome, 0) + 1

        def level_json(item: dict[str, Any] | None) -> dict[str, str] | None:
            if item is None:
                return None
            return {
                "price": text(Fraction(int(item["price_raw"]), 10**9), 9),
                "quantity": text(Fraction(int(item["quantity_raw"]), 10**8), 8),
            }

        return {
            "account": account.name,
            "arrival_at_ns": order["arrival_at_ns"],
            "cancellation_reason": cancellation,
            "cash_after": text(account.cash, 8),
            "client_order_id": order["client_order_id"],
            "executed_at_ns": event["receive_timestamp_ns"],
            "fill": fill,
            "observation": {
                "ask": level_json(event["ask"]),
                "bid": level_json(event["bid"]),
                "ingest_sequence": event["ingest_sequence"],
                "receive_timestamp_ns": event["receive_timestamp_ns"],
                "suspect": "potentially_invalid_book" in event["quality"],
            },
            "outcome": outcome,
            "position_after": text(account.position, 8),
            "record": RECORD,
            "rejection": rejection,
            "signal_at_ns": order["signal_at_ns"],
            "type": "execution",
        }

    def _valuation(self, account: _Account, latest: dict[str, Any] | None, at: int) -> dict[str, Any]:
        mark = None
        market_value = Fraction(0)
        if account.position != 0:
            usable = (
                latest is not None
                and latest["bid"] is not None
                and "potentially_invalid_book" not in latest["quality"]
                and at - int(latest["receive_timestamp_ns"]) <= self.maximum_quote_age_ns
            )
            if not usable:
                return {
                    "unavailable": {
                        "code": "position_mark_unavailable",
                        "instruments": [
                            {
                                "instrument": INSTRUMENT,
                                "message": "unusable",
                                "reason": "awaiting_fresh_quote",
                            }
                        ],
                        "message": "a position cannot be marked",
                    }
                }
            assert latest is not None
            bid = Fraction(int(latest["bid"]["price_raw"]), 10**9)
            market_value = (self.value_rounding or rounded_money)(bid * account.position)
            mark = {
                "bid": text(bid, 9),
                "ingest_sequence": latest["ingest_sequence"],
                "receive_timestamp_ns": latest["receive_timestamp_ns"],
            }
        equity = account.cash + market_value
        net = equity - Fraction(self.starting_cash)
        return {
            "cash": text(account.cash, 8),
            "equity": text(equity, 8),
            "fees_paid": text(account.fees, 8),
            "gross_realized_pnl": "0",
            "gross_unrealized_pnl": text(net + account.fees, 8),
            "mark": mark,
            "market_value": text(market_value, 8),
            "net_total_pnl": text(net, 8),
            "position": text(account.position, 8),
            "return_percent": "0",
        }

    def _results(self, account: _Account, valuation: dict[str, Any]) -> dict[str, Any]:
        return {
            "fills": str(account.fills),
            "final": {
                "ledger": {
                    "cash": text(account.cash, 8),
                    "fees_paid": text(account.fees, 8),
                    "gross_realized_pnl": "0",
                    "open_cost_basis": "0",
                    "position": text(account.position, 8),
                },
                "valuation": valuation,
            },
            "orders": {
                "cancelled": str(account.outcomes.get("cancelled", 0)),
                "expired": str(account.outcomes.get("expired", 0)),
                "filled": str(account.outcomes.get("filled", 0)),
                "partially_filled_and_cancelled": str(
                    account.outcomes.get("partially_filled_and_cancelled", 0)
                ),
                "rejected": str(account.outcomes.get("rejected", 0)),
                "rejections": {key: str(value) for key, value in sorted(account.rejections.items())},
                "scheduled": str(account.orders),
            },
        }


def tie(mode: str, latency_ns: int = 1_000_000) -> Script:
    """One fill, or one held position, whose exact amount is a tie at 10^-8:
    buying 1 at a 2.000000005 ask, selling 0.5 at a 100.00000001 bid, or
    holding 1 marked at a 2.000000005 bid."""
    if mode == "sell":
        events = [
            quote(index, (index + 1) * latency_ns, "100.00000001", "100.00000002") for index in range(4)
        ]
        acts = [Act(0, quantity="0.5"), Act(2, side="sell", quantity="0.5")]
        return Script(
            events,
            [row(i, (i + 1) * latency_ns) for i in range(4)],
            acts,
            latency_ns=latency_ns,
            target="0.5",
        )
    bid, ask = ("2", "2.000000005") if mode == "buy" else ("2.000000005", "2.000000015")
    events = [quote(index, (index + 1) * latency_ns, bid, ask) for index in range(3)]
    return Script(events, [row(i, (i + 1) * latency_ns) for i in range(3)], [Act(0)], latency_ns=latency_ns)


def market_events(
    events: Sequence[dict[str, Any]], dataset: str = DATASET, stream: str = STREAM
) -> MarketEvents:
    market = MarketEvents(dataset, stream, INSTRUMENT)
    for position, event in enumerate(events, start=1):
        market.add(event, f"event {position}")
    return market


def schedule(rows: Sequence[dict[str, Any]]) -> dict[int, ScheduleRow]:
    parsed = [schedule_row(item, STREAM, INSTRUMENT, f"row {n}") for n, item in enumerate(rows, start=1)]
    return {item.ingest_sequence: item for item in parsed}


def inputs(
    script: Script,
    manifest: dict[str, Any] | None = None,
    journal: list[dict[str, Any]] | None = None,
    events: Sequence[dict[str, Any]] | None = None,
) -> RunInputs:
    """The run's inputs, optionally with its manifest, journal, or events
    replaced, so a test can tamper with one of them."""
    built_manifest, built_journal = script.run()
    return RunInputs(
        run_id=RUN,
        manifest=manifest if manifest is not None else built_manifest,
        journal=journal if journal is not None else built_journal,
        events=market_events(events if events is not None else script.events),
        schedule=schedule(script.rows),
    )
