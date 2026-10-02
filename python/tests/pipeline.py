"""Builds the C++ artifacts a test evaluates: synthetic events to a dataset,
then features, labels, and a split, each with chronos_dataset."""

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Tools:
    chronos_dataset: Path
    synthetic_dataset: Path


@dataclass(frozen=True)
class Pipeline:
    tools: Tools
    catalog: Path
    features: Path
    labels: Path
    splits: Path
    dataset_id: str
    feature_id: str
    label_id: str
    split_id: str


def run_json(*command: str | Path) -> dict[str, Any]:
    completed = subprocess.run([str(part) for part in command], capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise AssertionError(f"{command[1:3]} failed: {completed.stderr}")
    result: dict[str, Any] = json.loads(completed.stdout)
    return result


def build_pipeline(
    root: Path,
    tools: Tools,
    events: str,
    *,
    horizon_ns: int = 150_000,
    percentages: tuple[int, int, int] = (60, 20, 20),
) -> Pipeline:
    catalog, features, labels, splits = (root / name for name in ("catalog", "features", "labels", "splits"))
    catalog.mkdir()
    (root / "events.csv").write_text(events)
    dataset_id = run_json(tools.synthetic_dataset, root / "events.csv", catalog, "synthetic-stream")[
        "dataset_id"
    ]
    tool = tools.chronos_dataset
    feature_id = run_json(
        tool, "features", "extract", "--catalog", catalog, "--dataset", dataset_id, "--output", features
    )["artifact"]["artifact_id"]
    label_id = run_json(
        tool,
        "labels",
        "extract",
        "--features",
        features,
        "--feature-artifact",
        feature_id,
        "--output",
        labels,
        "--horizon-ns",
        str(horizon_ns),
    )["artifact"]["artifact_id"]
    train, validation, test = percentages
    split_id = run_json(
        tool,
        "splits",
        "create",
        "--labels",
        labels,
        "--label-artifact",
        label_id,
        "--output",
        splits,
        "--train-percent",
        str(train),
        "--validation-percent",
        str(validation),
        "--test-percent",
        str(test),
    )["artifact"]["artifact_id"]
    return Pipeline(
        tools=tools,
        catalog=catalog,
        features=features,
        labels=labels,
        splits=splits,
        dataset_id=dataset_id,
        feature_id=feature_id,
        label_id=label_id,
        split_id=split_id,
    )
