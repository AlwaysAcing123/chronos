"""`python -m chronos_research evaluate|inspect`.

    evaluate  verifies a split artifact and its ancestry with chronos_dataset,
              trains and evaluates both baselines, and publishes the experiment
    inspect   verifies a published experiment without retraining

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
from chronos_research.experiment import (
    MANIFEST,
    SCOPE,
    ExperimentConfiguration,
    build_experiment,
    load_model_data,
)
from chronos_research.inspection import InspectionError, verify_experiment
from chronos_research.publication import PublicationError, publish


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
    inspect = commands.add_parser("inspect", allow_abbrev=False, help="verify a published experiment")
    inspect.add_argument("--output", required=True, type=_path, action=_Once, help="the experiment root")
    inspect.add_argument(
        "--experiment", required=True, type=_artifact_id, action=_Once, help="the experiment's ID"
    )
    return parser


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


def _verified_manifest(directory: Path) -> bytes:
    verify_experiment(directory)
    return (directory / MANIFEST).read_bytes()


def _evaluate(arguments: argparse.Namespace) -> dict[str, Any]:
    sources = verify_sources(
        arguments.chronos_dataset,
        features_root=arguments.features,
        labels_root=arguments.labels,
        splits_root=arguments.splits,
        split_artifact_id=arguments.split_artifact,
        catalog=arguments.catalog,
    )
    experiment = build_experiment(sources, load_model_data(sources), ExperimentConfiguration())
    output: Path = arguments.output
    if not output.is_dir():
        output.mkdir()
    outcome, warning = publish(
        output,
        experiment.experiment_id,
        experiment.files,
        MANIFEST,
        _verified_manifest,
    )
    metrics = json.loads(experiment.files["metrics.json"])
    return {
        "directory": str(output / experiment.experiment_id),
        "experiment_id": experiment.experiment_id,
        "headline": _headline(metrics["splits"]),
        "outcome": outcome.value,
        "scope": SCOPE,
        "sources": json.loads(experiment.files[MANIFEST])["sources"],
        "warning": warning,
    }


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
    try:
        result = _evaluate(arguments) if arguments.command == "evaluate" else _inspect(arguments)
    except (ArtifactError, InspectionError, PublicationError, OSError) as error:
        return _fail(type(error).__name__, str(error))
    except Exception as error:  # Still one JSON document, as the C++ tools give.
        return _fail("internal_error", f"{type(error).__name__}: {error}")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0
