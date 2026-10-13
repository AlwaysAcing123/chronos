#!/usr/bin/env python3
"""Independent audit of strategy diagnostics (ADR 0024).

    python -I -S tools/strategy_diagnostics_audit/audit.py <artifacts> <diagnostic-id>

Standard library only, and nothing from Chronos: run it with `-I -S` so no
installed package can be used. From the raw files alone (each study, its
plan, every run's manifest and journal, every schedule's rows, and each
evaluation recording's normalized events, feature rows, and label rows) it
rebuilds the diagnostics' every figure, row, cohort, and pool, and compares
them field by field with the artifact:

- the artifact's identity, its files' checksums and row counts, and every
  source file's checksum;
- every fill: its order, decision, and signal quote; its execution quote,
  which must be the dataset's first top-of-book event of the instrument at
  or after the order's arrival; and its spread cost, midpoint movement, and
  notional rounding, exactly;
- every account: the ledger rebuilt from its fills against the engine's,
  each attribution component from the formulas of ADR 0024 in Fraction
  arithmetic, both identities, and its orders, timings, quoted spreads, and
  holding episodes;
- every decision cohort and scorable-prediction cohort, from label rows
  joined by (stream, instrument, ingest sequence) under the plan's scoring
  cutoff, with its fixed confidence bins, exact ratios, and its scored
  items' future returns (label return_raw) by predicted class and bin;
- every pool over folds and studies, and every strategy-minus-benchmark
  difference; a pool with no completed run has no financial figures.

Every source is authenticated before it is used: its manifest's identity
and schema, the checksum, size, and row count of the file consumed, and its
binding to the study, plan, collection member, and each other.

Prose (definitions, scope, verification) is not audited. It prints one JSON
report and exits 1 if anything differs or cannot be read.
"""

import hashlib
import json
import re
import sys
from collections import Counter
from fractions import Fraction
from pathlib import Path
from typing import Any

type Json = dict[str, Any]
type Key = tuple[str, str]
type Values = dict[str, dict[str, Any]]
# One fill: its execution, order, decision, signed quantity, price, signed
# notional, fee, execution quote's sequence, and signal quote's sequence.
type Leg = tuple[Json, Json, Json, Fraction, Fraction, Fraction, Fraction, int, int]

ACCOUNTS = ("strategy", "buy_and_hold", "cash")
MODELS = ("majority", "logistic")
CLASSES = ("down", "flat", "up")
REASONS = (
    "already_at_target",
    "hold_below_threshold",
    "hold_flat_prediction",
    "order_pending",
    "order_scheduled",
)
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
INPUTS = ("spread", "imbalance", "microprice", "midpoint", "bid_size", "ask_size")
SUSPECT = {"potentially_invalid_book", "questionable_receive_timestamp"}
CANONICAL = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]*[1-9])?")
BIN = 100_000_000


class Unreadable(Exception):
    pass


class Refused(Exception):
    """A source failed authentication; its problem is already recorded."""


class Audit:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.checks: Counter[str] = Counter()

    def check(self, condition: bool, category: str, what: str) -> None:
        self.checks[category] += 1
        if not condition:
            self.problems.append(f"{category}: {what}")

    def same(self, expected: Any, claimed: Any, category: str, where: str) -> None:
        """Every leaf compared, the first difference reported."""
        leaves = _leaves(expected)
        self.checks[category] += max(leaves, 1)
        if expected != claimed:
            self.problems.append(f"{category}: {where}{_where(expected, claimed)}")


def _leaves(value: Any) -> int:
    if isinstance(value, dict):
        return sum(_leaves(item) for item in value.values())
    if isinstance(value, list):
        return sum(_leaves(item) for item in value)
    return 1


def _where(expected: Any, claimed: Any, path: str = "") -> str:
    if isinstance(expected, dict) and isinstance(claimed, dict):
        for key in sorted(set(expected) | set(claimed)):
            if expected.get(key) != claimed.get(key):
                return _where(expected.get(key), claimed.get(key), f"{path}.{key}")
    if (
        isinstance(expected, list)
        and isinstance(claimed, list)
        and len(expected) == len(claimed)
    ):
        for index, (left, right) in enumerate(zip(expected, claimed, strict=True)):
            if left != right:
                return _where(left, right, f"{path}[{index}]")
    return f"{path}: expected {json.dumps(expected)[:160]}, found {json.dumps(claimed)[:160]}"


# --- Bytes and numbers ------------------------------------------------------------------------------------


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def spelled(document: Any) -> bytes:
    return json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode()


def identified(path: Path, key: str, newline: bool) -> Json:
    """A manifest whose canonical bytes, without its ID, hash to its ID."""
    data = path.read_bytes()
    document: Json = json.loads(data)
    if spelled(document) + b"\n" != data:
        raise Unreadable(f"{path} is not canonical")
    body = spelled({name: value for name, value in document.items() if name != key}) + (
        b"\n" if newline else b""
    )
    if sha256(body) != document.get(key) or document.get(key) != path.parent.name:
        raise Unreadable(f"{path}'s identity does not match its content and directory")
    return document


# --- Sources, authenticated before use ---------------------------------------------------------------------

SCHEMAS = {
    "collections": {"name": "chronos.research_collection", "version": "1"},
    "features": {"name": "chronos.feature_artifact", "version": "1"},
    "labels": {"name": "chronos.label_artifact", "version": "1"},
    "normalized": {"name": "chronos.normalized_market_dataset", "version": "1"},
    "schedules": {"name": "chronos.inference_schedule", "version": "3"},
    "strategies": {"name": "chronos.strategy_run", "version": "3"},
    "walk_forward_plans": {"name": "chronos.walk_forward_plan", "version": "1"},
    "walk_forward_studies": {"name": "chronos.walk_forward_study", "version": "1"},
}
# Each manifest's ID key, and whether its identity hashes its newline (the
# Python artifacts) or not (the C++ ones).
IDENTITIES = {
    "collections": ("collection_id", True),
    "features": ("artifact_id", False),
    "labels": ("artifact_id", False),
    "normalized": ("dataset_id", False),
    "schedules": ("artifact_id", True),
    "strategies": ("artifact_id", False),
    "walk_forward_plans": ("plan_id", True),
    "walk_forward_studies": ("study_id", True),
}


def refuse(audit: Audit, message: str) -> Refused:
    audit.check(False, "sources", message)
    return Refused(message)


def source(audit: Audit, root: Path, kind: str, identifier: Any) -> Json:
    """A source's manifest: canonical, its identity its content's and its
    directory's, and of the expected schema."""
    key, newline = IDENTITIES[kind]
    if not isinstance(identifier, str) or not re.fullmatch("[0-9a-f]{64}", identifier):
        raise refuse(audit, f"{kind}: {identifier!r} is not an artifact ID")
    try:
        document = identified(root / kind / identifier / "manifest.json", key, newline)
    except (Unreadable, OSError, ValueError) as error:
        raise refuse(audit, f"{kind} {identifier[:12]}: {error}") from error
    audit.check(
        document.get("schema") == SCHEMAS[kind],
        "sources",
        f"{kind} {identifier[:12]} is {SCHEMAS[kind]['name']}",
    )
    return document


def source_file(
    audit: Audit,
    root: Path,
    kind: str,
    manifest: Json,
    key: str,
    name: str,
    rows: Any = None,
) -> bytes:
    """A source's consumed file: the name, size, and checksum its manifest
    lists, and its row count where the manifest gives one."""
    identifier: str = manifest[IDENTITIES[kind][0]]
    digest = manifest.get("files", {}).get(key)
    if not isinstance(digest, dict) or digest.get("name") != name:
        raise refuse(audit, f"{kind} {identifier[:12]} does not list {name}")
    try:
        data = (root / kind / identifier / name).read_bytes()
    except OSError as error:
        raise refuse(
            audit, f"{kind} {identifier[:12]}/{name} cannot be read"
        ) from error
    audit.check(
        sha256(data) == digest.get("sha256")
        and str(len(data)) == digest.get("size_bytes"),
        "sources",
        f"{kind} {identifier[:12]}/{name} has its manifest's checksum and size",
    )
    if rows is not None:
        audit.check(
            str(len(data.splitlines())) == rows,
            "sources",
            f"{kind} {identifier[:12]}/{name} has its manifest's {rows} rows",
        )
    return data


def lines(data: bytes) -> list[Json]:
    return [json.loads(line) for line in data.splitlines()]


def decimal(text: Any) -> Fraction:
    if not isinstance(text, str) or not CANONICAL.fullmatch(text) or text == "-0":
        raise Unreadable(f"{text!r} is not a canonical decimal")
    return Fraction(text)


def engine_rounding(value: Fraction) -> Fraction:
    """The engine's notional and market value: to 10^-8, half away from zero,
    from the integer quotient and remainder."""
    numerator = abs(value.numerator) * 10**8
    units, remainder = divmod(numerator, value.denominator)
    if 2 * remainder >= value.denominator:
        units += 1
    return Fraction(-units if value < 0 else units, 10**8)


def written(value: Fraction) -> str:
    """At most 18 places, canonical."""
    scaled = value * 10**18
    if scaled.denominator != 1:
        raise Unreadable(f"{value} needs more than 18 places")
    units, rest = divmod(abs(scaled.numerator), 10**18)
    text = str(units) + ("." + str(rest).rjust(18, "0").rstrip("0") if rest else "")
    return "-" + text if scaled.numerator < 0 else text


class Gap:
    """A value that does not exist, and why."""

    def __init__(self, reason: str) -> None:
        self.reason = reason


def figure(value: "Fraction | Gap") -> Json:
    return (
        {"unavailable": value.reason}
        if isinstance(value, Gap)
        else {"value": written(value)}
    )


def minus(left: "Fraction | Gap", right: "Fraction | Gap") -> "Fraction | Gap":
    if isinstance(left, Gap):
        return left
    if isinstance(right, Gap):
        return right
    return left - right


def summed(values: list["Fraction | Gap"]) -> "Fraction | Gap":
    total = Fraction(0)
    for value in values:
        if isinstance(value, Gap):
            return value
        total += value
    return total


def scaled_by(
    quantity: Fraction, value: "Fraction | Gap", prefix: str
) -> "Fraction | Gap":
    return (
        Gap(f"{prefix}:{value.reason}") if isinstance(value, Gap) else quantity * value
    )


def six_places(value: Fraction) -> str:
    magnitude = abs(value) * 10**6
    units = magnitude.numerator // magnitude.denominator
    if magnitude - units >= Fraction(1, 2):
        units += 1
    whole, rest = divmod(units, 10**6)
    text = str(whole) + ("." + str(rest).rjust(6, "0").rstrip("0") if rest else "")
    return "-" + text if value < 0 and units else text


def fraction(numerator: int, denominator: int, undefined: str) -> Json:
    if denominator == 0:
        return {"undefined": undefined}
    value = Fraction(numerator, denominator)
    return {
        "denominator": str(value.denominator),
        "numerator": str(value.numerator),
        "rounded": six_places(value),
    }


# --- Market events ----------------------------------------------------------------------------------------


class Book:
    """One dataset's top-of-book events of one instrument, by ingest sequence."""

    def __init__(self, records: list[Json], stream: str, instrument: str) -> None:
        self.quotes: dict[int, Json] = {}
        self.kinds: dict[int, str] = {}
        self.order: list[int] = []
        for record in records:
            if record["stream"] != stream:
                raise Unreadable("an event is not on its dataset's stream")
            sequence = int(record["ingest_sequence"])
            if sequence in self.kinds:
                raise Unreadable(f"ingest sequence {sequence} repeats within a dataset")
            self.kinds[sequence] = f"{record['type']}:{record['instrument']}"
            if record["type"] == "top_of_book" and record["instrument"] == instrument:
                self.quotes[sequence] = record
                self.order.append(sequence)

    def side(self, sequence: int, name: str) -> Fraction | None:
        level = self.quotes[sequence][name]
        return None if level is None else Fraction(int(level["price_raw"]), 10**9)

    def size(self, sequence: int, name: str) -> Fraction | None:
        level = self.quotes[sequence][name]
        return None if level is None else Fraction(int(level["quantity_raw"]), 10**8)

    def mid(self, sequence: int) -> "Fraction | Gap":
        bid, ask = self.side(sequence, "bid"), self.side(sequence, "ask")
        if bid is None:
            return Gap("bid_missing")
        if ask is None:
            return Gap("ask_missing")
        return (bid + ask) / 2

    def spread(self, sequence: int) -> "Fraction | Gap":
        bid, ask = self.side(sequence, "bid"), self.side(sequence, "ask")
        if bid is None:
            return Gap("bid_missing")
        if ask is None:
            return Gap("ask_missing")
        return ask - bid

    def received(self, sequence: int) -> int:
        return int(self.quotes[sequence]["receive_timestamp_ns"])

    def first_from(self, at: int, end: int) -> int | None:
        for sequence in self.order:
            received = self.received(sequence)
            if received >= end:
                return None
            if received >= at:
                return sequence
        return None


# --- One run ----------------------------------------------------------------------------------------------


def run_figures(
    audit: Audit,
    where: str,
    manifest: Json,
    journal: list[Json],
    book: Book,
    rows: dict[int, Json],
) -> tuple[Json, list[Json], list[Json], list[Json], dict[str, dict[str, Any]]]:
    """The run's account documents, fill rows, episode rows, strategy
    decisions, and component values, rebuilt from its journal."""
    latency = int(manifest["configuration"]["execution"]["latency_ns"])
    fee_per_fill = decimal(manifest["configuration"]["execution"]["fee_per_fill"])
    starting = decimal(manifest["configuration"]["starting_cash"])
    end = int(manifest["window"]["end_at_ns"])
    valued_at = int(manifest["results"]["valued_at_ns"])
    decisions: dict[tuple[str, str], Json] = {}
    orders: dict[tuple[str, str], Json] = {}
    resolved: dict[tuple[str, str], Json] = {}
    executions: list[Json] = []
    final: Json | None = None
    for entry in journal:
        kind = entry["type"]
        if kind == "valuation":
            if entry["trigger"] == "window_end":
                final = entry
            continue
        key_account = entry["account"]
        audit.check(
            key_account in ACCOUNTS, "joins", f"{where}: unknown account {key_account}"
        )
        if kind == "decision":
            key = (key_account, entry["decision"])
            audit.check(key not in decisions, "joins", f"{where}: decision {key} twice")
            sequence = int(entry["signal"]["ingest_sequence"])
            row = rows.get(sequence)
            audit.check(
                row is not None
                and row["available_at_ns"] == entry["signal"]["available_at_ns"]
                and row["prediction"].get("class") == entry["signal"]["class"]
                and row["prediction"].get("confidence_ppb")
                == entry["signal"]["confidence_ppb"],
                "joins",
                f"{where}: decision {key}'s signal is not its schedule row",
            )
            audit.check(
                sequence in book.quotes,
                "joins",
                f"{where}: signal {sequence} is not a quote",
            )
            decisions[key] = entry
        elif kind == "order":
            key = (key_account, entry["client_order_id"])
            audit.check(key not in orders, "joins", f"{where}: order {key} twice")
            decision = decisions.get((key_account, entry["decision"]))
            audit.check(
                decision is not None, "joins", f"{where}: order {key} has no decision"
            )
            if decision is not None:
                signal = int(decision["signal"]["ingest_sequence"])
                audit.check(
                    int(entry["signal_at_ns"]) == book.received(signal)
                    and int(entry["arrival_at_ns"])
                    == int(entry["signal_at_ns"]) + latency,
                    "timing",
                    f"{where}: order {key}'s signal time or latency",
                )
            orders[key] = entry
        else:
            key = (key_account, entry["client_order_id"])
            audit.check(
                key in orders and key not in resolved,
                "joins",
                f"{where}: resolution of {key}",
            )
            resolved[key] = entry
            first = (
                book.first_from(int(orders[key]["arrival_at_ns"]), end)
                if key in orders
                else None
            )
            if kind == "order_expired":
                audit.check(
                    first is None,
                    "execution",
                    f"{where}: {key} expired with a quote left",
                )
            else:
                sequence = int(entry["observation"]["ingest_sequence"])
                audit.check(
                    first == sequence,
                    "execution",
                    f"{where}: {key} not on the first quote at or after arrival",
                )
                audit.check(
                    sequence in book.quotes
                    and int(entry["executed_at_ns"]) == book.received(sequence),
                    "execution",
                    f"{where}: {key}'s execution quote",
                )
                executions.append(entry)
    audit.check(
        final is not None and set(resolved) == set(orders),
        "joins",
        f"{where}: unresolved orders or no final valuation",
    )
    if final is None:
        raise Unreadable(f"{where} has no final valuation")
    documents: Json = {}
    fill_rows: list[Json] = []
    episode_rows: list[Json] = []
    values: dict[str, dict[str, Any]] = {}
    for account in ACCOUNTS:
        mine = [entry for entry in executions if entry["account"] == account]
        filled = [entry for entry in mine if entry["fill"] is not None]
        results = manifest["results"]["accounts"][account]
        audit.same(
            results["final"]["valuation"],
            final["accounts"][account],
            "ledger",
            f"{where} {account} final valuation",
        )
        cash, position, fees = starting, Fraction(0), Fraction(0)
        legs = []
        for entry in mine:
            order = orders[(account, entry["client_order_id"])]
            fill = entry["fill"]
            if fill is not None:
                sign = 1 if order["side"] == "buy" else -1
                quantity, price = decimal(fill["quantity"]), decimal(fill["price"])
                notional, fee = decimal(fill["notional"]), decimal(fill["fee"])
                sequence = int(entry["observation"]["ingest_sequence"])
                side = "ask" if sign == 1 else "bid"
                audit.check(
                    book.side(sequence, side) == price,
                    "execution",
                    f"{where}: {entry['client_order_id']} price is the {side}",
                )
                audit.check(
                    notional == engine_rounding(price * quantity) and notional > 0,
                    "execution",
                    f"{where}: {entry['client_order_id']} notional is price x quantity "
                    "rounded half away from zero to 10^-8",
                )
                audit.check(
                    fee == fee_per_fill,
                    "execution",
                    f"{where}: {entry['client_order_id']} fee is the run's fee_per_fill",
                )
                cash -= sign * notional + fee
                position += sign * quantity
                fees += fee
                decision = decisions[(account, order["decision"])]
                legs.append(
                    (
                        entry,
                        order,
                        decision,
                        sign * quantity,
                        price,
                        sign * notional,
                        fee,
                        sequence,
                        int(decision["signal"]["ingest_sequence"]),
                    )
                )
            audit.check(
                decimal(entry["cash_after"]) == cash
                and decimal(entry["position_after"]) == position,
                "ledger",
                f"{where}: {account} cash and position after {entry['client_order_id']}",
            )
        ledger = results["final"]["ledger"]
        audit.check(
            (
                decimal(ledger["cash"]),
                decimal(ledger["position"]),
                decimal(ledger["fees_paid"]),
                int(results["fills"]),
            )
            == (cash, position, fees, len(filled)),
            "ledger",
            f"{where}: {account} final ledger",
        )
        valuation = results["final"]["valuation"]
        exec_value = summed(
            [
                scaled_by(q, book.mid(seq), "execution_midpoint_unavailable")
                for (_, _, _, q, _, _, _, seq, _) in legs
            ]
        )
        signal_value = summed(
            [
                scaled_by(q, book.mid(sig), "signal_midpoint_unavailable")
                for (_, _, _, q, _, _, _, _, sig) in legs
            ]
        )
        spread = summed(
            [
                scaled_by(q, minus(p, book.mid(seq)), "execution_midpoint_unavailable")
                for (_, _, _, q, p, _, _, seq, _) in legs
            ]
        )
        rounding_fills = sum(
            (n - q * p for (_, _, _, q, p, n, _, _, _) in legs), Fraction(0)
        )
        final_mark: Json | None = None
        if "unavailable" in valuation:
            gap = Gap(f"final_valuation_unavailable:{valuation['unavailable']['code']}")
            booked: Fraction | Gap = gap
            market: Fraction | Gap = gap
            at_bid: Fraction | Gap = gap if position != 0 else Fraction(0)
            at_mid: Fraction | Gap = gap if position != 0 else Fraction(0)
            final_mark = {"unavailable": gap.reason}
        else:
            booked = decimal(valuation["net_total_pnl"])
            market = decimal(valuation["market_value"])
            audit.check(
                decimal(valuation["equity"]) == cash + market
                and booked == cash + market - starting,
                "ledger",
                f"{where}: {account} equity and net PnL",
            )
            if position == 0:
                audit.check(
                    market == 0, "ledger", f"{where}: {account} flat market value"
                )
                at_bid = at_mid = Fraction(0)
            else:
                sequence = int(valuation["mark"]["ingest_sequence"])
                bid = decimal(valuation["mark"]["bid"])
                audit.check(
                    sequence in book.quotes
                    and book.side(sequence, "bid") == bid
                    and book.received(sequence)
                    == int(valuation["mark"]["receive_timestamp_ns"]),
                    "valuation",
                    f"{where}: {account} final mark",
                )
                audit.check(
                    market == engine_rounding(position * bid),
                    "valuation",
                    f"{where}: {account} market value is position x bid "
                    "rounded half away from zero to 10^-8",
                )
                midpoint = book.mid(sequence)
                at_bid = position * bid
                at_mid = (
                    Gap(f"final_midpoint_unavailable:{midpoint.reason}")
                    if isinstance(midpoint, Gap)
                    else position * midpoint
                )
                ask = book.side(sequence, "ask")
                final_mark = {
                    "ask": None if ask is None else written(ask),
                    "bid": valuation["mark"]["bid"],
                    "ingest_sequence": str(sequence),
                    "midpoint": figure(midpoint),
                    "receive_at_ns": str(book.received(sequence)),
                }
        market_rounding = minus(market, at_bid)
        component: dict[str, Any] = {
            "booked_net_pnl": booked,
            "midpoint_inventory_pnl": minus(at_mid, exec_value),
            "execution_spread_cost": spread,
            "fees": fees,
            "terminal_bid_markdown": minus(at_mid, at_bid),
            "rounding_adjustment": minus(market_rounding, rounding_fills),
            "market_value_rounding": market_rounding,
            "notional_rounding": rounding_fills,
            "observed_midpoint_movement": minus(exec_value, signal_value),
            "signal_reference_pnl": minus(at_mid, signal_value),
            "inventory_pnl_at_bid": minus(at_bid, exec_value),
        }
        values[account] = component
        reconciliation = _reconcile(audit, where, account, component)
        episodes = _episodes(account, legs, book, valuation, valued_at)
        episode_rows.extend(episodes)
        documents[account] = {
            "booked": _booked(
                ledger, valuation, manifest["configuration"]["starting_cash"]
            ),
            "components": {name: figure(component[name]) for name in COMPONENTS},
            "episodes": _episode_summary(episodes),
            "fills": str(len(filled)),
            "final_mark": final_mark,
            "orders": _orders(audit, where, account, orders, resolved, results),
            "quoted_spreads": {
                "execution": _spreads([book.spread(seq) for (*_, seq, _) in legs]),
                "signal": _spreads([book.spread(sig) for (*_, sig) in legs]),
            },
            "reconciliation": reconciliation,
            "timing": _timing(account, orders, resolved, latency),
        }
        for (
            entry,
            order,
            decision,
            quantity,
            price,
            notional,
            _,
            sequence,
            signal,
        ) in legs:
            observation = entry["observation"]
            fill_rows.append(
                {
                    "account": account,
                    "cash_after": entry["cash_after"],
                    "client_order_id": entry["client_order_id"],
                    "contributions": {
                        "execution_spread_cost": figure(
                            scaled_by(
                                quantity,
                                minus(price, book.mid(sequence)),
                                "execution_midpoint_unavailable",
                            )
                        ),
                        "notional_rounding": figure(notional - quantity * price),
                        "observed_midpoint_movement": figure(
                            scaled_by(
                                quantity,
                                minus(book.mid(sequence), book.mid(signal)),
                                "midpoint_unavailable",
                            )
                        ),
                    },
                    "decision": order["decision"],
                    "execution": {
                        "ask": observation["ask"],
                        "bid": observation["bid"],
                        "executed_at_ns": entry["executed_at_ns"],
                        "ingest_sequence": observation["ingest_sequence"],
                        "midpoint": figure(book.mid(sequence)),
                        "quoted_spread": figure(book.spread(sequence)),
                    },
                    "fee": entry["fill"]["fee"],
                    "fill_id": entry["fill"]["fill_id"],
                    "notional": entry["fill"]["notional"],
                    "outcome": entry["outcome"],
                    "position_after": entry["position_after"],
                    "price": entry["fill"]["price"],
                    "quantity": entry["fill"]["quantity"],
                    "side": order["side"],
                    "signal": {
                        "ask": None
                        if book.side(signal, "ask") is None
                        else written(book.side(signal, "ask")),  # type: ignore[arg-type]
                        "available_at_ns": decision["signal"]["available_at_ns"],
                        "bid": None
                        if book.side(signal, "bid") is None
                        else written(book.side(signal, "bid")),  # type: ignore[arg-type]
                        "class": decision["signal"]["class"],
                        "confidence_ppb": decision["signal"]["confidence_ppb"],
                        "ingest_sequence": str(signal),
                        "midpoint": figure(book.mid(signal)),
                        "quoted_spread": figure(book.spread(signal)),
                        "signal_at_ns": order["signal_at_ns"],
                    },
                    "timing": {
                        "arrival_to_execution_duration_ns": str(
                            int(entry["executed_at_ns"]) - int(order["arrival_at_ns"])
                        ),
                        "configured_latency_ns": str(latency),
                        "signal_to_arrival_duration_ns": str(
                            int(order["arrival_at_ns"]) - int(order["signal_at_ns"])
                        ),
                        "signal_to_execution_duration_ns": str(
                            int(entry["executed_at_ns"]) - int(order["signal_at_ns"])
                        ),
                    },
                }
            )
    strategy_decisions = []
    for (account, number), entry in decisions.items():
        if account != "strategy":
            continue
        outcome = None
        for (owner, _), order in orders.items():
            if owner == "strategy" and order["decision"] == number:
                resolution = resolved[(owner, order["client_order_id"])]
                outcome = (
                    "expired"
                    if resolution["type"] == "order_expired"
                    else resolution["outcome"]
                )
        strategy_decisions.append(
            {
                **entry["signal"],
                "number": number,
                "outcome": outcome,
                "reason": entry["reason"],
            }
        )
    return documents, fill_rows, episode_rows, strategy_decisions, values


def _reconcile(audit: Audit, where: str, account: str, values: dict[str, Any]) -> Json:
    booked, spread, fees, rounding = (
        values[name]
        for name in (
            "booked_net_pnl",
            "execution_spread_cost",
            "fees",
            "rounding_adjustment",
        )
    )
    inventory, markdown, at_bid = (
        values["midpoint_inventory_pnl"],
        values["terminal_bid_markdown"],
        values["inventory_pnl_at_bid"],
    )
    document: Json = {}
    base = [booked, spread, fees, rounding]
    if not any(isinstance(value, Gap) for value in [*base, inventory, markdown]):
        audit.check(
            booked == inventory - spread - fees - markdown + rounding,
            "identities",
            f"{where} {account} net PnL identity",
        )
        document["booked_net_pnl"] = {"state": "reconciled"}
    elif not any(isinstance(value, Gap) for value in [*base, at_bid]):
        audit.check(
            booked == at_bid - spread - fees + rounding,
            "identities",
            f"{where} {account} net PnL identity at the bid",
        )
        document["booked_net_pnl"] = {"state": "reconciled_at_bid"}
    else:
        document["booked_net_pnl"] = {
            "unavailable": next(v.reason for v in [*base, at_bid] if isinstance(v, Gap))
        }
    signal_terms = [
        inventory,
        values["signal_reference_pnl"],
        values["observed_midpoint_movement"],
    ]
    gaps = [value for value in signal_terms if isinstance(value, Gap)]
    if gaps:
        document["signal_reference"] = {"unavailable": gaps[0].reason}
    else:
        audit.check(
            signal_terms[0] == signal_terms[1] - signal_terms[2],
            "identities",
            f"{where} {account} signal identity",
        )
        document["signal_reference"] = {"state": "reconciled"}
    return document


def _booked(ledger: Json, valuation: Json, starting_cash: str) -> Json:
    booked = {
        "cash": ledger["cash"],
        "fees_paid": ledger["fees_paid"],
        "final_quantity": ledger["position"],
        "starting_cash": starting_cash,
    }
    if "unavailable" in valuation:
        booked["valuation"] = {"unavailable": valuation["unavailable"]["code"]}
    else:
        booked["valuation"] = {
            key: valuation[key]
            for key in ("equity", "market_value", "net_total_pnl", "return_percent")
        }
    return booked


def _orders(
    audit: Audit,
    where: str,
    account: str,
    orders: dict[Key, Json],
    resolved: dict[Key, Json],
    results: Json,
) -> Json:
    outcomes = dict.fromkeys(
        (
            "cancelled",
            "expired",
            "filled",
            "partially_filled_and_cancelled",
            "rejected",
        ),
        0,
    )
    cancellations: Counter[str] = Counter()
    rejections: Counter[str] = Counter()
    scheduled = 0
    for key in orders:
        if key[0] != account:
            continue
        scheduled += 1
        entry = resolved[key]
        if entry["type"] == "order_expired":
            outcomes["expired"] += 1
            continue
        outcomes[entry["outcome"]] += 1
        if entry["cancellation_reason"] is not None:
            cancellations[entry["cancellation_reason"]] += 1
        if entry["rejection"] is not None:
            rejections[f"{entry['rejection']['by']}:{entry['rejection']['code']}"] += 1
    reported = results["orders"]
    audit.check(
        all(int(reported[key]) == count for key, count in outcomes.items())
        and int(reported["scheduled"]) == scheduled,
        "orders",
        f"{where}: {account} order counts against the manifest",
    )
    return {
        "cancellation_reasons": {
            key: str(count) for key, count in sorted(cancellations.items())
        },
        "outcomes": {
            **{key: str(count) for key, count in outcomes.items()},
            "scheduled": str(scheduled),
        },
        "rejections": {key: str(count) for key, count in sorted(rejections.items())},
    }


def _timing(
    account: str, orders: dict[Key, Json], resolved: dict[Key, Json], latency: int
) -> Json:
    measured: dict[str, list[int]] = {
        "arrival_to_execution_duration_ns": [],
        "signal_to_arrival_duration_ns": [],
        "signal_to_execution_duration_ns": [],
    }
    expired = 0
    for key, order in orders.items():
        if key[0] != account:
            continue
        entry = resolved[key]
        if entry["type"] == "order_expired":
            expired += 1
            continue
        signal, arrival, executed = (
            int(order["signal_at_ns"]),
            int(order["arrival_at_ns"]),
            int(entry["executed_at_ns"]),
        )
        measured["arrival_to_execution_duration_ns"].append(executed - arrival)
        measured["signal_to_arrival_duration_ns"].append(arrival - signal)
        measured["signal_to_execution_duration_ns"].append(executed - signal)
    document: Json = {
        "configured_latency_ns": str(latency),
        "executions": str(len(measured["signal_to_arrival_duration_ns"])),
        "expired": str(expired),
    }
    for name, values in measured.items():
        document[name] = (
            {
                "maximum": str(max(values)),
                "minimum": str(min(values)),
                "sum": str(sum(values)),
            }
            if values
            else {"unavailable": "no_executions"}
        )
    return document


def _spreads(values: list["Fraction | Gap"]) -> Json:
    present = [value for value in values if not isinstance(value, Gap)]
    if not present:
        return {"unavailable": "no_fills_with_both_sides"}
    return {
        "fills": str(len(present)),
        "maximum": written(max(present)),
        "minimum": written(min(present)),
        "sum": written(sum(present, Fraction(0))),
        "without_both_sides": str(len(values) - len(present)),
    }


def _episodes(
    account: str, legs: list[Leg], book: Book, valuation: Json, valued_at: int
) -> list[Json]:
    episodes: list[Json] = []
    held = Fraction(0)
    start = 0
    for index, leg in enumerate(legs):
        held += leg[3]
        if held == 0 or index == len(legs) - 1:
            part = legs[start : index + 1]
            censored = held != 0
            entry_at = int(part[0][0]["executed_at_ns"])
            end_at = valued_at if censored else int(part[-1][0]["executed_at_ns"])
            flow = -sum((leg[5] + leg[6] for leg in part), Fraction(0))
            if not censored:
                pnl: Fraction | Gap = flow
            elif "unavailable" in valuation:
                pnl = Gap(
                    f"final_valuation_unavailable:{valuation['unavailable']['code']}"
                )
            else:
                pnl = flow + decimal(valuation["market_value"])
            running, peak = Fraction(0), Fraction(0)
            for item in part:
                running += item[3]
                peak = max(peak, running)
            episodes.append(
                {
                    "account": account,
                    "basis": "marked_at_final_valuation" if censored else "booked",
                    "bought": written(
                        sum((item[3] for item in part if item[3] > 0), Fraction(0))
                    ),
                    "cash_flow": figure(flow),
                    "censored_at_ns": str(end_at) if censored else None,
                    "duration_ns": str(end_at - entry_at),
                    "entry_at_ns": str(entry_at),
                    "episode": str(len(episodes) + 1),
                    "execution_spread_cost": figure(
                        summed(
                            [
                                scaled_by(
                                    item[3],
                                    minus(item[4], book.mid(item[7])),
                                    "execution_midpoint_unavailable",
                                )
                                for item in part
                            ]
                        )
                    ),
                    "exit_at_ns": None if censored else str(end_at),
                    "fees": figure(sum((item[6] for item in part), Fraction(0))),
                    "fills": str(len(part)),
                    "maximum_position": written(peak),
                    "pnl": figure(pnl),
                    "sold": written(
                        -sum((item[3] for item in part if item[3] < 0), Fraction(0))
                    ),
                    "state": "censored" if censored else "completed",
                }
            )
            start = index + 1
    return episodes


def _episode_summary(episodes: list[Json]) -> Json:
    done = [episode for episode in episodes if episode["state"] == "completed"]
    durations = [int(episode["duration_ns"]) for episode in done]
    pnls = [Fraction(episode["pnl"]["value"]) for episode in done]
    return {
        "censored": str(len(episodes) - len(done)),
        "completed": str(len(done)),
        "completed_duration_ns": {
            "maximum": str(max(durations)),
            "minimum": str(min(durations)),
            "sum": str(sum(durations)),
        }
        if durations
        else {"unavailable": "no_completed_episodes"},
        "completed_pnl": figure(sum(pnls, Fraction(0))),
        "completed_with_gain": str(sum(1 for pnl in pnls if pnl > 0)),
        "completed_with_loss": str(sum(1 for pnl in pnls if pnl < 0)),
    }


# --- Cohorts ----------------------------------------------------------------------------------------------


type Status = tuple[str, str | None, int | None]


class Cohort:
    def __init__(self) -> None:
        self.items = 0
        self.labels: Counter[str] = Counter()
        self.predicted = Counter[str]()
        self.matrix: Counter[tuple[str, str]] = Counter()
        self.bins = [[0, 0, 0] for _ in range(10)]
        self.reasons: Counter[str] = Counter()
        # Scored items' return_raw values: all, by predicted class, by bin.
        self.returns: dict[str, list[int]] = {}

    def add(
        self,
        predicted: str,
        confidence: int,
        status: Status,
        reason: str | None = None,
    ) -> None:
        self.items += 1
        self.labels[status[0]] += 1
        self.predicted[predicted] += 1
        bucket = self.bins[min(confidence // BIN, 9)]
        bucket[0] += 1
        if reason is not None:
            self.reasons[reason] += 1
        if status[0] == "scored":
            true = status[1]
            assert true is not None
            self.matrix[(true, predicted)] += 1
            bucket[1] += 1
            bucket[2] += true == predicted
            value = status[2]
            assert value is not None
            for group in (
                "all",
                f"class:{predicted}",
                f"bin:{min(confidence // BIN, 9)}",
            ):
                self.returns.setdefault(group, []).append(value)

    def plus(self, other: "Cohort") -> "Cohort":
        merged = Cohort()
        for part in (self, other):
            merged.items += part.items
            merged.labels.update(part.labels)
            merged.predicted.update(part.predicted)
            merged.matrix.update(part.matrix)
            merged.reasons.update(part.reasons)
            for mine, theirs in zip(merged.bins, part.bins, strict=True):
                for index in range(3):
                    mine[index] += theirs[index]
            for group, values in part.returns.items():
                merged.returns.setdefault(group, []).extend(values)
        return merged

    def summary(self, group: str) -> Json:
        values = self.returns.get(group, [])
        undefined = {"undefined": "no_scored_items"}
        return {
            "maximum_raw": {"value": str(max(values))} if values else undefined,
            "mean_raw": fraction(sum(values), len(values), "no_scored_items"),
            "minimum_raw": {"value": str(min(values))} if values else undefined,
            "rows": str(len(values)),
            "sum_raw": str(sum(values)),
        }

    def document(self) -> Json:
        scored = sum(self.matrix.values())
        correct = sum(self.matrix[(name, name)] for name in CLASSES)
        recalls = []
        for name in CLASSES:
            occurs = sum(self.matrix[(name, other)] for other in CLASSES)
            if occurs:
                recalls.append(Fraction(self.matrix[(name, name)], occurs))
        balanced = sum(recalls, Fraction(0)) / len(recalls) if recalls else None
        return {
            "accuracy": fraction(correct, scored, "no_scored_items"),
            "balanced_accuracy": {"undefined": "no_scored_items"}
            if balanced is None
            else fraction(balanced.numerator, balanced.denominator, ""),
            "confidence_bins": [
                {
                    "accuracy": fraction(c, s, "no_scored_items"),
                    "correct": str(c),
                    "items": str(n),
                    "name": f"{i / 10:.1f}-{(i + 1) / 10:.1f}",
                    "scored": str(s),
                }
                for i, (n, s, c) in enumerate(self.bins)
            ],
            "confusion": {
                true: {pred: str(self.matrix[(true, pred)]) for pred in CLASSES}
                for true in CLASSES
            },
            "correct": str(correct),
            "future_returns": {
                "all": self.summary("all"),
                "by_confidence_bin": [
                    {
                        "name": f"{i / 10:.1f}-{(i + 1) / 10:.1f}",
                        **self.summary(f"bin:{i}"),
                    }
                    for i in range(10)
                ],
                "by_predicted_class": {
                    name: self.summary(f"class:{name}") for name in CLASSES
                },
                "not_scored": str(self.items - scored),
            },
            "items": str(self.items),
            "labels": {key: str(count) for key, count in sorted(self.labels.items())},
            "predicted": {name: str(self.predicted[name]) for name in CLASSES},
            "realized": {
                name: str(sum(self.matrix[(name, other)] for other in CLASSES))
                for name in CLASSES
            },
            "reasons": {key: str(count) for key, count in sorted(self.reasons.items())},
            "scored": str(scored),
        }


def statuses(
    features: list[Json], labels: list[Json], instrument: str, start: int, end: int
) -> dict[tuple[str, str, int], Status]:
    """Each window row's status under the plan's rule, joined by identity."""
    by_identity = {
        (row["stream"], row["instrument"], int(row["ingest_sequence"])): row
        for row in labels
    }
    result: dict[tuple[str, str, int], Status] = {}
    for feature in features:
        if feature["instrument"] != instrument:
            continue
        available = int(feature["available_at_ns"])
        if not start <= available < end:
            continue
        identity = (
            feature["stream"],
            feature["instrument"],
            int(feature["ingest_sequence"]),
        )
        label = by_identity[identity]
        missing = next(
            (
                feature["features"][name]["unavailable"]
                for name in INPUTS
                if "raw" not in feature["features"][name]
            ),
            None,
        )
        if missing is not None:
            result[identity] = (f"feature_unavailable:{missing}", None, None)
        elif "direction" not in label["label"]:
            result[identity] = (
                f"label_unavailable:{label['label']['unavailable']}",
                None,
                None,
            )
        elif (
            label["label_available_at_ns"] is None
            or int(label["label_available_at_ns"]) >= end
        ):
            result[identity] = ("label_after_scoring_cutoff", None, None)
        else:
            result[identity] = (
                "scored",
                label["label"]["direction"],
                int(label["label"]["return_raw"]),
            )
    return result


# --- Pools ------------------------------------------------------------------------------------------------


def pool(
    parts: list[tuple[str, dict[str, dict[str, Any]]]],
) -> dict[str, dict[str, Any]]:
    """Exact sums over the runs pooled; with no run, nothing was evaluated,
    so no financial figure exists."""
    pooled: dict[str, dict[str, Any]] = {}
    for account in ACCOUNTS:
        pooled[account] = {}
        for name in COMPONENTS:
            if not parts:
                pooled[account][name] = Gap("no_completed_runs")
                continue
            total: Fraction | Gap = Fraction(0)
            for label, values in parts:
                value = values[account][name]
                if isinstance(value, Gap):
                    total = Gap(f"{label}:{value.reason}")
                    break
                total += value
            pooled[account][name] = total
    return pooled


def pooled_document(pooled: dict[str, dict[str, Any]]) -> Json:
    return {
        "accounts": {
            account: {name: figure(pooled[account][name]) for name in COMPONENTS}
            for account in ACCOUNTS
        },
        "deltas": {
            benchmark: {
                name: figure(minus(pooled["strategy"][name], pooled[benchmark][name]))
                for name in COMPONENTS
            }
            for benchmark in ("buy_and_hold", "cash")
        },
    }


def summed_cohorts(cohorts: list["Cohort"]) -> "Cohort":
    total = Cohort()
    for cohort in cohorts:
        total = total.plus(cohort)
    return total


# --- The audit --------------------------------------------------------------------------------------------


def run_audit(root: Path, diagnostic_id: str) -> Json:
    audit = Audit()
    directory = root / "strategy_diagnostics" / diagnostic_id
    try:
        manifest = identified(directory / "manifest.json", "diagnostic_id", True)
        audit.check(
            sorted(p.name for p in directory.iterdir())
            == ["episodes.ndjson", "fills.ndjson", "manifest.json"],
            "artifact",
            "the artifact's files",
        )
        stored: dict[str, list[Json]] = {}
        for key in ("fills", "episodes"):
            digest = manifest["files"][key]
            data = (directory / digest["name"]).read_bytes()
            audit.check(
                sha256(data) == digest["sha256"]
                and str(len(data)) == digest["size_bytes"],
                "artifact",
                f"{key} checksum",
            )
            stored[key] = lines(data)
            audit.check(
                str(len(stored[key])) == digest["rows"], "artifact", f"{key} rows"
            )
        rebuilt = rebuild(audit, root, manifest)
    except Refused:
        return report(audit, diagnostic_id)
    except (
        Unreadable,
        OSError,
        KeyError,
        TypeError,
        ValueError,
        IndexError,
        AttributeError,
    ) as error:
        audit.problems.append(f"unreadable: {type(error).__name__}: {error}")
        return report(audit, diagnostic_id)
    expected_manifest, fills, episodes = rebuilt
    audit.same(fills, stored["fills"], "fills", "fills.ndjson")
    audit.same(episodes, stored["episodes"], "episodes", "episodes.ndjson")
    for key in (
        "request",
        "schema",
        "status",
        "counts",
        "generator",
        "confidence_bins",
    ):
        audit.same(expected_manifest[key], manifest.get(key), "manifest", key)
    for index, (mine, theirs) in enumerate(
        zip(expected_manifest["studies"], manifest["studies"], strict=False)
    ):
        for key in (
            "collection_id",
            "evidence",
            "instrument",
            "missing",
            "plan_id",
            "scenarios",
            "status",
            "study_id",
            "study_status",
        ):
            audit.same(mine[key], theirs.get(key), "studies", f"study {index} {key}")
        for fold_index, (fold, claimed) in enumerate(
            zip(mine["folds"], theirs["folds"], strict=False)
        ):
            for key in (
                "dataset_id",
                "events_sha256",
                "feature_artifact_id",
                "features_sha256",
                "index",
                "label_artifact_id",
                "labels_sha256",
                "stream",
                "window",
            ):
                audit.same(
                    fold[key],
                    claimed.get(key),
                    "sources",
                    f"study {index} fold {fold_index + 1} {key}",
                )
            for name in MODELS:
                audit.same(
                    fold["models"][name],
                    claimed["models"].get(name),
                    "predictions",
                    f"study {index} fold {fold_index + 1} {name}",
                )
            audit.same(
                len(fold["runs"]),
                len(claimed["runs"]),
                "runs",
                f"study {index} fold {fold_index + 1} runs",
            )
            for run, claimed_run in zip(fold["runs"], claimed["runs"], strict=False):
                label = f"study {index} fold {fold_index + 1} {run['model']} {run['scenario']}"
                for key in sorted(set(run) - {"accounts", "decisions", "deltas"}):
                    audit.same(run[key], claimed_run.get(key), "runs", f"{label} {key}")
                for account in run.get("accounts", {}):
                    audit.same(
                        run["accounts"][account],
                        claimed_run["accounts"].get(account),
                        "accounts",
                        f"{label} {account}",
                    )
                if "decisions" in run:
                    audit.same(
                        run["decisions"],
                        claimed_run.get("decisions"),
                        "cohorts",
                        f"{label} decisions",
                    )
                    audit.same(
                        run["deltas"],
                        claimed_run.get("deltas"),
                        "deltas",
                        f"{label} deltas",
                    )
        audit.same(
            mine["pooled"], theirs.get("pooled"), "pools", f"study {index} pooled"
        )
    audit.same(
        len(expected_manifest["studies"]),
        len(manifest["studies"]),
        "studies",
        "study count",
    )
    audit.same(
        expected_manifest.get("across_studies"),
        manifest.get("across_studies"),
        "pools",
        "across_studies",
    )
    return report(audit, diagnostic_id)


def report(audit: Audit, diagnostic_id: str) -> Json:
    return {
        "checks": dict(sorted(audit.checks.items())),
        "checks_total": sum(audit.checks.values()),
        "diagnostic_id": diagnostic_id,
        "not_audited": ["definitions, scope, and verification prose"],
        "problems": audit.problems[:200],
        "problems_total": len(audit.problems),
        "verdict": "pass" if not audit.problems else "fail",
    }


def rebuild(
    audit: Audit, root: Path, manifest: Json
) -> tuple[Json, list[Json], list[Json]]:
    request = manifest["request"]
    study_ids = request["study_ids"]
    audit.check(
        study_ids == sorted(set(study_ids)) and 1 <= len(study_ids) <= 4,
        "manifest",
        "the request's studies",
    )
    sections, fills, episodes, pools, predictions = [], [], [], [], []
    runs_total = attributed = 0
    for study_id in study_ids:
        section, study_fills, study_episodes, study_pools, study_predictions, counts = (
            rebuild_study(audit, root, study_id)
        )
        sections.append(section)
        fills.extend(study_fills)
        episodes.extend(study_episodes)
        pools.append(study_pools)
        predictions.append(study_predictions)
        runs_total += counts[0]
        attributed += counts[1]
    expected: Json = {
        "confidence_bins": [
            {
                "lower_ppb": str(i * BIN),
                "name": f"{i / 10:.1f}-{(i + 1) / 10:.1f}",
                "upper": "inclusive" if i == 9 else "exclusive",
                "upper_ppb": str((i + 1) * BIN),
            }
            for i in range(10)
        ],
        "counts": {
            "episodes": str(len(episodes)),
            "fills": str(len(fills)),
            "runs": str(runs_total),
            "runs_attributed": str(attributed),
        },
        "generator": {"name": "chronos_research.strategy_diagnostics", "version": "2"},
        "request": {
            "schema": {"name": "chronos.strategy_diagnostics_request", "version": "1"},
            "study_ids": study_ids,
        },
        "schema": {"name": "chronos.strategy_diagnostics", "version": "2"},
        "status": "partial"
        if any(section["missing"] for section in sections)
        else "complete",
        "studies": sections,
    }
    if len(sections) > 1:
        shared = [
            scenario
            for scenario in sections[0]["scenarios"]
            if all(scenario in other["scenarios"] for other in sections[1:])
        ]
        trading: Json = {}
        for name in MODELS:
            trading[name] = {}
            for scenario in shared:
                parts = [
                    (section["study_id"], study_pools[(name, scenario["name"])])
                    for section, study_pools in zip(sections, pools, strict=True)
                ]
                # Only a study with a completed fold of the cell contributes.
                evaluated = [
                    section["study_id"]
                    for section in sections
                    if section["pooled"]["trading"][name][scenario["name"]]["folds"][
                        "included"
                    ]
                ]
                values = pool(
                    [
                        (identifier[:12], part[0])
                        for identifier, part in parts
                        if identifier in evaluated
                    ]
                )
                cohorts = {
                    key: summed_cohorts([part[1][key] for _, part in parts]).document()
                    for key in (
                        "sampled_decisions",
                        "order_scheduling_decisions",
                        "filled_orders",
                    )
                }
                if not evaluated:
                    status = "unavailable"
                elif len(evaluated) == len(parts) and all(part[2] for _, part in parts):
                    status = "complete"
                else:
                    status = "partial"
                trading[name][scenario["name"]] = {
                    "cohorts": cohorts,
                    "status": status,
                    "studies": {
                        "included": evaluated,
                        "missing": [
                            identifier
                            for identifier, _ in parts
                            if identifier not in evaluated
                        ],
                    },
                    **pooled_document(values),
                }
        across_predictions: Json = {}
        for name in MODELS:
            folds_of = [
                section["pooled"]["predictions"][name]["folds"] for section in sections
            ]
            with_folds = [
                section["study_id"]
                for section, folds in zip(sections, folds_of, strict=True)
                if folds["included"]
            ]
            whole = all(
                str(len(folds["included"])) == folds["planned"] for folds in folds_of
            )
            across_predictions[name] = {
                "cohort": summed_cohorts(
                    [study_predictions[name] for study_predictions in predictions]
                ).document(),
                "status": "unavailable"
                if not with_folds
                else (
                    "complete"
                    if whole and len(with_folds) == len(sections)
                    else "partial"
                ),
                "studies": {
                    "included": with_folds,
                    "missing": [
                        section["study_id"]
                        for section in sections
                        if section["study_id"] not in with_folds
                    ],
                },
            }
        expected["across_studies"] = {
            "predictions": across_predictions,
            "scenarios": shared,
            "studies": [section["study_id"] for section in sections],
            "trading": trading,
            "unshared_scenarios": [
                {"scenario": scenario["name"], "study_id": section["study_id"]}
                for section in sections
                for scenario in section["scenarios"]
                if scenario not in shared
            ],
        }
    return expected, fills, episodes


def rebuild_study(
    audit: Audit, root: Path, study_id: str
) -> tuple[
    Json,
    list[Json],
    list[Json],
    dict[Key, tuple[Values, dict[str, "Cohort"], bool]],
    dict[str, "Cohort"],
    tuple[int, int],
]:
    study = source(audit, root, "walk_forward_studies", study_id)
    plan = source(audit, root, "walk_forward_plans", study["plan_id"])
    instrument = study["instrument"]
    audit.check(
        plan["instrument"] == instrument
        and plan["scenarios"] == study["scenarios"]
        and plan["collection"] == study["collection"]
        and len(plan["folds"]) == len(study["folds"]),
        "sources",
        f"study {study_id[:12]} is its plan's",
    )
    collection = source(audit, root, "collections", plan["collection"]["collection_id"])
    members = {
        member["dataset"]["dataset_id"]: member for member in collection["members"]
    }
    folds: list[Json] = []
    missing: list[Json] = []
    fills: list[Json] = []
    episodes: list[Json] = []
    cells: dict[Key, list[tuple[str, Values, dict[str, Cohort]]]] = {}
    predictions: dict[tuple[str, str], Cohort] = {}
    runs_total = attributed = 0
    for fold, planned in zip(study["folds"], plan["folds"], strict=True):
        index = fold["index"]
        evaluation = planned["evaluation"]
        audit.check(
            planned["index"] == index,
            "sources",
            f"study {study_id[:12]} fold {index} is its plan's",
        )
        member = members.get(evaluation["dataset_id"])
        if member is None:
            raise refuse(
                audit, f"fold {index}'s dataset is not a member of its collection"
            )
        dataset_manifest = source(audit, root, "normalized", evaluation["dataset_id"])
        events = source_file(
            audit,
            root,
            "normalized",
            dataset_manifest,
            "events",
            "events.ndjson",
            dataset_manifest["counts"]["events"],
        )
        stream = dataset_manifest["stream"]
        feature_manifest = source(
            audit, root, "features", evaluation["feature_artifact_id"]
        )
        feature_bytes = source_file(
            audit,
            root,
            "features",
            feature_manifest,
            "features",
            "features.ndjson",
            feature_manifest["counts"]["rows"],
        )
        label_manifest = source(audit, root, "labels", evaluation["label_artifact_id"])
        label_bytes = source_file(
            audit,
            root,
            "labels",
            label_manifest,
            "labels",
            "labels.ndjson",
            label_manifest["counts"]["rows"],
        )
        audit.check(
            (
                member["dataset"]["events_sha256"],
                member["dataset"]["stream"],
                member["features"]["artifact_id"],
                member["features"]["features_sha256"],
                member["labels"]["artifact_id"],
                member["labels"]["labels_sha256"],
            )
            == (
                sha256(events),
                stream,
                evaluation["feature_artifact_id"],
                sha256(feature_bytes),
                evaluation["label_artifact_id"],
                sha256(label_bytes),
            )
            and evaluation["stream"] == stream,
            "sources",
            f"fold {index}'s recording is its collection member's",
        )
        audit.check(
            (
                feature_manifest["source"]["dataset_id"],
                feature_manifest["source"]["events_sha256"],
                feature_manifest["source"]["stream"],
                feature_manifest["counts"]["rows_by_instrument"].get(instrument),
            )
            == (
                evaluation["dataset_id"],
                sha256(events),
                stream,
                evaluation["feature_rows"],
            ),
            "sources",
            f"fold {index}'s features are of its dataset",
        )
        audit.check(
            (
                label_manifest["source"]["dataset_id"],
                label_manifest["source"]["feature_artifact_id"],
                label_manifest["source"]["features_sha256"],
                label_manifest["source"]["stream"],
            )
            == (
                evaluation["dataset_id"],
                evaluation["feature_artifact_id"],
                sha256(feature_bytes),
                stream,
            ),
            "sources",
            f"fold {index}'s labels are of its features",
        )
        book = Book(lines(events), stream, instrument)
        window = evaluation["window"]
        start, end = int(window["start_at_ns"]), int(window["end_at_ns"])
        status = statuses(
            lines(feature_bytes), lines(label_bytes), instrument, start, end
        )
        document: Json = {
            "dataset_id": evaluation["dataset_id"],
            "events_sha256": sha256(events),
            "feature_artifact_id": evaluation["feature_artifact_id"],
            "features_sha256": sha256(feature_bytes),
            "index": index,
            "label_artifact_id": evaluation["label_artifact_id"],
            "labels_sha256": sha256(label_bytes),
            "stream": stream,
            "window": window,
            "models": {},
            "runs": [],
        }
        for name in MODELS:
            model = fold["models"][name]
            if model["state"] != "completed" or "schedule" not in model:
                reason = f"model_{model['state']}:{model.get('reason', 'no_schedule')}"
                document["models"][name] = {"reason": reason, "state": "unavailable"}
                missing.append(
                    {"fold": index, "item": f"model:{name}", "reason": reason}
                )
                for cell in model["cells"]:
                    document["runs"].append(
                        {
                            "fold": index,
                            "model": name,
                            "reason": reason,
                            "scenario": cell["scenario"],
                            "state": "unavailable",
                        }
                    )
                    runs_total += 1
                continue
            schedule_id = model["schedule"]["artifact_id"]
            schedule_manifest = source(audit, root, "schedules", schedule_id)
            schedule_bytes = source_file(
                audit,
                root,
                "schedules",
                schedule_manifest,
                "schedule",
                "schedule.ndjson",
                schedule_manifest["counts"]["rows"],
            )
            audit.check(
                (
                    schedule_manifest["source"]["collection_id"],
                    schedule_manifest["source"]["dataset_id"],
                    schedule_manifest["source"]["feature_artifact_id"],
                    schedule_manifest["source"]["features_sha256"],
                    schedule_manifest["source"]["stream"],
                )
                == (
                    plan["collection"]["collection_id"],
                    evaluation["dataset_id"],
                    evaluation["feature_artifact_id"],
                    sha256(feature_bytes),
                    stream,
                ),
                "sources",
                f"fold {index} {name}'s schedule is of its features",
            )
            rows = {
                int(row["identity"]["ingest_sequence"]): row
                for row in lines(schedule_bytes)
            }
            scorable = Cohort()
            unavailable: Counter[str] = Counter()
            for sequence, row in rows.items():
                item = status[(stream, instrument, sequence)]
                if "unavailable" in row["prediction"]:
                    unavailable[item[0]] += 1
                else:
                    scorable.add(
                        row["prediction"]["class"],
                        int(row["prediction"]["confidence_ppb"]),
                        item,
                    )
            audit.check(
                len(rows) == len(status),
                "predictions",
                f"fold {index} {name}: every window row scheduled",
            )
            predictions[(index, name)] = scorable
            document["models"][name] = {
                "predictions": {
                    "cohort": scorable.document(),
                    "feature_unavailable": {
                        k: str(v) for k, v in sorted(unavailable.items())
                    },
                    "scheduled": str(len(rows)),
                },
                "schedule_id": schedule_id,
                "schedule_sha256": sha256(schedule_bytes),
                "state": "completed",
                "study_scores": "agree"
                if "metrics" in model.get("predictive", {})
                else f"unavailable:{model.get('predictive', {}).get('unavailable')}",
            }
            if "metrics" in model.get("predictive", {}):
                predictive = model["predictive"]
                audit.check(
                    predictive["rows"]["scored"] == scorable.document()["scored"],
                    "predictions",
                    f"fold {index} {name}: the study's scored rows",
                )
                audit.check(
                    predictive["metrics"]["confusion_matrix"]
                    == scorable.document()["confusion"],
                    "predictions",
                    f"fold {index} {name}: the study's confusion",
                )
            for cell in model["cells"]:
                runs_total += 1
                scenario = next(
                    item
                    for item in study["scenarios"]
                    if item["name"] == cell["scenario"]
                )
                if cell["state"] != "completed":
                    reason = f"cell_{cell['state']}:{cell['reason']}"
                    document["runs"].append(
                        {
                            "fold": index,
                            "model": name,
                            "reason": reason,
                            "scenario": cell["scenario"],
                            "state": "unavailable",
                        }
                    )
                    missing.append(
                        {
                            "fold": index,
                            "item": f"cell:{name}:{cell['scenario']}",
                            "reason": reason,
                        }
                    )
                    continue
                attributed += 1
                run_id = cell["run"]["artifact_id"]
                run_manifest = source(audit, root, "strategies", run_id)
                journal_bytes = source_file(
                    audit,
                    root,
                    "strategies",
                    run_manifest,
                    "journal",
                    "journal.ndjson",
                )
                bound = run_manifest["source"]
                audit.check(
                    (
                        bound["schedule_id"],
                        bound["dataset_id"],
                        bound["events_sha256"],
                        bound["feature_artifact_id"],
                        bound["collection_id"],
                        bound["instrument"],
                        bound["stream"],
                        bound["plan_id"],
                        bound["fold"],
                        bound["model"],
                    )
                    == (
                        schedule_id,
                        evaluation["dataset_id"],
                        sha256(events),
                        evaluation["feature_artifact_id"],
                        plan["collection"]["collection_id"],
                        instrument,
                        stream,
                        study["plan_id"],
                        index,
                        name,
                    )
                    and run_manifest["configuration"]["execution"]["fee_per_fill"]
                    == scenario["fee_per_fill"]
                    and run_manifest["configuration"]["execution"]["latency_ns"]
                    == scenario["latency_ns"],
                    "sources",
                    f"run {run_id[:12]} is its cell's",
                )
                where = f"run {run_id[:12]}"
                accounts, run_fills, run_episodes, decisions, values = run_figures(
                    audit, where, run_manifest, lines(journal_bytes), book, rows
                )
                location = {
                    "fold": index,
                    "model": name,
                    "run_id": run_id,
                    "scenario": scenario["name"],
                    "study_id": study_id,
                }
                fills.extend({**location, **row} for row in run_fills)
                episodes.extend({**location, **row} for row in run_episodes)
                sampled, scheduling, filled = Cohort(), Cohort(), Cohort()
                for decision in decisions:
                    item = status[
                        (stream, instrument, int(decision["ingest_sequence"]))
                    ]
                    sampled.add(
                        decision["class"],
                        int(decision["confidence_ppb"]),
                        item,
                        decision["reason"],
                    )
                    if decision["reason"] == "order_scheduled":
                        scheduling.add(
                            decision["class"],
                            int(decision["confidence_ppb"]),
                            item,
                            decision["outcome"],
                        )
                        if decision["outcome"] in (
                            "filled",
                            "partially_filled_and_cancelled",
                        ):
                            filled.add(
                                decision["class"],
                                int(decision["confidence_ppb"]),
                                item,
                                decision["outcome"],
                            )
                reported = run_manifest["results"]["decisions"]
                journaled = Counter(decision["reason"] for decision in decisions)
                audit.check(
                    int(reported["evaluated"]) == len(decisions)
                    and int(reported["orders"]) == journaled["order_scheduled"],
                    "cohorts",
                    f"{where} decision counts",
                )
                cohorts = {
                    "sampled_decisions": sampled,
                    "order_scheduling_decisions": scheduling,
                    "filled_orders": filled,
                }
                cells.setdefault((name, scenario["name"]), []).append(
                    (index, values, cohorts)
                )
                document["runs"].append(
                    {
                        "accounts": accounts,
                        "decisions": {
                            "cohorts": {
                                key: cohort.document()
                                for key, cohort in cohorts.items()
                            },
                            "evaluated": reported["evaluated"],
                            "journaled": {
                                reason: str(journaled[reason]) for reason in REASONS
                            },
                            "skipped_decision_interval": reported[
                                "skipped_decision_interval"
                            ],
                            "skipped_prediction_unavailable": reported[
                                "skipped_prediction_unavailable"
                            ],
                        },
                        "deltas": pooled_document(values)["deltas"],
                        "engine_replay": {
                            "recorded_by": "study",
                            "statement": cell["run"].get("replay"),
                        },
                        "fee_per_fill": scenario["fee_per_fill"],
                        "fold": index,
                        "journal_sha256": sha256(journal_bytes),
                        "latency_ns": scenario["latency_ns"],
                        "model": name,
                        "run_id": run_id,
                        "scenario": scenario["name"],
                        "state": "completed",
                    }
                )
        folds.append(document)
    indices = [fold["index"] for fold in study["folds"]]
    trading: Json = {}
    pools: dict[Key, tuple[Values, dict[str, Cohort], bool]] = {}
    for name in MODELS:
        trading[name] = {}
        for scenario in study["scenarios"]:
            chosen = cells.get((name, scenario["name"]), [])
            included = [index for index, _, _ in chosen]
            absent = [index for index in indices if index not in included]
            values = pool([(f"fold_{index}", part) for index, part, _ in chosen])
            cohorts = {
                key: summed_cohorts([part[key] for _, _, part in chosen])
                for key in (
                    "sampled_decisions",
                    "order_scheduling_decisions",
                    "filled_orders",
                )
            }
            pools[(name, scenario["name"])] = (values, cohorts, not absent)
            trading[name][scenario["name"]] = {
                "cohorts": {key: cohort.document() for key, cohort in cohorts.items()},
                "folds": {
                    "included": included,
                    "missing": absent,
                    "planned": str(len(indices)),
                },
                "status": "complete"
                if not absent
                else ("partial" if included else "unavailable"),
                **pooled_document(values),
            }
    prediction_pools: dict[str, Cohort] = {}
    predicted: Json = {}
    for name in MODELS:
        included = [index for index in indices if (index, name) in predictions]
        prediction_pools[name] = summed_cohorts(
            [predictions[(index, name)] for index in included]
        )
        predicted[name] = {
            "cohort": prediction_pools[name].document(),
            "folds": {"included": included, "planned": str(len(indices))},
            "status": "complete"
            if len(included) == len(indices)
            else ("partial" if included else "unavailable"),
        }
    section = {
        "collection_id": study["collection"]["collection_id"],
        "evidence": study["collection"]["evidence"],
        "folds": folds,
        "instrument": instrument,
        "missing": missing,
        "plan_id": study["plan_id"],
        "pooled": {"predictions": predicted, "trading": trading},
        "scenarios": study["scenarios"],
        "status": "partial" if missing else "complete",
        "study_id": study_id,
        "study_status": study["status"],
    }
    return section, fills, episodes, pools, prediction_pools, (runs_total, attributed)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: audit.py <artifacts> <diagnostic-id>", file=sys.stderr)
        return 2
    result = run_audit(Path(argv[1]), argv[2])
    print(json.dumps(result, indent=2))
    return 0 if result["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
