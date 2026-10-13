"""Exact attribution of one strategy run, from its journal, its dataset's
events, and its schedule (ADR 0024).

These are pure functions over parsed records; strategy_diagnostics.py reads
and verifies the files, and the independent auditor recomputes everything
without this code. Every figure is a Fraction, exact: prices are 10^-9,
quantities and money 10^-8, and a midpoint can be half a price unit, so
every figure lies on the 5 x 10^-18 lattice and is written with at most 18
fractional digits (`diagnostic_text`).

For fill i of an account, with q_i its signed quantity (+ buy, - sell), p_i
its price, n_i its booked notional signed the same way, f_i its fee, m_i the
midpoint of its execution observation, and s_i the midpoint of its
decision's signal quote; Q the final position, B the final mark's bid, M
that quote's midpoint, and V the booked final market value:

    midpoint_inventory_pnl     = Q*M - sum q_i*m_i
    execution_spread_cost      = sum q_i*(p_i - m_i)
    fees                       = sum f_i
    terminal_bid_markdown      = Q*(M - B)
    rounding_adjustment        = (V - Q*B) - sum (n_i - q_i*p_i)
    booked_net_pnl             = midpoint_inventory_pnl - execution_spread_cost - fees
                                 - terminal_bid_markdown + rounding_adjustment
    observed_midpoint_movement = sum q_i*(m_i - s_i)      (positive = adverse)
    signal_reference_pnl       = Q*M - sum q_i*s_i
    midpoint_inventory_pnl     = signal_reference_pnl - observed_midpoint_movement

booked_net_pnl is the engine's own final net_total_pnl, and the rounding
adjustment has its own definition, so both identities are checks: a run
whose figures do not reconcile exactly is refused. A balanced ledger is not
enough: every booked notional n_i and the final market value V must be the
engine's own rounding of p_i*q_i and B*Q (half away from zero to 10^-8),
and every fee the run's frozen fee_per_fill, or the run is refused. A
figure that needs a value that does not exist is Missing with the reason,
never a substitute.

Joins use complete identities. An event is an ingest sequence of the run's
own dataset (one stream), and it must be a top-of-book event of the run's
instrument wherever a quote is needed; a decision's signal is its schedule
row and that row's quote; an order is (account, decision); its one
resolution is (account, client order ID). Anything else is refused.
"""

import bisect
import re
from collections import Counter
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from typing import Any

PRICE_SCALE = 10**9
QUANTITY_SCALE = 10**8
PRICE_PLACES = 9
MONEY_PLACES = 8
QUANTITY_PLACES = 8
DIAGNOSTIC_PLACES = 18
ACCOUNTS = ("strategy", "buy_and_hold", "cash")
JOURNAL_RECORD = {"name": "chronos.strategy_journal_entry", "version": "1"}
ENTRY_TYPES = ("decision", "order", "execution", "order_expired", "valuation")
OUTCOMES = ("filled", "partially_filled_and_cancelled", "cancelled", "rejected")
FILLED = ("filled", "partially_filled_and_cancelled")
DECISION_REASONS = (
    "already_at_target",
    "hold_below_threshold",
    "hold_flat_prediction",
    "order_pending",
    "order_scheduled",
)
BENCHMARK_ENTRY = "benchmark_entry"
EXPIRY_REASON = "window_ended_before_an_eligible_observation"
SUSPECT_FLAGS = frozenset({"potentially_invalid_book", "questionable_receive_timestamp"})
CLASSES = ("down", "flat", "up")
COMPONENTS = (
    "booked_net_pnl",
    "midpoint_inventory_pnl",
    "execution_spread_cost",
    "fees",
    "terminal_bid_markdown",
    "rounding_adjustment",
    "market_value_rounding",
    "notional_rounding",
    "observed_midpoint_movement",
    "signal_reference_pnl",
    "inventory_pnl_at_bid",
)
_DECIMAL = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]*[1-9])?")
_INTEGER = re.compile(r"0|[1-9][0-9]*")


class DiagnosticsError(Exception):
    """Diagnostics cannot be built from these records, and why."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def join_failed(message: str) -> DiagnosticsError:
    return DiagnosticsError("join_failed", message)


# --- Exact values -----------------------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Missing:
    """A figure that has no value, and why."""

    reason: str


type Figure = Fraction | Missing


def engine_decimal(value: Any, places: int, what: str) -> Fraction:
    """A canonical engine decimal (no exponent, trailing zeros, or negative
    zero) with at most `places` fractional digits, exactly."""
    if (
        not isinstance(value, str)
        or not _DECIMAL.fullmatch(value)
        or value == "-0"
        or len(value.partition(".")[2]) > places
    ):
        raise DiagnosticsError("artifact_invalid", f"{what} is not an engine decimal: {value!r}")
    return Fraction(value)


def unsigned(value: Any, what: str) -> int:
    if not isinstance(value, str) or not _INTEGER.fullmatch(value):
        raise DiagnosticsError("artifact_invalid", f"{what} is not an unsigned integer: {value!r}")
    return int(value)


def diagnostic_text(value: Fraction) -> str:
    """An exact figure as a canonical decimal of at most 18 places."""
    scaled = value * 10**DIAGNOSTIC_PLACES
    if scaled.denominator != 1:
        raise DiagnosticsError("arithmetic_invalid", f"{value} is not a multiple of 10^-{DIAGNOSTIC_PLACES}")
    whole, fraction = divmod(abs(scaled.numerator), 10**DIAGNOSTIC_PLACES)
    text = str(whole)
    if fraction:
        text += "." + str(fraction).rjust(DIAGNOSTIC_PLACES, "0").rstrip("0")
    return f"-{text}" if scaled.numerator < 0 else text


def money_rounding(value: Fraction) -> Fraction:
    """The engine's notional rounding: to 10^-8, half away from zero."""
    scaled = abs(value) * 10**MONEY_PLACES
    units = scaled.numerator // scaled.denominator
    if 2 * (scaled - units) >= 1:
        units += 1
    return Fraction(units if value >= 0 else -units, 10**MONEY_PLACES)


def figure_json(value: Figure) -> dict[str, str]:
    if isinstance(value, Missing):
        return {"unavailable": value.reason}
    return {"value": diagnostic_text(value)}


def first_missing(*values: Figure) -> Missing | None:
    return next((value for value in values if isinstance(value, Missing)), None)


def total(values: Sequence[Figure]) -> Figure:
    """The exact sum, or the first value that is missing."""
    missing = first_missing(*values)
    return (
        missing
        if missing is not None
        else sum((value for value in values if not isinstance(value, Missing)), Fraction(0))
    )


def difference(left: Figure, right: Figure) -> Figure:
    missing = first_missing(left, right)
    if missing is not None:
        return missing
    assert not isinstance(left, Missing)
    assert not isinstance(right, Missing)
    return left - right


# --- Market events ----------------------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Level:
    price: Fraction
    quantity: Fraction


@dataclass(frozen=True, slots=True)
class Quote:
    """A top-of-book event of the run's instrument."""

    ingest_sequence: int
    receive_ns: int
    bid: Level | None
    ask: Level | None
    suspect: bool

    def midpoint(self) -> Figure:
        if self.bid is None:
            return Missing("bid_missing")
        if self.ask is None:
            return Missing("ask_missing")
        return (self.bid.price + self.ask.price) / 2

    def quoted_spread(self) -> Figure:
        if self.bid is None:
            return Missing("bid_missing")
        if self.ask is None:
            return Missing("ask_missing")
        return self.ask.price - self.bid.price


@dataclass
class MarketEvents:
    """One dataset's events as the diagnostics join them: the instrument's
    top-of-book events by ingest sequence, in ingest order, and what every
    other sequence is, so a reference to it can say why it is refused."""

    dataset_id: str
    stream: str
    instrument: str
    quotes: dict[int, Quote] = field(default_factory=dict)
    others: dict[int, str] = field(default_factory=dict)
    _times: list[int] = field(default_factory=list)
    _sequences: list[int] = field(default_factory=list)

    def add(self, record: Mapping[str, Any], where: str) -> None:
        if record.get("stream") != self.stream:
            raise join_failed(f"{where} is not on the dataset's stream {self.stream}")
        sequence = unsigned(record.get("ingest_sequence"), f"{where} ingest_sequence")
        if sequence in self.quotes or sequence in self.others:
            raise join_failed(
                f"{where}: ingest sequence {sequence} appears twice in dataset {self.dataset_id}"
            )
        kind, instrument = record.get("type"), record.get("instrument")
        if kind != "top_of_book" or instrument != self.instrument:
            self.others[sequence] = f"a {kind} event of {instrument}"
            return
        receive = unsigned(record.get("receive_timestamp_ns"), f"{where} receive_timestamp_ns")
        if self._times and receive < self._times[-1]:
            raise join_failed(f"{where}: receive times move backward")
        quality = record.get("quality")
        if not isinstance(quality, list):
            raise DiagnosticsError("artifact_invalid", f"{where} has no quality flags")
        quote = Quote(
            ingest_sequence=sequence,
            receive_ns=receive,
            bid=_raw_level(record.get("bid"), f"{where} bid"),
            ask=_raw_level(record.get("ask"), f"{where} ask"),
            suspect=bool(SUSPECT_FLAGS.intersection(quality)),
        )
        self.quotes[sequence] = quote
        self._times.append(receive)
        self._sequences.append(sequence)

    def quote(self, sequence: int, what: str) -> Quote:
        """The instrument's top-of-book event at this sequence of this dataset."""
        found = self.quotes.get(sequence)
        if found is not None:
            return found
        other = self.others.get(sequence)
        detail = f"is {other}" if other is not None else "is not in the dataset"
        raise join_failed(
            f"{what} names ingest sequence {sequence} of dataset {self.dataset_id}, which {detail}, "
            f"not a top-of-book event of {self.instrument}"
        )

    def first_at_or_after(self, at_ns: int, end_ns: int) -> Quote | None:
        """The first quote of the instrument received at or after `at_ns` and
        before the window's exclusive end, in ingest order."""
        index = bisect.bisect_left(self._times, at_ns)
        if index == len(self._times) or self._times[index] >= end_ns:
            return None
        return self.quotes[self._sequences[index]]


def _raw_level(level: Any, what: str) -> Level | None:
    if level is None:
        return None
    if not isinstance(level, dict):
        raise DiagnosticsError("artifact_invalid", f"{what} is not a book level")
    return Level(
        Fraction(unsigned(level.get("price_raw"), f"{what} price"), PRICE_SCALE),
        Fraction(unsigned(level.get("quantity_raw"), f"{what} quantity"), QUANTITY_SCALE),
    )


def _journal_level(level: Any, what: str) -> Level | None:
    if level is None:
        return None
    if not isinstance(level, dict) or set(level) != {"price", "quantity"}:
        raise DiagnosticsError("artifact_invalid", f"{what} is not a book level")
    return Level(
        engine_decimal(level["price"], PRICE_PLACES, f"{what} price"),
        engine_decimal(level["quantity"], QUANTITY_PLACES, f"{what} quantity"),
    )


# --- Schedule rows ----------------------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class ScheduleRow:
    ingest_sequence: int
    available_at_ns: int
    predicted_class: str | None
    confidence_ppb: int | None


def schedule_row(record: Mapping[str, Any], stream: str, instrument: str, where: str) -> ScheduleRow:
    identity = record.get("identity")
    if not isinstance(identity, dict) or (identity.get("stream"), identity.get("instrument")) != (
        stream,
        instrument,
    ):
        raise join_failed(f"{where} is not a row of {stream} {instrument}")
    prediction = record.get("prediction")
    if not isinstance(prediction, dict):
        raise DiagnosticsError("artifact_invalid", f"{where} has no prediction")
    predicted = prediction.get("class")
    if "unavailable" in prediction:
        predicted, confidence = None, None
    elif predicted not in CLASSES:
        raise DiagnosticsError("artifact_invalid", f"{where} predicts an unknown class")
    else:
        confidence = unsigned(prediction.get("confidence_ppb"), f"{where} confidence_ppb")
    return ScheduleRow(
        ingest_sequence=unsigned(identity.get("ingest_sequence"), f"{where} ingest_sequence"),
        available_at_ns=unsigned(record.get("available_at_ns"), f"{where} available_at_ns"),
        predicted_class=predicted,
        confidence_ppb=confidence,
    )


# --- One run ----------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class RunInputs:
    run_id: str
    # The verified run manifest and its journal's entries, in order.
    manifest: Mapping[str, Any]
    journal: Sequence[Mapping[str, Any]]
    events: MarketEvents
    # The run's schedule rows by ingest sequence.
    schedule: Mapping[int, ScheduleRow]


@dataclass(frozen=True)
class Decision:
    """A journaled strategy decision, joined to its signal and its order."""

    number: int
    ingest_sequence: int
    predicted_class: str
    confidence_ppb: int
    reason: str
    # The outcome of the order it scheduled, if it scheduled one.
    order_outcome: str | None


@dataclass(frozen=True)
class AccountFigures:
    """One account's exact figures, kept as values so they can be pooled."""

    components: Mapping[str, Figure]
    document: dict[str, Any]


@dataclass(frozen=True)
class RunAttribution:
    accounts: Mapping[str, AccountFigures]
    fills: list[dict[str, Any]]
    episodes: list[dict[str, Any]]
    decisions: list[Decision]
    decision_counts: dict[str, Any]


@dataclass
class _Order:
    entry: Mapping[str, Any]
    decision: Mapping[str, Any]
    signal: Quote
    resolution: Mapping[str, Any] | None = None


@dataclass
class _Fill:
    order: _Order
    execution: Mapping[str, Any]
    quote: Quote
    signed_quantity: Fraction
    price: Fraction
    signed_notional: Fraction
    fee: Fraction


def _check(condition: bool, message: str) -> None:
    if not condition:
        raise join_failed(message)


def _rule(condition: bool, message: str) -> None:
    """A booked figure that the engine's execution rules could not produce."""
    if not condition:
        raise DiagnosticsError("ledger_invalid", message)


def _account_of(entry: Mapping[str, Any], where: str) -> str:
    account = entry.get("account")
    if account not in ACCOUNTS:
        raise join_failed(f"{where} names an account that is not one of this run's: {account!r}")
    return str(account)


class _Journal:
    """The journal's entries joined into decisions, orders, and resolutions."""

    def __init__(self, inputs: RunInputs) -> None:
        self.inputs = inputs
        source = inputs.manifest["source"]
        self.instrument = str(source["instrument"])
        self.stream = str(source["stream"])
        _check(
            (inputs.events.stream, inputs.events.instrument, inputs.events.dataset_id)
            == (self.stream, self.instrument, source["dataset_id"]),
            f"run {inputs.run_id}'s events are not of its own dataset, stream, and instrument",
        )
        execution = inputs.manifest["configuration"]["execution"]
        self.latency_ns = unsigned(execution["latency_ns"], "latency_ns")
        self.fee_per_fill = engine_decimal(execution["fee_per_fill"], MONEY_PLACES, "fee_per_fill")
        window = inputs.manifest["window"]
        self.window_start_ns = unsigned(window["start_at_ns"], "window start")
        self.window_end_ns = unsigned(window["end_at_ns"], "window end")
        self.decisions: dict[tuple[str, int], Mapping[str, Any]] = {}
        self.signals: dict[tuple[str, int], Quote] = {}
        self.orders: dict[tuple[str, str], _Order] = {}
        self.ordered: set[tuple[str, int]] = set()
        self.sequence: list[tuple[str, Mapping[str, Any]]] = []
        self.final: Mapping[str, Any] | None = None
        for position, entry in enumerate(inputs.journal, start=1):
            self._entry(entry, f"run {inputs.run_id} journal entry {position}")
        unresolved = [key for key, order in self.orders.items() if order.resolution is None]
        _check(not unresolved, f"run {inputs.run_id}: order {unresolved[:1]} has no execution or expiry")
        _check(self.final is not None, f"run {inputs.run_id}'s journal has no window_end valuation")

    def _entry(self, entry: Mapping[str, Any], where: str) -> None:
        if not isinstance(entry, dict) or entry.get("record") != JOURNAL_RECORD:
            raise DiagnosticsError("artifact_invalid", f"{where} is not a strategy journal entry")
        kind = entry.get("type")
        if kind not in ENTRY_TYPES:
            raise join_failed(f"{where} has an unknown type {kind!r}")
        _check(self.final is None, f"{where} follows the window_end valuation")
        if kind == "valuation":
            if entry.get("trigger") == "window_end":
                self.final = entry
            return
        account = _account_of(entry, where)
        if kind == "decision":
            self._decision(entry, account, where)
        elif kind == "order":
            self._order(entry, account, where)
        else:
            self._resolution(entry, account, where, kind)
        self.sequence.append((str(kind), entry))

    def _decision(self, entry: Mapping[str, Any], account: str, where: str) -> None:
        number = unsigned(entry.get("decision"), f"{where} decision")
        reason = entry.get("reason")
        if account == "strategy":
            _check(reason in DECISION_REASONS, f"{where} has an unknown reason {reason!r}")
        else:
            _check(
                account == "buy_and_hold" and reason == BENCHMARK_ENTRY,
                f"{where}: only the strategy decides, and buy and hold enters once",
            )
        key = (account, number)
        _check(key not in self.decisions, f"{where}: decision {number} of {account} appears twice")
        signal = entry.get("signal")
        if not isinstance(signal, dict):
            raise join_failed(f"{where} has no signal")
        sequence = unsigned(signal["ingest_sequence"], f"{where} signal ingest_sequence")
        row = self.inputs.schedule.get(sequence)
        _check(
            row is not None,
            f"{where}'s signal names ingest sequence {sequence}, which is not a row of the run's schedule",
        )
        assert row is not None
        _check(
            row.available_at_ns == unsigned(signal["available_at_ns"], f"{where} signal time")
            and row.predicted_class == signal.get("class")
            and row.confidence_ppb == unsigned(signal["confidence_ppb"], f"{where} signal confidence"),
            f"{where}'s signal differs from its schedule row at ingest sequence {sequence}",
        )
        self.signals[key] = self.inputs.events.quote(sequence, f"{where}'s signal")
        self.decisions[key] = entry

    def _order(self, entry: Mapping[str, Any], account: str, where: str) -> None:
        number = unsigned(entry.get("decision"), f"{where} decision")
        decision = self.decisions.get((account, number))
        _check(decision is not None, f"{where} names decision {number}, which {account} did not make")
        assert decision is not None
        _check(
            decision["reason"] in ("order_scheduled", BENCHMARK_ENTRY)
            and (account, number) not in self.ordered,
            f"{where}: decision {number} of {account} did not schedule exactly this one order",
        )
        identifier = entry.get("client_order_id")
        _check(isinstance(identifier, str) and identifier != "", f"{where} has no client order ID")
        key = (account, str(identifier))
        _check(key not in self.orders, f"{where}: client order {identifier} of {account} appears twice")
        signal = self.signals[(account, number)]
        signal_at = unsigned(entry.get("signal_at_ns"), f"{where} signal_at_ns")
        arrival = unsigned(entry.get("arrival_at_ns"), f"{where} arrival_at_ns")
        _check(
            signal_at == signal.receive_ns,
            f"{where}'s signal time is not its signal quote's receive time",
        )
        _check(
            arrival == signal_at + self.latency_ns, f"{where} does not arrive the configured latency later"
        )
        _check(entry.get("side") in ("buy", "sell"), f"{where} has no side")
        engine_decimal(entry.get("quantity"), QUANTITY_PLACES, f"{where} quantity")
        self.orders[key] = _Order(entry, decision, signal)
        self.ordered.add((account, number))

    def _resolution(self, entry: Mapping[str, Any], account: str, where: str, kind: Any) -> None:
        identifier = entry.get("client_order_id")
        order = self.orders.get((account, str(identifier)))
        _check(order is not None, f"{where} resolves {identifier!r}, which is not an order of {account}")
        assert order is not None
        _check(order.resolution is None, f"{where}: order {identifier} of {account} is resolved twice")
        _check(
            entry.get("arrival_at_ns") == order.entry["arrival_at_ns"],
            f"{where}'s arrival differs from its order's",
        )
        arrival = unsigned(order.entry["arrival_at_ns"], "arrival")
        first = self.inputs.events.first_at_or_after(arrival, self.window_end_ns)
        if kind == "order_expired":
            _check(entry.get("reason") == EXPIRY_REASON, f"{where} expires for an unknown reason")
            _check(
                first is None, f"{where} expires although a quote of {self.instrument} followed its arrival"
            )
        else:
            _check(entry.get("signal_at_ns") == order.entry["signal_at_ns"], f"{where}'s signal time differs")
            observation = entry.get("observation")
            _check(isinstance(observation, dict), f"{where} has no observation")
            assert isinstance(observation, dict)
            sequence = unsigned(observation.get("ingest_sequence"), f"{where} observation")
            quote = self.inputs.events.quote(sequence, f"{where}'s observation")
            _check(
                _journal_level(observation.get("bid"), f"{where} bid") == quote.bid
                and _journal_level(observation.get("ask"), f"{where} ask") == quote.ask
                and unsigned(observation.get("receive_timestamp_ns"), f"{where} receive") == quote.receive_ns
                and observation.get("suspect") is quote.suspect,
                f"{where}'s observation differs from the dataset's event at ingest sequence {sequence}",
            )
            _check(
                unsigned(entry.get("executed_at_ns"), f"{where} executed_at_ns") == quote.receive_ns,
                f"{where} did not execute at its observation's receive time",
            )
            _check(
                first is not None and first.ingest_sequence == sequence,
                f"{where} did not execute on the first quote of {self.instrument} at or after its arrival",
            )
            outcome = entry.get("outcome")
            _check(outcome in OUTCOMES, f"{where} has an unknown outcome {outcome!r}")
            _check(
                (entry.get("fill") is not None) == (outcome in FILLED)
                and (entry.get("rejection") is not None) == (outcome == "rejected"),
                f"{where}'s fill or rejection does not match its outcome {outcome}",
            )
        order.resolution = entry


def attribute_run(inputs: RunInputs) -> RunAttribution:
    """Every account's exact attribution, every fill and holding episode, and
    the strategy's joined decisions."""
    journal = _Journal(inputs)
    manifest = inputs.manifest
    configuration = manifest["configuration"]
    starting = engine_decimal(configuration["starting_cash"], MONEY_PLACES, "starting_cash")
    results = manifest["results"]
    valued_at = unsigned(results["valued_at_ns"], "valued_at_ns")
    accounts: dict[str, AccountFigures] = {}
    fills_out: list[dict[str, Any]] = []
    episodes_out: list[dict[str, Any]] = []
    assert journal.final is not None
    for account in ACCOUNTS:
        fills = _account_fills(journal, account, starting)
        reported = results["accounts"][account]
        _check(
            journal.final["accounts"][account] == reported["final"]["valuation"],
            f"run {inputs.run_id}: the journal's final valuation of {account} differs from the manifest's",
        )
        figures, final_mark = _attribution(inputs, journal, account, fills, starting, reported)
        episodes = _episodes(account, fills, reported, valued_at)
        accounts[account] = AccountFigures(
            components=figures,
            document={
                "booked": _booked(reported, configuration["starting_cash"]),
                "components": {name: figure_json(figures[name]) for name in COMPONENTS},
                "episodes": _episode_summary(episodes),
                "fills": str(len(fills)),
                "final_mark": final_mark,
                "orders": _orders(journal, account, reported),
                "reconciliation": _reconciliation(figures, inputs.run_id, account),
                "quoted_spreads": _quoted_spreads(fills),
                "timing": _timing(journal, account),
            },
        )
        fills_out.extend(_fill_row(journal, fill) for fill in fills)
        episodes_out.extend(episode for episode, _ in episodes)
    decisions, counts = _decisions(journal, results["decisions"], inputs.run_id)
    return RunAttribution(accounts, fills_out, episodes_out, decisions, counts)


def _account_fills(journal: _Journal, account: str, starting: Fraction) -> list[_Fill]:
    fills: list[_Fill] = []
    cash, position = starting, Fraction(0)
    for kind, entry in journal.sequence:
        if kind != "execution" or entry["account"] != account:
            continue
        order = journal.orders[(account, entry["client_order_id"])]
        where = f"run {journal.inputs.run_id} {account} execution of {entry['client_order_id']}"
        fill = entry["fill"]
        if fill is not None:
            _check(
                isinstance(fill, dict) and set(fill) == {"fee", "fill_id", "notional", "price", "quantity"},
                f"{where} has a malformed fill",
            )
            sign = 1 if order.entry["side"] == "buy" else -1
            quantity = engine_decimal(fill["quantity"], QUANTITY_PLACES, f"{where} quantity")
            notional = engine_decimal(fill["notional"], MONEY_PLACES, f"{where} notional")
            fee = engine_decimal(fill["fee"], MONEY_PLACES, f"{where} fee")
            price = engine_decimal(fill["price"], PRICE_PLACES, f"{where} price")
            _check(
                quantity > 0 and notional > 0 and price > 0 and fee >= 0, f"{where} is not a positive fill"
            )
            _check(
                quantity
                <= engine_decimal(order.entry["quantity"], QUANTITY_PLACES, f"{where} order quantity"),
                f"{where} fills more than its order",
            )
            _rule(
                notional == money_rounding(price * quantity),
                f"{where}: notional {fill['notional']} is not price x quantity rounded half away from zero "
                f"to 10^-8 ({diagnostic_text(money_rounding(price * quantity))})",
            )
            _rule(
                fee == journal.fee_per_fill,
                f"{where}: fee {fill['fee']} is not the run's fee_per_fill "
                f"{diagnostic_text(journal.fee_per_fill)}",
            )
            quote = journal.inputs.events.quote(
                unsigned(entry["observation"]["ingest_sequence"], f"{where} observation"), where
            )
            fills.append(_Fill(order, entry, quote, sign * quantity, price, sign * notional, fee))
            cash -= sign * notional + fee
            position += sign * quantity
        _check(
            engine_decimal(entry["cash_after"], MONEY_PLACES, f"{where} cash_after") == cash
            and engine_decimal(entry["position_after"], QUANTITY_PLACES, f"{where} position_after")
            == position,
            f"{where}: cash or position after it differs from its fills",
        )
    return fills


def _attribution(
    inputs: RunInputs,
    journal: _Journal,
    account: str,
    fills: Sequence[_Fill],
    starting: Fraction,
    reported: Mapping[str, Any],
) -> tuple[dict[str, Figure], dict[str, Any] | None]:
    where = f"run {inputs.run_id} {account}"
    ledger = reported["final"]["ledger"]
    fees = sum((fill.fee for fill in fills), Fraction(0))
    quantity = sum((fill.signed_quantity for fill in fills), Fraction(0))
    cash = starting - sum((fill.signed_notional for fill in fills), Fraction(0)) - fees
    _check(
        engine_decimal(ledger["cash"], MONEY_PLACES, f"{where} cash") == cash
        and engine_decimal(ledger["position"], QUANTITY_PLACES, f"{where} position") == quantity
        and engine_decimal(ledger["fees_paid"], MONEY_PLACES, f"{where} fees") == fees
        and unsigned(reported["fills"], f"{where} fills") == len(fills),
        f"{where}: the final ledger differs from its journal's fills",
    )
    execution_midpoints = [fill.quote.midpoint() for fill in fills]
    signal_midpoints = [fill.order.signal.midpoint() for fill in fills]
    by_execution = total(
        [
            _times(fill.signed_quantity, midpoint, "execution_midpoint_unavailable")
            for fill, midpoint in zip(fills, execution_midpoints, strict=True)
        ]
    )
    by_signal = total(
        [
            _times(fill.signed_quantity, midpoint, "signal_midpoint_unavailable")
            for fill, midpoint in zip(fills, signal_midpoints, strict=True)
        ]
    )
    spread = total(
        [
            _times(fill.signed_quantity, difference(fill.price, midpoint), "execution_midpoint_unavailable")
            for fill, midpoint in zip(fills, execution_midpoints, strict=True)
        ]
    )
    notional_rounding = sum(
        (fill.signed_notional - fill.signed_quantity * fill.price for fill in fills), Fraction(0)
    )
    valuation = reported["final"]["valuation"]
    final_mark: dict[str, Any] | None = None
    if "unavailable" in valuation:
        unavailable = Missing(f"final_valuation_unavailable:{valuation['unavailable']['code']}")
        booked: Figure = unavailable
        market_value: Figure = unavailable
        at_bid: Figure = unavailable if quantity != 0 else Fraction(0)
        at_midpoint: Figure = unavailable if quantity != 0 else Fraction(0)
        final_mark = {"unavailable": unavailable.reason}
    else:
        booked = engine_decimal(valuation["net_total_pnl"], MONEY_PLACES, f"{where} net_total_pnl")
        market_value = engine_decimal(valuation["market_value"], MONEY_PLACES, f"{where} market_value")
        equity = engine_decimal(valuation["equity"], MONEY_PLACES, f"{where} equity")
        _check(
            engine_decimal(valuation["cash"], MONEY_PLACES, f"{where} valuation cash") == cash
            and engine_decimal(valuation["position"], QUANTITY_PLACES, f"{where} valuation position")
            == quantity
            and equity == cash + market_value
            and booked == equity - starting,
            f"{where}: the final valuation is not its ledger's cash, market value, and equity",
        )
        if quantity == 0:
            _check(market_value == 0, f"{where}: a flat account has a market value")
            at_bid = at_midpoint = Fraction(0)
        else:
            mark = valuation["mark"]
            _check(isinstance(mark, dict), f"{where}: an open position's valuation has no mark")
            quote = inputs.events.quote(
                unsigned(mark["ingest_sequence"], f"{where} mark"), f"{where}'s final mark"
            )
            bid = engine_decimal(mark["bid"], PRICE_PLACES, f"{where} mark bid")
            _check(
                quote.bid is not None
                and quote.bid.price == bid
                and quote.receive_ns == unsigned(mark["receive_timestamp_ns"], f"{where} mark time"),
                f"{where}'s final mark differs from the dataset's event at its ingest sequence",
            )
            _rule(
                market_value == money_rounding(quantity * bid),
                f"{where}: market value {valuation['market_value']} is not the position x the bid rounded "
                f"half away from zero to 10^-8 ({diagnostic_text(money_rounding(quantity * bid))})",
            )
            midpoint = quote.midpoint()
            at_bid = quantity * bid
            at_midpoint = (
                Missing(f"final_midpoint_unavailable:{midpoint.reason}")
                if isinstance(midpoint, Missing)
                else quantity * midpoint
            )
            final_mark = {
                "ask": None if quote.ask is None else _price_text(quote.ask.price),
                "bid": mark["bid"],
                "ingest_sequence": str(quote.ingest_sequence),
                "midpoint": figure_json(midpoint),
                "receive_at_ns": str(quote.receive_ns),
            }
    market_value_rounding = difference(market_value, at_bid)
    figures: dict[str, Figure] = {
        "booked_net_pnl": booked,
        "midpoint_inventory_pnl": difference(at_midpoint, by_execution),
        "execution_spread_cost": spread,
        "fees": fees,
        "terminal_bid_markdown": difference(at_midpoint, at_bid),
        "rounding_adjustment": difference(market_value_rounding, notional_rounding),
        "market_value_rounding": market_value_rounding,
        "notional_rounding": notional_rounding,
        "observed_midpoint_movement": difference(by_execution, by_signal),
        "signal_reference_pnl": difference(at_midpoint, by_signal),
        "inventory_pnl_at_bid": difference(at_bid, by_execution),
    }
    return figures, final_mark


def _times(quantity: Fraction, value: Figure, reason: str) -> Figure:
    if isinstance(value, Missing):
        return Missing(f"{reason}:{value.reason}")
    return quantity * value


def _price_text(value: Fraction) -> str:
    return diagnostic_text(value)


def _reconciliation(figures: Mapping[str, Figure], run_id: str, account: str) -> dict[str, Any]:
    """Both identities, checked exactly where every term exists."""
    where = f"run {run_id} {account}"
    booked = figures["booked_net_pnl"]
    terms = [figures[name] for name in ("execution_spread_cost", "fees", "rounding_adjustment")]
    at_midpoint = [figures["midpoint_inventory_pnl"], figures["terminal_bid_markdown"]]
    document: dict[str, Any] = {}
    missing = first_missing(booked, *terms)
    if missing is None and first_missing(*at_midpoint) is None:
        explained = (
            _value(figures["midpoint_inventory_pnl"])
            - _value(figures["execution_spread_cost"])
            - _value(figures["fees"])
            - _value(figures["terminal_bid_markdown"])
            + _value(figures["rounding_adjustment"])
        )
        if explained != booked:
            raise DiagnosticsError(
                "reconciliation_failed", f"{where}: the attribution does not sum to its net PnL"
            )
        document["booked_net_pnl"] = {"state": "reconciled"}
    elif missing is None and not isinstance(figures["inventory_pnl_at_bid"], Missing):
        explained = (
            _value(figures["inventory_pnl_at_bid"])
            - _value(figures["execution_spread_cost"])
            - _value(figures["fees"])
            + _value(figures["rounding_adjustment"])
        )
        if explained != booked:
            raise DiagnosticsError(
                "reconciliation_failed", f"{where}: the attribution at the bid does not sum"
            )
        document["booked_net_pnl"] = {"state": "reconciled_at_bid"}
    else:
        reason = first_missing(booked, *terms, figures["inventory_pnl_at_bid"])
        assert reason is not None
        document["booked_net_pnl"] = {"unavailable": reason.reason}
    signal_terms = (
        figures["midpoint_inventory_pnl"],
        figures["signal_reference_pnl"],
        figures["observed_midpoint_movement"],
    )
    missing = first_missing(*signal_terms)
    if missing is None:
        if _value(signal_terms[0]) != _value(signal_terms[1]) - _value(signal_terms[2]):
            raise DiagnosticsError("reconciliation_failed", f"{where}: the signal-reference identity fails")
        document["signal_reference"] = {"state": "reconciled"}
    else:
        document["signal_reference"] = {"unavailable": missing.reason}
    return document


def _value(figure: Figure) -> Fraction:
    assert not isinstance(figure, Missing)
    return figure


def _booked(reported: Mapping[str, Any], starting_cash: str) -> dict[str, Any]:
    ledger, valuation = reported["final"]["ledger"], reported["final"]["valuation"]
    booked = {
        "cash": ledger["cash"],
        "fees_paid": ledger["fees_paid"],
        "final_quantity": ledger["position"],
        "starting_cash": starting_cash,
    }
    if "unavailable" in valuation:
        return {**booked, "valuation": {"unavailable": valuation["unavailable"]["code"]}}
    return {
        **booked,
        "valuation": {
            key: valuation[key] for key in ("equity", "market_value", "net_total_pnl", "return_percent")
        },
    }


def _orders(journal: _Journal, account: str, reported: Mapping[str, Any]) -> dict[str, Any]:
    """Orders by outcome, cancellation reason, and rejection code, from the
    journal, which must agree with the manifest's counts."""
    outcomes: Counter[str] = Counter()
    cancellations: Counter[str] = Counter()
    rejections: Counter[str] = Counter()
    scheduled = 0
    for (owner, _), order in journal.orders.items():
        if owner != account:
            continue
        scheduled += 1
        resolution = order.resolution
        assert resolution is not None
        if resolution["type"] == "order_expired":
            outcomes["expired"] += 1
            continue
        outcomes[str(resolution["outcome"])] += 1
        if resolution["cancellation_reason"] is not None:
            cancellations[str(resolution["cancellation_reason"])] += 1
        if resolution["rejection"] is not None:
            rejection = resolution["rejection"]
            rejections[f"{rejection['by']}:{rejection['code']}"] += 1
    counts = reported["orders"]
    expected = {
        "cancelled": outcomes["cancelled"],
        "expired": outcomes["expired"],
        "filled": outcomes["filled"],
        "partially_filled_and_cancelled": outcomes["partially_filled_and_cancelled"],
        "rejected": outcomes["rejected"],
        "scheduled": scheduled,
    }
    by_code = Counter({key.partition(":")[2]: value for key, value in rejections.items()})
    _check(
        {key: unsigned(counts[key], key) for key in expected} == expected
        and {key: unsigned(value, key) for key, value in counts["rejections"].items()} == dict(by_code),
        f"run {journal.inputs.run_id} {account}: the manifest's order counts differ from its journal",
    )
    return {
        "cancellation_reasons": {key: str(value) for key, value in sorted(cancellations.items())},
        "outcomes": {key: str(value) for key, value in expected.items()},
        "rejections": {key: str(value) for key, value in sorted(rejections.items())},
    }


_TIMINGS = (
    "signal_to_arrival_duration_ns",
    "arrival_to_execution_duration_ns",
    "signal_to_execution_duration_ns",
)


def _timings(order: _Order, execution: Mapping[str, Any]) -> dict[str, int]:
    signal = unsigned(order.entry["signal_at_ns"], "signal_at_ns")
    arrival = unsigned(order.entry["arrival_at_ns"], "arrival_at_ns")
    executed = unsigned(execution["executed_at_ns"], "executed_at_ns")
    return {
        "arrival_to_execution_duration_ns": executed - arrival,
        "signal_to_arrival_duration_ns": arrival - signal,
        "signal_to_execution_duration_ns": executed - signal,
    }


def _timing(journal: _Journal, account: str) -> dict[str, Any]:
    """The configured latency, and what was observed between each order's
    signal, its arrival, and its execution."""
    observed: dict[str, list[int]] = {name: [] for name in _TIMINGS}
    expired = 0
    for (owner, _), order in journal.orders.items():
        if owner != account:
            continue
        assert order.resolution is not None
        if order.resolution["type"] == "order_expired":
            expired += 1
            continue
        for name, value in _timings(order, order.resolution).items():
            observed[name].append(value)
    document: dict[str, Any] = {
        "configured_latency_ns": str(journal.latency_ns),
        "executions": str(len(observed[_TIMINGS[0]])),
        "expired": str(expired),
    }
    for name, values in observed.items():
        document[name] = (
            {"maximum": str(max(values)), "minimum": str(min(values)), "sum": str(sum(values))}
            if values
            else {"unavailable": "no_executions"}
        )
    return document


def _spread_statistics(values: Sequence[Figure]) -> dict[str, Any]:
    present = [value for value in values if not isinstance(value, Missing)]
    if not present:
        return {"unavailable": "no_fills_with_both_sides"}
    return {
        "fills": str(len(present)),
        "maximum": diagnostic_text(max(present)),
        "minimum": diagnostic_text(min(present)),
        "sum": diagnostic_text(sum(present, Fraction(0))),
        "without_both_sides": str(len(values) - len(present)),
    }


def _quoted_spreads(fills: Sequence[_Fill]) -> dict[str, Any]:
    """Ask minus bid of each fill's execution quote and signal quote: what the
    midpoint of each was taken between. No threshold makes a quote wide."""
    return {
        "execution": _spread_statistics([fill.quote.quoted_spread() for fill in fills]),
        "signal": _spread_statistics([fill.order.signal.quoted_spread() for fill in fills]),
    }


def _fill_row(journal: _Journal, fill: _Fill) -> dict[str, Any]:
    order, execution = fill.order, fill.execution
    signal_midpoint = order.signal.midpoint()
    execution_midpoint = fill.quote.midpoint()
    decision = order.decision["signal"]
    return {
        "account": execution["account"],
        "cash_after": execution["cash_after"],
        "client_order_id": execution["client_order_id"],
        "contributions": {
            "execution_spread_cost": figure_json(
                _times(
                    fill.signed_quantity,
                    difference(fill.price, execution_midpoint),
                    "execution_midpoint_unavailable",
                )
            ),
            "notional_rounding": figure_json(fill.signed_notional - fill.signed_quantity * fill.price),
            "observed_midpoint_movement": figure_json(
                _times(
                    fill.signed_quantity,
                    difference(execution_midpoint, signal_midpoint),
                    "midpoint_unavailable",
                )
            ),
        },
        "decision": order.entry["decision"],
        "execution": {
            "ask": execution["observation"]["ask"],
            "bid": execution["observation"]["bid"],
            "executed_at_ns": execution["executed_at_ns"],
            "ingest_sequence": execution["observation"]["ingest_sequence"],
            "midpoint": figure_json(execution_midpoint),
            "quoted_spread": figure_json(fill.quote.quoted_spread()),
        },
        "fee": execution["fill"]["fee"],
        "fill_id": execution["fill"]["fill_id"],
        "notional": execution["fill"]["notional"],
        "outcome": execution["outcome"],
        "position_after": execution["position_after"],
        "price": execution["fill"]["price"],
        "quantity": execution["fill"]["quantity"],
        "side": order.entry["side"],
        "signal": {
            "available_at_ns": decision["available_at_ns"],
            "class": decision["class"],
            "confidence_ppb": decision["confidence_ppb"],
            "ask": None if order.signal.ask is None else diagnostic_text(order.signal.ask.price),
            "bid": None if order.signal.bid is None else diagnostic_text(order.signal.bid.price),
            "ingest_sequence": decision["ingest_sequence"],
            "midpoint": figure_json(signal_midpoint),
            "quoted_spread": figure_json(order.signal.quoted_spread()),
            "signal_at_ns": order.entry["signal_at_ns"],
        },
        "timing": {
            "configured_latency_ns": str(journal.latency_ns),
            **{name: str(value) for name, value in _timings(order, execution).items()},
        },
    }


def _episodes(
    account: str, fills: Sequence[_Fill], reported: Mapping[str, Any], valued_at_ns: int
) -> list[tuple[dict[str, Any], Figure]]:
    """Holding episodes: from a fill that leaves a flat position to the fill
    that makes it flat again, or censored at the window's end. Each with
    its PnL: booked for a completed episode, marked at the final valuation
    for a censored one."""
    episodes: list[tuple[dict[str, Any], Figure]] = []
    position = Fraction(0)
    current: list[_Fill] = []
    valuation = reported["final"]["valuation"]

    def close(open_at_end: bool) -> None:
        entry_at = unsigned(current[0].execution["executed_at_ns"], "executed_at_ns")
        cash_flow = -sum((fill.signed_notional + fill.fee for fill in current), Fraction(0))
        spread = total(
            [
                _times(
                    fill.signed_quantity,
                    difference(fill.price, fill.quote.midpoint()),
                    "execution_midpoint_unavailable",
                )
                for fill in current
            ]
        )
        if open_at_end:
            end_at = valued_at_ns
            pnl: Figure = (
                Missing(f"final_valuation_unavailable:{valuation['unavailable']['code']}")
                if "unavailable" in valuation
                else cash_flow + engine_decimal(valuation["market_value"], MONEY_PLACES, "market_value")
            )
        else:
            end_at = unsigned(current[-1].execution["executed_at_ns"], "executed_at_ns")
            pnl = cash_flow
        maximum = Fraction(0)
        running = Fraction(0)
        for fill in current:
            running += fill.signed_quantity
            maximum = max(maximum, running)
        episodes.append(
            (
                {
                    "account": account,
                    "basis": "marked_at_final_valuation" if open_at_end else "booked",
                    "bought": diagnostic_text(
                        sum((f.signed_quantity for f in current if f.signed_quantity > 0), Fraction(0))
                    ),
                    "cash_flow": figure_json(cash_flow),
                    "censored_at_ns": str(end_at) if open_at_end else None,
                    "duration_ns": str(end_at - entry_at),
                    "entry_at_ns": str(entry_at),
                    "episode": str(len(episodes) + 1),
                    "execution_spread_cost": figure_json(spread),
                    "exit_at_ns": None if open_at_end else str(end_at),
                    "fees": figure_json(sum((fill.fee for fill in current), Fraction(0))),
                    "fills": str(len(current)),
                    "maximum_position": diagnostic_text(maximum),
                    "pnl": figure_json(pnl),
                    "sold": diagnostic_text(
                        -sum((f.signed_quantity for f in current if f.signed_quantity < 0), Fraction(0))
                    ),
                    "state": "censored" if open_at_end else "completed",
                },
                pnl,
            )
        )

    for fill in fills:
        current.append(fill)
        position += fill.signed_quantity
        _check(position >= 0, f"{account} sold more than it held")
        if position == 0:
            close(open_at_end=False)
            current = []
    if current:
        close(open_at_end=True)
    return episodes


def _episode_summary(episodes: Sequence[tuple[dict[str, Any], Figure]]) -> dict[str, Any]:
    completed = [(episode, pnl) for episode, pnl in episodes if episode["state"] == "completed"]
    durations = [int(episode["duration_ns"]) for episode, _ in completed]
    return {
        "censored": str(len(episodes) - len(completed)),
        "completed": str(len(completed)),
        "completed_duration_ns": (
            {"maximum": str(max(durations)), "minimum": str(min(durations)), "sum": str(sum(durations))}
            if durations
            else {"unavailable": "no_completed_episodes"}
        ),
        "completed_pnl": figure_json(total([pnl for _, pnl in completed])),
        "completed_with_gain": str(
            sum(1 for _, pnl in completed if not isinstance(pnl, Missing) and pnl > 0)
        ),
        "completed_with_loss": str(
            sum(1 for _, pnl in completed if not isinstance(pnl, Missing) and pnl < 0)
        ),
    }


def _decisions(
    journal: _Journal, counts: Mapping[str, Any], run_id: str
) -> tuple[list[Decision], dict[str, Any]]:
    """The strategy's journaled decisions, each with its order's outcome,
    and their reasons, which must agree with the manifest's counts."""
    decisions: list[Decision] = []
    reasons: Counter[str] = Counter()
    outcomes = {
        int(order.entry["decision"]): (
            "expired" if order.resolution["type"] == "order_expired" else str(order.resolution["outcome"])
        )
        for (account, _), order in journal.orders.items()
        if account == "strategy" and order.resolution is not None
    }
    for (account, number), entry in journal.decisions.items():
        if account != "strategy":
            continue
        signal = entry["signal"]
        reasons[str(entry["reason"])] += 1
        decisions.append(
            Decision(
                number=number,
                ingest_sequence=int(signal["ingest_sequence"]),
                predicted_class=str(signal["class"]),
                confidence_ppb=int(signal["confidence_ppb"]),
                reason=str(entry["reason"]),
                order_outcome=outcomes.get(number),
            )
        )
    expected = {
        "already_at_target": reasons["already_at_target"],
        "evaluated": len(decisions),
        "hold_below_threshold": reasons["hold_below_threshold"],
        "hold_flat_prediction": reasons["hold_flat_prediction"],
        "order_pending": reasons["order_pending"],
        "orders": reasons["order_scheduled"],
    }
    _check(
        {key: unsigned(counts[key], key) for key in expected} == expected
        and sorted(number for number, _ in ((d.number, d) for d in decisions))
        == list(range(1, len(decisions) + 1)),
        f"run {run_id}: the manifest's decision counts differ from its journal",
    )
    return decisions, {
        "journaled": {reason: str(reasons[reason]) for reason in DECISION_REASONS},
        "skipped_decision_interval": counts["skipped_decision_interval"],
        "skipped_prediction_unavailable": counts["skipped_prediction_unavailable"],
        "evaluated": counts["evaluated"],
    }
