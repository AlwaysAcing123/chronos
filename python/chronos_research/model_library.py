"""What the model library shows: listings and details of registered model
versions, for the CLI and the research service. Everything here is read
from versions that verified; nothing is defaulted or invented. A missing
fact keeps its reason.
"""

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from chronos_research.registry import ModelVersion, RegistryRoots, lifecycle, source_experiment_status

# What each family can do today, kept here so a detail never claims more.
EVALUATION_REQUIRES = (
    "a verified split of a stream with {instrument} feature rows; the information cutoff must be at or "
    "before the evaluation window's start"
)
DEPLOYMENT_REQUIRES = (
    "a paper-trading server started with --model-deployments that trades {instrument}, and for an imported "
    "model --onnx-docker; the deployment decides only on market data received at or after the information "
    "cutoff, and only after an explicit Start"
)
FAMILY_LIMITATIONS = {
    "majority": (
        "The majority model predicts its training class frequencies for every row; it ignores its inputs."
    ),
    "logistic": (
        "The logistic model is linear in five top-of-book inputs, fitted on one instrument's training rows."
    ),
    "onnx": (
        "An imported model's training, data, target, and cutoff are its uploader's declarations. Chronos "
        "verified the file, its contract, and its outputs on a fixed smoke corpus, not how it was made."
    ),
}
ORIGIN_LIMITATIONS = {
    "experiment": (
        "It was trained on one short recorded session: its training window covers minutes, not market "
        "regimes.",
    ),
    "import": (
        "Any result from it is conditional on the declared cutoff being true; a cutoff Chronos cannot "
        "check cannot rule out look-ahead.",
    ),
}
LIMITATIONS = (
    "Its predictions classify one-horizon midpoint moves; they are not trading signals or evidence of "
    "profit.",
)


def summary(roots: RegistryRoots, version: ModelVersion, names: list[dict[str, str]]) -> dict[str, Any]:
    manifest = version.manifest
    return {
        "family": manifest["family"],
        "format": manifest["format"],
        "inputs": manifest["inputs"],
        "instrument": manifest["instrument"],
        "lifecycle": lifecycle(roots, version.model_version_id),
        "model_version_id": version.model_version_id,
        "names": names,
        "output": manifest["output"],
        "payload": manifest["payload"],
        "provenance": manifest["provenance"],
        "runtime": manifest["runtime"],
        "target": manifest["target"],
        "training": manifest["training"],
        "verification": manifest["verification"],
    }


def _deployment(version: ModelVersion, state: str) -> dict[str, Any]:
    """Whether a new paper deployment may serve the version: it must be
    active, with an information cutoff to tell old data from new."""
    if state != "active":
        return {
            "detail": "an archived version cannot be chosen for a new deployment",
            "reason": "archived",
            "status": "unavailable",
        }
    if version.information_cutoff_ns is None:
        return {
            "detail": "without an information cutoff no deployment could tell data it may decide on",
            "reason": "cutoff_unavailable",
            "status": "unavailable",
        }
    return {"requires": DEPLOYMENT_REQUIRES.format(instrument=version.instrument), "status": "compatible"}


def _evaluation(version: ModelVersion, sandbox: Mapping[str, Any], state: str) -> dict[str, Any]:
    """Whether a new comparison may choose the version, by the rules its
    runner applies: active, with an information cutoff, and for an import
    the sandbox. Whether the cutoff precedes a window is checked for that
    window when the comparison runs."""
    if state != "active":
        return {
            "detail": "an archived version cannot be chosen for a new comparison",
            "reason": "archived",
            "status": "unavailable",
        }
    if version.information_cutoff_ns is None:
        return {
            "detail": "without an information cutoff no comparison window can be shown to follow it",
            "reason": "cutoff_unavailable",
            "status": "unavailable",
        }
    requires = EVALUATION_REQUIRES.format(instrument=version.instrument)
    if version.family == "onnx" and sandbox["status"] != "available":
        return {
            "detail": sandbox["detail"],
            "reason": f"sandbox_{sandbox['reason']}",
            "requires": requires + "; and the ONNX sandbox",
            "status": "unavailable",
        }
    return {"requires": requires, "status": "compatible"}


def _compatibility(version: ModelVersion, sandbox: Mapping[str, Any], lifecycle_state: str) -> dict[str, Any]:
    """What the version can be used for here, and why not when it cannot."""
    return {
        "deployment": _deployment(version, lifecycle_state),
        "evaluation": _evaluation(version, sandbox, lifecycle_state),
    }


def _source_check(experiments: Path, version: ModelVersion) -> dict[str, str]:
    if version.origin == "import":
        return {"reason": "imported_model_has_no_source_experiment", "state": "not_applicable"}
    return source_experiment_status(experiments, version)


def detail(
    roots: RegistryRoots,
    experiments: Path,
    version: ModelVersion,
    names: list[dict[str, str]],
    sandbox: Mapping[str, Any],
) -> dict[str, Any]:
    """A summary with what it is compatible with, its limitations, and a
    fresh check of the experiment it came from, if any."""
    return {
        **summary(roots, version, names),
        "compatibility": _compatibility(
            version, sandbox, lifecycle(roots, version.model_version_id)["state"]
        ),
        "limitations": [
            FAMILY_LIMITATIONS[version.family],
            *ORIGIN_LIMITATIONS[version.origin],
            *LIMITATIONS,
        ],
        "source_check": _source_check(experiments, version),
    }


def listing_entry(entry: Mapping[str, Any]) -> dict[str, Any]:
    """The part of a summary a library list shows."""
    return {
        key: entry[key]
        for key in ("family", "instrument", "lifecycle", "model_version_id", "names", "payload", "training")
    } | {"origin": entry["provenance"]["origin"]}
