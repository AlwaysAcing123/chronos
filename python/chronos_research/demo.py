"""The saved research demonstration: one command from local market data to a
verified baseline experiment and its strategy evaluations.

    python -m chronos_research demo --tools <build dir> --artifacts <data dir>
        (--recording <file.dbn.zst> | --synthetic-events <file.csv>) --stream-id <id>

It runs every pipeline step, takes each artifact ID from the tool that made
it, verifies the whole ancestry (every derived artifact reproduced from its
source, the features from the dataset), verifies the experiment, and reports
each instrument's models as they are, unevaluable ones with their reasons.
For each fitted logistic model it then publishes a label-free inference
schedule for the validation and the test window and runs each through the
C++ strategy runner, in fresh accounts, beside the cash and buy-and-hold
benchmarks; an unevaluable model is reported as not run. Run again, every
step finds its artifact already present.

The demonstration verifies machinery. A few minutes of two symbols say
nothing about whether any model generalizes or any strategy is profitable.
"""

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research.experiment import MODEL, SCOPE
from chronos_research.inspection import verify_experiment
from chronos_research.pipeline import (
    ArtifactRoots,
    Step,
    StrategyOptions,
    Tools,
    create_split,
    evaluate_baselines,
    extract_features,
    extract_labels,
    infer_schedule,
    normalize_recording,
    run_strategy,
    synthesize_dataset,
    verified_sources,
)
from chronos_research.schedule import WINDOWS

DEFAULT_HORIZON_NS = 1_000_000_000
LIMITATIONS = (
    "This demonstration verifies the pipeline's machinery on a few minutes of data. It cannot show that a "
    "model generalizes or that any strategy would be profitable: the sample is one short session, its "
    "evaluation periods are minutes long, and predictive accuracy is not trading profit. The strategy runs "
    "are paper simulations against recorded top-of-book quotes with an assumed fee, one share at a time, "
    "and no depth, queue position, or market impact."
)


@dataclass(frozen=True)
class DemoSource:
    stream_id: str
    recording: Path | None = None
    synthetic_events: Path | None = None


def _models(experiment_directory: Path) -> dict[str, Any]:
    """Each instrument's two models, fitted or unevaluable with the reason."""
    models = json.loads((experiment_directory / MODEL).read_bytes())["instruments"]
    summary: dict[str, Any] = {}
    for instrument, entry in sorted(models.items()):
        logistic = entry["logistic"]
        summary[instrument] = {
            "logistic": (
                {"status": "fitted", "iterations": logistic["iterations"]}
                if logistic["status"] == "fitted"
                else {"status": "unevaluable", "reason": logistic["reason"], "detail": logistic["detail"]}
            ),
            "majority": entry["majority"].get("majority_class", entry["majority"].get("reason")),
        }
    return summary


def strategy_summary(manifest: dict[str, Any]) -> dict[str, Any]:
    """Each account's outcome, an unavailable valuation as such, never zero."""
    results = manifest["results"]
    accounts: dict[str, Any] = {}
    for name, account in sorted(results["accounts"].items()):
        valuation = account["final"]["valuation"]
        complete = "unavailable" not in valuation
        accounts[name] = {
            "fees_paid": account["final"]["ledger"]["fees_paid"],
            "fills": account["fills"],
            "maximum_drawdown": account["drawdown"]["maximum"],
            "net_total_pnl": valuation["net_total_pnl"] if complete else None,
            "return_percent": valuation["return_percent"] if complete else None,
            "turnover": account["turnover"]["notional"],
            "valuation": "complete" if complete else f"unavailable: {valuation['unavailable']['code']}",
        }
    return {
        "accounts": accounts,
        "decisions": results["decisions"]["evaluated"],
        "instrument": manifest["source"]["instrument"],
        "model": manifest["source"]["model"],
        "run_id": manifest["artifact_id"],
        "schedule_id": manifest["source"]["schedule_id"],
        "window": manifest["window"]["name"],
    }


def _headline(metrics: dict[str, Any]) -> dict[str, Any]:
    return {
        split: {
            model: {
                "accuracy": entry["metrics"]["accuracy"],
                "balanced_accuracy": entry["metrics"]["balanced_accuracy"],
                "instruments": entry["instruments"],
                "rows": entry["metrics"]["rows"],
            }
            for model, entry in details["aggregate"].items()
        }
        for split, details in metrics["splits"].items()
    }


def run_demo(tools: Tools, roots: ArtifactRoots, source: DemoSource, horizon_ns: int) -> dict[str, Any]:
    if (source.recording is None) == (source.synthetic_events is None):
        raise ValueError("a demonstration reads exactly one of a recording or synthetic events")
    if source.recording is not None:
        dataset = normalize_recording(tools, source.recording, source.stream_id, roots.catalog)
    else:
        assert source.synthetic_events is not None
        dataset = synthesize_dataset(tools, source.synthetic_events, source.stream_id, roots.catalog)
    for root in (roots.features, roots.labels, roots.splits):
        root.mkdir(parents=True, exist_ok=True)
    features = extract_features(tools, roots, dataset.artifact_id)
    labels = extract_labels(tools, roots, features.artifact_id, horizon_ns)
    split = create_split(tools, roots, labels.artifact_id)
    sources = verified_sources(tools, roots, split.artifact_id, reproduce_features=True)
    experiment, metrics = evaluate_baselines(sources, roots.experiments)
    verify_experiment(experiment.directory)
    steps: list[Step] = [dataset, features, labels, split, experiment]
    models = _models(experiment.directory)
    strategy_runs: list[dict[str, Any]] = []
    for instrument, entry in models.items():
        logistic = entry["logistic"]
        if logistic["status"] != "fitted":
            strategy_runs.append(
                {
                    "instrument": instrument,
                    "model": "logistic",
                    "not_run": f"the logistic model is unevaluable ({logistic['reason']})",
                }
            )
            continue
        for window in WINDOWS:
            schedule = infer_schedule(sources, roots, experiment.artifact_id, instrument, "logistic", window)
            run, manifest = run_strategy(tools, roots, schedule.artifact_id, StrategyOptions())
            steps += [schedule, run]
            strategy_runs.append(strategy_summary(manifest))
    return {
        "artifacts": [step.as_json() for step in steps],
        "headline": _headline(metrics),
        "limitations": LIMITATIONS,
        "models": models,
        "scope": SCOPE,
        "strategy_runs": strategy_runs,
        "verified": (
            "the split, labels, and features each reproduced from its source by chronos_dataset, the "
            "features from the dataset, the experiment's files, ID, models, predictions, and metrics, and "
            "each strategy run's schedule rows against the features recomputed from the verified dataset"
        ),
    }
