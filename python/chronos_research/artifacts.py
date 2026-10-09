"""Verified access to Chronos feature, label, and split artifacts.

The C++ `chronos_dataset` tool owns these formats. Python never decides
whether an artifact is valid: it runs `<kind> inspect` for the split, its
label artifact, and that label artifact's feature artifact, with
reproduction from each source where a root is given, and refuses to go on
unless every one verifies. It then reads the rows files itself while
hashing every byte, and uses rows only if those bytes are exactly the ones
the verifier checked, so an artifact cannot change between verification
and use.
"""

import hashlib
import json
import subprocess
from collections.abc import Callable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NamedTuple

FEATURE_SET = "top_of_book_v1"
LABEL_SET = "future_midpoint_return_v1"
SPLIT_SET = "chronological_purged_v1"
CLASS_ORDER = ("down", "flat", "up")
SPLITS = ("train", "validation", "test")

_ARTIFACT_ID_LENGTH = 64


class ArtifactError(Exception):
    """An artifact is missing, invalid, or inconsistent with its sources."""


class RowIdentity(NamedTuple):
    """The feature row a label or split row belongs to."""

    stream: str
    instrument: str
    ingest_sequence: int


@dataclass(frozen=True, slots=True)
class Unavailable:
    reason: str


type FeatureValue = int | Unavailable


@dataclass(frozen=True, slots=True)
class FeatureRow:
    identity: RowIdentity
    available_at_ns: int
    values: Mapping[str, FeatureValue]


@dataclass(frozen=True, slots=True)
class LabelRow:
    identity: RowIdentity
    prediction_at_ns: int
    label_available_at_ns: int | None
    # Both set when the label has a value; otherwise unavailable_reason.
    direction: str | None
    return_raw: int | None
    unavailable_reason: str | None


@dataclass(frozen=True, slots=True)
class SplitRow:
    identity: RowIdentity
    prediction_at_ns: int
    label_available_at_ns: int | None
    label_usable: bool
    period: str
    split: str
    exclusion: str | None


@dataclass(frozen=True)
class VerifiedArtifact:
    artifact_id: str
    rows_path: Path
    rows_sha256: str
    rows: int
    # The verifier's own summary of the artifact.
    summary: Mapping[str, Any]
    reproduced: str


@dataclass(frozen=True)
class VerifiedSources:
    dataset_id: str
    stream: str
    horizon_ns: int
    features: VerifiedArtifact
    labels: VerifiedArtifact
    splits: VerifiedArtifact
    # The first prediction time of validation and of test.
    validation_start_at_ns: int
    test_start_at_ns: int


def is_artifact_id(text: str) -> bool:
    return len(text) == _ARTIFACT_ID_LENGTH and all(c in "0123456789abcdef" for c in text)


def _inspect(tool: Path, arguments: Sequence[str], key: str = "artifact") -> Mapping[str, Any]:
    try:
        completed = subprocess.run(
            [str(tool), *arguments], capture_output=True, text=True, check=False, timeout=3600
        )
    except OSError as error:
        raise ArtifactError(f"cannot run {tool}: {error.strerror}") from error
    except subprocess.TimeoutExpired as error:
        raise ArtifactError(
            f"`chronos_dataset {' '.join(arguments[:2])}` did not finish in an hour"
        ) from error
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise ArtifactError(f"`chronos_dataset {' '.join(arguments[:2])}` refused the artifact: {detail}")
    try:
        document = json.loads(completed.stdout)
    except ValueError as error:
        raise ArtifactError("the verifier's output is not JSON") from error
    if not isinstance(document, dict) or not isinstance(document.get(key), dict):
        raise ArtifactError("the verifier's output has an unexpected shape")
    return document


def _verified(document: Mapping[str, Any], root: Path, rows_file: str, sha256_key: str) -> VerifiedArtifact:
    artifact = document["artifact"]
    artifact_id = artifact["artifact_id"]
    return VerifiedArtifact(
        artifact_id=artifact_id,
        rows_path=root / artifact_id / rows_file,
        rows_sha256=sha256_key,
        rows=int(artifact["rows"] if "rows" in artifact else artifact["counts"]["rows"]),
        summary=artifact,
        reproduced=str(document["reproduced"]),
    )


def verify_sources(
    tool: Path,
    *,
    features_root: Path,
    labels_root: Path,
    splits_root: Path,
    split_artifact_id: str,
    catalog: Path | None = None,
) -> VerifiedSources:
    """Verifies a split artifact and its ancestry with the C++ verifier.

    The split and label artifacts are always reproduced from their sources;
    the feature artifact is reproduced from its dataset when `catalog` is
    given.
    """
    try:
        return _verify_sources(tool, features_root, labels_root, splits_root, split_artifact_id, catalog)
    except (KeyError, TypeError, ValueError) as error:
        raise ArtifactError(
            f"the verifier's output has an unexpected shape ({type(error).__name__}: {error})"
        ) from error


def _verify_sources(
    tool: Path,
    features_root: Path,
    labels_root: Path,
    splits_root: Path,
    split_artifact_id: str,
    catalog: Path | None,
) -> VerifiedSources:
    if not is_artifact_id(split_artifact_id):
        raise ArtifactError("a split artifact ID is 64 lowercase hexadecimal digits")
    split_document = _inspect(
        tool,
        [
            "splits",
            "inspect",
            "--output",
            str(splits_root),
            "--artifact",
            split_artifact_id,
            "--labels",
            str(labels_root),
        ],
    )
    split = split_document["artifact"]
    if split["split_set"] != {"name": "chronological_purged", "version": "1"}:
        raise ArtifactError(f"unsupported split set {split['split_set']}")
    source = split["source"]
    label_document, feature_document = _verify_labels_and_features(
        tool, features_root, labels_root, source["label_artifact_id"], catalog
    )
    labels = label_document["artifact"]
    features = feature_document["artifact"]
    ancestry = {
        "split -> label artifact": (source["label_artifact_id"], labels["artifact_id"]),
        "split -> labels checksum": (source["labels_sha256"], labels["labels_sha256"]),
        "split -> feature artifact": (source["feature_artifact_id"], features["artifact_id"]),
        "split -> dataset": (source["dataset_id"], features["source"]["dataset_id"]),
        "split -> horizon": (source["horizon_ns"], labels["horizon_ns"]),
        "stream": (source["stream"], features["source"]["stream"]),
        "rows": (split["counts"]["rows"], labels["rows"]),
    }
    _require_ancestry(ancestry)
    if not str(split_document["reproduced"]).startswith("recomputed"):
        raise ArtifactError("the split artifact was not reproduced from its source")

    return VerifiedSources(
        dataset_id=features["source"]["dataset_id"],
        stream=features["source"]["stream"],
        horizon_ns=int(labels["horizon_ns"]),
        features=_verified(feature_document, features_root, "features.ndjson", features["features_sha256"]),
        labels=_verified(label_document, labels_root, "labels.ndjson", labels["labels_sha256"]),
        splits=_verified(split_document, splits_root, "splits.ndjson", split["files"]["splits"]["sha256"]),
        validation_start_at_ns=int(split["boundaries"]["validation_start"]["prediction_at_ns"]),
        test_start_at_ns=int(split["boundaries"]["test_start"]["prediction_at_ns"]),
    )


def _require_ancestry(ancestry: Mapping[str, tuple[Any, Any]]) -> None:
    for relation, (claimed, actual) in ancestry.items():
        if claimed != actual:
            raise ArtifactError(f"artifact ancestry does not match ({relation}): {claimed} != {actual}")


def _verify_labels_and_features(
    tool: Path, features_root: Path, labels_root: Path, label_artifact_id: str, catalog: Path | None
) -> tuple[Mapping[str, Any], Mapping[str, Any]]:
    """A label artifact reproduced from its feature artifact, and that
    feature artifact, reproduced from its dataset when `catalog` is given,
    with their ancestry cross-checked: the verifier documents of both."""
    label_document = _inspect(
        tool,
        [
            "labels",
            "inspect",
            "--output",
            str(labels_root),
            "--artifact",
            label_artifact_id,
            "--features",
            str(features_root),
        ],
    )
    labels = label_document["artifact"]
    feature_arguments = [
        "features",
        "inspect",
        "--output",
        str(features_root),
        "--artifact",
        labels["source"]["feature_artifact_id"],
    ]
    if catalog is not None:
        feature_arguments += ["--catalog", str(catalog)]
    feature_document = _inspect(tool, feature_arguments)
    features = feature_document["artifact"]
    if labels["label_set"] != LABEL_SET or features["feature_set"] != FEATURE_SET:
        raise ArtifactError(
            f"unsupported label set {labels['label_set']} or feature set {features['feature_set']}"
        )
    _require_ancestry(
        {
            "label -> features checksum": (labels["source"]["features_sha256"], features["features_sha256"]),
            "label -> dataset": (labels["source"]["dataset_id"], features["source"]["dataset_id"]),
            "label stream": (labels["source"]["stream"], features["source"]["stream"]),
            "label rows": (labels["rows"], features["rows"]),
        }
    )
    if not str(label_document["reproduced"]).startswith("recomputed"):
        raise ArtifactError("the label artifact was not reproduced from its source")
    return label_document, feature_document


@dataclass(frozen=True)
class VerifiedRecording:
    """One recording's dataset, feature artifact, and label artifact, each
    verified completely by the C++ tool and reproduced from its source."""

    dataset_id: str
    stream: str
    horizon_ns: int
    features: VerifiedArtifact
    labels: VerifiedArtifact
    # The verifier's dataset summary and the dataset's manifest.
    dataset: Mapping[str, Any]
    dataset_manifest: Mapping[str, Any]


def verify_recording(
    tool: Path, *, catalog: Path, features_root: Path, labels_root: Path, label_artifact_id: str
) -> VerifiedRecording:
    """Verifies a label artifact, its feature artifact, and its dataset, each
    from its source, and cross-checks the whole ancestry."""
    if not is_artifact_id(label_artifact_id):
        raise ArtifactError("a label artifact ID is 64 lowercase hexadecimal digits")
    try:
        label_document, feature_document = _verify_labels_and_features(
            tool, features_root, labels_root, label_artifact_id, catalog
        )
        labels = label_document["artifact"]
        features = feature_document["artifact"]
        if not str(feature_document["reproduced"]).startswith("recomputed"):
            raise ArtifactError("the feature artifact was not reproduced from its dataset")
        dataset_document = _inspect(
            tool,
            ["inspect", "--catalog", str(catalog), "--dataset", features["source"]["dataset_id"]],
            key="dataset",
        )
        dataset = dataset_document["dataset"]
        manifest = dataset_document["manifest"]
        _require_ancestry(
            {
                "features -> dataset": (features["source"]["dataset_id"], dataset["dataset_id"]),
                "features -> events checksum": (
                    features["source"]["events_sha256"],
                    manifest["files"]["events"]["sha256"],
                ),
                "features -> stream": (features["source"]["stream"], dataset["stream"]),
                "dataset manifest": (manifest["dataset_id"], dataset["dataset_id"]),
            }
        )
        return VerifiedRecording(
            dataset_id=dataset["dataset_id"],
            stream=dataset["stream"],
            horizon_ns=int(labels["horizon_ns"]),
            features=_verified(
                feature_document, features_root, "features.ndjson", features["features_sha256"]
            ),
            labels=_verified(label_document, labels_root, "labels.ndjson", labels["labels_sha256"]),
            dataset=dataset,
            dataset_manifest=manifest,
        )
    except (KeyError, TypeError, ValueError) as error:
        raise ArtifactError(
            f"the verifier's output has an unexpected shape ({type(error).__name__}: {error})"
        ) from error


def _records(artifact: VerifiedArtifact) -> Iterator[dict[str, Any]]:
    """Each record of a rows file, hashed as read. Raises once the file ends
    unless its bytes and row count are exactly the verified ones; a consumer
    must therefore exhaust the iterator before using what it read."""
    digest = hashlib.sha256()
    count = 0
    with artifact.rows_path.open("rb") as file:
        for line in file:
            digest.update(line)
            count += 1
            yield json.loads(line)
    if digest.hexdigest() != artifact.rows_sha256 or count != artifact.rows:
        raise ArtifactError(f"{artifact.rows_path.name} changed after it was verified")


def _read[Row](artifact: VerifiedArtifact, parse: Callable[[dict[str, Any]], Row]) -> list[Row]:
    try:
        return [parse(record) for record in _records(artifact)]
    except (KeyError, TypeError, ValueError) as error:
        raise ArtifactError(f"{artifact.rows_path.name}: unexpected row shape: {error}") from error


def _identity(record: Mapping[str, Any]) -> RowIdentity:
    return RowIdentity(record["stream"], record["instrument"], int(record["ingest_sequence"]))


def _require(record: Mapping[str, Any], key: str, expected: str) -> None:
    if record[key] != expected:
        raise ValueError(f"{key} is {record[key]!r}, not {expected!r}")


def _optional_integer(value: str | None) -> int | None:
    return None if value is None else int(value)


def _feature_value(value: Mapping[str, str]) -> FeatureValue:
    if "raw" in value:
        return int(value["raw"])
    return Unavailable(value["unavailable"])


def _feature_row(record: dict[str, Any]) -> FeatureRow:
    _require(record, "feature_set", "top_of_book")
    _require(record, "version", "1")
    return FeatureRow(
        identity=_identity(record),
        available_at_ns=int(record["available_at_ns"]),
        values={name: _feature_value(value) for name, value in record["features"].items()},
    )


def _label_row(record: dict[str, Any]) -> LabelRow:
    _require(record, "label_set", "future_midpoint_return")
    _require(record, "version", "1")
    label = record["label"]
    return LabelRow(
        identity=_identity(record),
        prediction_at_ns=int(record["prediction_at_ns"]),
        label_available_at_ns=_optional_integer(record["label_available_at_ns"]),
        direction=label.get("direction"),
        return_raw=_optional_integer(label.get("return_raw")),
        unavailable_reason=label.get("unavailable"),
    )


def _split_row(record: dict[str, Any]) -> SplitRow:
    _require(record, "split_set", "chronological_purged")
    _require(record, "version", "1")
    return SplitRow(
        identity=_identity(record),
        prediction_at_ns=int(record["prediction_at_ns"]),
        label_available_at_ns=_optional_integer(record["label_available_at_ns"]),
        label_usable=bool(record["label_usable"]),
        period=record["period"],
        split=record["split"],
        exclusion=record["exclusion"],
    )


def read_feature_rows(artifact: VerifiedArtifact) -> list[FeatureRow]:
    return _read(artifact, _feature_row)


def read_label_rows(artifact: VerifiedArtifact) -> list[LabelRow]:
    return _read(artifact, _label_row)


def read_split_rows(artifact: VerifiedArtifact) -> list[SplitRow]:
    return _read(artifact, _split_row)
