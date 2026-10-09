#!/usr/bin/env python3
"""Independent audit of a walk-forward study (ADR 0022).

    python -I -S tools/walk_forward_audit/audit.py <artifacts> <study-id>

Standard library only, and nothing from Chronos: run it with `-I -S` so no
installed package can be used. From the raw files alone (normalized events,
feature rows, label rows, schedule rows, and strategy journals) it
recomputes, and compares with what the study, plan, fold models, and run
manifests claim:

- fold membership and cutoffs: the members holding the instrument ordered by
  first receive time, expanding training, windows [first, last + 1), and
  the exclusive information boundary;
- each fold's training rows and class support: inputs available, a usable
  label, and both prediction and label-availability time strictly before
  the boundary; and the latest of each of those times;
- that every evaluation feature row is scheduled, in order, with its raw
  inputs and no label;
- per-fold and pooled confusion counts exactly, and accuracy, balanced
  accuracy, log loss, and Brier score within 1e-12 (binary floating point);
- every journal decision's signal against the schedule row it used;
- every fill against the raw quote it executed on (the ask for a buy, the bid
  for a sell, after the latency, never the signal's own quote), its fee, and
  the cash and position it left; every account's final cash, position, mark,
  equity, fees, PnL, and return, exactly, with Decimal;
- every run against the plan, field by field: its schema, its whole
  configuration (the eight frozen options with its scenario's fee and
  latency), its window, and every source field, with the events and
  schedule files hashed here and the boundary and training cutoffs taken
  from the recomputation above; and every decision against the plan's
  policy;
- each cell's deltas, and every complete or partial aggregate: summed PnL,
  fees, turnover, fills, and starting capital, and the return as total PnL
  over total capital rounded once, half away from zero, to 8 places.

Money is exact. A fold model, model, or cell that did not complete is listed
as not audited, with its state. It prints one JSON report and exits 1 if
anything differs.
"""

import bisect
import hashlib
import json
import math
import sys
from collections import Counter
from decimal import ROUND_HALF_UP, Decimal, getcontext
from fractions import Fraction
from pathlib import Path
from typing import Any

getcontext().prec = 60
type Json = dict[str, Any]

CLASSES = ("down", "flat", "up")
ACCOUNTS = ("strategy", "buy_and_hold", "cash")
BENCHMARKS = ("buy_and_hold", "cash")
# Canonical events carry prices at 10^-9 (chronos.market_data_event/1).
PRICE_SCALE = Decimal(10) ** 9
INPUTS = ("spread", "imbalance", "microprice", "midpoint", "bid_size", "ask_size")


class Audit:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.checks: Counter[str] = Counter()
        self.not_audited: list[str] = []

    def check(self, condition: bool, what: str) -> None:
        self.checks[what.split(":")[0]] += 1
        if not condition:
            self.problems.append(what)


def read_json(path: Path) -> Json:
    document: Json = json.loads(path.read_text())
    return document


def ndjson(path: Path) -> list[Json]:
    return [json.loads(line) for line in path.read_text().splitlines()]


def raw_price(text: str) -> Decimal:
    return Decimal(text) / PRICE_SCALE


def close(left: float, right: float, tolerance: float = 1e-12) -> bool:
    return abs(left - right) <= tolerance * max(1.0, abs(left), abs(right))


def metric(entry: Json) -> float | None:
    return float(entry["value"]) if "value" in entry else None


def percentage(part: Decimal, whole: Decimal) -> Decimal:
    return (part / whole * 100).quantize(Decimal("0.00000001"), rounding=ROUND_HALF_UP)


def canonical(value: Decimal) -> str:
    text = format(value.normalize(), "f")
    return "0" if text in ("-0", "0") else text


def inputs_of(feature: Json) -> list[int] | None:
    values = feature["features"]
    if any("raw" not in values[name] for name in INPUTS):
        return None
    raw = {name: int(values[name]["raw"]) for name in INPUTS}
    return [
        raw["spread"],
        raw["imbalance"],
        raw["microprice"] - raw["midpoint"],
        raw["bid_size"],
        raw["ask_size"],
    ]


class Root:
    def __init__(self, path: Path) -> None:
        self.path = path

    def events_path(self, dataset_id: str) -> Path:
        return self.path / "normalized" / dataset_id / "events.ndjson"

    def events(self, dataset_id: str) -> list[Json]:
        return ndjson(self.events_path(dataset_id))

    def features(self, feature_id: str) -> list[Json]:
        return ndjson(self.path / "features" / feature_id / "features.ndjson")

    def labels(self, label_id: str) -> list[Json]:
        return ndjson(self.path / "labels" / label_id / "labels.ndjson")


def audit_plan(
    audit: Audit, root: Root, plan: Json, collection: Json
) -> dict[str, Json]:
    """Recomputes the folds from raw events; returns each member's facts."""
    instrument = plan["instrument"]
    facts: dict[str, Json] = {}
    for member in collection["members"]:
        dataset_id = member["dataset"]["dataset_id"]
        events = root.events(dataset_id)
        times = [int(event["receive_timestamp_ns"]) for event in events]
        facts[dataset_id] = {
            "end": max(times) + 1,
            "feature_id": member["features"]["artifact_id"],
            "holds": any(event["instrument"] == instrument for event in events),
            "label_id": member["labels"]["artifact_id"],
            "start": min(times),
        }
    timeline = sorted(
        (key for key in facts if facts[key]["holds"]),
        key=lambda key: int(facts[key]["start"]),
    )
    minimum = int(plan["request"]["minimum_training_sources"])
    audit.check(len(plan["folds"]) == len(timeline) - minimum, "plan: fold count")
    for position, fold in enumerate(plan["folds"]):
        where = f"plan: fold {fold['index']}"
        evaluated = timeline[minimum + position]
        window = fold["evaluation"]["window"]
        audit.check(
            fold["evaluation"]["dataset_id"] == evaluated,
            f"{where} evaluates the next recording",
        )
        audit.check(
            int(window["start_at_ns"]) == facts[evaluated]["start"],
            f"{where} window start",
        )
        audit.check(
            int(window["end_at_ns"]) == facts[evaluated]["end"], f"{where} window end"
        )
        audit.check(
            fold["information_boundary_ns"] == window["start_at_ns"],
            f"{where} boundary",
        )
        trained = [member["dataset_id"] for member in fold["training"]["members"]]
        audit.check(
            trained == timeline[: minimum + position], f"{where} expanding membership"
        )
    return facts


def file_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def latest(*times: int | None) -> int | None:
    known = [time for time in times if time is not None]
    return max(known) if known else None


def eligible(
    root: Root, fact: Json, instrument: str, boundary: int
) -> tuple[Counter[str], int | None, int | None]:
    """A member's training class support for a boundary, and the latest
    prediction and label-availability times among those rows."""
    features = {
        row["ingest_sequence"]: row
        for row in root.features(fact["feature_id"])
        if row["instrument"] == instrument
    }
    support: Counter[str] = Counter()
    predicted: int | None = None
    available: int | None = None
    for label in root.labels(fact["label_id"]):
        feature = (
            features.get(label["ingest_sequence"])
            if label["instrument"] == instrument
            else None
        )
        if (
            feature is None
            or inputs_of(feature) is None
            or "direction" not in label["label"]
        ):
            continue
        prediction = int(label["prediction_at_ns"])
        availability = int(label["label_available_at_ns"])
        if prediction < boundary and availability < boundary:
            support[label["label"]["direction"]] += 1
            predicted = latest(predicted, prediction)
            available = latest(available, availability)
    return support, predicted, available


def scored_rows(rows: list[Json], labels: dict[str, Json], end: int) -> Json:
    truth: list[int] = []
    predicted: list[int] = []
    probabilities: list[list[float]] = []
    for row in rows:
        label = labels[row["identity"]["ingest_sequence"]]
        prediction = row["prediction"]
        if "class" not in prediction or "direction" not in label["label"]:
            continue
        if int(label["label_available_at_ns"]) >= end:
            continue
        truth.append(CLASSES.index(label["label"]["direction"]))
        predicted.append(CLASSES.index(prediction["class"]))
        probabilities.append([float(value) for value in prediction["probabilities"]])
    return {"predicted": predicted, "probabilities": probabilities, "truth": truth}


def metrics(scored: Json) -> Json:
    truth: list[int] = scored["truth"]
    predicted: list[int] = scored["predicted"]
    probabilities: list[list[float]] = scored["probabilities"]
    confusion = {actual: dict.fromkeys(CLASSES, 0) for actual in CLASSES}
    for actual, guess in zip(truth, predicted, strict=True):
        confusion[CLASSES[actual]][CLASSES[guess]] += 1
    rows = len(truth)
    recalls = [
        Fraction(confusion[name][name], sum(confusion[name].values()))
        for name in CLASSES
        if sum(confusion[name].values())
    ]
    true_probability = [probabilities[index][truth[index]] for index in range(rows)]
    brier = math.fsum(
        math.fsum(
            (probabilities[index][k] - (1.0 if k == truth[index] else 0.0)) ** 2
            for k in range(3)
        )
        for index in range(rows)
    )
    return {
        "accuracy": sum(confusion[name][name] for name in CLASSES) / rows,
        "balanced_accuracy": float(sum(recalls) / len(recalls)),
        "brier": brier / rows,
        "confusion": confusion,
        "log_loss": (
            None
            if min(true_probability) <= 0
            else -math.fsum(math.log(p) for p in true_probability) / rows
        ),
        "rows": rows,
    }


def compare_metrics(audit: Audit, claimed: Json, mine: Json, where: str) -> None:
    counts = {
        actual: {guess: int(value) for guess, value in row.items()}
        for actual, row in claimed["confusion_matrix"].items()
    }
    audit.check(counts == mine["confusion"], f"scores: {where} confusion counts")
    audit.check(int(claimed["rows"]) == mine["rows"], f"scores: {where} scored rows")
    for name, key in (
        ("accuracy", "accuracy"),
        ("balanced_accuracy", "balanced_accuracy"),
        ("brier_score", "brier"),
    ):
        value = metric(claimed[name])
        audit.check(
            value is not None and close(value, mine[key]),
            f"scores: {where} {name} {value} vs {mine[key]}",
        )
    loss = metric(claimed["log_loss"])
    expected = mine["log_loss"]
    audit.check(
        (loss is None) == (expected is None)
        and (loss is None or expected is None or close(loss, expected)),
        f"scores: {where} log_loss {loss} vs {expected}",
    )


def flattened(document: Json, prefix: str = "") -> dict[str, Any]:
    """Every leaf of a JSON object by its dotted path."""
    leaves: dict[str, Any] = {}
    for key, value in document.items():
        if isinstance(value, dict):
            leaves.update(flattened(value, f"{prefix}{key}."))
        else:
            leaves[f"{prefix}{key}"] = value
    return leaves


def expected_configuration(plan: Json, scenario: Json) -> Json:
    """The run configuration the plan requires of a cell: its eight frozen
    options with the scenario's fee and latency, in the engine's form."""
    options = plan["configuration"]
    return {
        "execution": {
            "fee_per_fill": scenario["fee_per_fill"],
            "latency_ns": scenario["latency_ns"],
            "maximum_order_notional": options["maximum_order_notional"],
            "maximum_quote_age_ns": options["maximum_quote_age_ns"],
            "order_type": "market",
            "time_in_force": "immediate_or_cancel",
        },
        "policy": {
            "decision_interval_ns": options["decision_interval_ns"],
            "name": "long_flat_threshold",
            "target_quantity": options["target_quantity"],
            "threshold_ppb": options["threshold_ppb"],
            "version": "1",
        },
        "starting_cash": options["starting_cash"],
    }


def check_against_plan(audit: Audit, run: Json, expected: Json, short: str) -> None:
    """Every setting, source, and window the plan fixes, field by field."""
    observed = flattened(
        {
            "configuration": run["configuration"],
            "schema": run["schema"],
            "source": {key: run["source"].get(key) for key in expected["source"]},
            "window": run["window"],
        }
    )
    for path, value in flattened(expected).items():
        audit.check(
            observed.get(path) == value,
            f"run: {short} {path} is {observed.get(path)!r}, the plan's {value!r}",
        )


def audit_run(
    audit: Audit,
    root: Root,
    cell: Json,
    schedule_rows: list[Json],
    events: list[Json],
    instrument: str,
    window: Json,
    expected: Json,
) -> dict[str, Json]:
    """Replays one run's journal against raw quotes, and every decision
    against the plan's policy; its accounts' exact figures."""
    run_id = cell["run"]["artifact_id"]
    short = run_id[:12]
    directory = root.path / "strategies" / run_id
    run = read_json(directory / "manifest.json")
    entries = ndjson(directory / "journal.ndjson")
    check_against_plan(audit, run, expected, short)
    configuration = run["configuration"]
    fee = Decimal(configuration["execution"]["fee_per_fill"])
    latency = int(configuration["execution"]["latency_ns"])
    starting = Decimal(configuration["starting_cash"])
    policy = expected["configuration"]["policy"]
    threshold = int(policy["threshold_ppb"])
    target_quantity = Decimal(policy["target_quantity"])
    interval = int(policy["decision_interval_ns"])
    quotes = {
        event["ingest_sequence"]: event
        for event in events
        if event["instrument"] == instrument
    }
    # The instrument's quotes in ingest order, which is receive order.
    timeline = sorted(quotes.values(), key=lambda quote: int(quote["ingest_sequence"]))
    receive_times = [int(quote["receive_timestamp_ns"]) for quote in timeline]
    predictions = {
        row["identity"]["ingest_sequence"]: row["prediction"] for row in schedule_rows
    }
    start, end = int(window["start_at_ns"]), int(window["end_at_ns"])
    state: dict[str, Json] = {
        name: {
            "cash": starting,
            "fees": Decimal(0),
            "fills": 0,
            "position": Decimal(0),
            "turnover": Decimal(0),
        }
        for name in ACCOUNTS
    }
    orders: dict[str, Json] = {}
    resolved: Counter[str] = Counter()
    pending: dict[str, str | None] = dict.fromkeys(ACCOUNTS)
    last_decided: int | None = None
    scheduled_target: Decimal | None = None
    for entry in entries:
        if entry["type"] == "decision" and entry["account"] == "strategy":
            last_decided, scheduled_target = audit_decision(
                audit,
                entry,
                state["strategy"]["position"],
                pending["strategy"] is not None,
                (threshold, target_quantity, interval),
                last_decided,
                short,
            )
        if entry["type"] == "decision" and entry["account"] == "buy_and_hold":
            audit.check(
                entry["reason"] == "benchmark_entry"
                and Decimal(entry["target"]) == target_quantity,
                f"policy: {short} buy and hold enters at the plan's target",
            )
        if entry["type"] == "order":
            orders[entry["client_order_id"]] = entry
            pending[entry["account"]] = entry["client_order_id"]
            if entry["account"] == "strategy":
                position = state["strategy"]["position"]
                audit.check(
                    scheduled_target is not None
                    and Decimal(entry["quantity"]) == abs(scheduled_target - position)
                    and entry["side"]
                    == ("buy" if scheduled_target > position else "sell"),
                    f"policy: {short} {entry['client_order_id']} trades to its decision's target",
                )
                scheduled_target = None
        if entry["type"] in ("execution", "order_expired"):
            resolved[entry["client_order_id"]] += 1
            pending[entry["account"]] = None
            first = bisect.bisect_left(receive_times, int(entry["arrival_at_ns"]))
            eligible = timeline[first] if first < len(timeline) else None
            if eligible is not None and int(eligible["receive_timestamp_ns"]) >= end:
                eligible = None
            where = f"order: {short} {entry['client_order_id']}"
            if entry["type"] == "execution":
                audit.check(
                    eligible is not None
                    and entry["observation"]["ingest_sequence"]
                    == eligible["ingest_sequence"],
                    f"{where} executes on the first quote at or after its arrival",
                )
            else:
                audit.check(
                    eligible is None, f"{where} expires only with no eligible quote"
                )
        if entry["type"] == "decision":
            signal = entry["signal"]
            prediction = predictions.get(signal["ingest_sequence"])
            audit.check(
                prediction is not None
                and prediction.get("class") == signal["class"]
                and prediction.get("confidence_ppb") == signal["confidence_ppb"],
                f"journal: {short} decision signal is its schedule row's prediction",
            )
        if entry["type"] != "execution" or not entry["fill"]:
            continue
        account = state[entry["account"]]
        fill = entry["fill"]
        where = f"fill: {fill['fill_id']}"
        quote = quotes[entry["observation"]["ingest_sequence"]]
        buying = Decimal(entry["position_after"]) > account["position"]
        price = raw_price((quote["ask"] if buying else quote["bid"])["price_raw"])
        quantity = Decimal(fill["quantity"])
        notional = price * quantity
        audit.check(
            Decimal(fill["price"]) == price,
            f"{where} price is the raw {'ask' if buying else 'bid'}",
        )
        audit.check(Decimal(fill["notional"]) == notional, f"{where} notional")
        audit.check(Decimal(fill["fee"]) == fee, f"{where} fee per fill")
        audit.check(
            int(entry["arrival_at_ns"]) == int(entry["signal_at_ns"]) + latency,
            f"{where} latency",
        )
        audit.check(
            int(entry["executed_at_ns"])
            >= int(entry["arrival_at_ns"])
            > int(entry["signal_at_ns"]),
            f"{where} executes after arrival, never on the signal's quote",
        )
        audit.check(
            int(quote["receive_timestamp_ns"]) == int(entry["executed_at_ns"]),
            f"{where} on its quote",
        )
        audit.check(
            start <= int(entry["executed_at_ns"]) < end, f"{where} inside the window"
        )
        account["cash"] += -notional - fee if buying else notional - fee
        account["position"] += quantity if buying else -quantity
        account["fees"] += fee
        account["turnover"] += notional
        account["fills"] += 1
        audit.check(
            Decimal(entry["cash_after"]) == account["cash"], f"{where} cash after"
        )
        audit.check(
            Decimal(entry["position_after"]) == account["position"],
            f"{where} position after",
        )
    audit.check(
        set(resolved) == set(orders) and all(count == 1 for count in resolved.values()),
        f"order: {short} every order resolves exactly once",
    )
    final = [entry for entry in entries if entry["type"] == "valuation"][-1]
    valued_at = int(run["results"]["valued_at_ns"])
    marks = [
        quote
        for quote in quotes.values()
        if int(quote["receive_timestamp_ns"]) <= valued_at
    ]
    mark = (
        raw_price(
            max(marks, key=lambda quote: int(quote["ingest_sequence"]))["bid"][
                "price_raw"
            ]
        )
        if marks
        else None
    )
    results: dict[str, Json] = {}
    for name in ACCOUNTS:
        where = f"account: {short} {name}"
        account = state[name]
        claimed = run["results"]["accounts"][name]
        valuation = claimed["final"]["valuation"]
        if "unavailable" in valuation:
            audit.not_audited.append(
                f"{where} valuation unavailable ({valuation['unavailable']['code']})"
            )
            results[name] = {"unavailable": valuation["unavailable"]["code"]}
            continue
        held = (
            account["position"] * mark
            if account["position"] and mark is not None
            else Decimal(0)
        )
        equity = account["cash"] + held
        pnl = equity - starting
        audit.check(
            Decimal(claimed["final"]["ledger"]["cash"]) == account["cash"],
            f"{where} cash",
        )
        audit.check(
            Decimal(claimed["final"]["ledger"]["position"]) == account["position"],
            f"{where} position",
        )
        audit.check(
            Decimal(valuation["equity"]) == equity,
            f"{where} equity {valuation['equity']} vs {equity}",
        )
        audit.check(Decimal(valuation["fees_paid"]) == account["fees"], f"{where} fees")
        audit.check(Decimal(valuation["net_total_pnl"]) == pnl, f"{where} pnl")
        audit.check(
            Decimal(valuation["return_percent"]) == percentage(pnl, starting),
            f"{where} return",
        )
        audit.check(
            Decimal(claimed["turnover"]["notional"]) == account["turnover"],
            f"{where} turnover",
        )
        audit.check(int(claimed["fills"]) == account["fills"], f"{where} fills")
        placed = [order for order in orders.values() if order["account"] == name]
        expired = [
            entry
            for entry in entries
            if entry["type"] == "order_expired" and entry["account"] == name
        ]
        audit.check(
            int(claimed["orders"]["scheduled"]) == len(placed),
            f"{where} orders scheduled",
        )
        audit.check(
            int(claimed["orders"]["expired"]) == len(expired), f"{where} orders expired"
        )
        audit.check(
            final["accounts"][name]["net_total_pnl"] == valuation["net_total_pnl"],
            f"journal: {short} {name} final",
        )
        audit.check(
            cell["accounts"][name]["valuation"]["net_total_pnl"]
            == valuation["net_total_pnl"],
            f"study: {short} {name} cell pnl",
        )
        results[name] = {**account, "capital": starting, "pnl": pnl}
    for benchmark in BENCHMARKS:
        if "unavailable" in results["strategy"] or "unavailable" in results[benchmark]:
            audit.check(
                "unavailable" in cell["deltas"][benchmark],
                f"study: {short} delta vs {benchmark} unavailable",
            )
            continue
        difference = results["strategy"]["pnl"] - results[benchmark]["pnl"]
        claimed_delta = cell["deltas"][benchmark]
        audit.check(
            Decimal(claimed_delta["net_total_pnl"]) == difference,
            f"study: {short} delta vs {benchmark} pnl",
        )
        audit.check(
            Decimal(claimed_delta["return_percent"])
            == percentage(difference, starting),
            f"study: {short} delta vs {benchmark} return",
        )
    return results


def audit_decision(
    audit: Audit,
    entry: Json,
    position: Decimal,
    order_pending: bool,
    policy: tuple[int, Decimal, int],
    last_decided: int | None,
    short: str,
) -> tuple[int, Decimal | None]:
    """One strategy decision against long_flat_threshold version 1 under
    the plan's threshold, target, and decision interval: a flat prediction
    holds; one below the threshold holds; otherwise up targets the target
    and down targets flat, held while an order is pending or at the target."""
    threshold, target_quantity, interval = policy
    signal = entry["signal"]
    at = int(signal["available_at_ns"])
    where = f"policy: {short} decision {entry['decision']}"
    audit.check(
        last_decided is None or at - last_decided >= interval,
        f"{where} is at least the decision interval after the last",
    )
    audit.check(
        Decimal(entry["position_before"]) == position, f"{where} position before"
    )
    target: Decimal | None = None
    if signal["class"] == "flat":
        reason = "hold_flat_prediction"
    elif int(signal["confidence_ppb"]) < threshold:
        reason = "hold_below_threshold"
    else:
        target = target_quantity if signal["class"] == "up" else Decimal(0)
        if order_pending:
            reason = "order_pending"
        elif position == target:
            reason = "already_at_target"
        else:
            reason = "order_scheduled"
    recorded = None if entry["target"] is None else Decimal(entry["target"])
    audit.check(
        entry["reason"] == reason and recorded == target,
        f"{where} is {entry['reason']} toward {entry['target']}, the plan's policy gives {reason} toward {target}",
    )
    return at, target if reason == "order_scheduled" else None


def audit_totals(
    audit: Audit, claimed: Json, folds: list[dict[str, Json]], where: str
) -> None:
    capital = sum((fold["strategy"]["capital"] for fold in folds), Decimal(0))
    audit.check(
        Decimal(claimed["starting_capital"]) == capital,
        f"aggregate: {where} total capital",
    )
    for account in ACCOUNTS:
        figures = claimed["accounts"][account]
        pnl = sum((fold[account]["pnl"] for fold in folds), Decimal(0))
        name = f"aggregate: {where} {account}"
        audit.check(Decimal(figures["net_total_pnl"]) == pnl, f"{name} pnl")
        audit.check(
            Decimal(figures["fees_paid"])
            == sum((f[account]["fees"] for f in folds), Decimal(0)),
            f"{name} fees",
        )
        audit.check(
            Decimal(figures["turnover"])
            == sum((f[account]["turnover"] for f in folds), Decimal(0)),
            f"{name} turnover",
        )
        audit.check(
            int(figures["fills"]) == sum(int(f[account]["fills"]) for f in folds),
            f"{name} fills",
        )
        expected = canonical(percentage(pnl, capital))
        audit.check(
            figures["return_percent"] == expected,
            f"{name} return {figures['return_percent']} vs {expected}",
        )
    for benchmark in BENCHMARKS:
        delta = sum(
            (fold["strategy"]["pnl"] - fold[benchmark]["pnl"] for fold in folds),
            Decimal(0),
        )
        audit.check(
            Decimal(claimed["deltas"][benchmark]["net_total_pnl"]) == delta,
            f"aggregate: {where} vs {benchmark}",
        )


def run_audit(artifacts: Path, study_id: str) -> Json:
    audit = Audit()
    root = Root(artifacts)
    study = read_json(artifacts / "walk_forward_studies" / study_id / "manifest.json")
    plan = read_json(
        artifacts / "walk_forward_plans" / study["plan_id"] / "manifest.json"
    )
    collection = read_json(
        artifacts
        / "collections"
        / plan["collection"]["collection_id"]
        / "manifest.json"
    )
    instrument = plan["instrument"]
    facts = audit_plan(audit, root, plan, collection)
    pooled: dict[str, list[Json]] = {name: [] for name in study["models"]}
    runs: dict[tuple[str, str], dict[str, dict[str, Json]]] = {}
    for fold, planned in zip(study["folds"], plan["folds"], strict=True):
        index = fold["index"]
        boundary = int(planned["information_boundary_ns"])
        support: Counter[str] = Counter()
        predicted: int | None = None
        available: int | None = None
        for member in planned["training"]["members"]:
            member_support, member_predicted, member_available = eligible(
                root, facts[member["dataset_id"]], instrument, boundary
            )
            support += member_support
            predicted = latest(predicted, member_predicted)
            available = latest(available, member_available)
        claimed_support = {
            name: int(planned["training"]["class_support"][name]) for name in CLASSES
        }
        audit.check(
            claimed_support == {name: support[name] for name in CLASSES},
            f"training: fold {index} plan support",
        )
        if fold["fold_model"]["state"] != "completed":
            audit.not_audited.append(
                f"fold {index}: fold model {fold['fold_model']['state']}"
            )
            continue
        fold_model = read_json(
            artifacts
            / "walk_forward_models"
            / fold["fold_model"]["fold_model_id"]
            / "manifest.json"
        )
        training = fold_model["training"]
        audit.check(
            {name: int(training["class_support"][name]) for name in CLASSES}
            == {name: support[name] for name in CLASSES},
            f"training: fold {index} class support",
        )
        audit.check(
            int(training["rows"]) == sum(support.values()),
            f"training: fold {index} eligible rows",
        )
        cutoffs = (
            None if available is None else str(available),
            None if predicted is None else str(predicted),
        )
        audit.check(
            (training["max_label_available_at_ns"], training["max_prediction_at_ns"])
            == cutoffs,
            f"training: fold {index} latest label and prediction times",
        )
        audit.check(
            available is None or available < boundary,
            f"training: fold {index} labels before boundary",
        )
        events_sha256 = file_sha256(
            root.events_path(planned["evaluation"]["dataset_id"])
        )
        evaluated = facts[planned["evaluation"]["dataset_id"]]
        window = planned["evaluation"]["window"]
        features = [
            row
            for row in root.features(evaluated["feature_id"])
            if row["instrument"] == instrument
        ]
        labels = {
            row["ingest_sequence"]: row
            for row in root.labels(evaluated["label_id"])
            if row["instrument"] == instrument
        }
        events = root.events(planned["evaluation"]["dataset_id"])
        for name in study["models"]:
            entry = fold["models"][name]
            if entry["state"] != "completed":
                audit.not_audited.append(
                    f"fold {index}: {name} {entry['state']} ({entry['reason']})"
                )
                continue
            schedule_path = (
                artifacts
                / "schedules"
                / entry["schedule"]["artifact_id"]
                / "schedule.ndjson"
            )
            schedule = ndjson(schedule_path)
            where = f"schedule: fold {index} {name}"
            audit.check(
                [row["identity"]["ingest_sequence"] for row in schedule]
                == [row["ingest_sequence"] for row in features],
                f"{where} every evaluation feature row, in order",
            )
            audit.check(
                all("label" not in json.dumps(row) for row in schedule),
                f"{where} carries no label",
            )
            audit.check(
                all(
                    row["inputs"]["raw"] == [str(value) for value in inputs]
                    for row, feature in zip(schedule, features, strict=False)
                    if (inputs := inputs_of(feature)) is not None
                ),
                f"{where} inputs are the raw features",
            )
            scored = scored_rows(schedule, labels, int(window["end_at_ns"]))
            if "unavailable" in entry["predictive"]:
                audit.not_audited.append(f"fold {index}: {name} scores unavailable")
            else:
                pooled[name].append(scored)
                compare_metrics(
                    audit,
                    entry["predictive"]["metrics"],
                    metrics(scored),
                    f"fold {index} {name}",
                )
            schedule_manifest = read_json(
                artifacts
                / "schedules"
                / entry["schedule"]["artifact_id"]
                / "manifest.json"
            )
            audit.check(
                (
                    schedule_manifest["model"]["plan_id"],
                    schedule_manifest["model"]["fold"],
                    schedule_manifest["model"]["name"],
                    schedule_manifest["window"],
                )
                == (plan["plan_id"], index, name, window),
                f"schedule: fold {index} {name} is the plan's",
            )
            audit.check(
                [cell["scenario"] for cell in entry["cells"]]
                == [scenario["name"] for scenario in plan["scenarios"]],
                f"plan: fold {index} {name} holds every planned scenario once",
            )
            for cell, scenario in zip(entry["cells"], plan["scenarios"], strict=False):
                audit.check(
                    (cell["fee_per_fill"], cell["latency_ns"])
                    == (scenario["fee_per_fill"], scenario["latency_ns"]),
                    f"plan: fold {index} {name} {cell['scenario']} is the plan's scenario",
                )
                if cell["state"] != "completed":
                    audit.not_audited.append(
                        f"fold {index}: {name} {cell['scenario']} {cell['state']}"
                    )
                    continue
                if "run" not in cell:
                    audit.check(
                        False,
                        f"fold {index}: {name} {cell['scenario']}: completed cell has no run",
                    )
                    continue
                expected = {
                    "configuration": expected_configuration(plan, scenario),
                    "schema": {"name": "chronos.strategy_run", "version": "3"},
                    "source": {
                        "collection_id": plan["collection"]["collection_id"],
                        "dataset_id": planned["evaluation"]["dataset_id"],
                        "events_sha256": events_sha256,
                        "feature_artifact_id": planned["evaluation"][
                            "feature_artifact_id"
                        ],
                        "fold": index,
                        "fold_model_id": fold["fold_model"]["fold_model_id"],
                        "information_boundary_ns": planned["information_boundary_ns"],
                        "instrument": instrument,
                        "model": name,
                        "parameters_sha256": fold_model["models"][name][
                            "parameters_sha256"
                        ],
                        "plan_id": plan["plan_id"],
                        "schedule_id": entry["schedule"]["artifact_id"],
                        "schedule_sha256": file_sha256(schedule_path),
                        "stream": planned["evaluation"]["stream"],
                        "training_labels_available_through_ns": cutoffs[0],
                        "training_predictions_through_ns": cutoffs[1],
                    },
                    "window": window,
                }
                result = audit_run(
                    audit, root, cell, schedule, events, instrument, window, expected
                )
                runs.setdefault((name, cell["scenario"]), {})[index] = result
    for name, parts in pooled.items():
        aggregate = study["aggregates"]["predictive"][name]
        if not parts:
            audit.check(
                aggregate["status"] == "unavailable",
                f"scores: pooled {name} unavailable",
            )
            continue
        joined = {
            key: [value for part in parts for value in part[key]]
            for key in ("predicted", "probabilities", "truth")
        }
        compare_metrics(
            audit, aggregate["pooled"]["metrics"], metrics(joined), f"pooled {name}"
        )
    for name in study["models"]:
        for scenario in (cell["name"] for cell in study["scenarios"]):
            aggregate = study["aggregates"]["trading"][name][scenario]
            valued = {
                index: figures
                for index, figures in runs.get((name, scenario), {}).items()
                if all("unavailable" not in figures[account] for account in ACCOUNTS)
            }
            planned_folds = [fold["index"] for fold in study["folds"]]
            audit.check(
                aggregate["folds"]["included"]
                == [index for index in planned_folds if index in valued],
                f"aggregate: {name} {scenario} included folds",
            )
            folds = [valued[index] for index in planned_folds if index in valued]
            if "unavailable" in aggregate["complete"]:
                audit.check(
                    len(valued) < len(planned_folds),
                    f"aggregate: {name} {scenario} complete is unavailable",
                )
                if "partial" in aggregate:
                    audit_totals(
                        audit, aggregate["partial"], folds, f"{name} {scenario} partial"
                    )
            else:
                audit.check(
                    len(valued) == len(planned_folds),
                    f"aggregate: {name} {scenario} complete covers all",
                )
                audit_totals(audit, aggregate["complete"], folds, f"{name} {scenario}")
    return {
        "checks": dict(sorted(audit.checks.items())),
        "checks_total": sum(audit.checks.values()),
        "not_audited": audit.not_audited,
        "problems": audit.problems,
        "study_id": study["study_id"],
        "verdict": "pass" if not audit.problems else "fail",
    }


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: audit.py <artifacts> <study-id>", file=sys.stderr)
        return 2
    report = run_audit(Path(argv[1]), argv[2])
    print(json.dumps(report, indent=2))
    return 0 if report["verdict"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
