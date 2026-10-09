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
    inference-worker  one model deployment's long-lived inference, for the
                      paper-trading server (see inference_worker.py)
    register-model    registers one fitted model of a verified experiment,
                      whose sources chronos_dataset verifies again, as an
                      immutable model version with a name and version
    list-models       lists the registered model versions
    inspect-model     verifies one model version and traces its source
    set-model-lifecycle  archives or reactivates a model version
    onnx-sandbox      reports whether the ONNX sandbox can run here, or
                      builds its pinned image
    import-model      validates an ONNX model in the sandbox and imports
                      exactly those bytes as a model version; the research
                      service runs it as a child job on a staged import
    export-onnx       writes a registered logistic version as an ONNX model
                      and an import manifest, with a parity report
    compare-models    runs a chronos.model_comparison_spec/1 and publishes
                      the comparison; the research service runs it as a job
    inspect-comparison  verifies a comparison and replays every member's run
    create-collection   publishes a chronos.research_collection/1 of recordings,
                      each verified from its sources
    list-collections, inspect-collection
    plan-walk-forward   builds a chronos.walk_forward_plan/1 from a request and
                      publishes it, or with --preview only shows it
    list-walk-forward-plans, inspect-walk-forward-plan
    run-walk-forward    runs a plan's every fold, model, and scenario through
                      chronos_strategy and publishes the
                      chronos.walk_forward_study/1; the research service runs
                      it as a job, with --progress
    list-walk-forward-studies, inspect-walk-forward-study

Every option is given at most once, never abbreviated, and never empty.
Success prints one JSON document on stdout and exits 0; a refused input or
an unexpected failure prints {"error": {...}} on stderr and exits 1;
malformed usage exits 2.
"""

import argparse
import hashlib
import json
import os
import stat
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from chronos_research import collection, comparison, model_library, walk_forward, walk_forward_study
from chronos_research.artifacts import ArtifactError, is_artifact_id, verify_sources
from chronos_research.demo import DEFAULT_HORIZON_NS, DemoSource, run_demo
from chronos_research.encoding import StrictJsonError, strict_json_object
from chronos_research.experiment import MANIFEST, SCOPE
from chronos_research.import_staging import StagingError, read_staged_model
from chronos_research.inspection import InspectionError, verify_experiment
from chronos_research.model_import import ModelImportError, import_model
from chronos_research.onnx_profile import MAX_MODEL_BYTES
from chronos_research.onnx_sandbox import (
    SUPERVISOR_DIGITS,
    Sandbox,
    SandboxFailedError,
    SandboxRejectedError,
    SandboxUnavailableError,
    is_supervisor,
)
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
from chronos_research.registry import (
    EXPERIMENT_FAMILIES,
    LIFECYCLE_STATES,
    RegistryError,
    RegistryRoots,
    model_version_ids,
    names_by_version,
    register_experiment_model,
    set_lifecycle,
    verified_model_version,
)
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


def _supervisor(text: str) -> str:
    if not is_supervisor(text):
        raise argparse.ArgumentTypeError(f"must be {SUPERVISOR_DIGITS} lowercase hexadecimal digits")
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
    worker = commands.add_parser(
        "inference-worker", allow_abbrev=False, help="one deployment's model inference (server use)"
    )
    worker.add_argument("--artifacts", required=True, type=_path, action=_Once, help="the artifact roots")
    worker.add_argument(
        "--experiment", type=_artifact_id, action=_Once, help="an experiment's model: the experiment"
    )
    worker.add_argument("--instrument", required=True, action=_Once, help="the instrument")
    worker.add_argument(
        "--model", choices=SCHEDULE_MODELS, action=_Once, help="an experiment's model: the model"
    )
    worker.add_argument(
        "--model-version", type=_artifact_id, action=_Once, help="a registered model version, by its ID"
    )
    worker.add_argument("--describe", action="store_true", help="report the model's identity and exit")
    _docker_options(worker, supervised=True)
    register = commands.add_parser(
        "register-model", allow_abbrev=False, help="register a verified experiment model as a model version"
    )
    for option, help_text in (
        ("--tools", "the directory of built Chronos tools"),
        ("--artifacts", "the directory of artifact roots"),
    ):
        register.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    register.add_argument(
        "--experiment", required=True, type=_artifact_id, action=_Once, help="the experiment"
    )
    register.add_argument("--instrument", required=True, action=_Once, help="the instrument")
    register.add_argument(
        "--model", required=True, choices=EXPERIMENT_FAMILIES, action=_Once, help="the model"
    )
    register.add_argument("--name", required=True, action=_Once, help="the model name, such as aapl-logistic")
    register.add_argument("--version", required=True, action=_Once, help="its version, such as 1")
    listing = commands.add_parser("list-models", allow_abbrev=False, help="list registered model versions")
    listing.add_argument("--artifacts", required=True, type=_path, action=_Once, help="the artifact roots")
    inspect_model = commands.add_parser("inspect-model", allow_abbrev=False, help="verify one model version")
    inspect_model.add_argument(
        "--artifacts", required=True, type=_path, action=_Once, help="the artifact roots"
    )
    inspect_model.add_argument(
        "--model-version", required=True, type=_artifact_id, action=_Once, help="the model version ID"
    )
    _docker_options(inspect_model)
    lifecycle = commands.add_parser(
        "set-model-lifecycle", allow_abbrev=False, help="archive or reactivate a model version"
    )
    lifecycle.add_argument("--artifacts", required=True, type=_path, action=_Once, help="the artifact roots")
    lifecycle.add_argument(
        "--model-version", required=True, type=_artifact_id, action=_Once, help="the model version ID"
    )
    lifecycle.add_argument("--state", required=True, choices=LIFECYCLE_STATES, action=_Once, help="the state")
    lifecycle.add_argument("--reason", required=True, action=_Once, help="why, for the record")
    sandbox = commands.add_parser("onnx-sandbox", allow_abbrev=False, help="the ONNX sandbox's capability")
    sandbox.add_argument("action", choices=("status", "build"), help="report its status, or build its image")
    _docker_options(sandbox)
    importing = commands.add_parser(
        "import-model", allow_abbrev=False, help="validate an ONNX model in the sandbox and import it"
    )
    importing.add_argument("--artifacts", required=True, type=_path, action=_Once, help="the artifact roots")
    importing.add_argument("--staged", type=_path, action=_Once, help="a staged import (service use)")
    importing.add_argument("--manifest", type=_path, action=_Once, help="the chronos.model_import/1 manifest")
    importing.add_argument("--model", type=_path, action=_Once, help="the ONNX model file")
    importing.add_argument(
        "--model-sha256", required=True, type=_artifact_id, action=_Once, help="the reviewed model's SHA-256"
    )
    _docker_options(importing, supervised=True)
    export = commands.add_parser(
        "export-onnx", allow_abbrev=False, help="export a registered logistic version as an ONNX model"
    )
    for option, help_text in (
        ("--tools", "the directory of built Chronos tools, to verify the parity rows' sources"),
        ("--artifacts", "the directory of artifact roots"),
    ):
        export.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    export.add_argument(
        "--model-version", required=True, type=_artifact_id, action=_Once, help="the logistic version"
    )
    export.add_argument("--output", required=True, type=_path, action=_Once, help="a new directory")
    export.add_argument("--name", required=True, action=_Once, help="the import's model name")
    export.add_argument("--version", required=True, action=_Once, help="the import's version")
    _docker_options(export)
    compare = commands.add_parser(
        "compare-models", allow_abbrev=False, help="compare registered model versions"
    )
    for option, help_text in (
        ("--tools", "the directory of built Chronos tools"),
        ("--artifacts", "the directory of artifact roots"),
    ):
        compare.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    compare.add_argument("--spec", required=True, action=_Once, help="the comparison spec, as JSON")
    _docker_options(compare, supervised=True)
    inspect_comparison = commands.add_parser(
        "inspect-comparison", allow_abbrev=False, help="verify a comparison and replay its members' runs"
    )
    for option, help_text in (
        ("--tools", "the directory of built Chronos tools"),
        ("--artifacts", "the directory of artifact roots"),
    ):
        inspect_comparison.add_argument(option, required=True, type=_path, action=_Once, help=help_text)
    inspect_comparison.add_argument(
        "--comparison", required=True, type=_artifact_id, action=_Once, help="the comparison ID"
    )
    _walk_forward_commands(commands)
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
    _docker_options(serve)
    return parser


def _roots(parser: argparse.ArgumentParser, tools: bool = True) -> None:
    if tools:
        parser.add_argument(
            "--tools", required=True, type=_path, action=_Once, help="the directory of built Chronos tools"
        )
    parser.add_argument(
        "--artifacts", required=True, type=_path, action=_Once, help="the directory of artifact roots"
    )


def _session_label(text: str) -> tuple[str, str]:
    label_id, separator, label = text.partition("=")
    if not separator or not is_artifact_id(label_id) or not label:
        raise argparse.ArgumentTypeError("must be <label-artifact-id>=<declared session label>")
    return label_id, label


def _walk_forward_commands(commands: Any) -> None:
    create = commands.add_parser(
        "create-collection", allow_abbrev=False, help="publish a collection of verified recordings"
    )
    _roots(create)
    create.add_argument(
        "--label-artifact",
        required=True,
        type=_artifact_id,
        action="append",
        help="a member recording's label artifact; once per member",
    )
    create.add_argument(
        "--session-label",
        type=_session_label,
        action="append",
        help="<label-artifact-id>=<text>: a declared session or date label, recorded as declared",
    )
    _roots(commands.add_parser("list-collections", allow_abbrev=False, help="list collections"), tools=False)
    inspect = commands.add_parser(
        "inspect-collection", allow_abbrev=False, help="verify a collection and every member's sources"
    )
    _roots(inspect)
    inspect.add_argument(
        "--collection", required=True, type=_artifact_id, action=_Once, help="the collection ID"
    )
    plan = commands.add_parser(
        "plan-walk-forward", allow_abbrev=False, help="build a walk-forward plan from a request"
    )
    _roots(plan)
    plan.add_argument(
        "--request", required=True, action=_Once, help="the chronos.walk_forward_request/1, as JSON"
    )
    plan.add_argument("--preview", action="store_true", help="show the plan without publishing it")
    _roots(
        commands.add_parser("list-walk-forward-plans", allow_abbrev=False, help="list walk-forward plans"),
        tools=False,
    )
    inspect_plan = commands.add_parser(
        "inspect-walk-forward-plan",
        allow_abbrev=False,
        help="verify a plan by rebuilding it from its request",
    )
    _roots(inspect_plan)
    inspect_plan.add_argument("--plan", required=True, type=_artifact_id, action=_Once, help="the plan ID")
    study = commands.add_parser(
        "run-walk-forward", allow_abbrev=False, help="run a walk-forward plan and publish its study"
    )
    _roots(study)
    study.add_argument("--plan", required=True, type=_artifact_id, action=_Once, help="the plan ID")
    study.add_argument(
        "--progress", action="store_true", help='write {"progress": {...}} lines to stderr as it runs'
    )
    _roots(
        commands.add_parser(
            "list-walk-forward-studies", allow_abbrev=False, help="list walk-forward studies"
        ),
        tools=False,
    )
    inspect_study = commands.add_parser(
        "inspect-walk-forward-study",
        allow_abbrev=False,
        help="verify a study by rebuilding every fold and replaying every run",
    )
    _roots(inspect_study)
    inspect_study.add_argument("--study", required=True, type=_artifact_id, action=_Once, help="the study ID")


def _create_collection(arguments: argparse.Namespace) -> dict[str, Any]:
    labels = dict(arguments.session_label or [])
    if len(labels) != len(arguments.session_label or []):
        raise collection.CollectionError("invalid_request", "a member has at most one session label")
    return collection.create_collection(
        Tools(arguments.tools), arguments.artifacts, arguments.label_artifact, labels
    )


def _listing(root: Path, ids: list[str], verify: Any, key: str) -> dict[str, Any]:
    """Every entry that verifies, in ID order, and every other one with why."""
    entries, invalid = [], []
    for identifier in ids:
        try:
            entries.append(verify(root, identifier))
        except (
            collection.CollectionError,
            walk_forward.PlanError,
            walk_forward_study.StudyError,
        ) as error:
            invalid.append({"id": identifier, "reason": str(error)})
    return {key: entries, "invalid_entries": invalid}


def _list_collections(arguments: argparse.Namespace) -> dict[str, Any]:
    root = collection.collections_root(arguments.artifacts)
    return _listing(root, collection.collection_ids(root), collection.verified_collection, "collections")


def _inspect_collection(arguments: argparse.Namespace) -> dict[str, Any]:
    return {
        "collection": collection.inspect_collection(
            Tools(arguments.tools), arguments.artifacts, arguments.collection
        ),
        "verified": "every member re-verified from its sources; the collection they give is identical",
    }


def _plan_walk_forward(arguments: argparse.Namespace) -> dict[str, Any]:
    try:
        request = strict_json_object(arguments.request.encode())
    except StrictJsonError as error:
        raise walk_forward.PlanError("invalid_request", f"the request: {error}") from error
    if arguments.preview:
        return {
            "outcome": "preview",
            "plan": walk_forward.build_plan(Tools(arguments.tools), arguments.artifacts, request),
        }
    return walk_forward.create_plan(Tools(arguments.tools), arguments.artifacts, request)


def _list_walk_forward_plans(arguments: argparse.Namespace) -> dict[str, Any]:
    root = walk_forward.plans_root(arguments.artifacts)
    return _listing(root, walk_forward.plan_ids(root), walk_forward.verified_plan, "plans")


def _inspect_walk_forward_plan(arguments: argparse.Namespace) -> dict[str, Any]:
    return {
        "plan": walk_forward.inspect_plan(Tools(arguments.tools), arguments.artifacts, arguments.plan),
        "verified": "the collection's sources re-verified; the plan rebuilt from its request is identical",
    }


def _progress(update: Any) -> None:
    sys.stderr.write(json.dumps({"progress": dict(update)}, sort_keys=True) + "\n")
    sys.stderr.flush()


def _run_walk_forward(arguments: argparse.Namespace) -> dict[str, Any]:
    return walk_forward_study.run_study(
        Tools(arguments.tools),
        arguments.artifacts,
        arguments.plan,
        _progress if arguments.progress else walk_forward_study.no_progress,
    )


def _list_walk_forward_studies(arguments: argparse.Namespace) -> dict[str, Any]:
    root = walk_forward_study.studies_root(arguments.artifacts)
    listed = _listing(
        root,
        walk_forward_study.study_ids(root),
        lambda root, identifier: walk_forward_study.listing(
            walk_forward_study.verified_study(root, identifier)
        ),
        "studies",
    )
    return listed


def _inspect_walk_forward_study(arguments: argparse.Namespace) -> dict[str, Any]:
    return walk_forward_study.inspect_study(Tools(arguments.tools), arguments.artifacts, arguments.study)


def _docker_options(parser: argparse.ArgumentParser, supervised: bool = False) -> None:
    parser.add_argument("--docker", action=_Once, help="the Docker CLI for the ONNX sandbox (default docker)")
    parser.add_argument("--docker-host", action=_Once, help="DOCKER_HOST for it, if not the default")
    if supervised:
        parser.add_argument(
            "--sandbox-supervisor",
            type=_supervisor,
            action=_Once,
            help="the token of the process that runs this one, to label its sandbox containers",
        )


def _sandbox(arguments: argparse.Namespace) -> Sandbox:
    return Sandbox(
        docker=arguments.docker or "docker",
        docker_host=arguments.docker_host,
        supervisor=getattr(arguments, "sandbox_supervisor", None),
    )


def _bounded_file(path: Path, limit: int, what: str) -> bytes:
    """A regular file of at most `limit` bytes, read once."""
    with open(path, "rb") as file:
        if not stat.S_ISREG(os.fstat(file.fileno()).st_mode):
            raise ModelImportError("invalid_upload", f"the {what} is not a regular file")
        data = file.read(limit + 1)
    if len(data) > limit:
        raise ModelImportError(
            "model_too_large" if what == "model" else "invalid_manifest", f"the {what} exceeds {limit} bytes"
        )
    return data


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
        sandbox=_sandbox(arguments),
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


def _register_model(arguments: argparse.Namespace) -> dict[str, Any]:
    tools, roots = Tools(arguments.tools), ArtifactRoots.under(arguments.artifacts)
    registry = RegistryRoots.under(arguments.artifacts)
    split_id = experiment_split(roots.experiments, arguments.experiment)
    sources = verified_sources(tools, roots, split_id, reproduce_features=True)
    registration = register_experiment_model(
        registry,
        roots.experiments,
        sources,
        arguments.experiment,
        arguments.instrument,
        arguments.model,
        arguments.name,
        arguments.version,
    )
    version = verified_model_version(registry, registration.model_version_id)
    names = names_by_version(registry).get(version.model_version_id, [])
    return {**registration.as_json(), "model": model_library.summary(registry, version, names)}


def _list_models(arguments: argparse.Namespace) -> dict[str, Any]:
    registry = RegistryRoots.under(arguments.artifacts)
    names = names_by_version(registry)
    models, invalid = [], []
    for version_id in model_version_ids(registry):
        try:
            version = verified_model_version(registry, version_id)
            models.append(
                model_library.listing_entry(
                    model_library.summary(registry, version, names.get(version_id, []))
                )
            )
        except RegistryError as error:
            invalid.append({"model_version_id": version_id, "reason": str(error)})
    return {"invalid_entries": invalid, "models": models}


def _inspect_model(arguments: argparse.Namespace) -> dict[str, Any]:
    registry = RegistryRoots.under(arguments.artifacts)
    version = verified_model_version(registry, arguments.model_version)
    names = names_by_version(registry).get(version.model_version_id, [])
    return {
        "model": model_library.detail(
            registry,
            ArtifactRoots.under(arguments.artifacts).experiments,
            version,
            names,
            _sandbox(arguments).capability(),
        ),
        "verified": (
            "the file set, the canonical manifest and its ID, the payload's size and checksum, the payload "
            "parsed as a valid model of its family, and its training and provenance records"
        ),
    }


def _onnx_sandbox(arguments: argparse.Namespace) -> dict[str, Any]:
    sandbox = _sandbox(arguments)
    if arguments.action == "build":
        return {"image": sandbox.build(), "sandbox": sandbox.capability()}
    return {"reaped": sandbox.reap_stale(), "sandbox": sandbox.capability()}


def _import_inputs(arguments: argparse.Namespace) -> tuple[dict[str, Any], bytes]:
    """The manifest and the model bytes, read once; the bytes must be the
    reviewed ones."""
    if arguments.staged is not None:
        if arguments.manifest is not None or arguments.model is not None:
            raise ModelImportError("invalid_request", "--staged replaces --manifest and --model")
        return read_staged_model(arguments.staged, arguments.model_sha256)
    if arguments.manifest is None or arguments.model is None:
        raise ModelImportError("invalid_request", "give --manifest and --model, or --staged")
    try:
        manifest = strict_json_object(_bounded_file(arguments.manifest, 16 * 1024, "manifest"))
    except StrictJsonError as error:
        raise ModelImportError("invalid_manifest", f"the manifest: {error}") from error
    model = _bounded_file(arguments.model, MAX_MODEL_BYTES, "model")
    if hashlib.sha256(model).hexdigest() != arguments.model_sha256:
        raise ModelImportError("model_mismatch", "the model file is not the one whose SHA-256 was given")
    return manifest, model


def _import_model(arguments: argparse.Namespace) -> dict[str, Any]:
    registry = RegistryRoots.under(arguments.artifacts)
    manifest, model = _import_inputs(arguments)
    registration = import_model(registry, manifest, model, _sandbox(arguments))
    version = verified_model_version(registry, registration.model_version_id)
    names = names_by_version(registry).get(version.model_version_id, [])
    return {**registration.as_json(), "model": model_library.summary(registry, version, names)}


def _export_onnx(arguments: argparse.Namespace) -> dict[str, Any]:
    from chronos_research.onnx_export import export_logistic

    registry = RegistryRoots.under(arguments.artifacts)
    version = verified_model_version(registry, arguments.model_version)
    return export_logistic(
        Tools(arguments.tools),
        arguments.artifacts,
        version,
        arguments.output,
        arguments.name,
        arguments.version,
        _sandbox(arguments),
    )


def _compare_models(arguments: argparse.Namespace) -> dict[str, Any]:
    try:
        spec = strict_json_object(arguments.spec.encode())
    except StrictJsonError as error:
        raise comparison.ComparisonError(f"the spec: {error}") from error
    return comparison.compare(Tools(arguments.tools), arguments.artifacts, spec, _sandbox(arguments))


def _inspect_comparison(arguments: argparse.Namespace) -> dict[str, Any]:
    roots = ArtifactRoots.under(arguments.artifacts)
    manifest = comparison.verified_comparison(
        comparison.comparisons_root(arguments.artifacts), arguments.comparison
    )
    replays = {
        member["model_version_id"]: inspect_strategy(
            Tools(arguments.tools), roots, member["run"]["artifact_id"]
        )["reproduced"]
        for member in manifest["members"]
        if "run" in member
    }
    return {
        "artifacts": comparison.member_artifacts(roots, manifest),
        "comparison_id": manifest["comparison_id"],
        "replayed": replays,
        "status": manifest["status"],
    }


def _set_model_lifecycle(arguments: argparse.Namespace) -> dict[str, Any]:
    registry = RegistryRoots.under(arguments.artifacts)
    record = set_lifecycle(registry, arguments.model_version, arguments.state, arguments.reason)
    return {"lifecycle": record, "model_version_id": arguments.model_version}


def _fail(kind: str, message: str, reason: str | None = None) -> int:
    error = {"kind": kind, "message": message} | ({} if reason is None else {"reason": reason})
    print(json.dumps({"error": error}, indent=2), file=sys.stderr)
    return 1


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.command == "inference-worker":
        # A line protocol of its own, not one JSON document.
        from chronos_research.inference_worker import main as worker_main

        return worker_main(arguments)
    commands = {
        "evaluate": _evaluate,
        "inspect": _inspect,
        "demo": _demo,
        "infer": _infer,
        "inspect-schedule": _inspect_schedule,
        "evaluate-strategy": _evaluate_strategy,
        "serve": _serve,
        "register-model": _register_model,
        "list-models": _list_models,
        "inspect-model": _inspect_model,
        "set-model-lifecycle": _set_model_lifecycle,
        "onnx-sandbox": _onnx_sandbox,
        "import-model": _import_model,
        "export-onnx": _export_onnx,
        "compare-models": _compare_models,
        "inspect-comparison": _inspect_comparison,
        "create-collection": _create_collection,
        "list-collections": _list_collections,
        "inspect-collection": _inspect_collection,
        "plan-walk-forward": _plan_walk_forward,
        "list-walk-forward-plans": _list_walk_forward_plans,
        "inspect-walk-forward-plan": _inspect_walk_forward_plan,
        "run-walk-forward": _run_walk_forward,
        "list-walk-forward-studies": _list_walk_forward_studies,
        "inspect-walk-forward-study": _inspect_walk_forward_study,
    }
    try:
        result = commands[arguments.command](arguments)
    except (
        collection.CollectionError,
        ModelImportError,
        SandboxFailedError,
        SandboxRejectedError,
        SandboxUnavailableError,
        StagingError,
        walk_forward.PlanError,
        walk_forward_study.StudyError,
    ) as error:
        return _fail(type(error).__name__, str(error), error.reason)
    except (
        ArtifactError,
        comparison.ComparisonError,
        InspectionError,
        PipelineError,
        PublicationError,
        RegistryError,
        ScheduleError,
        OSError,
    ) as error:
        return _fail(type(error).__name__, str(error))
    except Exception as error:  # Still one JSON document, as the C++ tools give.
        return _fail("internal_error", f"{type(error).__name__}: {error}")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0
