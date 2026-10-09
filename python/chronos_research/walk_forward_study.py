"""Walk-forward studies: a plan's every fold, model, and scenario cell, run
through the C++ engine, scored after its predictions exist, and aggregated
without inventing a portfolio (ADR 0022).

    <artifacts>/walk_forward_studies/<study_id>/manifest.json

For each fold of a verified plan, in order:

1. Its fresh fold models are fitted on its training rows and published
   (fold_models.py).
2. Each fitted model's version 3 schedule is built from the evaluation
   recording's features and the frozen parameters, every feature row of the
   window, and published (walk_forward_schedule.py).
3. Each schedule runs once per scenario through `chronos_strategy`, with
   only the fee per fill and the latency changed, and each run is replayed by
   `chronos_strategy inspect`, which recomputes every row from the dataset.
   Every run is its own strategy, cash, and buy-and-hold accounts with the
   configured starting cash.
4. Only then are the evaluation labels read, and each schedule is scored.

Every planned fold, model, and cell is kept: completed, unevaluable (a model
that could not be fitted), failed (something ran and failed), or
unavailable (nothing to run, because its model or fold has no schedule),
each with its reason. A study with anything not completed is `partial`, and
lists what is missing.

Aggregates follow fixed rules. Classification is pooled over the scored
rows of every fold of a model, so every row weighs the same; per-fold
scores stay beside it, and fold metrics are never averaged. Money is summed
over independent reset accounts in the engine's own fixed point (8 decimal
places, 64-bit range, checked): PnL, fees, turnover, and total starting
capital, with the return derived once from total PnL over total capital,
rounded half away from zero to 8 places, as the engine rounds. A fold whose
cell did not complete, or whose final valuation is unavailable, makes the
complete aggregate unavailable; a partial aggregate then names its included
and planned folds. Nothing here splices equity curves, averages drawdowns,
annualizes, computes confidence intervals, or picks a winner.

The study ID is the SHA-256 of the canonical manifest without the ID, with
its newline. Inspection verifies the sources and plan again, rebuilds every
fold model and schedule and compares their bytes with the published ones,
replays every recorded run through the engine, rescores, re-aggregates,
and requires the manifest it rebuilds to equal the stored one.

A run counts for a cell only if it is of that cell's schedule, with the
schedule's source and window and the evaluation recording's events, under
the plan's eight frozen options with the scenario's fee and latency
(`expected_run`); any other run makes the cell failed (`run_mismatch`).
The only thing inspection takes from a stored study without re-deriving it
is a run that never started: a cell without a run must be exactly
`{detail, fee_per_fill, latency_ns, reason: run_failed, scenario, state:
failed}` under its own scenario, or the study is refused
(`artifact_invalid`).
"""

import json
import re
from collections import Counter
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    listed_files,
    verified_file,
    verified_manifest,
)
from chronos_research.artifacts import ArtifactError, VerifiedRecording, is_artifact_id
from chronos_research.collection import CollectionError
from chronos_research.comparison import account_summary
from chronos_research.encoding import canonical_json, encode_integer, sha256_hex
from chronos_research.experiment import environment
from chronos_research.fold_models import (
    FoldModel,
    FoldModelError,
    build_fold_model,
    fold_models_root,
    publish_fold_model,
    verified_fold_model,
)
from chronos_research.models import Unevaluable
from chronos_research.pipeline import (
    ArtifactRoots,
    PipelineError,
    Tools,
    inspect_strategy,
    run_strategy,
)
from chronos_research.publication import PublicationError, publish
from chronos_research.schedule import MANIFEST as SCHEDULE_MANIFEST
from chronos_research.schedule import ROWS, Schedule, ScheduleError, verify_schedule
from chronos_research.scoring import ScoredRows, ScoringError, pooled, scored_rows, scores
from chronos_research.strategy_configuration import strategy_options
from chronos_research.walk_forward import MODELS, PlanError, PlanSources, verified_plan_sources
from chronos_research.walk_forward_schedule import (
    build_fold_schedule,
    fold_labels,
    fold_window,
    publish_fold_schedule,
)

SCHEMA = {"name": "chronos.walk_forward_study", "version": "1"}
MANIFEST = "manifest.json"
ACCOUNTS = ("strategy", "buy_and_hold", "cash")
BENCHMARKS = ("buy_and_hold", "cash")
# The engine's fixed point (core/include/chronos/fixed_decimal.hpp).
MONEY_SCALE = 10**8
PERCENT_SCALE = 10**8
_INT64_MIN, _INT64_MAX = -(2**63), 2**63 - 1
_MONEY = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]{0,7}[1-9])?")
SCOPE = (
    "historical evaluation only: every figure describes what the frozen fold models' predictions did in "
    "these recordings; nothing is promoted, registered, or deployed"
)
EXECUTION = (
    "each fold model's schedule predicts every feature row of its evaluation window from its features and "
    "frozen parameters alone; labels are read only afterwards, to score it",
    "each schedule runs once per scenario through chronos_strategy with only the fee per fill and the "
    "latency changed, and each run is replayed by chronos_strategy inspect",
    "every run is its own strategy, cash, and buy-and-hold accounts with the configured starting cash; "
    "nothing carries from one fold, model, or scenario to another",
    "no stress result is derived from another run: latency changes fills, quantities, risk outcomes, and "
    "later decisions, so every cell is executed",
)
AGGREGATION = {
    "classification": (
        "pooled over the scored rows of every included fold of a model, so every scored row weighs the "
        "same; fold accuracies are never averaged; per-fold scores are reported beside it"
    ),
    "complete": (
        "a complete aggregate needs every planned fold's cell completed with every account's final "
        "valuation; otherwise it is unavailable, with what is missing"
    ),
    "money": (
        "independent reset accounts, one per fold: PnL, fees paid, and turnover are summed, as is the "
        "starting capital each fold's account was given; nothing is compounded"
    ),
    "partial": (
        "a partial aggregate covers only the included folds, which it names beside the planned count; it "
        "is never the complete result"
    ),
    "return_percent": (
        "total net PnL as a percentage of total starting capital, rounded once, half away from zero, to 8 "
        "decimal places, as the engine rounds a run's return; never a sum or average of returns"
    ),
    "not_computed": (
        "no spliced equity curve, portfolio drawdown, annualized ratio, confidence interval, or winner"
    ),
}

type Progress = Callable[[Mapping[str, str]], None]


class StudyError(Exception):
    """A study cannot be run, read, or reproduced."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def studies_root(artifacts: Path) -> Path:
    return artifacts / "walk_forward_studies"


# --- Exact money ----------------------------------------------------------------------------------------


def _checked(raw: int) -> int:
    if not _INT64_MIN <= raw <= _INT64_MAX:
        raise StudyError("arithmetic_overflow", "an aggregate exceeds the engine's 64-bit fixed-point range")
    return raw


def money_raw(text: str) -> int:
    """A canonical engine decimal as its fixed-point integer, exactly."""
    if not _MONEY.fullmatch(text):
        raise StudyError("artifact_invalid", f"{text!r} is not an engine decimal")
    negative = text.startswith("-")
    whole, _, fraction = text.lstrip("-").partition(".")
    magnitude = int(whole) * MONEY_SCALE + int(fraction.ljust(8, "0") or "0")
    return _checked(-magnitude if negative else magnitude)


def money_text(raw: int) -> str:
    """A fixed-point integer as the engine writes it: no trailing zeros."""
    _checked(raw)
    whole, fraction = divmod(abs(raw), MONEY_SCALE)
    text = str(whole)
    if fraction:
        text += "." + str(fraction).rjust(8, "0").rstrip("0")
    return f"-{text}" if raw < 0 else text


def percentage_of(part: int, whole: int) -> str:
    """`part` as a percentage of `whole`, both fixed-point money, rounded
    half away from zero to the percentage scale, as the engine's
    percentage_of rounds."""
    if whole <= 0:
        raise StudyError("arithmetic_invalid", "a return needs positive starting capital")
    numerator = part * 100 * PERCENT_SCALE
    quotient, remainder = divmod(abs(numerator), whole)
    if remainder >= whole - remainder:
        quotient += 1
    return money_text(_checked(-quotient if numerator < 0 else quotient))


# --- Running one study ----------------------------------------------------------------------------------


@dataclass(frozen=True)
class StudyContext:
    tools: Tools
    artifacts: Path
    roots: ArtifactRoots
    sources: PlanSources
    progress: Progress
    # When inspecting: the stored study, whose artifacts must already exist.
    stored: Mapping[str, Any] | None = None

    @property
    def plan(self) -> Mapping[str, Any]:
        return self.sources.plan


def _fold_model(context: StudyContext, index: int) -> FoldModel:
    fold = context.sources.fold(index)
    built = build_fold_model(context.plan, fold, context.sources.training(index))
    root = fold_models_root(context.artifacts)
    if context.stored is None:
        publish_fold_model(context.artifacts, built)
    try:
        stored = verified_fold_model(root, built.fold_model_id)
    except FoldModelError as error:
        raise FoldModelError("artifact_invalid", f"the published fold model: {error}") from error
    if stored.files != built.files:
        raise FoldModelError("not_reproduced", "the published fold model differs from the one refitted now")
    return built


def _schedule(context: StudyContext, index: int, model: FoldModel, name: str) -> Schedule:
    recording, _ = context.sources.evaluation(index)
    built = build_fold_schedule(context.plan, context.sources.fold(index), model, name, recording)
    if context.stored is None:
        publish_fold_schedule(context.roots.schedules, built)
    directory = context.roots.schedules / built.schedule_id
    verify_schedule(directory)
    for file_name, contents in built.files.items():
        if (directory / file_name).read_bytes() != contents:
            raise ScheduleError(f"the published schedule's {file_name} differs from the one rebuilt now")
    return built


def _stored_cell(context: StudyContext, index: int, name: str, scenario: str) -> Mapping[str, Any]:
    assert context.stored is not None
    try:
        cell: Mapping[str, Any] = next(
            cell
            for cell in context.stored["folds"][index - 1]["models"][name]["cells"]
            if cell["scenario"] == scenario
        )
    except (KeyError, IndexError, StopIteration, TypeError) as error:
        raise StudyError("not_reproduced", f"the study has no fold {index} {name} {scenario} cell") from error
    return cell


# The only cell a study may hold without a run: one whose run never started.
RUNLESS_FAILURE = ("detail", "fee_per_fill", "latency_ns", "reason", "scenario", "state")
RUN_SCHEMA = {"name": "chronos.strategy_run", "version": "3"}


def engine_configuration(configuration: Mapping[str, str], scenario: Mapping[str, str]) -> dict[str, Any]:
    """The run configuration the engine records for the plan's eight frozen
    options with the scenario's fee and latency: what every run of a cell
    must have run under."""
    return {
        "execution": {
            "fee_per_fill": scenario["fee_per_fill"],
            "latency_ns": scenario["latency_ns"],
            "maximum_order_notional": configuration["maximum_order_notional"],
            "maximum_quote_age_ns": configuration["maximum_quote_age_ns"],
            "order_type": "market",
            "time_in_force": "immediate_or_cancel",
        },
        "policy": {
            "decision_interval_ns": configuration["decision_interval_ns"],
            "name": "long_flat_threshold",
            "target_quantity": configuration["target_quantity"],
            "threshold_ppb": configuration["threshold_ppb"],
            "version": "1",
        },
        "starting_cash": configuration["starting_cash"],
    }


def expected_run(
    schedule: Schedule, recording: VerifiedRecording, configuration: Mapping[str, Any]
) -> dict[str, Any]:
    """What a cell's run must say: its schema, its source as the schedule
    and the verified evaluation recording name it, its window, and its whole
    configuration."""
    manifest = json.loads(schedule.files[SCHEDULE_MANIFEST])
    source, model, training = manifest["source"], manifest["model"], manifest["training"]
    return {
        "configuration": configuration,
        "schema": RUN_SCHEMA,
        "source": {
            "collection_id": source["collection_id"],
            "dataset_id": source["dataset_id"],
            "events_sha256": recording.dataset_manifest["files"]["events"]["sha256"],
            "feature_artifact_id": source["feature_artifact_id"],
            "fold": model["fold"],
            "fold_model_id": model["fold_model_id"],
            "information_boundary_ns": training["information_boundary_ns"],
            "instrument": model["instrument"],
            "model": model["name"],
            "parameters_sha256": model["parameters_sha256"],
            "plan_id": model["plan_id"],
            "schedule_id": schedule.schedule_id,
            "schedule_sha256": manifest["files"]["schedule"]["sha256"],
            "stream": source["stream"],
            "training_labels_available_through_ns": training["label_available_through_ns"],
            "training_predictions_through_ns": training["prediction_through_ns"],
        },
        "window": manifest["window"],
    }


def _run_difference(run: Mapping[str, Any], expected: Mapping[str, Any]) -> str | None:
    """Where a run differs from what its cell requires, if it does."""
    observed = {
        "configuration": run.get("configuration"),
        "schema": run.get("schema"),
        "source": {key: run.get("source", {}).get(key) for key in expected["source"]},
        "window": run.get("window"),
    }
    return None if observed == expected else _first_difference(dict(expected), observed)


def _runless(stored: Mapping[str, Any], cell: Mapping[str, Any], where: str) -> dict[str, Any]:
    """A stored cell without a run, kept only as exactly a run that never
    started under this cell's scenario: never results, figures, or any other
    state. Inspection cannot execute it again, so it is not trusted further."""
    if (
        tuple(sorted(stored)) != RUNLESS_FAILURE
        or stored["state"] != "failed"
        or stored["reason"] != "run_failed"
        or not isinstance(stored["detail"], str)
        or any(stored[key] != cell[key] for key in ("fee_per_fill", "latency_ns", "scenario"))
    ):
        raise StudyError(
            "artifact_invalid", f"{where}: a cell without a run can only be a run that failed to start"
        )
    return {**cell, "detail": stored["detail"], "reason": "run_failed", "state": "failed"}


def _cell(
    context: StudyContext, index: int, name: str, schedule: Schedule, scenario: Mapping[str, str]
) -> dict[str, Any]:
    """One scenario's run of one schedule: run and replayed, or failed. A
    run counts only if it is of this schedule, with this source, events, and
    window, under the plan's eight frozen options and this scenario's fee
    and latency."""
    where = f"fold {index} {name} {scenario['name']}"
    cell: dict[str, Any] = {
        "fee_per_fill": scenario["fee_per_fill"],
        "latency_ns": scenario["latency_ns"],
        "scenario": scenario["name"],
    }
    configuration = engine_configuration(context.plan["configuration"], scenario)
    if context.stored is None:
        options = {
            **context.plan["configuration"],
            **{key: scenario[key] for key in ("fee_per_fill", "latency_ns")},
        }
        try:
            step, _ = run_strategy(
                context.tools, context.roots, schedule.schedule_id, strategy_options(options)
            )
        except PipelineError as error:
            return {**cell, "detail": str(error), "reason": "run_failed", "state": "failed"}
        run_id = step.artifact_id
    else:
        stored = _stored_cell(context, index, name, scenario["name"])
        if "run" not in stored:
            return _runless(stored, cell, where)
        recorded = stored["run"].get("artifact_id") if isinstance(stored["run"], dict) else None
        if not isinstance(recorded, str) or not is_artifact_id(recorded):
            raise StudyError("artifact_invalid", f"{where}: not a run ID")
        run_id = recorded
    cell["run"] = {"artifact_id": run_id}
    try:
        replay = inspect_strategy(context.tools, context.roots, run_id)
    except PipelineError as error:
        return {**cell, "detail": str(error), "reason": "replay_failed", "state": "failed"}
    run = replay["artifact"]
    recording, _ = context.sources.evaluation(index)
    difference = _run_difference(run, expected_run(schedule, recording, configuration))
    if difference is not None:
        return {
            **cell,
            "detail": f"the run is not this cell's: it differs at {difference}",
            "reason": "run_mismatch",
            "state": "failed",
        }
    cell["run"]["replay"] = replay["reproduced"]
    cell["accounts"] = {account: account_summary(run["results"]["accounts"][account]) for account in ACCOUNTS}
    cell["starting_cash"] = configuration["starting_cash"]
    cell["deltas"] = cell_deltas(cell["accounts"], configuration["starting_cash"])
    return {**cell, "state": "completed"}


def cell_deltas(accounts: Mapping[str, Any], starting_cash: str) -> dict[str, Any]:
    """One run's strategy against its own benchmarks, exactly: the PnL
    difference, and that difference as a percentage of the starting cash,
    rounded once as a return is. Unavailable when either valuation is."""
    deltas: dict[str, Any] = {}
    for benchmark in BENCHMARKS:
        missing = [
            f"valuation_unavailable:{account}:{accounts[account]['valuation']['unavailable']['code']}"
            for account in ("strategy", benchmark)
            if "unavailable" in accounts[account]["valuation"]
        ]
        if missing:
            deltas[benchmark] = {"unavailable": missing[0]}
            continue
        difference = _checked(
            money_raw(accounts["strategy"]["valuation"]["net_total_pnl"])
            - money_raw(accounts[benchmark]["valuation"]["net_total_pnl"])
        )
        deltas[benchmark] = {
            "net_total_pnl": money_text(difference),
            "return_percent": percentage_of(difference, money_raw(starting_cash)),
        }
    return deltas


def _unavailable_cells(plan: Mapping[str, Any], reason: str) -> list[dict[str, Any]]:
    return [
        {
            "fee_per_fill": scenario["fee_per_fill"],
            "latency_ns": scenario["latency_ns"],
            "reason": reason,
            "scenario": scenario["name"],
            "state": "unavailable",
        }
        for scenario in plan["scenarios"]
    ]


def _fold_header(fold: Mapping[str, Any]) -> dict[str, Any]:
    evaluation = fold["evaluation"]
    return {
        "evaluation": {key: evaluation[key] for key in ("dataset_id", "feature_rows", "stream", "window")},
        "index": fold["index"],
        "information_boundary_ns": fold["information_boundary_ns"],
    }


def _run_fold(context: StudyContext, index: int, scored: dict[tuple[int, str], ScoredRows]) -> dict[str, Any]:
    fold = context.sources.fold(index)
    folds = str(len(context.plan["folds"]))
    record = _fold_header(fold)

    def report(step: str, **detail: str) -> None:
        context.progress({"fold": fold["index"], "folds": folds, "stage": "fold", "step": step, **detail})

    report("fit")
    try:
        model = _fold_model(context, index)
    except (FoldModelError, PublicationError) as error:
        reason = getattr(error, "reason", "publication_failed")
        record["fold_model"] = {"detail": str(error), "reason": reason, "state": "failed"}
        record["models"] = {
            name: {
                "cells": _unavailable_cells(context.plan, "fold_failed"),
                "reason": "fold_failed",
                "state": "unavailable",
            }
            for name in MODELS
        }
        return record
    record["fold_model"] = {"fold_model_id": model.fold_model_id, "state": "completed"}
    training = model.manifest["training"]
    record["training"] = {
        key: training[key]
        for key in ("class_support", "max_label_available_at_ns", "max_prediction_at_ns", "rows")
    }
    models: dict[str, dict[str, Any]] = {}
    schedules: dict[str, Schedule] = {}
    for name in MODELS:
        fit = model.models[name]
        if isinstance(fit, Unevaluable):
            models[name] = {
                "cells": _unavailable_cells(context.plan, "model_unevaluable"),
                "detail": dict(fit.detail),
                "reason": fit.reason,
                "state": "unevaluable",
            }
            continue
        report("schedule", model=name)
        try:
            schedules[name] = _schedule(context, index, model, name)
        except (ArtifactError, OSError, PublicationError, ScheduleError) as error:
            models[name] = {
                "cells": _unavailable_cells(context.plan, "schedule_failed"),
                "detail": str(error),
                "reason": "schedule_failed",
                "state": "failed",
            }
            continue
        manifest = json.loads(schedules[name].files[SCHEDULE_MANIFEST])
        models[name] = {
            "schedule": {"artifact_id": schedules[name].schedule_id, "counts": manifest["counts"]},
            "state": "completed",
        }
    for name, schedule in schedules.items():
        cells = []
        for scenario in context.plan["scenarios"]:
            report("run", model=name, scenario=scenario["name"])
            cells.append(_cell(context, index, name, schedule, scenario))
        models[name]["cells"] = cells
    # Labels are read only now, after every one of this fold's schedules exists.
    report("score")
    recording, _ = context.sources.evaluation(index)
    _score_fold(context, recording, fold, schedules, models, index, scored)
    record["models"] = {name: models[name] for name in MODELS}
    return record


def _score_fold(
    context: StudyContext,
    recording: VerifiedRecording,
    fold: Mapping[str, Any],
    schedules: Mapping[str, Schedule],
    models: dict[str, dict[str, Any]],
    index: int,
    scored: dict[tuple[int, str], ScoredRows],
) -> None:
    try:
        labels = fold_labels(recording, context.plan["instrument"], fold_window(fold))
    except (ArtifactError, ScheduleError) as error:
        for name in schedules:
            models[name]["predictive"] = {"detail": str(error), "unavailable": "labels_unavailable"}
        return
    for name, schedule in schedules.items():
        try:
            rows = scored_rows(schedule.files[ROWS], labels)
        except ScoringError as error:
            models[name]["predictive"] = {"detail": str(error), "unavailable": "scoring_failed"}
            continue
        scored[(index, name)] = rows
        models[name]["predictive"] = scores(rows)


# --- Aggregates -----------------------------------------------------------------------------------------


def _predictive_aggregate(
    folds: Sequence[Mapping[str, Any]], scored: Mapping[tuple[int, str], ScoredRows], name: str
) -> dict[str, Any]:
    included = [int(fold["index"]) for fold in folds if (int(fold["index"]), name) in scored]
    coverage = {
        "included": [encode_integer(index) for index in included],
        "planned": encode_integer(len(folds)),
    }
    if not included:
        return {"folds": coverage, "status": "unavailable"}
    return {
        "folds": coverage,
        "pooled": scores(pooled([scored[(index, name)] for index in included])),
        "status": "complete" if len(included) == len(folds) else "partial",
    }


def _valued(cell: Mapping[str, Any]) -> str | None:
    """Why a cell has no complete set of final valuations, if it has not."""
    if cell["state"] != "completed":
        return f"cell_{cell['state']}:{cell['reason']}"
    for account in ACCOUNTS:
        valuation = cell["accounts"][account]["valuation"]
        if "unavailable" in valuation:
            return f"valuation_unavailable:{account}:{valuation['unavailable']['code']}"
    return None


def _totals(cells: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    capital = sum(money_raw(cell["starting_cash"]) for cell in cells)
    accounts: dict[str, Any] = {}
    pnl: dict[str, int] = {}
    for account in ACCOUNTS:
        summaries = [cell["accounts"][account] for cell in cells]
        pnl[account] = _checked(sum(money_raw(item["valuation"]["net_total_pnl"]) for item in summaries))
        orders = Counter[str]()
        for item in summaries:
            orders.update({key: int(value) for key, value in item["orders"].items()})
        accounts[account] = {
            "fees_paid": money_text(sum(money_raw(item["valuation"]["fees_paid"]) for item in summaries)),
            "fills": encode_integer(sum(int(item["fills"]) for item in summaries)),
            "net_total_pnl": money_text(pnl[account]),
            "orders": {key: encode_integer(orders[key]) for key in sorted(orders)},
            "return_percent": percentage_of(pnl[account], capital),
            "turnover": money_text(sum(money_raw(item["turnover"]["notional"]) for item in summaries)),
        }
    return {
        "accounts": accounts,
        "deltas": {
            benchmark: {
                "net_total_pnl": money_text(pnl["strategy"] - pnl[benchmark]),
                "return_percent": percentage_of(_checked(pnl["strategy"] - pnl[benchmark]), capital),
            }
            for benchmark in BENCHMARKS
        },
        "starting_capital": money_text(_checked(capital)),
    }


def _trading_aggregate(folds: Sequence[Mapping[str, Any]], name: str, scenario: str) -> dict[str, Any]:
    cells = [
        (fold["index"], next(cell for cell in fold["models"][name]["cells"] if cell["scenario"] == scenario))
        for fold in folds
    ]
    missing = [{"fold": index, "reason": why} for index, cell in cells if (why := _valued(cell)) is not None]
    included = [(index, cell) for index, cell in cells if _valued(cell) is None]
    result: dict[str, Any] = {
        "folds": {"included": [index for index, _ in included], "planned": encode_integer(len(folds))}
    }
    if not missing:
        result["complete"] = _totals([cell for _, cell in included])
        return result
    result["complete"] = {"missing": missing, "unavailable": "folds_missing"}
    if included:
        result["partial"] = _totals([cell for _, cell in included])
    return result


def _missing(folds: Sequence[Mapping[str, Any]]) -> list[dict[str, str]]:
    missing: list[dict[str, str]] = []
    for fold in folds:
        if fold["fold_model"]["state"] != "completed":
            missing.append(
                {"fold": fold["index"], "item": "fold_model", "reason": fold["fold_model"]["reason"]}
            )
        for name in MODELS:
            model = fold["models"][name]
            if model["state"] != "completed":
                missing.append({"fold": fold["index"], "item": f"model:{name}", "reason": model["reason"]})
                continue
            if "unavailable" in model["predictive"]:
                missing.append(
                    {
                        "fold": fold["index"],
                        "item": f"scores:{name}",
                        "reason": model["predictive"]["unavailable"],
                    }
                )
            for cell in model["cells"]:
                if cell["state"] != "completed":
                    missing.append(
                        {
                            "fold": fold["index"],
                            "item": f"cell:{name}:{cell['scenario']}",
                            "reason": cell["reason"],
                        }
                    )
    return missing


def _outcomes(folds: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    cells: Counter[str] = Counter()
    models: Counter[str] = Counter()
    for fold in folds:
        for name in MODELS:
            models[fold["models"][name]["state"]] += 1
            cells.update(cell["state"] for cell in fold["models"][name]["cells"])
    return {
        "cells": {state: encode_integer(count) for state, count in sorted(cells.items())},
        "models": {state: encode_integer(count) for state, count in sorted(models.items())},
    }


def study_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json({key: value for key, value in manifest.items() if key != "study_id"}))


def no_progress(_: Mapping[str, str]) -> None:
    return None


def build_study(context: StudyContext) -> dict[str, Any]:
    """Every planned fold, model, and cell, run (or, when inspecting,
    rebuilt and replayed), scored, and aggregated."""
    plan = context.plan
    scored: dict[tuple[int, str], ScoredRows] = {}
    folds = [_run_fold(context, index, scored) for index in range(1, len(plan["folds"]) + 1)]
    context.progress({"stage": "aggregate"})
    missing = _missing(folds)
    manifest = {
        "aggregates": {
            "predictive": {name: _predictive_aggregate(folds, scored, name) for name in MODELS},
            "trading": {
                name: {
                    scenario["name"]: _trading_aggregate(folds, name, scenario["name"])
                    for scenario in plan["scenarios"]
                }
                for name in MODELS
            },
        },
        "collection": plan["collection"],
        "configuration": plan["configuration"],
        "definitions": {"aggregation": AGGREGATION, "execution": list(EXECUTION), "scoring": plan["scoring"]},
        "environment": environment(),
        "folds": folds,
        "instrument": plan["instrument"],
        "missing": missing,
        "models": list(MODELS),
        "outcomes": _outcomes(folds),
        "plan_id": plan["plan_id"],
        "scenarios": plan["scenarios"],
        "schema": SCHEMA,
        "scope": SCOPE,
        "status": "partial" if missing else "complete",
    }
    return {**manifest, "study_id": study_identity(manifest)}


# --- The study artifact ---------------------------------------------------------------------------------


def verified_study(root: Path, study_id: str) -> dict[str, Any]:
    try:
        directory = artifact_directory(root, study_id)
        if listed_files(directory) != [MANIFEST]:
            raise StudyError("artifact_invalid", f"a study holds exactly {MANIFEST}")
        manifest = verified_manifest(directory, "study_id", with_newline=True)
    except ArtifactUnavailableError as error:
        raise StudyError("artifact_invalid", str(error)) from error
    if manifest.get("schema") != SCHEMA:
        raise StudyError("artifact_invalid", f"{study_id} is not {SCHEMA}")
    return manifest


def _context(
    tools: Tools, artifacts: Path, plan_id: str, progress: Progress, stored: Mapping[str, Any] | None
) -> StudyContext:
    progress({"stage": "verify_sources"})
    try:
        sources = verified_plan_sources(tools, artifacts, plan_id)
    except (CollectionError, PlanError) as error:
        raise StudyError(error.reason, str(error)) from error
    except ArtifactError as error:
        raise StudyError("source_invalid", str(error)) from error
    return StudyContext(
        tools=tools,
        artifacts=artifacts,
        roots=ArtifactRoots.under(artifacts),
        sources=sources,
        progress=progress,
        stored=stored,
    )


def run_study(
    tools: Tools, artifacts: Path, plan_id: str, progress: Progress = no_progress
) -> dict[str, Any]:
    """Runs a verified plan's study and publishes it, whatever its outcomes."""
    manifest = build_study(_context(tools, artifacts, plan_id, progress, None))
    root = studies_root(artifacts)
    root.mkdir(parents=True, exist_ok=True)
    progress({"stage": "publish"})

    def existing(directory: Path) -> bytes:
        verified_study(root, directory.name)
        return (directory / MANIFEST).read_bytes()

    try:
        outcome, warning = publish(
            root, manifest["study_id"], {MANIFEST: canonical_json(manifest)}, MANIFEST, existing
        )
    except PublicationError as error:
        raise StudyError("publication_failed", str(error)) from error
    return {"outcome": outcome.value, "study": manifest, "warning": warning}


def _first_difference(stored: Any, rebuilt: Any, path: str = "") -> str:
    if isinstance(stored, dict) and isinstance(rebuilt, dict):
        for key in sorted(set(stored) | set(rebuilt)):
            if stored.get(key) != rebuilt.get(key):
                return _first_difference(stored.get(key), rebuilt.get(key), f"{path}.{key}")
    if isinstance(stored, list) and isinstance(rebuilt, list) and len(stored) == len(rebuilt):
        for position, (left, right) in enumerate(zip(stored, rebuilt, strict=True)):
            if left != right:
                return _first_difference(left, right, f"{path}[{position}]")
    return path or "."


def inspect_study(
    tools: Tools, artifacts: Path, study_id: str, progress: Progress = no_progress
) -> dict[str, Any]:
    """The verified study, reproduced: its plan and sources verified again,
    every fold model and schedule rebuilt to the published bytes, every
    completed run replayed by the engine, and every score and aggregate
    recomputed, giving the stored manifest exactly. A run that failed when
    the study ran is not executed again."""
    stored = verified_study(studies_root(artifacts), study_id)
    rebuilt = build_study(_context(tools, artifacts, stored["plan_id"], progress, stored))
    if rebuilt != stored:
        raise StudyError(
            "not_reproduced", f"rebuilding the study changes {_first_difference(stored, rebuilt)}"
        )
    return {
        "reproduced": (
            "verified the sources and plan again, rebuilt every fold model and schedule to the published "
            "bytes, replayed every completed run through the engine, and recomputed every score and "
            "aggregate: identical study"
        ),
        "study": stored,
    }


def study_artifacts(artifacts: Path, manifest: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Whether each fold model, schedule, and run a study names still
    verifies, read again now: the fold model's files and parameters, the
    schedule's every row and checksum, and the run manifest's identity and
    journal checksum. Rebuilding and replaying is inspect_study."""
    roots = ArtifactRoots.under(artifacts)

    def state(check: Callable[[str], object], identifier: str) -> str:
        try:
            check(identifier)
        except (
            ArtifactUnavailableError,
            FoldModelError,
            ScheduleError,
            OSError,
            ValueError,
            KeyError,
        ) as error:
            return f"invalid: {error}"
        return "verified"

    def run(run_id: str) -> None:
        directory = artifact_directory(roots.strategies, run_id)
        run_manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        verified_file(directory, run_manifest["files"]["journal"])

    checks = []
    for fold in manifest["folds"]:
        entry: dict[str, Any] = {"fold": fold["index"], "models": {}}
        if fold["fold_model"]["state"] == "completed":
            entry["fold_model"] = state(
                lambda identifier: verified_fold_model(fold_models_root(artifacts), identifier),
                fold["fold_model"]["fold_model_id"],
            )
        for name, model in fold["models"].items():
            checked: dict[str, Any] = {
                "runs": {
                    cell["scenario"]: state(run, cell["run"]["artifact_id"])
                    for cell in model["cells"]
                    if "run" in cell
                }
            }
            if "schedule" in model:
                checked["schedule"] = state(
                    lambda identifier: verify_schedule(roots.schedules / identifier),
                    model["schedule"]["artifact_id"],
                )
            entry["models"][name] = checked
        checks.append(entry)
    return checks


def study_ids(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    return sorted(name for name in (entry.name for entry in root.iterdir()) if is_artifact_id(name))


def listing(manifest: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "collection_id": manifest["collection"]["collection_id"],
        "evidence": manifest["collection"]["evidence"],
        "folds": encode_integer(len(manifest["folds"])),
        "instrument": manifest["instrument"],
        "missing": encode_integer(len(manifest["missing"])),
        "plan_id": manifest["plan_id"],
        "status": manifest["status"],
        "study_id": manifest["study_id"],
    }
