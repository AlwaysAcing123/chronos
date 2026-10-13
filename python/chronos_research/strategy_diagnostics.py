"""Strategy diagnostics: what explains each walk-forward run's PnL and
trading decisions, as an immutable artifact (ADR 0024).

    <artifacts>/strategy_diagnostics/<diagnostic_id>/manifest.json
    <artifacts>/strategy_diagnostics/<diagnostic_id>/fills.ndjson
    <artifacts>/strategy_diagnostics/<diagnostic_id>/episodes.ndjson

Nothing is fitted, scheduled, or run. The sources are read through
non-refitting verifiers: the study and its plan by identity; the plan
rebuilt from its request over the collection's members, each verified by
the C++ tools (which recompute features and labels in memory and write
nothing); every schedule by verify_schedule; every run by its manifest's
identity and its journal's checksum; and every dataset's events by the
checksum its verified manifest gives. Each run must be its cell's run,
exactly as the study requires (walk_forward_study.expected_run). Every
consumed file is read once, through no symlink, and its bytes verified
before use.

strategy_attribution.py attributes each run's PnL exactly, and
decision_diagnostics.py describes its decisions. Here they are joined to
the study, pooled over folds and, with several studies, over studies, and
published. The ID is the SHA-256 of the canonical manifest without its ID,
with its newline; no environment is recorded, as nothing depends on
floating point. `inspect_diagnostics` rebuilds an artifact from its request
and requires identical bytes. Labels are read only after every figure from
the runs is fixed, to describe outcomes; they never select or rank
anything.
"""

import json
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import Any

from chronos_research import decision_diagnostics as cohorts
from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    listed_files,
    verified_file,
    verified_manifest,
)
from chronos_research.artifacts import ArtifactError, RowIdentity, VerifiedRecording, is_artifact_id
from chronos_research.collection import CollectionError
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.pipeline import ArtifactRoots, Tools
from chronos_research.publication import PublicationError, publish
from chronos_research.schedule import MANIFEST as SCHEDULE_MANIFEST
from chronos_research.schedule import ROWS, Schedule, ScheduleError, verify_schedule
from chronos_research.strategy_attribution import (
    ACCOUNTS,
    COMPONENTS,
    DiagnosticsError,
    Figure,
    MarketEvents,
    Missing,
    RunAttribution,
    RunInputs,
    ScheduleRow,
    attribute_run,
    difference,
    figure_json,
    schedule_row,
)
from chronos_research.walk_forward import MODELS, PlanError, PlanSources, verified_plan_sources
from chronos_research.walk_forward_schedule import fold_window
from chronos_research.walk_forward_study import (
    RUN_SCHEMA,
    StudyError,
    engine_configuration,
    expected_run,
    studies_root,
    verified_study,
)

SCHEMA = {"name": "chronos.strategy_diagnostics", "version": "2"}
REQUEST_SCHEMA = {"name": "chronos.strategy_diagnostics_request", "version": "1"}
GENERATOR = {"name": "chronos_research.strategy_diagnostics", "version": "2"}
MANIFEST = "manifest.json"
FILLS = "fills.ndjson"
EPISODES = "episodes.ndjson"
FILES = {"episodes": EPISODES, "fills": FILLS}
MAX_STUDIES = 4
MAX_DETAIL_ROWS = 2_000_000
BENCHMARKS = ("buy_and_hold", "cash")
SCOPE = (
    "retrospective explanation only: how each recorded run's PnL and decisions arose in these recordings; "
    "nothing is fitted, scheduled, run, ranked, promoted, or deployed"
)
DEFINITIONS = {
    "attribution": {
        "booked_net_pnl": "the engine's final net_total_pnl",
        "execution_spread_cost": "sum q_i*(p_i - m_i)",
        "fees": "sum f_i",
        "inventory_pnl_at_bid": "Q*B - sum q_i*m_i = midpoint_inventory_pnl - terminal_bid_markdown",
        "market_value_rounding": "V - Q*B",
        "midpoint_inventory_pnl": "Q*M - sum q_i*m_i",
        "notional_rounding": "sum (n_i - q_i*p_i)",
        "observed_midpoint_movement": "sum q_i*(m_i - s_i); positive is adverse",
        "rounding_adjustment": "(V - Q*B) - sum (n_i - q_i*p_i)",
        "signal_reference_pnl": "Q*M - sum q_i*s_i",
        "terminal_bid_markdown": "Q*(M - B)",
    },
    "identities": [
        "booked_net_pnl = midpoint_inventory_pnl - execution_spread_cost - fees - terminal_bid_markdown "
        "+ rounding_adjustment, checked exactly; when only M is missing, booked_net_pnl = "
        "inventory_pnl_at_bid - execution_spread_cost - fees + rounding_adjustment",
        "midpoint_inventory_pnl = signal_reference_pnl - observed_midpoint_movement, checked exactly",
    ],
    "symbols": (
        "for fill i of an account: q_i its quantity, positive for a buy and negative for a sell; p_i its "
        "price; n_i its booked notional, signed as q_i; f_i its fee; m_i the (bid + ask) / 2 of its "
        "execution observation; s_i the (bid + ask) / 2 of its decision's signal quote. Q is the final "
        "position, B the final mark's bid, M that quote's (bid + ask) / 2, and V the booked final "
        "market value"
    ),
    "scales": (
        "prices 10^-9, quantities and money 10^-8; a midpoint can be half a price unit, so every attribution "
        "figure is an exact decimal of at most 18 places, with no exponent, trailing zeros, or negative zero"
    ),
    "unavailable": (
        "a figure that needs a value that does not exist is unavailable with its reason; nothing is "
        "substituted, not the ask, a trade, the cost basis, an earlier quote, or zero. M and B are needed "
        "only for an open final position"
    ),
    "joins": (
        "events by ingest sequence within the run's own dataset and stream, and only top-of-book events of "
        "the run's instrument as quotes; a decision's signal by its schedule row's identity and that row's "
        "quote; an order by (account, decision); its one resolution by (account, client order ID)"
    ),
    "timing": (
        "the configured latency, and per execution the observed durations from signal to arrival, arrival "
        "to execution, and signal to execution; expired orders have no execution time"
    ),
    "episodes": (
        "from a fill that leaves a flat position to the fill that makes it flat again (completed, booked PnL "
        "= -sum n_i - sum f_i) or to the window's end (censored, marked at the final valuation)"
    ),
    "cohorts": {
        "filled_orders": "order-scheduling decisions whose order filled, at least in part",
        "order_scheduling_decisions": "sampled decisions whose reason is order_scheduled",
        "sampled_decisions": (
            "the strategy's journaled decisions; rows skipped by the decision interval are counted"
        ),
        "scorable_predictions": "schedule rows with a prediction; its scored rows equal the study's",
    },
    "labels": (
        "the evaluation recording's verified labels under the plan's scoring rule, read only to describe "
        "outcomes; they never reach a model, a policy, an order, or a choice of what to run"
    ),
    "ratios": (
        "accuracy = correct / scored; balanced accuracy = mean recall over the realized classes that occur; "
        "exact reduced fractions, with a 6-place decimal rounded half away from zero; undefined when nothing "
        "is scored"
    ),
    "future_returns": (
        "each cohort's scored items' label return_raw, the label artifact's unit: (future midpoint - current "
        "midpoint) / current midpoint in parts per billion, rounded half away from zero by the label "
        "generator; rows, exact sum, minimum, maximum, and exact mean, for the cohort, each predicted class, "
        "and each fixed confidence bin; items without a scored label are counted as not_scored. "
        "Retrospective only: no threshold, ranking, or choice of what to run uses them"
    ),
    "pooling": (
        "exact sums over independent reset accounts of the included folds, and with several studies over "
        "studies for scenarios with identical names, fees, and latencies; decision cohorts pool counts and "
        "then compute their ratios; a fold whose cell did not complete, or a study with no completed fold "
        "of the cell, is listed as missing and the pool is partial; a pool with no completed run has no "
        "financial figures (unavailable: no_completed_runs), and its counts are zero"
    ),
    "not_computed": (
        "no spliced equity curve, annualization, Sharpe ratio, significance test, threshold search, ranking, "
        "or winner"
    ),
}
VERIFICATION = (
    "at generation: the study and plan by identity; the plan rebuilt from its request over the collection's "
    "members, each verified by the C++ tools; every schedule's rows and checksum; every run's manifest "
    "identity and journal checksum, and that it is its cell's run; every dataset's events by checksum. "
    "Engine replay is the study's own record; nothing here replays or refits"
)

type Progress = Callable[[Mapping[str, str]], None]


def no_progress(_: Mapping[str, str]) -> None:
    return None


def diagnostics_root(artifacts: Path) -> Path:
    return artifacts / "strategy_diagnostics"


def validated_request(document: Any) -> dict[str, Any]:
    """The request, strict: its schema and one to four distinct study IDs in
    ascending order."""
    if not isinstance(document, dict) or set(document) != {"schema", "study_ids"}:
        raise DiagnosticsError("invalid_request", "a diagnostics request has exactly schema and study_ids")
    if document["schema"] != REQUEST_SCHEMA:
        raise DiagnosticsError("invalid_request", f"the request's schema is {REQUEST_SCHEMA}")
    studies = document["study_ids"]
    if (
        not isinstance(studies, list)
        or not 1 <= len(studies) <= MAX_STUDIES
        or not all(isinstance(study, str) and is_artifact_id(study) for study in studies)
    ):
        raise DiagnosticsError("invalid_request", f"study_ids is a list of 1 to {MAX_STUDIES} study IDs")
    if studies != sorted(set(studies)):
        raise DiagnosticsError("invalid_request", "study_ids are distinct and in ascending order")
    return {"schema": REQUEST_SCHEMA, "study_ids": list(studies)}


def request_for(study_ids: Sequence[str]) -> dict[str, Any]:
    return validated_request({"schema": REQUEST_SCHEMA, "study_ids": sorted(set(study_ids))})


# --- Sources, read once and verified ----------------------------------------------------------------------


@dataclass(frozen=True)
class VerifiedRun:
    run_id: str
    manifest: dict[str, Any]
    journal: list[dict[str, Any]]
    journal_sha256: str


def _source_error(error: Exception) -> DiagnosticsError:
    return DiagnosticsError("source_invalid", str(error))


def _events(roots: ArtifactRoots, recording: VerifiedRecording, instrument: str) -> MarketEvents:
    """The instrument's quotes from the dataset's events, read once and
    hashed against the verified dataset manifest's checksum."""
    try:
        directory = artifact_directory(roots.catalog, recording.dataset_id)
        data = verified_file(directory, recording.dataset_manifest["files"]["events"])
    except ArtifactUnavailableError as error:
        raise _source_error(error) from error
    events = MarketEvents(recording.dataset_id, recording.stream, instrument)
    for number, line in enumerate(data.splitlines(), start=1):
        try:
            record = json.loads(line)
        except ValueError as error:
            raise DiagnosticsError("source_invalid", f"dataset event {number} is not JSON") from error
        events.add(record, f"dataset {recording.dataset_id[:12]} event {number}")
    return events


def _schedule(
    roots: ArtifactRoots, schedule_id: str, stream: str, instrument: str
) -> tuple[Schedule, dict[int, ScheduleRow], list[ScheduleRow]]:
    try:
        directory = artifact_directory(roots.schedules, schedule_id)
        manifest = verify_schedule(directory)
        data = verified_file(directory, manifest["files"]["schedule"])
    except (ArtifactUnavailableError, ScheduleError) as error:
        raise _source_error(error) from error
    rows = [
        schedule_row(json.loads(line), stream, instrument, f"schedule {schedule_id[:12]} row {number}")
        for number, line in enumerate(data.splitlines(), start=1)
    ]
    by_sequence = {row.ingest_sequence: row for row in rows}
    if len(by_sequence) != len(rows):
        raise DiagnosticsError("join_failed", f"schedule {schedule_id} repeats an ingest sequence")
    schedule = Schedule(schedule_id, {SCHEDULE_MANIFEST: canonical_json(manifest), ROWS: data})
    return schedule, by_sequence, rows


def _run(roots: ArtifactRoots, run_id: str) -> VerifiedRun:
    try:
        directory = artifact_directory(roots.strategies, run_id)
        if listed_files(directory) != ["journal.ndjson", "manifest.json"]:
            raise ArtifactUnavailableError("a run holds exactly journal.ndjson and manifest.json")
        manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        journal = verified_file(directory, manifest["files"]["journal"])
        entries = [json.loads(line) for line in journal.splitlines()]
    except (ArtifactUnavailableError, KeyError, TypeError, ValueError) as error:
        raise _source_error(error) from error
    return VerifiedRun(run_id, manifest, entries, sha256_hex(journal))


def _run_difference(run: Mapping[str, Any], expected: Mapping[str, Any]) -> str | None:
    """The first field where a run is not its cell's, if any."""
    for key in ("schema", "configuration", "window"):
        if run.get(key) != expected[key]:
            return key
    source = run.get("source", {})
    for key, value in expected["source"].items():
        if source.get(key) != value:
            return f"source.{key}"
    return None


# --- One study --------------------------------------------------------------------------------------------


@dataclass
class _Cell:
    """A completed run's attribution, kept as values for pooling."""

    fold: int
    model: str
    scenario: str
    attribution: RunAttribution
    decisions: dict[str, cohorts.CohortCounts]


@dataclass
class _Study:
    document: dict[str, Any]
    cells: list[_Cell]
    predictions: dict[tuple[int, str], cohorts.Predictions]
    fills: list[dict[str, Any]]
    episodes: list[dict[str, Any]]
    folds: list[int]
    scenarios: list[dict[str, str]]


def _unavailable(fold: int, model: str, scenario: str, reason: str) -> dict[str, Any]:
    return {"fold": str(fold), "model": model, "reason": reason, "scenario": scenario, "state": "unavailable"}


def _study(tools: Tools, artifacts: Path, study_id: str, progress: Progress) -> _Study:
    progress({"stage": "verify_sources"})
    try:
        study = verified_study(studies_root(artifacts), study_id)
        sources = verified_plan_sources(tools, artifacts, study["plan_id"])
    except (StudyError, PlanError, CollectionError) as error:
        raise DiagnosticsError("source_invalid", f"study {study_id}: {error}") from error
    except ArtifactError as error:
        raise _source_error(error) from error
    plan = sources.plan
    if (
        plan["instrument"] != study["instrument"]
        or plan["scenarios"] != study["scenarios"]
        or len(plan["folds"]) != len(study["folds"])
    ):
        raise DiagnosticsError("source_invalid", f"study {study_id} is not its plan's")
    roots = ArtifactRoots.under(artifacts)
    instrument = str(study["instrument"])
    folds_document: list[dict[str, Any]] = []
    cells: list[_Cell] = []
    missing: list[dict[str, str]] = []
    predictions: dict[tuple[int, str], cohorts.Predictions] = {}
    fills: list[dict[str, Any]] = []
    episodes: list[dict[str, Any]] = []
    for position, fold in enumerate(study["folds"], start=1):
        if fold["index"] != str(position) or plan["folds"][position - 1]["index"] != fold["index"]:
            raise DiagnosticsError("source_invalid", f"study {study_id}'s folds are not its plan's")
        folds_document.append(
            _fold(
                roots,
                sources,
                study,
                position,
                instrument,
                progress,
                cells,
                predictions,
                fills,
                episodes,
                missing,
            )
        )
    document = {
        "collection_id": study["collection"]["collection_id"],
        "evidence": study["collection"]["evidence"],
        "folds": folds_document,
        "instrument": instrument,
        "missing": missing,
        "plan_id": study["plan_id"],
        "scenarios": study["scenarios"],
        "status": "partial" if missing else "complete",
        "study_id": study_id,
        "study_status": study["status"],
    }
    return _Study(
        document,
        cells,
        predictions,
        fills,
        episodes,
        list(range(1, len(study["folds"]) + 1)),
        list(study["scenarios"]),
    )


def _fold(
    roots: ArtifactRoots,
    sources: PlanSources,
    study: Mapping[str, Any],
    index: int,
    instrument: str,
    progress: Progress,
    cells: list[_Cell],
    predictions: dict[tuple[int, str], cohorts.Predictions],
    fills: list[dict[str, Any]],
    episodes: list[dict[str, Any]],
    missing: list[dict[str, str]],
) -> dict[str, Any]:
    fold = study["folds"][index - 1]
    plan_fold = sources.fold(index)
    recording, labelled = sources.evaluation(index)
    window = fold_window(plan_fold)
    evaluation = plan_fold["evaluation"]
    document: dict[str, Any] = {
        "dataset_id": recording.dataset_id,
        "events_sha256": recording.dataset_manifest["files"]["events"]["sha256"],
        "feature_artifact_id": recording.features.artifact_id,
        "features_sha256": recording.features.rows_sha256,
        "index": str(index),
        "label_artifact_id": recording.labels.artifact_id,
        "labels_sha256": recording.labels.rows_sha256,
        "stream": recording.stream,
        "window": evaluation["window"],
    }
    if (recording.dataset_id, recording.features.artifact_id, recording.labels.artifact_id) != (
        evaluation["dataset_id"],
        evaluation["feature_artifact_id"],
        evaluation["label_artifact_id"],
    ):
        raise DiagnosticsError("source_invalid", f"fold {index}'s recording is not its plan's")
    runs: list[dict[str, Any]] = []
    models: dict[str, Any] = {}
    events: MarketEvents | None = None
    statuses: dict[RowIdentity, cohorts.LabelStatus] | None = None
    for name in MODELS:
        model = fold["models"][name]
        if model["state"] != "completed" or "schedule" not in model:
            reason = f"model_{model['state']}:{model.get('reason', 'no_schedule')}"
            models[name] = {"reason": reason, "state": "unavailable"}
            missing.append({"fold": str(index), "item": f"model:{name}", "reason": reason})
            runs.extend(_unavailable(index, name, cell["scenario"], reason) for cell in model["cells"])
            continue
        schedule_id = model["schedule"]["artifact_id"]
        schedule, by_sequence, rows = _schedule(roots, schedule_id, recording.stream, instrument)
        if events is None:
            events = _events(roots, recording, instrument)
        if statuses is None:
            statuses = cohorts.label_statuses(labelled, window)
        try:
            scored = cohorts.scorable_predictions(rows, recording.stream, instrument, statuses)
        except ValueError as error:
            raise DiagnosticsError("join_failed", f"fold {index} {name}: {error}") from error
        predictive = model.get("predictive", {})
        if "metrics" in predictive:
            disagreement = cohorts.study_scores_agree(scored, predictive)
            if disagreement is not None:
                raise DiagnosticsError(
                    "source_invalid", f"fold {index} {name}: the study's scores differ in {disagreement}"
                )
        predictions[(index, name)] = scored
        models[name] = {
            "predictions": cohorts.predictions_document(scored),
            "schedule_id": schedule_id,
            "schedule_sha256": json.loads(schedule.files[SCHEDULE_MANIFEST])["files"]["schedule"]["sha256"],
            "state": "completed",
            "study_scores": "agree"
            if "metrics" in predictive
            else f"unavailable:{predictive.get('unavailable')}",
        }
        for cell in model["cells"]:
            progress(
                {
                    "fold": str(index),
                    "folds": str(len(study["folds"])),
                    "model": name,
                    "scenario": cell["scenario"],
                    "stage": "attribute",
                }
            )
            scenario = next(item for item in study["scenarios"] if item["name"] == cell["scenario"])
            if cell["state"] != "completed":
                reason = f"cell_{cell['state']}:{cell['reason']}"
                runs.append(_unavailable(index, name, cell["scenario"], reason))
                missing.append(
                    {"fold": str(index), "item": f"cell:{name}:{cell['scenario']}", "reason": reason}
                )
                continue
            joined = _Joined(schedule, by_sequence, events, statuses, recording)
            runs.append(
                _cell(roots, sources, study, index, name, cell, scenario, joined, cells, fills, episodes)
            )
    document["models"] = models
    document["runs"] = runs
    return document


@dataclass(frozen=True)
class _Joined:
    """What every run of one fold model's schedule is joined to."""

    schedule: Schedule
    rows: Mapping[int, ScheduleRow]
    events: MarketEvents
    statuses: Mapping[RowIdentity, cohorts.LabelStatus]
    recording: VerifiedRecording


def _cell(
    roots: ArtifactRoots,
    sources: PlanSources,
    study: Mapping[str, Any],
    index: int,
    name: str,
    cell: Mapping[str, Any],
    scenario: Mapping[str, str],
    joined: _Joined,
    cells: list[_Cell],
    fills: list[dict[str, Any]],
    episodes: list[dict[str, Any]],
) -> dict[str, Any]:
    run_id = cell["run"]["artifact_id"]
    where = f"fold {index} {name} {scenario['name']}"
    run = _run(roots, run_id)
    configuration = engine_configuration(sources.plan["configuration"], scenario)
    expected = expected_run(joined.schedule, joined.recording, configuration)
    difference_at = _run_difference(run.manifest, expected)
    if difference_at is not None or run.manifest.get("schema") != RUN_SCHEMA:
        raise DiagnosticsError("source_invalid", f"{where}: run {run_id} is not its cell's ({difference_at})")
    attribution = attribute_run(RunInputs(run_id, run.manifest, run.journal, joined.events, joined.rows))
    decided = cohorts.decision_cohorts(
        attribution.decisions, joined.recording.stream, str(study["instrument"]), joined.statuses
    )
    cells.append(_Cell(index, name, scenario["name"], attribution, decided))
    location = {
        "fold": str(index),
        "model": name,
        "run_id": run_id,
        "scenario": scenario["name"],
        "study_id": study["study_id"],
    }
    fills.extend({**location, **row} for row in attribution.fills)
    episodes.extend({**location, **row} for row in attribution.episodes)
    return {
        "accounts": {account: attribution.accounts[account].document for account in ACCOUNTS},
        "decisions": {
            **attribution.decision_counts,
            "cohorts": {key: decided[key].document() for key in cohorts.COHORTS[1:]},
        },
        "deltas": _deltas({account: attribution.accounts[account].components for account in ACCOUNTS}),
        "engine_replay": {"recorded_by": "study", "statement": cell["run"].get("replay")},
        "fee_per_fill": scenario["fee_per_fill"],
        "fold": str(index),
        "journal_sha256": run.journal_sha256,
        "latency_ns": scenario["latency_ns"],
        "model": name,
        "run_id": run_id,
        "scenario": scenario["name"],
        "state": "completed",
    }


def _deltas(accounts: Mapping[str, Mapping[str, Figure]]) -> dict[str, Any]:
    """The strategy minus each benchmark, component by component."""
    return {
        benchmark: {
            component: figure_json(
                difference(accounts["strategy"][component], accounts[benchmark][component])
            )
            for component in COMPONENTS
        }
        for benchmark in BENCHMARKS
    }


# --- Pooling ----------------------------------------------------------------------------------------------


def _sum(parts: Sequence[tuple[str, Figure]]) -> Figure:
    """An exact sum of labelled parts, or the first missing one, named."""
    missing = next(((label, value) for label, value in parts if isinstance(value, Missing)), None)
    if missing is not None:
        return Missing(f"{missing[0]}:{missing[1].reason}")
    return sum((value for _, value in parts if not isinstance(value, Missing)), Fraction(0))


NO_COMPLETED_RUNS = Missing("no_completed_runs")


def _pool_accounts(
    parts: Sequence[tuple[str, Mapping[str, Mapping[str, Figure]]]],
) -> dict[str, dict[str, Figure]]:
    """Each account's components summed over the pool's runs. With no run,
    nothing was evaluated: every figure is unavailable, never zero."""
    if not parts:
        return {account: dict.fromkeys(COMPONENTS, NO_COMPLETED_RUNS) for account in ACCOUNTS}
    return {
        account: {
            component: _sum([(label, accounts[account][component]) for label, accounts in parts])
            for component in COMPONENTS
        }
        for account in ACCOUNTS
    }


def _pooled_document(pooled: Mapping[str, Mapping[str, Figure]]) -> dict[str, Any]:
    return {
        "accounts": {
            account: {component: figure_json(pooled[account][component]) for component in COMPONENTS}
            for account in ACCOUNTS
        },
        "deltas": _deltas(pooled),
    }


@dataclass
class _Pool:
    accounts: dict[str, dict[str, Figure]]
    decisions: dict[str, cohorts.CohortCounts]
    complete: bool
    # Whether any fold's run is in the pool.
    evaluated: bool


def _study_pools(
    study: _Study,
) -> tuple[dict[str, Any], dict[tuple[str, str], _Pool], dict[str, cohorts.CohortCounts]]:
    trading: dict[str, Any] = {}
    pools: dict[tuple[str, str], _Pool] = {}
    for name in MODELS:
        trading[name] = {}
        for scenario in study.scenarios:
            chosen = [cell for cell in study.cells if (cell.model, cell.scenario) == (name, scenario["name"])]
            included = [cell.fold for cell in chosen]
            absent = [fold for fold in study.folds if fold not in included]
            accounts = _pool_accounts(
                [
                    (f"fold_{cell.fold}", {a: cell.attribution.accounts[a].components for a in ACCOUNTS})
                    for cell in chosen
                ]
            )
            decisions = {
                key: cohorts.pooled(cell.decisions[key] for cell in chosen) for key in cohorts.COHORTS[1:]
            }
            complete = not absent
            pools[(name, scenario["name"])] = _Pool(accounts, decisions, complete, bool(included))
            trading[name][scenario["name"]] = {
                "cohorts": {key: decisions[key].document() for key in cohorts.COHORTS[1:]},
                "folds": {
                    "included": [str(fold) for fold in included],
                    "missing": [str(fold) for fold in absent],
                    "planned": str(len(study.folds)),
                },
                "status": "complete" if complete else ("partial" if included else "unavailable"),
                **_pooled_document(accounts),
            }
    predictions: dict[str, Any] = {}
    prediction_pools: dict[str, cohorts.CohortCounts] = {}
    for name in MODELS:
        included = [fold for fold in study.folds if (fold, name) in study.predictions]
        prediction_pools[name] = cohorts.pooled(study.predictions[(fold, name)].cohort for fold in included)
        predictions[name] = {
            "cohort": prediction_pools[name].document(),
            "folds": {"included": [str(fold) for fold in included], "planned": str(len(study.folds))},
            "status": "complete"
            if len(included) == len(study.folds)
            else ("partial" if included else "unavailable"),
        }
    return {"predictions": predictions, "trading": trading}, pools, prediction_pools


def _across(
    studies: Sequence[_Study],
    pools: Sequence[dict[tuple[str, str], _Pool]],
    predictions: Sequence[dict[str, cohorts.CohortCounts]],
) -> dict[str, Any]:
    """With several studies: the same pools summed over studies, for
    scenarios identical in all of them."""
    shared = [
        scenario
        for scenario in studies[0].scenarios
        if all(scenario in study.scenarios for study in studies[1:])
    ]
    identifiers = [study.document["study_id"] for study in studies]
    trading: dict[str, Any] = {}
    for name in MODELS:
        trading[name] = {}
        for scenario in shared:
            parts = [
                (identifier, pool[(name, scenario["name"])])
                for identifier, pool in zip(identifiers, pools, strict=True)
            ]
            # A study with no completed fold of the cell contributes nothing
            # and is missing; it is not a zero-PnL account.
            included = [(identifier, pool) for identifier, pool in parts if pool.evaluated]
            accounts = _pool_accounts([(identifier[:12], pool.accounts) for identifier, pool in included])
            decisions = {
                key: cohorts.pooled(pool.decisions[key] for _, pool in parts) for key in cohorts.COHORTS[1:]
            }
            trading[name][scenario["name"]] = {
                "cohorts": {key: decisions[key].document() for key in cohorts.COHORTS[1:]},
                "status": _coverage(len(included), len(parts), all(pool.complete for _, pool in parts)),
                "studies": {
                    "included": [identifier for identifier, _ in included],
                    "missing": [identifier for identifier, pool in parts if not pool.evaluated],
                },
                **_pooled_document(accounts),
            }
    prediction_pools: dict[str, Any] = {}
    for name in MODELS:
        included_studies = [
            identifier
            for identifier, study in zip(identifiers, studies, strict=True)
            if any((fold, name) in study.predictions for fold in study.folds)
        ]
        complete = all(all((fold, name) in study.predictions for fold in study.folds) for study in studies)
        prediction_pools[name] = {
            "cohort": cohorts.pooled(pool[name] for pool in predictions).document(),
            "status": _coverage(len(included_studies), len(studies), complete),
            "studies": {
                "included": included_studies,
                "missing": [identifier for identifier in identifiers if identifier not in included_studies],
            },
        }
    return {
        "predictions": prediction_pools,
        "scenarios": shared,
        "studies": [study.document["study_id"] for study in studies],
        "trading": trading,
        "unshared_scenarios": [
            {"scenario": scenario["name"], "study_id": study.document["study_id"]}
            for study in studies
            for scenario in study.scenarios
            if scenario not in shared
        ],
    }


def _coverage(included: int, planned: int, complete: bool) -> str:
    if included == 0:
        return "unavailable"
    return "complete" if included == planned and complete else "partial"


# --- The artifact -----------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Built:
    diagnostic_id: str
    manifest: dict[str, Any]
    files: dict[str, bytes]


def _ndjson(rows: Sequence[Mapping[str, Any]]) -> bytes:
    return b"".join(canonical_json(row) for row in rows)


def build_diagnostics(
    tools: Tools, artifacts: Path, document: Any, progress: Progress = no_progress
) -> Built:
    """The diagnostics of a request's studies, from their verified sources."""
    request = validated_request(document)
    studies = [_study(tools, artifacts, study_id, progress) for study_id in request["study_ids"]]
    progress({"stage": "aggregate"})
    fills = [row for study in studies for row in study.fills]
    episodes = [row for study in studies for row in study.episodes]
    if len(fills) + len(episodes) > MAX_DETAIL_ROWS:
        raise DiagnosticsError("workload_exceeded", f"more than {MAX_DETAIL_ROWS} fill and episode rows")
    pooled = [_study_pools(study) for study in studies]
    sections = []
    for study, (pools_document, _, _) in zip(studies, pooled, strict=True):
        sections.append({**study.document, "pooled": pools_document})
    contents = {FILLS: _ndjson(fills), EPISODES: _ndjson(episodes)}
    manifest: dict[str, Any] = {
        "confidence_bins": list(cohorts.CONFIDENCE_BINS),
        "counts": {
            "episodes": str(len(episodes)),
            "fills": str(len(fills)),
            "runs": str(sum(len(fold["runs"]) for study in studies for fold in study.document["folds"])),
            "runs_attributed": str(sum(len(study.cells) for study in studies)),
        },
        "definitions": DEFINITIONS,
        "files": {
            key: {
                "name": name,
                "rows": str(len(rows)),
                "sha256": sha256_hex(contents[name]),
                "size_bytes": str(len(contents[name])),
            }
            for key, name, rows in (("episodes", EPISODES, episodes), ("fills", FILLS, fills))
        },
        "generator": GENERATOR,
        "request": request,
        "schema": SCHEMA,
        "scope": SCOPE,
        "status": "partial" if any(study.document["missing"] for study in studies) else "complete",
        "studies": sections,
        "verification": VERIFICATION,
    }
    if len(studies) > 1:
        manifest["across_studies"] = _across(
            studies, [pools for _, pools, _ in pooled], [predictions for _, _, predictions in pooled]
        )
    manifest = {**manifest, "diagnostic_id": diagnostics_identity(manifest)}
    contents[MANIFEST] = canonical_json(manifest)
    return Built(manifest["diagnostic_id"], manifest, contents)


def diagnostics_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(
        canonical_json({key: value for key, value in manifest.items() if key != "diagnostic_id"})
    )


def verified_diagnostics(root: Path, diagnostic_id: str) -> tuple[dict[str, Any], dict[str, bytes]]:
    """The artifact from bytes read once: its files, identity, schema, and
    each detail file's checksum and row count."""
    try:
        directory = artifact_directory(root, diagnostic_id)
        if listed_files(directory) != sorted([EPISODES, FILLS, MANIFEST]):
            raise DiagnosticsError(
                "artifact_invalid", f"diagnostics hold exactly {EPISODES}, {FILLS}, and {MANIFEST}"
            )
        manifest = verified_manifest(directory, "diagnostic_id", with_newline=True)
        schema = manifest.get("schema")
        if schema != SCHEMA:
            message = f"{diagnostic_id} is not {SCHEMA['name']} version {SCHEMA['version']}"
            if isinstance(schema, dict) and schema.get("name") == SCHEMA["name"]:
                message = (
                    f"{diagnostic_id} is {SCHEMA['name']} version {schema.get('version')!r}, which this "
                    "version does not read; generate the diagnostics again"
                )
            raise DiagnosticsError("artifact_invalid", message)
        files = {MANIFEST: canonical_json(manifest)}
        for key, name in FILES.items():
            digest = manifest["files"][key]
            if digest.get("name") != name:
                raise DiagnosticsError("artifact_invalid", f"the manifest names {key} wrongly")
            data = verified_file(directory, digest)
            if str(len(data.splitlines())) != digest["rows"]:
                raise DiagnosticsError("artifact_invalid", f"{name} has a different number of rows")
            files[name] = data
    except ArtifactUnavailableError as error:
        raise DiagnosticsError("artifact_invalid", str(error)) from error
    except (KeyError, TypeError, AttributeError) as error:
        raise DiagnosticsError("artifact_invalid", f"{diagnostic_id} is malformed ({error})") from error
    return manifest, files


def generate_diagnostics(
    tools: Tools, artifacts: Path, document: Any, progress: Progress = no_progress
) -> dict[str, Any]:
    """Builds and publishes the diagnostics of a request; a summary, since
    the manifest is read with inspect or the service."""
    built = build_diagnostics(tools, artifacts, document, progress)
    root = diagnostics_root(artifacts)
    root.mkdir(parents=True, exist_ok=True)
    progress({"stage": "publish"})

    def existing(directory: Path) -> bytes:
        return verified_diagnostics(root, directory.name)[1][MANIFEST]

    try:
        outcome, warning = publish(root, built.diagnostic_id, built.files, MANIFEST, existing)
    except PublicationError as error:
        raise DiagnosticsError("publication_failed", str(error)) from error
    return {"diagnostics": listing(built.manifest), "outcome": outcome.value, "warning": warning}


def inspect_diagnostics(
    tools: Tools, artifacts: Path, diagnostic_id: str, progress: Progress = no_progress
) -> dict[str, Any]:
    """The artifact verified, then rebuilt from its request over its sources
    verified again now, with identical bytes required. It publishes nothing."""
    manifest, files = verified_diagnostics(diagnostics_root(artifacts), diagnostic_id)
    built = build_diagnostics(tools, artifacts, manifest["request"], progress)
    for name in sorted(files):
        if built.files[name] != files[name]:
            raise DiagnosticsError("not_reproduced", f"rebuilding the diagnostics changes {name}")
    return {
        "diagnostics": listing(manifest),
        "reproduced": (
            "verified the artifact, verified its sources again without refitting or replaying, and rebuilt "
            "every figure: identical bytes"
        ),
    }


def diagnostic_ids(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    return sorted(name for name in (entry.name for entry in root.iterdir()) if is_artifact_id(name))


def listing(manifest: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "counts": manifest["counts"],
        "diagnostic_id": manifest["diagnostic_id"],
        "status": manifest["status"],
        "studies": [
            {
                "evidence": study["evidence"],
                "instrument": study["instrument"],
                "missing": str(len(study["missing"])),
                "status": study["status"],
                "study_id": study["study_id"],
            }
            for study in manifest["studies"]
        ],
    }
