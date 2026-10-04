"""`python -m chronos_research evaluate|inspect|demo|infer|inspect-schedule`.

    evaluate          verifies a split artifact and its ancestry with
                      chronos_dataset, trains and evaluates both baselines,
                      and publishes the experiment
    inspect           verifies a published experiment without retraining
    demo              runs the whole pipeline from a local recording or
                      synthetic events into a directory of artifact roots,
                      and verifies it
    infer             publishes one fitted model's label-free inference
                      schedule for one instrument and evaluation window
    inspect-schedule  verifies a schedule and rebuilds it from its sources
    evaluate-strategy infers a schedule, runs it through chronos_strategy
                      beside both benchmarks, and verifies the run by running
                      it again; the research service runs it as a child job
    serve             the local research service (see service.py)

Every option is given at most once, never abbreviated, and never empty.
Success prints one JSON document on stdout and exits 0; a refused input or
an unexpected failure prints {"error": {...}} on stderr and exits 1;
malformed usage exits 2.
"""

import argparse
import json
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from chronos_research.artifacts import ArtifactError, is_artifact_id, verify_sources
from chronos_research.demo import DEFAULT_HORIZON_NS, DemoSource, run_demo
from chronos_research.experiment import MANIFEST, SCOPE
from chronos_research.inspection import InspectionError, verify_experiment
from chronos_research.pipeline import (
    ArtifactRoots,
    PipelineError,
    StrategyOptions,
    Tools,
    evaluate_baselines,
    experiment_split,
    infer_schedule,
    inspect_strategy,
    run_strategy,
    verified_sources,
)
from chronos_research.publication import PublicationError
from chronos_research.schedule import SCHEDULE_MODELS, WINDOWS, ScheduleError, reproduce_schedule


class _Once(argparse.Action):
    """Refuses an option given twice, where argparse keeps the last."""

    def __call__(
        self,
        parser: argparse.ArgumentParser,
        namespace: argparse.Namespace,
        values: str | Sequence[Any] | None,
        option_string: str | None = None,
    ) -> None:
        if getattr(namespace, self.dest, None) is not None:
            parser.error(f"{option_string} may be provided only once")
        setattr(namespace, self.dest, values)


def _path(text: str) -> Path:
    if not text:
        raise argparse.ArgumentTypeError("must not be empty")
    return Path(text)


def _positive_integer(text: str) -> int:
    if not text.isascii() or not text.isdigit() or text.startswith("0"):
        raise argparse.ArgumentTypeError("must be a positive whole number without a sign or leading zero")
    return int(text)


def _artifact_id(text: str) -> str:
    if not is_artifact_id(text):
        raise argparse.ArgumentTypeError("must be a 64-digit lowercase ID")
    return text


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="chronos_research", allow_abbrev=False, description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    evaluate = commands.add_parser("evaluate", allow_abbrev=False, help="train and evaluate both baselines")
    for option, help_text in (
        ("--chronos-dataset", "the chronos_dataset executable that verifies the source artifacts"),
        ("--features", "the feature artifact root"),
        ("--labels", "the label artifact root"),
        ("--splits", "the split artifact root"),
        ("--output", "the experiment root, created if its parent exists"),
    ):
        evaluate.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    evaluate.add_argument(
        "--split-artifact", required=True, type=_artifact_id, action=_Once, help="the split artifact's ID"
    )
    evaluate.add_argument(
        "--catalog",
        type=_path,
        action=_Once,
        help="the dataset catalog, to reproduce the feature artifact too",
    )
    demo = commands.add_parser("demo", allow_abbrev=False, help="run and verify the whole pipeline")
    demo.add_argument(
        "--tools", required=True, type=_path, action=_Once, help="the directory of built C++ tools"
    )
    demo.add_argument(
        "--artifacts",
        required=True,
        type=_path,
        action=_Once,
        help="the directory of artifact roots, such as data",
    )
    demo.add_argument("--stream-id", required=True, action=_Once, help="the market-data stream ID")
    source = demo.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--recording", type=_path, action=_Once, help="a local Databento EQUS.MINI MBP-1 recording"
    )
    source.add_argument(
        "--synthetic-events", type=_path, action=_Once, help="synthetic events for the test tool"
    )
    demo.add_argument(
        "--horizon-ns", type=_positive_integer, action=_Once, help="label horizon (default 1 s)"
    )
    inspect = commands.add_parser("inspect", allow_abbrev=False, help="verify a published experiment")
    inspect.add_argument("--output", required=True, type=_path, action=_Once, help="the experiment root")
    inspect.add_argument(
        "--experiment", required=True, type=_artifact_id, action=_Once, help="the experiment's ID"
    )
    infer = commands.add_parser("infer", allow_abbrev=False, help="publish a label-free inference schedule")
    inspect_schedule = commands.add_parser(
        "inspect-schedule", allow_abbrev=False, help="verify a schedule and rebuild it from its sources"
    )
    for command in (infer, inspect_schedule):
        command.add_argument(
            "--tools", required=True, type=_path, action=_Once, help="the directory of built Chronos tools"
        )
        command.add_argument(
            "--artifacts", required=True, type=_path, action=_Once, help="the directory of artifact roots"
        )
    infer.add_argument("--experiment", required=True, type=_artifact_id, action=_Once, help="the experiment")
    infer.add_argument("--instrument", required=True, action=_Once, help="the instrument, such as AAPL")
    infer.add_argument("--model", required=True, choices=SCHEDULE_MODELS, action=_Once, help="the model")
    infer.add_argument("--window", required=True, choices=WINDOWS, action=_Once, help="the evaluation window")
    inspect_schedule.add_argument(
        "--schedule", required=True, type=_artifact_id, action=_Once, help="the schedule's ID"
    )
    strategy = commands.add_parser(
        "evaluate-strategy", allow_abbrev=False, help="infer, run, and verify one strategy evaluation"
    )
    for option, help_text in (
        ("--tools", "the directory of built Chronos tools"),
        ("--artifacts", "the artifact roots"),
    ):
        strategy.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    strategy.add_argument(
        "--experiment", required=True, type=_artifact_id, action=_Once, help="the experiment"
    )
    strategy.add_argument("--instrument", required=True, action=_Once, help="the instrument")
    strategy.add_argument("--model", required=True, choices=SCHEDULE_MODELS, action=_Once, help="the model")
    strategy.add_argument(
        "--window", required=True, choices=WINDOWS, action=_Once, help="the evaluation window"
    )
    for option in STRATEGY_OPTIONS:
        strategy.add_argument(option, action=_Once, help="passed to chronos_strategy run unchanged")
    serve = commands.add_parser("serve", allow_abbrev=False, help="the local research service")
    for option, help_text in (
        ("--tools", "the directory of built Chronos tools"),
        ("--artifacts", "the directory of artifact roots"),
        ("--state", "the directory of durable job records"),
    ):
        serve.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    serve.add_argument("--host", action=_Once, help="a loopback address (default 127.0.0.1)")
    serve.add_argument("--port", type=_positive_integer, action=_Once, help="the port (default 8090)")
    serve.add_argument(
        "--max-running", type=_positive_integer, action=_Once, help="concurrent jobs (default 1)"
    )
    serve.add_argument("--max-queued", type=_positive_integer, action=_Once, help="queued jobs (default 8)")
    serve.add_argument("--job-timeout-s", type=_positive_integer, action=_Once, help="per job (default 1800)")
    serve.add_argument(
        "--retain-jobs", type=_positive_integer, action=_Once, help="job records (default 200)"
    )
    return parser


# The strategy options a job may set, by their chronos_strategy names.
STRATEGY_OPTIONS = {
    "--threshold-ppb": "threshold_ppb",
    "--target-quantity": "target_quantity",
    "--latency-ns": "latency_ns",
    "--decision-interval-ns": "decision_interval_ns",
    "--starting-cash": "starting_cash",
    "--fee-per-fill": "fee_per_fill",
    "--maximum-quote-age-ns": "maximum_quote_age_ns",
    "--maximum-order-notional": "maximum_order_notional",
}


def _evaluate_strategy(arguments: argparse.Namespace) -> dict[str, Any]:
    tools, roots = Tools(arguments.tools), ArtifactRoots.under(arguments.artifacts)
    split_id = experiment_split(roots.experiments, arguments.experiment)
    sources = verified_sources(tools, roots, split_id, reproduce_features=True)
    schedule = infer_schedule(
        sources, roots, arguments.experiment, arguments.instrument, arguments.model, arguments.window
    )
    options = StrategyOptions(**{name: getattr(arguments, name) for name in STRATEGY_OPTIONS.values()})
    run, _ = run_strategy(tools, roots, schedule.artifact_id, options)
    inspected = inspect_strategy(tools, roots, run.artifact_id)
    return {"reproduced": inspected["reproduced"], "run": run.as_json(), "schedule": schedule.as_json()}


def _serve(arguments: argparse.Namespace) -> dict[str, Any]:
    from chronos_research.service import ServiceConfiguration, serve

    configuration = ServiceConfiguration(
        tools=Tools(arguments.tools),
        artifacts=arguments.artifacts,
        state=arguments.state,
        host=arguments.host or "127.0.0.1",
        port=arguments.port or 8090,
        max_running=arguments.max_running or 1,
        max_queued=arguments.max_queued or 8,
        job_timeout_s=arguments.job_timeout_s or 1800,
        retain_jobs=arguments.retain_jobs or 200,
    )
    serve(configuration)
    return {"stopped": True}


def _schedule_summary(manifest: dict[str, Any]) -> dict[str, Any]:
    return {key: manifest[key] for key in ("counts", "model", "scope", "source", "training", "window")}


def _infer(arguments: argparse.Namespace) -> dict[str, Any]:
    tools, roots = Tools(arguments.tools), ArtifactRoots.under(arguments.artifacts)
    split_id = experiment_split(roots.experiments, arguments.experiment)
    sources = verified_sources(tools, roots, split_id, reproduce_features=True)
    step = infer_schedule(
        sources, roots, arguments.experiment, arguments.instrument, arguments.model, arguments.window
    )
    manifest = json.loads((step.directory / "manifest.json").read_bytes())
    return {**step.as_json(), **_schedule_summary(manifest)}


def _inspect_schedule(arguments: argparse.Namespace) -> dict[str, Any]:
    tools, roots = Tools(arguments.tools), ArtifactRoots.under(arguments.artifacts)
    directory = roots.schedules / arguments.schedule
    if not directory.is_dir():
        raise ScheduleError(f"the schedule {arguments.schedule} does not exist under {roots.schedules}")
    split_id = json.loads((directory / "manifest.json").read_bytes())["source"]["split_artifact_id"]
    if not isinstance(split_id, str) or not is_artifact_id(split_id):
        raise ScheduleError("the schedule names no split artifact")
    sources = verified_sources(tools, roots, split_id, reproduce_features=True)
    manifest = reproduce_schedule(directory, sources, roots.experiments)
    return {
        "artifact_id": manifest["artifact_id"],
        "directory": str(directory),
        **_schedule_summary(manifest),
        "verified": (
            "the file set, canonical documents, schedule ID, checksum, every row and count, and the whole "
            "schedule rebuilt from its verified sources and experiment: identical bytes"
        ),
    }


def _headline(splits: dict[str, Any]) -> dict[str, Any]:
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
        for split, details in splits.items()
    }


def _evaluate(arguments: argparse.Namespace) -> dict[str, Any]:
    sources = verify_sources(
        arguments.chronos_dataset,
        features_root=arguments.features,
        labels_root=arguments.labels,
        splits_root=arguments.splits,
        split_artifact_id=arguments.split_artifact,
        catalog=arguments.catalog,
    )
    output: Path = arguments.output
    if not output.is_dir():
        output.mkdir()
    step, metrics = evaluate_baselines(sources, output)
    return {
        "directory": str(step.directory),
        "experiment_id": step.artifact_id,
        "headline": _headline(metrics["splits"]),
        "outcome": step.outcome,
        "scope": SCOPE,
        "sources": json.loads((step.directory / MANIFEST).read_bytes())["sources"],
        "warning": step.warning,
    }


def _demo(arguments: argparse.Namespace) -> dict[str, Any]:
    return run_demo(
        Tools(arguments.tools),
        ArtifactRoots.under(arguments.artifacts),
        DemoSource(
            stream_id=arguments.stream_id,
            recording=arguments.recording,
            synthetic_events=arguments.synthetic_events,
        ),
        arguments.horizon_ns or DEFAULT_HORIZON_NS,
    )


def _inspect(arguments: argparse.Namespace) -> dict[str, Any]:
    directory = arguments.output / arguments.experiment
    manifest = verify_experiment(directory)
    metrics = json.loads((directory / "metrics.json").read_bytes())
    return {
        "directory": str(directory),
        "experiment_id": manifest["experiment_id"],
        "headline": _headline(metrics["splits"]),
        "scope": manifest["scope"],
        "sources": manifest["sources"],
        "verified": (
            "the file set, canonical documents, experiment ID, checksums, model parameters, every prediction "
            "against the stored models, and the metrics recomputed from the predictions"
        ),
    }


def _fail(kind: str, message: str) -> int:
    print(json.dumps({"error": {"kind": kind, "message": message}}, indent=2), file=sys.stderr)
    return 1


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    commands = {
        "evaluate": _evaluate,
        "inspect": _inspect,
        "demo": _demo,
        "infer": _infer,
        "inspect-schedule": _inspect_schedule,
        "evaluate-strategy": _evaluate_strategy,
        "serve": _serve,
    }
    try:
        result = commands[arguments.command](arguments)
    except (ArtifactError, InspectionError, PipelineError, PublicationError, ScheduleError, OSError) as error:
        return _fail(type(error).__name__, str(error))
    except Exception as error:  # Still one JSON document, as the C++ tools give.
        return _fail("internal_error", f"{type(error).__name__}: {error}")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0
