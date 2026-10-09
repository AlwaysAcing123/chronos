"""A verified research collection: independent recordings for one walk-forward
study, referenced by their existing artifacts (ADR 0022).

    <artifacts>/collections/<collection_id>/manifest.json

A collection names, for each member recording, its label artifact, the
feature artifact that label artifact was made from, and that feature
artifact's dataset, with their IDs and checksums. It concatenates nothing,
renumbers nothing, and changes no artifact: a row keeps its identity
(stream, instrument, ingest sequence), so equal sequence numbers in two
recordings never meet.

Every member is verified completely by the C++ tool (artifacts.verify_recording),
and its feature and label rows are then read while hashing, so the counts and
time coverage recorded here come from exactly the verified bytes. Members are
ordered by their first receive time and must be strictly disjoint in time.

What the evidence is, is recorded and never upgraded: `synthetic` for the
test generator's datasets, `observed` for a provider recording normalized by
a known converter. A session or date label is the creator's declaration.
Chronos has no exchange calendar, so no member is called a complete session:
each is `partial`, with its actual duration.

The collection ID is the SHA-256 of the canonical manifest without the ID,
with its newline.
"""

import itertools
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research.artifact_files import ArtifactUnavailableError, artifact_directory, listed_files
from chronos_research.artifact_files import verified_manifest as verified_manifest_file
from chronos_research.artifacts import (
    FEATURE_SET,
    LABEL_SET,
    ArtifactError,
    LabelRow,
    VerifiedRecording,
    is_artifact_id,
    read_feature_rows,
    read_label_rows,
    verify_recording,
)
from chronos_research.encoding import canonical_json, encode_integer, sha256_hex
from chronos_research.pipeline import CHRONOS_DATASET, ArtifactRoots, Tools
from chronos_research.publication import PublicationError, publish

SCHEMA = {"name": "chronos.research_collection", "version": "1"}
MANIFEST = "manifest.json"
MAX_SOURCES = 12
MAX_FEATURE_ROWS_PER_SOURCE = 2_000_000
MAX_SESSION_LABEL = 120
SYNTHETIC_PROVIDER = "synthetic"
_PLACEHOLDER_RECORDING = "0" * 64
DEFINITIONS = {
    "completeness": (
        "partial: Chronos has no exchange calendar, so no recording is called a complete session; "
        "each member states its actual receive-time coverage"
    ),
    "coverage": "receive-time range of the dataset's events; first and last available_at per instrument",
    "evidence": (
        "synthetic: the test generator's events; observed: a provider recording normalized by a known "
        "converter; neither claims more than its events show"
    ),
    "identity": "rows are identified by stream, instrument, and ingest sequence; nothing is renumbered",
    "order": "members by first receive time, strictly disjoint: a member ends before the next begins",
    "session": "a session or date label is the creator's declaration, not proof of a session",
}
VERIFICATION = (
    "each label artifact reproduced from its feature artifact by chronos_dataset labels inspect --features",
    "each feature artifact reproduced from its dataset by chronos_dataset features inspect --catalog",
    "each dataset's every event and quality report verified by chronos_dataset inspect",
    "the label, feature, and dataset IDs, checksums, stream, and row counts cross-checked",
    "the counts and coverage here computed from feature and label rows read while hashing, used only if "
    "they are exactly the verified bytes",
)


class CollectionError(Exception):
    """A collection cannot be built from these sources, or is not valid."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def collections_root(artifacts: Path) -> Path:
    return artifacts / "collections"


@dataclass(frozen=True)
class Member:
    """One verified recording and what its verified rows show."""

    recording: VerifiedRecording
    document: dict[str, Any]

    @property
    def first_receive_ns(self) -> int:
        return int(self.recording.dataset["time_range"]["receive_timestamp_ns"]["earliest"])

    @property
    def last_receive_ns(self) -> int:
        return int(self.recording.dataset["time_range"]["receive_timestamp_ns"]["latest"])


def _instruments(recording: VerifiedRecording) -> dict[str, Any]:
    """Per instrument: feature rows, usable labels, and the first and last
    available_at, from rows read while hashing."""
    features = read_feature_rows(recording.features)
    labels: list[LabelRow] = read_label_rows(recording.labels)
    if len(features) > MAX_FEATURE_ROWS_PER_SOURCE:
        raise CollectionError(
            "source_too_large",
            f"dataset {recording.dataset_id} has {len(features)} feature rows; at most "
            f"{MAX_FEATURE_ROWS_PER_SOURCE} per source are supported",
        )
    instruments: dict[str, dict[str, int]] = {}
    for row in features:
        entry = instruments.setdefault(
            row.identity.instrument,
            {"feature_rows": 0, "first": row.available_at_ns, "last": row.available_at_ns, "usable": 0},
        )
        entry["feature_rows"] += 1
        entry["first"] = min(entry["first"], row.available_at_ns)
        entry["last"] = max(entry["last"], row.available_at_ns)
    for label in labels:
        if label.direction is not None:
            instruments[label.identity.instrument]["usable"] += 1
    return {
        instrument: {
            "feature_rows": encode_integer(entry["feature_rows"]),
            "first_available_at_ns": encode_integer(entry["first"]),
            "last_available_at_ns": encode_integer(entry["last"]),
            "usable_labels": encode_integer(entry["usable"]),
        }
        for instrument, entry in sorted(instruments.items())
    }


def _provenance(manifest: Mapping[str, Any]) -> dict[str, Any]:
    source = manifest["source"]
    synthetic = source["provider"] == SYNTHETIC_PROVIDER
    recording = source["recording_sha256"]
    coverage = source["coverage"]
    return {
        "description": source["description"],
        "evidence": "synthetic" if synthetic else "observed",
        "normalizer": dict(source["normalizer"]),
        "provider": source["provider"],
        "provider_dataset": source["provider_dataset"],
        "provider_schema": source["provider_schema"],
        "query_coverage": dict(coverage) if coverage is not None else {"unavailable": "not_recorded"},
        "recording_sha256": (
            {"unavailable": "synthetic_placeholder"}
            if synthetic and recording == _PLACEHOLDER_RECORDING
            else recording
        ),
    }


def _member(recording: VerifiedRecording, session_label: str | None) -> Member:
    dataset = recording.dataset
    receive = dataset["time_range"]["receive_timestamp_ns"]
    first, last = int(receive["earliest"]), int(receive["latest"])
    document = {
        "coverage": {"completeness": "partial", "duration_ns": encode_integer(last - first)},
        "dataset": {
            "dataset_id": recording.dataset_id,
            "event_time_ns": dict(dataset["time_range"]["event_timestamp_ns"]),
            "events": str(recording.dataset_manifest["counts"]["events"]),
            "events_sha256": recording.dataset_manifest["files"]["events"]["sha256"],
            "receive_time_ns": dict(receive),
            "stream": recording.stream,
        },
        "features": {
            "artifact_id": recording.features.artifact_id,
            "features_sha256": recording.features.rows_sha256,
            "rows": encode_integer(recording.features.rows),
        },
        "instruments": _instruments(recording),
        "labels": {
            "artifact_id": recording.labels.artifact_id,
            "horizon_ns": encode_integer(recording.horizon_ns),
            "labels_sha256": recording.labels.rows_sha256,
            "rows": encode_integer(recording.labels.rows),
        },
        "provenance": _provenance(recording.dataset_manifest),
        "session": (
            {"basis": "declared", "label": session_label}
            if session_label is not None
            else {"unavailable": "not_declared"}
        ),
    }
    return Member(recording=recording, document=document)


def _require_distinct(members: Sequence[Member]) -> None:
    """No two members are one dataset, stream, or provider recording."""
    reasons = {"dataset": "duplicate_dataset", "stream": "duplicate_stream", "recording": "aliased_recording"}
    seen: dict[str, dict[str, str]] = {kind: {} for kind in reasons}
    for member in members:
        recording = member.recording
        keys = {
            "dataset": recording.dataset_id,
            "stream": recording.stream,
            # The synthetic placeholder is not a checksum, so it is not compared.
            "recording": member.document["provenance"]["recording_sha256"],
        }
        for kind, key in keys.items():
            if not isinstance(key, str):
                continue
            if key in seen[kind]:
                raise CollectionError(
                    reasons[kind],
                    f"label artifacts {seen[kind][key]} and {recording.labels.artifact_id} share one {kind}",
                )
            seen[kind][key] = recording.labels.artifact_id


def _require_consistent(members: Sequence[Member]) -> dict[str, Any]:
    """The shared contract and evidence, or the reason the members cannot
    form one collection."""
    first = members[0]
    contract = {
        "feature_set": FEATURE_SET,
        "horizon_ns": encode_integer(first.recording.horizon_ns),
        "label_set": LABEL_SET,
        "scales": dict(first.recording.dataset_manifest["scales"]),
    }
    evidence = {member.document["provenance"]["evidence"] for member in members}
    if len(evidence) > 1:
        raise CollectionError(
            "unsupported_mixture", "a collection holds synthetic or observed recordings, not both"
        )
    _require_distinct(members)
    for member in members:
        recording = member.recording
        if (
            recording.horizon_ns != first.recording.horizon_ns
            or dict(recording.dataset_manifest["scales"]) != contract["scales"]
        ):
            raise CollectionError(
                "inconsistent_contract",
                f"dataset {recording.dataset_id} has another horizon or scale than "
                f"{first.recording.dataset_id}",
            )
    for earlier, later in itertools.pairwise(members):
        if earlier.last_receive_ns >= later.first_receive_ns:
            raise CollectionError(
                "overlapping_sources",
                f"dataset {earlier.recording.dataset_id} ends at {earlier.last_receive_ns}, not before "
                f"dataset {later.recording.dataset_id} begins at {later.first_receive_ns}",
            )
    return {**contract, "evidence": evidence.pop()}


def collection_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(
        canonical_json({key: value for key, value in manifest.items() if key != "collection_id"})
    )


def validated_label_ids(label_ids: Sequence[str], session_labels: Mapping[str, str]) -> None:
    if not 1 <= len(label_ids) <= MAX_SOURCES:
        raise CollectionError("invalid_request", f"a collection has 1 to {MAX_SOURCES} label artifacts")
    if not all(is_artifact_id(item) for item in label_ids):
        raise CollectionError("invalid_request", "a label artifact ID is 64 lowercase hexadecimal digits")
    if len(set(label_ids)) != len(label_ids):
        raise CollectionError("duplicate_member", "a label artifact is named more than once")
    for label_id, text in session_labels.items():
        if label_id not in label_ids:
            raise CollectionError(
                "invalid_request", f"a session label names {label_id}, which is not a member"
            )
        if not text or len(text) > MAX_SESSION_LABEL or not text.isprintable():
            raise CollectionError(
                "invalid_request", f"a session label is 1 to {MAX_SESSION_LABEL} printable characters"
            )


def verified_members(tools: Tools, artifacts: Path, label_ids: Sequence[str]) -> list[VerifiedRecording]:
    roots = ArtifactRoots.under(artifacts)
    return [
        verify_recording(
            tools.path(CHRONOS_DATASET),
            catalog=roots.catalog,
            features_root=roots.features,
            labels_root=roots.labels,
            label_artifact_id=label_id,
        )
        for label_id in label_ids
    ]


def build_collection(
    tools: Tools,
    artifacts: Path,
    label_ids: Sequence[str],
    session_labels: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    """The collection of these label artifacts' recordings, each verified
    from its sources, or the reason there can be none."""
    labels = dict(session_labels or {})
    validated_label_ids(label_ids, labels)
    return _collection(verified_members(tools, artifacts, label_ids), labels)[0]


def _collection(
    recordings: Sequence[VerifiedRecording], session_labels: Mapping[str, str]
) -> tuple[dict[str, Any], list[VerifiedRecording]]:
    """The manifest, and the recordings in its member order."""
    members = [
        _member(recording, session_labels.get(recording.labels.artifact_id)) for recording in recordings
    ]
    members.sort(key=lambda member: member.first_receive_ns)
    contract = _require_consistent(members)
    evidence = contract.pop("evidence")
    instruments: dict[str, int] = {}
    for member in members:
        for instrument in member.document["instruments"]:
            instruments[instrument] = instruments.get(instrument, 0) + 1
    manifest = {
        "contract": contract,
        "counts": {
            "instruments": {name: encode_integer(count) for name, count in sorted(instruments.items())},
            "members": encode_integer(len(members)),
        },
        "definitions": DEFINITIONS,
        "evidence": evidence,
        "members": [member.document for member in members],
        "schema": SCHEMA,
        "verification": list(VERIFICATION),
    }
    return {**manifest, "collection_id": collection_identity(manifest)}, [
        member.recording for member in members
    ]


def verified_collection(root: Path, collection_id: str) -> dict[str, Any]:
    """The collection, its identity and schema verified from bytes read once.
    Its members are re-verified only by inspect_collection."""
    try:
        directory = artifact_directory(root, collection_id)
        if listed_files(directory) != [MANIFEST]:
            raise CollectionError("artifact_invalid", "a collection holds exactly manifest.json")
        manifest = verified_manifest_file(directory, "collection_id", with_newline=True)
    except ArtifactUnavailableError as error:
        raise CollectionError("artifact_invalid", str(error)) from error
    if manifest.get("schema") != SCHEMA:
        raise CollectionError("artifact_invalid", f"collection {collection_id} is not {SCHEMA}")
    return dict(manifest)


def member_label_ids(manifest: Mapping[str, Any]) -> list[str]:
    return [member["labels"]["artifact_id"] for member in manifest["members"]]


def _session_labels(manifest: Mapping[str, Any]) -> dict[str, str]:
    return {
        member["labels"]["artifact_id"]: member["session"]["label"]
        for member in manifest["members"]
        if "label" in member["session"]
    }


def verified_recordings(
    tools: Tools, artifacts: Path, collection_id: str
) -> tuple[dict[str, Any], list[VerifiedRecording]]:
    """The collection and its members' recordings, in member order: every
    member re-verified from its sources now, and the collection rebuilt from
    them identical to the published one."""
    manifest = verified_collection(collections_root(artifacts), collection_id)
    try:
        rebuilt, recordings = _collection(
            verified_members(tools, artifacts, member_label_ids(manifest)), _session_labels(manifest)
        )
    except ArtifactError as error:
        raise CollectionError("source_invalid", f"a member no longer verifies: {error}") from error
    if canonical_json(rebuilt) != canonical_json(manifest):
        raise CollectionError(
            "artifact_invalid", "the collection differs from the one its verified members give"
        )
    return manifest, recordings


def inspect_collection(tools: Tools, artifacts: Path, collection_id: str) -> dict[str, Any]:
    """Verifies the collection and re-verifies every member from its sources."""
    return verified_recordings(tools, artifacts, collection_id)[0]


def create_collection(
    tools: Tools,
    artifacts: Path,
    label_ids: Sequence[str],
    session_labels: Mapping[str, str] | None = None,
) -> dict[str, Any]:
    manifest = build_collection(tools, artifacts, label_ids, session_labels)
    root = collections_root(artifacts)
    root.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verified_collection(root, directory.name)
        return (directory / MANIFEST).read_bytes()

    try:
        outcome, warning = publish(
            root, manifest["collection_id"], {MANIFEST: canonical_json(manifest)}, MANIFEST, existing
        )
    except PublicationError as error:
        raise CollectionError("publication_failed", str(error)) from error
    return {"collection": manifest, "outcome": outcome.value, "warning": warning}


def collection_ids(root: Path) -> list[str]:
    if not root.is_dir():
        return []
    return sorted(name for name in (entry.name for entry in root.iterdir()) if is_artifact_id(name))
