"""What a read finds of the sources strategy diagnostics depend on, now
(ADR 0024).

A read of published diagnostics checks every source they depend on, kind
by kind: each study's, plan's, and collection's identity and what it names;
and each dataset's, feature artifact's, label artifact's, schedule's, and
run's manifest identity, its bindings to the others, and the size and
SHA-256 of the file the diagnostics consumed, compared with what they
read. Nothing is rebuilt, recomputed, audited, or replayed: that is the
CLI's inspection, the independent audit, and the study's own replay.

Hashing every consumed file on every read would read about 1 GB for the
real pilot, so `ContentDigests` keeps each file's SHA-256 as this process
last computed it, and reuses it only while the file's device, inode, size,
modification time, and change time are all unchanged; any change, a file
never read, or a file that changed while it was read is read and hashed
again. A reused digest is reported as such.

Each figure is then verified only if every source it depends on is: a
run's figures on its study, plan, collection, fold's dataset, features, and
labels, its model's schedule, and its own journal; a fold's predictions on
all of those but the journal; a pool on everything its runs or folds
depend on. Any other state names the sources that failed.
"""

import hashlib
import os
import stat
import threading
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    is_leaf_name,
    read_regular_file,
    verified_manifest,
)
from chronos_research.collection import collections_root
from chronos_research.pipeline import ArtifactRoots
from chronos_research.walk_forward import plans_root
from chronos_research.walk_forward_study import StudyError, studies_root, verified_study

KINDS = ("studies", "plans", "collections", "datasets", "features", "labels", "schedules", "runs")
DETAIL = (
    "checked by this read: each study's, plan's, and collection's identity and what it names; each "
    "dataset's, feature artifact's, label artifact's, schedule's, and run's manifest identity, its "
    "bindings to the others, and the size and SHA-256 of the file the diagnostics consumed, compared with "
    "what they read. A SHA-256 is computed now, or reused from this service's own earlier hash of the same "
    "file while its device, inode, size, modification time, and change time are unchanged. A figure is "
    "verified only if every source it depends on is. Nothing is rebuilt, audited, or replayed here"
)

type Signature = tuple[int, int, int, int, int]


def _signature(path: Path) -> Signature:
    try:
        status = os.lstat(path)
    except OSError as error:
        raise ArtifactUnavailableError(f"{path.name} does not exist") from error
    if not stat.S_ISREG(status.st_mode):
        raise ArtifactUnavailableError(f"{path.name} is not a regular file")
    return (status.st_dev, status.st_ino, status.st_size, status.st_mtime_ns, status.st_ctime_ns)


class ContentDigests:
    """Each consumed file's SHA-256 as this process last read it, reused only
    while the file is unchanged by every measure the file system keeps."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._known: dict[Path, tuple[Signature, str]] = {}

    def verify(self, directory: Path, digest: Mapping[str, Any]) -> bool:
        """Raises unless the listed file has the listed size and SHA-256.
        True if its SHA-256 was reused, False if it was read and hashed now."""
        name = digest.get("name")
        if not is_leaf_name(name):
            raise ArtifactUnavailableError("an artifact file name must be a single filename")
        path = directory / str(name)
        before = _signature(path)
        with self._lock:
            known = self._known.get(path)
        if known is not None and known[0] == before:
            sha256, reused = known[1], True
        else:
            data = read_regular_file(path)
            sha256, reused = hashlib.sha256(data).hexdigest(), False
            if len(data) == before[2] and _signature(path) == before:
                with self._lock:
                    self._known[path] = (before, sha256)
        if before[2] != int(digest["size_bytes"]) or sha256 != digest["sha256"]:
            raise ArtifactUnavailableError(f"{name} differs from its manifest's size or checksum")
        return reused


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


@dataclass
class _Read:
    artifacts: Path
    digests: ContentDigests
    states: dict[str, dict[str, str]] = field(default_factory=lambda: {kind: {} for kind in KINDS})
    hashed: int = 0
    reused: int = 0

    @property
    def roots(self) -> ArtifactRoots:
        return ArtifactRoots.under(self.artifacts)

    def check(self, kind: str, identifier: str, verify: Callable[..., None], *arguments: Any) -> str:
        """The source's state, checked once per read; its dependency key."""
        states = self.states[kind]
        if identifier not in states:
            try:
                verify(*arguments)
            except (
                ArtifactUnavailableError,
                StudyError,
                OSError,
                ValueError,
                KeyError,
                TypeError,
                AttributeError,
            ) as error:
                states[identifier] = f"invalid: {error}"
            else:
                states[identifier] = "verified"
        return f"{kind}:{identifier}"

    def content(self, directory: Path, digest: Mapping[str, Any], read: str) -> None:
        """The consumed file is its manifest's, and the one the diagnostics read."""
        if self.digests.verify(directory, digest):
            self.reused += 1
        else:
            self.hashed += 1
        _require(digest["sha256"] == read, f"{digest['name']} is not the file the diagnostics read")

    def trust(self, keys: set[str]) -> dict[str, Any]:
        failed = sorted(
            key for key in keys if self.states[key.partition(":")[0]][key.partition(":")[2]] != "verified"
        )
        return {"state": "invalid", "sources": failed} if failed else {"state": "verified"}

    # One check per kind of source.
    def study(self, section: Mapping[str, Any]) -> None:
        study = verified_study(studies_root(self.artifacts), section["study_id"])
        _require(
            (study["plan_id"], study["collection"]["collection_id"], study["instrument"], study["status"])
            == (section["plan_id"], section["collection_id"], section["instrument"], section["study_status"]),
            "the study is not the one the diagnostics read",
        )
        cells = {
            (fold["index"], name, cell["scenario"]): cell["run"]["artifact_id"]
            for fold in study["folds"]
            for name, model in fold["models"].items()
            for cell in model["cells"]
            if cell["state"] == "completed"
        }
        read = {
            (fold["index"], run["model"], run["scenario"]): run["run_id"]
            for fold in section["folds"]
            for run in fold["runs"]
            if run["state"] == "completed"
        }
        _require(cells == read, "the study's completed runs are not the ones the diagnostics read")

    def plan(self, section: Mapping[str, Any]) -> None:
        plan = verified_manifest(
            artifact_directory(plans_root(self.artifacts), section["plan_id"]), "plan_id", with_newline=True
        )
        _require(
            plan["collection"]["collection_id"] == section["collection_id"]
            and [
                (
                    fold["index"],
                    fold["evaluation"]["dataset_id"],
                    fold["evaluation"]["feature_artifact_id"],
                    fold["evaluation"]["label_artifact_id"],
                    fold["evaluation"]["stream"],
                    fold["evaluation"]["window"],
                )
                for fold in plan["folds"]
            ]
            == [
                (
                    fold["index"],
                    fold["dataset_id"],
                    fold["feature_artifact_id"],
                    fold["label_artifact_id"],
                    fold["stream"],
                    fold["window"],
                )
                for fold in section["folds"]
            ],
            "the plan's folds are not the ones the diagnostics read",
        )

    def collection(self, section: Mapping[str, Any]) -> None:
        collection = verified_manifest(
            artifact_directory(collections_root(self.artifacts), section["collection_id"]),
            "collection_id",
            with_newline=True,
        )
        members = {member["dataset"]["dataset_id"]: member for member in collection["members"]}
        for fold in section["folds"]:
            member = members.get(fold["dataset_id"])
            _require(
                member is not None, f"fold {fold['index']}'s recording is not a member of the collection"
            )
            assert member is not None
            _require(
                (
                    member["dataset"]["events_sha256"],
                    member["dataset"]["stream"],
                    member["features"]["artifact_id"],
                    member["features"]["features_sha256"],
                    member["labels"]["artifact_id"],
                    member["labels"]["labels_sha256"],
                )
                == (
                    fold["events_sha256"],
                    fold["stream"],
                    fold["feature_artifact_id"],
                    fold["features_sha256"],
                    fold["label_artifact_id"],
                    fold["labels_sha256"],
                ),
                f"fold {fold['index']}'s recording is not the collection member's",
            )

    def dataset(self, fold: Mapping[str, Any]) -> None:
        directory = artifact_directory(self.roots.catalog, fold["dataset_id"])
        manifest = verified_manifest(directory, "dataset_id", with_newline=False)
        _require(manifest["stream"] == fold["stream"], "the dataset's stream is not the fold's")
        self.content(directory, manifest["files"]["events"], fold["events_sha256"])

    def features(self, fold: Mapping[str, Any]) -> None:
        directory = artifact_directory(self.roots.features, fold["feature_artifact_id"])
        manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        source = manifest["source"]
        _require(
            (source["dataset_id"], source["events_sha256"], source["stream"])
            == (fold["dataset_id"], fold["events_sha256"], fold["stream"]),
            "the features are not of the fold's dataset",
        )
        self.content(directory, manifest["files"]["features"], fold["features_sha256"])

    def labels(self, fold: Mapping[str, Any]) -> None:
        directory = artifact_directory(self.roots.labels, fold["label_artifact_id"])
        manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        source = manifest["source"]
        _require(
            (source["dataset_id"], source["feature_artifact_id"], source["features_sha256"], source["stream"])
            == (fold["dataset_id"], fold["feature_artifact_id"], fold["features_sha256"], fold["stream"]),
            "the labels are not of the fold's features",
        )
        self.content(directory, manifest["files"]["labels"], fold["labels_sha256"])

    def schedule(self, fold: Mapping[str, Any], model: Mapping[str, Any]) -> None:
        directory = artifact_directory(self.roots.schedules, model["schedule_id"])
        manifest = verified_manifest(directory, "artifact_id", with_newline=True)
        source = manifest["source"]
        _require(
            (source["dataset_id"], source["feature_artifact_id"], source["features_sha256"], source["stream"])
            == (fold["dataset_id"], fold["feature_artifact_id"], fold["features_sha256"], fold["stream"]),
            "the schedule is not of the fold's features",
        )
        self.content(directory, manifest["files"]["schedule"], model["schedule_sha256"])

    def run(self, fold: Mapping[str, Any], model: Mapping[str, Any], run: Mapping[str, Any]) -> None:
        directory = artifact_directory(self.roots.strategies, run["run_id"])
        manifest = verified_manifest(directory, "artifact_id", with_newline=False)
        source = manifest["source"]
        _require(
            (
                source["schedule_id"],
                source["dataset_id"],
                source["events_sha256"],
                source["feature_artifact_id"],
            )
            == (model["schedule_id"], fold["dataset_id"], fold["events_sha256"], fold["feature_artifact_id"]),
            "the run is not of the fold's schedule and dataset",
        )
        self.content(directory, manifest["files"]["journal"], run["journal_sha256"])


def current_sources(artifacts: Path, manifest: Mapping[str, Any], digests: ContentDigests) -> dict[str, Any]:
    """Every source's state now, and the state of every figure from them."""
    read = _Read(artifacts, digests)
    runs: dict[str, Any] = {}
    studies: dict[str, Any] = {}
    # Each pool's dependencies, by study, for pooling across studies.
    trading_keys: dict[str, dict[tuple[str, str], set[str]]] = {}
    prediction_keys: dict[str, dict[str, set[str]]] = {}
    for section in manifest["studies"]:
        study_id = section["study_id"]
        whole = {
            read.check("studies", study_id, read.study, section),
            read.check("plans", section["plan_id"], read.plan, section),
            read.check("collections", section["collection_id"], read.collection, section),
        }
        trading = trading_keys[study_id] = {
            (name, scenario["name"]): set(whole)
            for name in section["pooled"]["trading"]
            for scenario in section["scenarios"]
        }
        predicted = prediction_keys[study_id] = {
            name: set(whole) for name in section["pooled"]["predictions"]
        }
        folds: dict[str, Any] = {}
        for fold in section["folds"]:
            recording = whole | {
                read.check("datasets", fold["dataset_id"], read.dataset, fold),
                read.check("features", fold["feature_artifact_id"], read.features, fold),
                read.check("labels", fold["label_artifact_id"], read.labels, fold),
            }
            models: dict[str, set[str]] = {}
            for name, model in fold["models"].items():
                if model["state"] == "completed":
                    models[name] = recording | {
                        read.check("schedules", model["schedule_id"], read.schedule, fold, model)
                    }
                    predicted[name] |= models[name]
            for run in fold["runs"]:
                if run["state"] != "completed":
                    continue
                keys = models[run["model"]] | {
                    read.check("runs", run["run_id"], read.run, fold, fold["models"][run["model"]], run)
                }
                runs[run["run_id"]] = keys
                trading[(run["model"], run["scenario"])] |= keys
            folds[fold["index"]] = {name: read.trust(keys) for name, keys in models.items()}
        studies[study_id] = {
            "folds": folds,
            "pooled": {
                "predictions": {name: read.trust(keys) for name, keys in predicted.items()},
                "trading": _by_scenario(read, trading),
            },
        }
    figures: dict[str, Any] = {
        "runs": {run_id: read.trust(keys) for run_id, keys in runs.items()},
        "studies": studies,
    }
    if "across_studies" in manifest:
        across = manifest["across_studies"]
        shared = [scenario["name"] for scenario in across["scenarios"]]
        figures["across_studies"] = {
            "predictions": {
                name: read.trust(set().union(*(keys[name] for keys in prediction_keys.values())))
                for name in across["predictions"]
            },
            "trading": {
                name: {
                    scenario: read.trust(
                        set().union(*(keys[(name, scenario)] for keys in trading_keys.values()))
                    )
                    for scenario in shared
                }
                for name in across["trading"]
            },
        }
    checked = [state for states in read.states.values() for state in states.values()]
    return {
        "content": {"hashed_now": str(read.hashed), "reused_unchanged": str(read.reused)},
        "detail": DETAIL,
        "figures": figures,
        "sources": read.states,
        "state": "verified" if all(state == "verified" for state in checked) else "invalid",
    }


def _by_scenario(read: _Read, pools: Mapping[tuple[str, str], set[str]]) -> dict[str, dict[str, Any]]:
    document: dict[str, dict[str, Any]] = {}
    for (name, scenario), keys in pools.items():
        document.setdefault(name, {})[scenario] = read.trust(keys)
    return document
