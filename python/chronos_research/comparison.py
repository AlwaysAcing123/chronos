"""Fair historical comparisons of registered model versions (ADR 0021).

A comparison runs 2 to 6 model versions under one frozen specification,
`chronos.model_comparison_spec/1`: the same verified split, instrument,
evaluation window, and strategy configuration for every member. Each member
is predicted point in time through the registered-model adapter
(registered_schedule), run by the C++ strategy engine in its own run beside
its own cash and buy-and-hold accounts, and verified twice: its schedule
rebuilt from its sources and version, and its run replayed. Nothing is
retrained.

A member is completed, refused (a compatibility rule kept it from running),
or failed (it ran and something failed), always with its reason; no member
disappears. The comparison, `chronos.model_comparison/1`, is published
whatever its members' outcomes, content-addressed like every artifact:

    <comparisons>/<comparison_id>/manifest.json

Trading metrics are the engine's exact decimals. Deltas are exact decimal
differences computed here, never in the browser; a delta whose input is
unavailable is unavailable with that reason. Prediction quality is scored
apart from them (scoring.py): once every member's predictions exist, each
completed member's stored predictions are scored against the window's
realized labels, with the experiment's metric definitions. A member whose
cutoff its uploader declared is marked conditional: its out-of-sample claim
holds only if the declaration does.
"""

import json
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from decimal import Context, Decimal, Inexact
from pathlib import Path
from typing import Any

from chronos_research import registry
from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    listed_files,
    verified_file,
    verified_manifest,
)
from chronos_research.artifacts import ArtifactError, VerifiedSources, is_artifact_id
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.experiment import environment, source_json
from chronos_research.onnx_sandbox import (
    Sandbox,
    SandboxFailedError,
    SandboxRejectedError,
    SandboxUnavailableError,
)
from chronos_research.pipeline import (
    ArtifactRoots,
    PipelineError,
    Tools,
    inspect_strategy,
    run_strategy,
    verified_sources,
)
from chronos_research.publication import PublicationError, publish
from chronos_research.registered_schedule import ScheduleRefusedError, build_registered_schedule
from chronos_research.registry import ModelVersion, RegistryRoots, names_by_version
from chronos_research.schedule import (
    MANIFEST,
    ROWS,
    WINDOWS,
    Schedule,
    ScheduleError,
    evaluation_window,
    verify_schedule,
)
from chronos_research.scoring import ScoringError, score_schedule, window_labels
from chronos_research.strategy_configuration import (
    configuration_problem,
    strategy_options,
)

SPEC_SCHEMA = {"name": "chronos.model_comparison_spec", "version": "1"}
SCHEMA = {"name": "chronos.model_comparison", "version": "1"}
MIN_MODELS = 2
MAX_MODELS = 6
ACCOUNTS = ("strategy", "buy_and_hold", "cash")
FAIRNESS = (
    "every member is predicted on the same feature rows of the same verified split, instrument, and window, "
    "the unlabelled tail included, and run with the same configuration over the same recorded events",
    "each member trades in its own strategy account beside its own cash and buy-and-hold accounts; no "
    "account shares liquidity, orders, or fills with another member's",
    "no member was retrained: an experiment model's verified parameters and an import's exact bytes are used "
    "as registered",
    "every member's information cutoff is at or before the window's start; a cutoff its uploader declared "
    "makes that member's result conditional on the declaration",
)
SCORING = (
    "prediction quality is scored only after every member's predictions exist, from each completed member's "
    "stored predictions, against the split's realized labels for the window; the model is not run again",
    "rows are joined to labels by stream, instrument, and ingest sequence; a row is scored when the split "
    "assigns it to the window with a usable label, and every other row is counted under the split's reason",
    "the metrics are the experiment's definitions, in binary floating point; a metric without a value is "
    "undefined with its reason; classification quality is not trading performance",
)
_EXACT = Context(prec=100, traps=[Inexact])


class ComparisonError(Exception):
    """The comparison as a whole cannot be built or read."""


# --- The specification ----------------------------------------------------------------------------------


def validated_spec(document: Any) -> dict[str, Any]:
    """The spec's structure; the strategy runner validates the configuration
    values, as it does for any evaluation."""
    keys = {"configuration", "instrument", "model_version_ids", "schema", "split_artifact_id", "window"}
    if not isinstance(document, dict) or set(document) != keys:
        raise ComparisonError(f"a comparison spec has exactly {', '.join(sorted(keys))}")
    if document["schema"] != SPEC_SCHEMA:
        raise ComparisonError(f"the spec's schema is {SPEC_SCHEMA}")
    models = document["model_version_ids"]
    if (
        not isinstance(models, list)
        or not MIN_MODELS <= len(models) <= MAX_MODELS
        or len(set(models)) != len(models)
        or not all(isinstance(item, str) and is_artifact_id(item) for item in models)
    ):
        raise ComparisonError(f"a comparison names {MIN_MODELS} to {MAX_MODELS} distinct model version IDs")
    if not isinstance(document["split_artifact_id"], str) or not is_artifact_id(
        document["split_artifact_id"]
    ):
        raise ComparisonError("split_artifact_id is a 64-digit lowercase ID")
    if document["window"] not in WINDOWS:
        raise ComparisonError(f"window is one of {', '.join(WINDOWS)}")
    if not isinstance(document["instrument"], str) or not registry.INSTRUMENT.fullmatch(
        document["instrument"]
    ):
        raise ComparisonError("instrument must be one valid instrument identifier")
    problem = configuration_problem(document["configuration"])
    if problem is not None:
        raise ComparisonError(problem)
    return dict(document)


def spec_digest(spec: Mapping[str, Any]) -> str:
    return sha256_hex(canonical_json(spec))


# --- Exact deltas ---------------------------------------------------------------------------------------


def exact_difference(left: str, right: str) -> str:
    """left - right, exactly, as a canonical decimal string."""
    value = _EXACT.subtract(Decimal(left), Decimal(right))
    text = format(value, "f")
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return "0" if text in ("-0", "") else text


def _valuation(account: Mapping[str, Any]) -> dict[str, Any]:
    return dict(account["valuation"])


def _delta(left: Mapping[str, Any], right: Mapping[str, Any], key: str) -> dict[str, str]:
    for side in (left, right):
        if "unavailable" in _valuation(side):
            return {"unavailable": f"valuation_unavailable:{_valuation(side)['unavailable']}"}
    return {"value": exact_difference(_valuation(left)[key], _valuation(right)[key])}


def deltas(member: Mapping[str, Any], reference: Mapping[str, Any] | None) -> dict[str, Any]:
    """The member's strategy against its benchmarks and the reference member."""
    accounts = member["accounts"]
    against = {"buy_and_hold": accounts["buy_and_hold"], "cash": accounts["cash"]}
    if reference is not None:
        against["reference"] = reference["accounts"]["strategy"]
    return {
        name: {key: _delta(accounts["strategy"], other, key) for key in ("net_total_pnl", "return_percent")}
        for name, other in against.items()
    }


# --- Members --------------------------------------------------------------------------------------------


def account_summary(results: Mapping[str, Any]) -> dict[str, Any]:
    valuation = results["final"]["valuation"]
    return {
        "drawdown": {
            key: results["drawdown"][key] for key in ("maximum", "maximum_percent_of_peak", "points_unvalued")
        },
        "fills": results["fills"],
        "orders": {key: results["orders"][key] for key in ("expired", "filled", "rejected", "scheduled")},
        "turnover": results["turnover"],
        "valuation": (
            {"unavailable": valuation["unavailable"]}
            if "unavailable" in valuation
            else {key: valuation[key] for key in ("equity", "fees_paid", "net_total_pnl", "return_percent")}
        ),
    }


def _cutoff(version: ModelVersion) -> dict[str, Any]:
    cutoff = version.manifest["training"]["information_cutoff"]
    return {key: cutoff[key] for key in ("at_ns", "basis", "reason") if key in cutoff}


@dataclass(frozen=True)
class ComparisonContext:
    tools: Tools
    roots: ArtifactRoots
    registry: RegistryRoots
    sources: VerifiedSources
    spec: Mapping[str, Any]
    sandbox: Sandbox


def _refusal(context: ComparisonContext, version: ModelVersion) -> tuple[str, str] | None:
    """Why this version may not run in this comparison, if it may not."""
    if registry.lifecycle(context.registry, version.model_version_id)["state"] != "active":
        return "archived", "an archived version cannot be chosen for a new comparison"
    if version.instrument != context.spec["instrument"]:
        return (
            "instrument_mismatch",
            f"the version predicts {version.instrument}, not {context.spec['instrument']}",
        )
    if version.manifest["target"]["horizon_ns"] != str(context.sources.horizon_ns):
        return (
            "horizon_mismatch",
            f"the version predicts a {version.manifest['target']['horizon_ns']} ns horizon; the split's "
            f"labels use {context.sources.horizon_ns} ns",
        )
    return None


def _publish_schedule(roots: ArtifactRoots, built: Schedule) -> None:
    roots.schedules.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verify_schedule(directory)
        return (directory / MANIFEST).read_bytes()

    publish(roots.schedules, built.schedule_id, built.files, MANIFEST, existing)


def _schedule(context: ComparisonContext, version: ModelVersion) -> Schedule:
    return build_registered_schedule(context.sources, version, context.spec["window"], context.sandbox)


def run_member(
    context: ComparisonContext, model_version_id: str, names: Sequence[Mapping[str, str]]
) -> dict[str, Any]:
    """One member, completed, refused, or failed, with its reason."""
    member: dict[str, Any] = {"model_version_id": model_version_id, "names": list(names)}
    try:
        version = registry.verified_model_version(context.registry, model_version_id)
    except registry.RegistryError as error:
        return {**member, "detail": str(error), "reason": "artifact_invalid", "state": "refused"}
    member.update(
        family=version.family,
        information_cutoff=_cutoff(version),
        origin=version.origin,
        conditional=version.manifest["training"]["information_cutoff"]["basis"] == "declared",
    )
    refusal = _refusal(context, version)
    if refusal is not None:
        return {**member, "detail": refusal[1], "reason": refusal[0], "state": "refused"}
    try:
        built = _schedule(context, version)
    except ScheduleRefusedError as error:
        state = "refused" if error.reason.startswith("cutoff_") else "failed"
        return {**member, "detail": str(error), "reason": error.reason, "state": state}
    except SandboxUnavailableError as error:
        return {
            **member,
            "detail": str(error),
            "reason": f"sandbox_unavailable:{error.reason}",
            "state": "refused",
        }
    except (SandboxFailedError, SandboxRejectedError) as error:
        return {**member, "detail": str(error), "reason": f"sandbox_{error.reason}", "state": "failed"}
    except (ArtifactError, ScheduleError) as error:
        return {**member, "detail": str(error), "reason": "schedule_failed", "state": "failed"}
    # Whether this process or an earlier one published an artifact is not
    # part of the comparison's content, so it is not recorded.
    try:
        _publish_schedule(context.roots, built)
    except PublicationError as error:
        return {**member, "detail": str(error), "reason": "schedule_not_published", "state": "failed"}
    schedule = {"artifact_id": built.schedule_id, "counts": json.loads(built.files[MANIFEST])["counts"]}
    member["schedule"] = schedule
    try:
        schedule["reproduced"] = _schedule(context, version).files == built.files
    except (ScheduleError, SandboxUnavailableError, SandboxFailedError, SandboxRejectedError) as error:
        return {**member, "detail": str(error), "reason": "schedule_not_reproduced", "state": "failed"}
    if not schedule["reproduced"]:
        return {
            **member,
            "detail": "rebuilding the schedule from its sources and version gave different bytes",
            "reason": "schedule_not_reproduced",
            "state": "failed",
        }
    try:
        step, manifest = run_strategy(
            context.tools, context.roots, built.schedule_id, strategy_options(context.spec["configuration"])
        )
    except PipelineError as error:
        return {**member, "detail": str(error), "reason": "run_failed", "state": "failed"}
    member["run"] = {"artifact_id": step.artifact_id, "replayed": False}
    try:
        replay = inspect_strategy(context.tools, context.roots, step.artifact_id)
    except PipelineError as error:
        return {**member, "detail": str(error), "reason": "replay_failed", "state": "failed"}
    member["run"].update(replayed=True, replay=replay["reproduced"])
    member["accounts"] = {name: account_summary(manifest["results"]["accounts"][name]) for name in ACCOUNTS}
    return {**member, "state": "completed"}


# --- The comparison artifact ----------------------------------------------------------------------------


def _benchmarks(members: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    """The benchmarks every completed member ran beside; identical by
    construction, which this checks rather than assumes."""
    completed = [member for member in members if member["state"] == "completed"]
    if not completed:
        return {"unavailable": "no_member_completed"}
    shared = {name: completed[0]["accounts"][name] for name in ("buy_and_hold", "cash")}
    differing = [
        member["model_version_id"]
        for member in completed
        if {name: member["accounts"][name] for name in ("buy_and_hold", "cash")} != shared
    ]
    return {"accounts": shared, "identical_across_members": not differing, "members_differing": differing}


def _score_members(context: ComparisonContext, completed: Sequence[dict[str, Any]]) -> None:
    """Each completed member's prediction quality. Labels are read only
    here, after every member was predicted; a member that cannot be scored
    keeps its trading results and says why it has no scores."""
    try:
        labels = window_labels(context.sources, context.spec["instrument"], context.spec["window"])
    except ArtifactError as error:
        for member in completed:
            member["predictive"] = {"detail": str(error), "unavailable": "labels_unavailable"}
        return
    for member in completed:
        directory = context.roots.schedules / member["schedule"]["artifact_id"]
        try:
            verify_schedule(directory)
            member["predictive"] = score_schedule((directory / ROWS).read_bytes(), labels)
        except (OSError, ScheduleError, ScoringError) as error:
            member["predictive"] = {"detail": str(error), "unavailable": "scoring_failed"}


def comparison_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(
        canonical_json({key: value for key, value in manifest.items() if key != "comparison_id"})
    )


def build_comparison(
    context: ComparisonContext, progress: Callable[[str], None] = lambda message: None
) -> dict[str, Any]:
    names = names_by_version(context.registry)
    members: list[dict[str, Any]] = []
    for model_version_id in context.spec["model_version_ids"]:
        progress(f"member {len(members) + 1} of {len(context.spec['model_version_ids'])}: {model_version_id}")
        members.append(run_member(context, model_version_id, names.get(model_version_id, [])))
    completed = [member for member in members if member["state"] == "completed"]
    if completed:
        _score_members(context, completed)
    reference = completed[0] if completed else None
    for member in completed:
        member["deltas"] = deltas(member, None if member is reference else reference)
    window = evaluation_window(context.sources, context.spec["window"])
    manifest: dict[str, Any] = {
        "benchmarks": _benchmarks(members),
        "environment": environment(),
        "fairness": list(FAIRNESS),
        "members": members,
        "reference_model_version_id": None if reference is None else reference["model_version_id"],
        "schema": SCHEMA,
        "scoring": list(SCORING),
        "sources": source_json(context.sources),
        "spec": dict(context.spec),
        "spec_sha256": spec_digest(context.spec),
        "status": {
            state: str(sum(member["state"] == state for member in members))
            for state in ("completed", "failed", "refused")
        },
        "window": window.as_json(),
    }
    return {**manifest, "comparison_id": comparison_identity(manifest)}


def comparisons_root(artifacts: Path) -> Path:
    return artifacts / "comparisons"


def verified_comparison(root: Path, comparison_id: str) -> dict[str, Any]:
    """The comparison, its identity and schema verified from bytes read once."""
    try:
        directory = artifact_directory(root, comparison_id)
        if listed_files(directory) != [MANIFEST]:
            raise ComparisonError("a comparison holds exactly manifest.json")
        manifest = verified_manifest(directory, "comparison_id", with_newline=True)
    except ArtifactUnavailableError as error:
        raise ComparisonError(str(error)) from error
    if manifest.get("schema") != SCHEMA or comparison_identity(manifest) != comparison_id:
        raise ComparisonError("not a comparison of a supported version, or its ID does not match its content")
    members = manifest.get("members")
    if not isinstance(members, list) or any(
        not isinstance(member, dict) or (member.get("state") == "completed" and "predictive" not in member)
        for member in members
    ):
        raise ComparisonError("every completed member of a comparison has its prediction quality")
    return dict(manifest)


def comparison_ids(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    return sorted(name for name in (entry.name for entry in root.iterdir()) if is_artifact_id(name))


def publish_comparison(root: Path, manifest: Mapping[str, Any]) -> str:
    root.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verified_comparison(root, directory.name)
        return (directory / MANIFEST).read_bytes()

    try:
        outcome, _ = publish(
            root, str(manifest["comparison_id"]), {MANIFEST: canonical_json(manifest)}, MANIFEST, existing
        )
    except PublicationError as error:
        raise ComparisonError(f"the comparison could not be published: {error}") from error
    return outcome.value


def compare(
    tools: Tools,
    artifacts: Path,
    spec: Mapping[str, Any],
    sandbox: Sandbox,
    progress: Callable[[str], None] = lambda message: None,
) -> dict[str, Any]:
    """Runs and publishes a comparison; its members' failures are recorded
    in it, and only a comparison that cannot be built at all raises."""
    checked = validated_spec(dict(spec))
    roots = ArtifactRoots.under(artifacts)
    try:
        sources = verified_sources(tools, roots, checked["split_artifact_id"], reproduce_features=True)
    except (ArtifactError, PipelineError) as error:
        raise ComparisonError(f"the split and its sources do not verify: {error}") from error
    context = ComparisonContext(tools, roots, RegistryRoots.under(artifacts), sources, checked, sandbox)
    manifest = build_comparison(context, progress)
    outcome = publish_comparison(comparisons_root(artifacts), manifest)
    return {"comparison": manifest, "outcome": outcome}


def listing(manifest: Mapping[str, Any]) -> dict[str, Any]:
    """The part of a comparison a list shows."""
    members = [
        {key: member[key] for key in ("model_version_id", "names", "state") if key in member}
        for member in manifest["members"]
    ]
    return {
        "comparison_id": manifest["comparison_id"],
        "instrument": manifest["spec"]["instrument"],
        "members": members,
        "status": manifest["status"],
        "window": manifest["window"],
    }


def _artifact_state[Argument](check: Callable[[Argument], object], argument: Argument) -> str:
    try:
        check(argument)
    except (ArtifactUnavailableError, ArtifactError, ScheduleError, OSError, ValueError, KeyError) as error:
        return f"invalid: {error}"
    return "verified"


def member_artifacts(roots: ArtifactRoots, manifest: Mapping[str, Any]) -> list[dict[str, Any]]:
    """Whether each member's schedule and run still verify, read again now:
    the schedule's every row and checksum, and the run manifest's identity
    and journal checksum. Replaying a run is the CLI's inspect-comparison."""

    def run(run_id: str) -> None:
        directory = artifact_directory(roots.strategies, run_id)
        run_manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        verified_file(directory, run_manifest["files"]["journal"])

    checks = []
    for member in manifest["members"]:
        entry: dict[str, Any] = {"model_version_id": member["model_version_id"]}
        if "schedule" in member:
            schedule_id = member["schedule"]["artifact_id"]
            entry["schedule"] = _artifact_state(verify_schedule, roots.schedules / schedule_id)
        if "run" in member:
            run_id = member["run"]["artifact_id"]
            entry["run"] = _artifact_state(run, run_id)
        checks.append(entry)
    return checks
