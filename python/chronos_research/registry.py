"""The model library: immutable, content-addressed model versions (ADR 0021).

    <artifacts>/models/<model_version_id>/manifest.json
    <artifacts>/models/<model_version_id>/<payload>
    <artifacts>/model-names/<binding_id>/manifest.json   an immutable name and version
    <artifacts>/model-lifecycle/<model_version_id>.json  active or archived, replaced atomically

A model version gives a model a durable identity of its own, independent of
any experiment. Its ID is the SHA-256 of its canonical manifest without the
ID, with its newline, as for experiments. The manifest keeps three
identities apart:

    model_version_id          the whole immutable record
    payload.sha256            the executable bytes
    provenance.experiment_id  only for a model taken from a verified experiment

and records the format and runtime contract, the one instrument, the exact
inputs, the outputs, the target, the information cutoff and its basis, the
provenance, and what Chronos itself verified.

A name and version is an immutable binding to one model version. Binding
the same name and version to a different model is refused, never replaced.
The lifecycle is presentation and policy only: an archived model cannot be
chosen for new comparisons or deployments, and nothing that already uses it
changes.

Every consumer reads a version through verified_model_version, which reads
each file once through pinned O_NOFOLLOW descriptors, verifies those bytes,
and hands out the bytes and the parsed model it verified.
"""

import json
import os
import re
import secrets
import stat
import time
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from chronos_research import onnx_profile
from chronos_research.artifact_files import (
    ArtifactUnavailableError,
    artifact_directory,
    is_leaf_name,
    listed_files,
    read_regular_file,
    verified_file,
    verified_manifest,
)
from chronos_research.artifacts import CLASS_ORDER, LABEL_SET, VerifiedSources, is_artifact_id
from chronos_research.encoding import canonical_json, decode_integer, sha256_hex
from chronos_research.experiment import MANIFEST, MODEL, model_feature_set_json, source_json
from chronos_research.inspection import InspectionError, stored_logistic, stored_majority, verified_experiment
from chronos_research.models import LogisticConfiguration, LogisticModel, MajorityModel, Unevaluable
from chronos_research.publication import Outcome, PublicationError, publish
from chronos_research.schedule import INFERENCE, training_labels_available_through

SCHEMA = {"name": "chronos.model_version", "version": "1"}
BINDING_SCHEMA = {"name": "chronos.model_name", "version": "1"}
LIFECYCLE_SCHEMA = {"name": "chronos.model_lifecycle", "version": "1"}
PARAMETERS = "parameters.json"
ONNX_MODEL = "model.onnx"
# The families a verified experiment holds, and the imported one.
EXPERIMENT_FAMILIES = ("logistic", "majority")
FAMILIES = (*EXPERIMENT_FAMILIES, "onnx")
LIFECYCLE_STATES = ("active", "archived")
MAX_MANIFEST_BYTES = 256 * 1024
MAX_PARAMETERS_BYTES = 64 * 1024
MAX_PAYLOAD_BYTES = {
    "logistic": MAX_PARAMETERS_BYTES,
    "majority": MAX_PARAMETERS_BYTES,
    "onnx": onnx_profile.MAX_MODEL_BYTES,
}
MODEL_NAME = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}")
MODEL_VERSION = re.compile(r"(0|[1-9][0-9]{0,5})(\.(0|[1-9][0-9]{0,5})){0,2}")

FORMATS = {
    **{
        family: {"name": "chronos.baseline_parameters", "payload": PARAMETERS, "version": "1"}
        for family in EXPERIMENT_FAMILIES
    },
    "onnx": {
        "name": "onnx",
        "payload": ONNX_MODEL,
        "profile": f"{onnx_profile.PROFILE['name']}/{onnx_profile.PROFILE['version']}",
        "version": "1",
    },
}
RUNTIMES = {
    "logistic": {
        "class": INFERENCE["class"],
        "description": (
            "each input standardized with the stored mean and scale, the logits summed in a fixed "
            "elementwise order, and the softmax of the logits shifted by their largest; binary64, one row at "
            "a time (schedule.logistic_predictions)"
        ),
        "name": "chronos.row_pure_logistic",
        "version": "1",
    },
    "majority": {
        "class": "the most frequent training class, ties to the earliest class in class_order",
        "description": "the training class frequencies for every row; the inputs are not used",
        "name": "chronos.majority_prior",
        "version": "1",
    },
    "onnx": {
        "class": "the first largest probability, so ties go to the earliest class in class_order",
        "description": (
            "ONNX Runtime on its CPU provider alone, one thread, no graph optimization, one row per run, "
            "inside the restricted ONNX sandbox container, which checks the profile again at every load"
        ),
        "name": "chronos.onnx_sandbox",
        "version": "1",
    },
}
_UNITS = {
    "spread": "price",
    "imbalance": "ratio from -1 to 1",
    "microprice_minus_midpoint": "price",
    "bid_size": "shares",
    "ask_size": "shares",
}
INPUT_CONTRACT = {
    "conversion": (
        "each exact integer divided by its scale and rounded once to binary64 (model_data.to_floats)"
    ),
    "fields": [{**field, "unit": _UNITS[field["name"]]} for field in model_feature_set_json()["inputs"]],
    "name": model_feature_set_json()["name"],
    "row": "one feature row's inputs, in this order; no label, label time, or future observation",
    "version": model_feature_set_json()["version"],
}
OUTPUT_CONTRACT = {
    "class_order": list(CLASS_ORDER),
    "confidence_ppb": INFERENCE["confidence_ppb"],
    "kind": "class_probabilities",
    "probabilities": "one binary64 probability per class, each from 0 to 1, together summing to one",
}
TARGET_DEFINITION = (
    "the direction of the midpoint return from the prediction time to the first row of the instrument at "
    "least one horizon later (future_midpoint_return version 1): down, flat, or up"
)
CUTOFF_MEANING = "no training row, label, or fitted statistic uses information at or after this time"
DECLARED_CUTOFF_MEANING = (
    "the uploader declares that no information at or after this time was used in training, preprocessing, "
    "tuning, or model selection; Chronos did not verify it"
)
CUTOFF_SCOPE = "training, preprocessing, tuning, and model selection"
_MANIFEST_KEYS = {
    "family", "format", "inputs", "instrument", "model_version_id", "output", "payload", "provenance",
    "runtime", "schema", "target", "training", "verification",
}  # fmt: skip
_SOURCE_KEYS = {
    "dataset_id", "feature_artifact_id", "feature_set", "features_sha256", "horizon_ns", "label_artifact_id",
    "label_set", "labels_sha256", "split_artifact_id", "split_set", "splits_sha256", "stream",
    "test_start_at_ns", "validation_start_at_ns",
}  # fmt: skip
INSTRUMENT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")


class RegistryError(Exception):
    """A model version cannot be registered, or is not valid."""


class NameVersionTakenError(RegistryError):
    def __init__(self, name: str, version: str, existing: str) -> None:
        super().__init__(f"{name} {version} already names model version {existing}")
        self.existing = existing


@dataclass(frozen=True)
class RegistryRoots:
    models: Path
    names: Path
    lifecycle: Path

    @classmethod
    def under(cls, base: Path) -> "RegistryRoots":
        return cls(models=base / "models", names=base / "model-names", lifecycle=base / "model-lifecycle")


@dataclass(frozen=True)
class OnnxPayload:
    """An imported model's bytes, which the host never parses: only the
    sandbox reads them, and checks them again at every load."""

    size: int


@dataclass(frozen=True)
class ModelVersion:
    """A verified model version, with the bytes and model that were verified."""

    manifest: Mapping[str, Any]
    payload: bytes
    model: MajorityModel | LogisticModel | OnnxPayload

    @property
    def origin(self) -> str:
        return str(self.manifest["provenance"]["origin"])

    @property
    def model_version_id(self) -> str:
        return str(self.manifest["model_version_id"])

    @property
    def family(self) -> str:
        return str(self.manifest["family"])

    @property
    def instrument(self) -> str:
        return str(self.manifest["instrument"])

    @property
    def information_cutoff_ns(self) -> int | None:
        cutoff = self.manifest["training"]["information_cutoff"]
        return decode_integer(cutoff["at_ns"]) if "at_ns" in cutoff else None


def _fail(message: str) -> RegistryError:
    return RegistryError(message)


def _keys(document: Any, expected: set[str], what: str) -> Mapping[str, Any]:
    if not isinstance(document, dict) or document.keys() != expected:
        raise _fail(f"{what} must have exactly the keys {', '.join(sorted(expected))}")
    return document


def _text(document: Mapping[str, Any], key: str, what: str) -> str:
    value = document[key]
    if not isinstance(value, str) or not value:
        raise _fail(f"{what}.{key} must be a nonempty string")
    return value


def _timestamp(text: Any, what: str) -> int:
    try:
        value = decode_integer(text)
    except ValueError as error:
        raise _fail(f"{what} must be a canonical integer") from error
    if not 0 <= value <= 2**63 - 1:
        raise _fail(f"{what} must fit a nonnegative int64")
    return value


def name_binding_id(name: str, version: str) -> str:
    return sha256_hex(canonical_json({"name": name, "schema": BINDING_SCHEMA, "version": version}))


def require_name(name: str, version: str) -> None:
    if not MODEL_NAME.fullmatch(name):
        raise _fail("a model name is 1 to 64 characters from [a-z0-9._-], starting with a letter or digit")
    if not MODEL_VERSION.fullmatch(version):
        raise _fail("a model version is one to three dot-separated whole numbers, such as 1 or 1.2.0")


# --- Building a version from a verified experiment -------------------------------------------------------


def experiment_model_version(
    experiment: Mapping[str, Any],
    sources: VerifiedSources,
    instrument: str,
    family: str,
    model: MajorityModel | LogisticModel,
    training_rows: tuple[int, ...],
) -> tuple[dict[str, Any], bytes]:
    """The manifest and payload of a model taken from a verified experiment
    whose sources were verified as `sources`."""
    payload = canonical_json(model.as_json())
    manifest: dict[str, Any] = {
        "family": family,
        "format": FORMATS[family],
        "inputs": INPUT_CONTRACT,
        "instrument": instrument,
        "output": OUTPUT_CONTRACT,
        "payload": {"name": PARAMETERS, "sha256": sha256_hex(payload), "size_bytes": str(len(payload))},
        "provenance": {
            "code": {"unavailable": "an experiment records its library versions, not a source revision"},
            "configuration": experiment["configuration"],
            "environment": experiment["environment"],
            "experiment_id": experiment["experiment_id"],
            "model": family,
            "origin": "experiment",
            "sources": experiment["sources"],
        },
        "runtime": RUNTIMES[family],
        "schema": SCHEMA,
        "target": {
            "definition": TARGET_DEFINITION,
            "horizon_ns": str(sources.horizon_ns),
            "label_set": LABEL_SET,
        },
        "training": {
            "class_counts": dict(zip(CLASS_ORDER, map(str, training_rows), strict=True)),
            "information_cutoff": {
                "at_ns": str(sources.validation_start_at_ns),
                "basis": "verified",
                "meaning": CUTOFF_MEANING,
            },
            "labels_available_through_ns": str(training_labels_available_through(sources)),
            "rows": str(sum(training_rows)),
            "split": "train",
        },
        "verification": {
            "cutoff": (
                "the information cutoff is the verified split's validation start: every training row is "
                "before it, and every training label became available before it"
            ),
            "experiment": (
                "the experiment's identity, file checksums, and every stored prediction re-derived from its "
                "parameters, at registration"
            ),
            "payload": (
                "the payload is the verified model's canonical parameters; its SHA-256 is the "
                "parameters_sha256 that schedules and deployments of this model record"
            ),
            "sources": (
                "the experiment's split, labels, and features verified by chronos_dataset, each reproduced "
                "from its source and the features from their dataset; the experiment names exactly those"
            ),
        },
    }
    return {**manifest, "model_version_id": version_identity(manifest)}, payload


def version_identity(manifest: Mapping[str, Any]) -> str:
    return sha256_hex(
        canonical_json({key: value for key, value in manifest.items() if key != "model_version_id"})
    )


# --- Verification on consumption -------------------------------------------------------------------------


def _parsed_document(data: bytes, what: str) -> Any:
    try:
        document = json.loads(data)
    except ValueError as error:
        raise _fail(f"{what} is not JSON") from error
    try:
        canonical = canonical_json(document)
    except TypeError as error:
        raise _fail(f"{what}: {error}") from error
    if canonical != data:
        raise _fail(f"{what} is not in canonical form")
    return document


def _check_import_training(training: Any, provenance: Mapping[str, Any]) -> None:
    """An imported model's cutoff is declared, with the uploader's statement
    of what it covers, or unavailable; Chronos never calls it verified."""
    _keys(training, {"information_cutoff"}, "training")
    cutoff = training["information_cutoff"]
    declared = provenance["declared"]["information_cutoff_ns"]
    if isinstance(cutoff, dict) and cutoff.get("basis") == "unavailable":
        _keys(cutoff, {"basis", "reason"}, "training.information_cutoff")
        if cutoff["reason"] != "not_declared" or declared is not None:
            raise _fail("an unavailable cutoff is one the uploader did not declare")
        return
    _keys(cutoff, {"at_ns", "basis", "meaning"}, "training.information_cutoff")
    if cutoff["basis"] != "declared" or cutoff["meaning"] != DECLARED_CUTOFF_MEANING:
        raise _fail("an imported model's cutoff is declared by its uploader, never verified")
    if _timestamp(cutoff["at_ns"], "training.information_cutoff.at_ns") == 0 or declared != cutoff["at_ns"]:
        raise _fail("the cutoff must be the uploader's declared one")


def _check_training(training: Any, family: str, payload_model: MajorityModel | LogisticModel) -> None:
    _keys(
        training,
        {"class_counts", "information_cutoff", "labels_available_through_ns", "rows", "split"},
        "training",
    )
    cutoff = _keys(
        training["information_cutoff"], {"at_ns", "basis", "meaning"}, "training.information_cutoff"
    )
    if cutoff["basis"] != "verified" or cutoff["meaning"] != CUTOFF_MEANING or training["split"] != "train":
        raise _fail("an experiment model's information cutoff is the verified split's validation start")
    at = _timestamp(cutoff["at_ns"], "training.information_cutoff.at_ns")
    through = _timestamp(training["labels_available_through_ns"], "training.labels_available_through_ns")
    if through >= at:
        raise _fail("a training label becomes available at or after the information cutoff")
    counts = _keys(training["class_counts"], set(CLASS_ORDER), "training.class_counts")
    rows = tuple(_timestamp(counts[name], "a training class count") for name in CLASS_ORDER)
    if _timestamp(training["rows"], "training.rows") != sum(rows) or sum(rows) == 0:
        raise _fail("the training rows must add up to the class counts and be positive")
    if (
        family == "majority"
        and isinstance(payload_model, MajorityModel)
        and payload_model.class_counts != rows
    ):
        raise _fail("a majority model's counts are its training class counts")


_VALIDATION_KEYS = {"onnxruntime", "profile", "sandbox_content_sha256", "smoke"}
_SMOKE_KEYS = {"corpus", "corpus_sha256", "outputs_sha256", "rows"}


def _strings(document: Any, what: str) -> None:
    if (
        not isinstance(document, dict)
        or not document
        or any(not isinstance(value, str) or not value for value in document.values())
    ):
        raise _fail(f"{what} must map names to nonempty strings")


def _check_import_provenance(provenance: Any) -> None:
    _keys(provenance, {"declared", "manifest_sha256", "not_verified", "origin", "validation"}, "provenance")
    declared = _keys(
        provenance["declared"], {"cutoff_covers", "information_cutoff_ns", "source", "training"}, "declared"
    )
    if not isinstance(declared["training"], str) or not declared["training"]:
        raise _fail("declared.training must describe how the model was trained")
    if not isinstance(declared["source"], str):
        raise _fail("declared.source must be a string")
    if (declared["information_cutoff_ns"] is None) != (declared["cutoff_covers"] is None) or (
        declared["cutoff_covers"] not in (None, CUTOFF_SCOPE)
    ):
        raise _fail(f"a declared cutoff must state that it covers {CUTOFF_SCOPE}")
    if not is_artifact_id(provenance["manifest_sha256"]):
        raise _fail("provenance.manifest_sha256 must be a checksum")
    _strings(provenance["not_verified"], "provenance.not_verified")
    validation = _keys(provenance["validation"], _VALIDATION_KEYS, "provenance.validation")
    smoke = _keys(validation["smoke"], _SMOKE_KEYS, "provenance.validation.smoke")
    if not is_artifact_id(validation["sandbox_content_sha256"]) or not all(
        is_artifact_id(smoke[key]) for key in ("corpus_sha256", "outputs_sha256")
    ):
        raise _fail("the validation record names its sandbox and smoke test by checksum")
    if (
        not isinstance(validation["profile"], dict)
        or validation["profile"].get("profile") != onnx_profile.PROFILE
    ):
        raise _fail("the validation record must report the profile it checked")
    if not isinstance(validation["onnxruntime"], str) or _timestamp(smoke["rows"], "smoke.rows") == 0:
        raise _fail("the validation record names its runtime and smoke rows")


def _check_provenance(provenance: Any, manifest: Mapping[str, Any]) -> LogisticConfiguration | None:
    """The provenance's structure for its origin, and for an experiment
    model the configuration its parameters are checked against."""
    origin = provenance.get("origin") if isinstance(provenance, dict) else None
    if origin == "import":
        if manifest["family"] != "onnx":
            raise _fail("only an ONNX model is imported")
        _check_import_provenance(provenance)
        return None
    if manifest["family"] not in EXPERIMENT_FAMILIES:
        raise _fail("an experiment model is a logistic or majority model")
    _keys(
        provenance,
        {"code", "configuration", "environment", "experiment_id", "model", "origin", "sources"},
        "provenance",
    )
    if provenance["origin"] != "experiment" or provenance["model"] != manifest["family"]:
        raise _fail("provenance must name the experiment model this version was taken from")
    if not is_artifact_id(provenance["experiment_id"]):
        raise _fail("provenance.experiment_id must be a 64-digit lowercase ID")
    sources = _keys(provenance["sources"], _SOURCE_KEYS, "provenance.sources")
    if any(not isinstance(value, str) or not value for value in sources.values()):
        raise _fail("provenance.sources fields must be nonempty strings")
    if sources["horizon_ns"] != manifest["target"]["horizon_ns"]:
        raise _fail("the target horizon differs from the source labels' horizon")
    if sources["validation_start_at_ns"] != manifest["training"]["information_cutoff"]["at_ns"]:
        raise _fail("the information cutoff differs from the source split's validation start")
    for key in ("environment", "code"):
        entry = provenance[key]
        if not isinstance(entry, dict) or any(not isinstance(value, str) for value in entry.values()):
            raise _fail(f"provenance.{key} must map names to strings")
    configuration = provenance["configuration"]
    try:
        maximum = decode_integer(configuration["logistic"]["maximum_iterations"])
    except (KeyError, TypeError, ValueError) as error:
        raise _fail("provenance.configuration must be an experiment configuration") from error
    return LogisticConfiguration(maximum_iterations=maximum)


def _payload_model(
    family: str, payload: bytes, configuration: LogisticConfiguration | None
) -> MajorityModel | LogisticModel | OnnxPayload:
    if family == "onnx":
        return OnnxPayload(size=len(payload))
    if configuration is None:
        raise _fail("an experiment model's payload needs its configuration")
    document = _parsed_document(payload, PARAMETERS)
    try:
        model: MajorityModel | LogisticModel | Unevaluable = (
            stored_majority(document, "the majority payload")
            if family == "majority"
            else stored_logistic(document, "the logistic payload", configuration)
        )
    except (InspectionError, KeyError, TypeError, ValueError) as error:
        raise _fail(f"the payload is not a valid {family} model: {error}") from error
    if isinstance(model, Unevaluable):
        raise _fail("a registered model must be fitted, not unevaluable")
    return model


def check_manifest(manifest: Any, directory_name: str) -> None:
    """The manifest's structure and identity, before its payload is read."""
    _keys(manifest, _MANIFEST_KEYS, "the manifest")
    if manifest["schema"] != SCHEMA:
        raise _fail(f"unsupported model version schema {manifest.get('schema')}")
    if (
        version_identity(manifest) != manifest["model_version_id"]
        or manifest["model_version_id"] != directory_name
    ):
        raise _fail("the model version ID does not match the manifest's content or the directory name")
    family = manifest["family"]
    if family not in FAMILIES:
        raise _fail(f"unsupported model family {family!r}")
    if manifest["format"] != FORMATS[family] or manifest["runtime"] != RUNTIMES[family]:
        raise _fail("the format or runtime contract is not the family's")
    if manifest["inputs"] != INPUT_CONTRACT or manifest["output"] != OUTPUT_CONTRACT:
        raise _fail("the input or output contract is not one Chronos supports")
    if not isinstance(manifest["instrument"], str) or not INSTRUMENT.fullmatch(manifest["instrument"]):
        raise _fail("instrument must be a valid instrument identifier")
    target = _keys(manifest["target"], {"definition", "horizon_ns", "label_set"}, "target")
    if target["label_set"] != LABEL_SET or target["definition"] != TARGET_DEFINITION:
        raise _fail("unsupported target")
    if _timestamp(target["horizon_ns"], "target.horizon_ns") == 0:
        raise _fail("target.horizon_ns must be positive")
    payload = _keys(manifest["payload"], {"name", "sha256", "size_bytes"}, "payload")
    if payload["name"] != FORMATS[family]["payload"] or not is_artifact_id(payload["sha256"]):
        raise _fail("the payload digest names an unexpected file or checksum")
    if _timestamp(payload["size_bytes"], "payload.size_bytes") > MAX_PAYLOAD_BYTES[family]:
        raise _fail("the payload is larger than its family allows")
    verification = manifest["verification"]
    if (
        not isinstance(verification, dict)
        or not verification
        or any(not isinstance(value, str) or not value for value in verification.values())
    ):
        raise _fail("verification must state what was verified")


def verified_model_version(roots: RegistryRoots, model_version_id: str) -> ModelVersion:
    """The model version, verified from bytes read once: identity, structure,
    payload checksum and size, and the parsed model."""
    try:
        directory = artifact_directory(roots.models, model_version_id)
        names = listed_files(directory)
        manifest_data = read_regular_file(directory / MANIFEST, MAX_MANIFEST_BYTES)
    except ArtifactUnavailableError as error:
        raise _fail(str(error)) from error
    manifest = _parsed_document(manifest_data, MANIFEST)
    check_manifest(manifest, model_version_id)
    payload_name = manifest["payload"]["name"]
    if names != sorted([MANIFEST, payload_name]) or not is_leaf_name(payload_name):
        raise _fail(f"a model version holds exactly {MANIFEST} and {payload_name}")
    try:
        payload = read_regular_file(directory / payload_name, MAX_PAYLOAD_BYTES[manifest["family"]])
    except ArtifactUnavailableError as error:
        raise _fail(str(error)) from error
    if (
        len(payload) != int(manifest["payload"]["size_bytes"])
        or sha256_hex(payload) != manifest["payload"]["sha256"]
    ):
        raise _fail(f"{payload_name} differs from the manifest's checksum or size")
    configuration = _check_provenance(manifest["provenance"], manifest)
    model = _payload_model(manifest["family"], payload, configuration)
    if isinstance(model, OnnxPayload):
        _check_import_training(manifest["training"], manifest["provenance"])
    else:
        _check_training(manifest["training"], manifest["family"], model)
    return ModelVersion(manifest=manifest, payload=payload, model=model)


def model_version_ids(roots: RegistryRoots) -> list[str]:
    try:
        if not stat.S_ISDIR(os.lstat(roots.models).st_mode):
            return []
    except OSError:
        return []
    return sorted(name for name in os.listdir(roots.models) if is_artifact_id(name))


# --- Publication ---------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Registration:
    model_version_id: str
    outcome: str
    binding_id: str
    binding_outcome: str
    warning: str | None

    def as_json(self) -> dict[str, Any]:
        return {
            "binding_id": self.binding_id,
            "binding_outcome": self.binding_outcome,
            "model_version_id": self.model_version_id,
            "outcome": self.outcome,
            "warning": self.warning,
        }


def publish_model_version(
    roots: RegistryRoots, manifest: Mapping[str, Any], payload: bytes
) -> tuple[Outcome, str | None]:
    roots.models.mkdir(parents=True, exist_ok=True)
    version_id = str(manifest["model_version_id"])

    def existing(directory: Path) -> bytes:
        verified_model_version(roots, directory.name)
        return read_regular_file(directory / MANIFEST, MAX_MANIFEST_BYTES)

    return publish(
        roots.models,
        version_id,
        {MANIFEST: canonical_json(manifest), str(manifest["payload"]["name"]): payload},
        MANIFEST,
        existing,
    )


def verified_binding(roots: RegistryRoots, binding_id: str) -> dict[str, Any]:
    try:
        directory = artifact_directory(roots.names, binding_id)
        if listed_files(directory) != [MANIFEST]:
            raise _fail("a name binding holds exactly manifest.json")
        data = read_regular_file(directory / MANIFEST, MAX_MANIFEST_BYTES)
    except ArtifactUnavailableError as error:
        raise _fail(str(error)) from error
    binding = dict(
        _keys(
            _parsed_document(data, MANIFEST),
            {"binding_id", "model_version_id", "name", "schema", "version"},
            "a name binding",
        )
    )
    if binding["schema"] != BINDING_SCHEMA or not is_artifact_id(binding["model_version_id"]):
        raise _fail("a name binding names one model version")
    require_name(binding["name"], binding["version"])
    if (
        name_binding_id(binding["name"], binding["version"]) != binding_id
        or binding["binding_id"] != binding_id
    ):
        raise _fail("the binding ID does not match its name and version or its directory")
    return binding


def bind_name(roots: RegistryRoots, name: str, version: str, model_version_id: str) -> tuple[str, Outcome]:
    """Binds name and version to a model version for good. Idempotent for the
    same model; NameVersionTakenError for another."""
    require_name(name, version)
    binding_id = name_binding_id(name, version)
    document = {
        "binding_id": binding_id,
        "model_version_id": model_version_id,
        "name": name,
        "schema": BINDING_SCHEMA,
        "version": version,
    }
    roots.names.mkdir(parents=True, exist_ok=True)

    def existing(directory: Path) -> bytes:
        verified_binding(roots, directory.name)
        return read_regular_file(directory / MANIFEST, MAX_MANIFEST_BYTES)

    try:
        outcome, _ = publish(
            roots.names, binding_id, {MANIFEST: canonical_json(document)}, MANIFEST, existing
        )
    except PublicationError as refused:
        # Something already has this name: another model's binding, or an
        # entry that is not a valid binding at all. Neither is touched.
        try:
            taken = verified_binding(roots, binding_id)
        except RegistryError as invalid:
            raise _fail(f"{name} {version} is taken by an invalid binding: {invalid}") from refused
        if taken["model_version_id"] != model_version_id:
            raise NameVersionTakenError(name, version, taken["model_version_id"]) from None
        raise _fail(f"{name} {version} could not be bound: {refused}") from refused
    return binding_id, outcome


def require_name_free(roots: RegistryRoots, name: str, version: str, model_version_id: str) -> None:
    """Refuses, before anything is published, a name and version that
    already name another model version; bind_name still decides races."""
    require_name(name, version)
    binding_id = name_binding_id(name, version)
    if not os.path.lexists(roots.names / binding_id):
        return
    taken = verified_binding(roots, binding_id)
    if taken["model_version_id"] != model_version_id:
        raise NameVersionTakenError(name, version, taken["model_version_id"])


def names_by_version(roots: RegistryRoots) -> dict[str, list[dict[str, str]]]:
    """Every valid binding, by the model version it names."""
    found: dict[str, list[dict[str, str]]] = {}
    try:
        entries = sorted(os.listdir(roots.names)) if stat.S_ISDIR(os.lstat(roots.names).st_mode) else []
    except OSError:
        entries = []
    for binding_id in entries:
        if not is_artifact_id(binding_id):
            continue
        try:
            binding = verified_binding(roots, binding_id)
        except RegistryError:
            continue
        found.setdefault(binding["model_version_id"], []).append(
            {"name": binding["name"], "version": binding["version"]}
        )
    return found


def register_experiment_model(
    roots: RegistryRoots,
    experiments: Path,
    sources: VerifiedSources,
    experiment_id: str,
    instrument: str,
    family: str,
    name: str,
    version: str,
) -> Registration:
    """Registers one fitted model of a verified experiment whose sources the
    caller verified with chronos_dataset."""
    require_name(name, version)
    if family not in EXPERIMENT_FAMILIES:
        raise _fail(f"the model is one of {', '.join(EXPERIMENT_FAMILIES)}, not {family!r}")
    if not is_artifact_id(experiment_id):
        raise _fail("an experiment ID is 64 lowercase hexadecimal digits")
    experiment, models = verified_experiment(experiments / experiment_id)
    if experiment["sources"] != source_json(sources):
        raise _fail(f"experiment {experiment_id} does not name exactly the verified sources")
    if instrument not in models:
        raise _fail(f"experiment {experiment_id} has no models for {instrument!r}")
    model = models[instrument][family]
    if isinstance(model, Unevaluable):
        raise _fail(f"the {family} model for {instrument} is unevaluable ({model.reason})")
    majority = models[instrument]["majority"]
    if not isinstance(majority, MajorityModel):
        raise _fail(f"{instrument} has no training rows")
    manifest, payload = experiment_model_version(
        experiment, sources, instrument, family, model, majority.class_counts
    )
    require_name_free(roots, name, version, manifest["model_version_id"])
    outcome, warning = publish_model_version(roots, manifest, payload)
    binding_id, binding_outcome = bind_name(roots, name, version, manifest["model_version_id"])
    return Registration(
        model_version_id=manifest["model_version_id"],
        outcome=outcome.value,
        binding_id=binding_id,
        binding_outcome=binding_outcome.value,
        warning=warning,
    )


# --- Lifecycle -------------------------------------------------------------------------------------------


def lifecycle(roots: RegistryRoots, model_version_id: str) -> dict[str, Any]:
    """The version's lifecycle; active unless a valid record says otherwise.
    An unreadable record is an error, never silently active."""
    if not is_artifact_id(model_version_id):
        raise _fail("not a model version ID")
    path = roots.lifecycle / f"{model_version_id}.json"
    try:
        os.lstat(path)
    except FileNotFoundError:
        return {"changed_at": None, "reason": None, "state": "active"}
    try:
        record = _keys(
            _parsed_document(read_regular_file(path, 4096), path.name),
            {"changed_at", "reason", "schema", "state"},
            "a lifecycle record",
        )
        if record["schema"] != LIFECYCLE_SCHEMA or record["state"] not in LIFECYCLE_STATES:
            raise _fail("it names an unknown state")
        if not isinstance(record["reason"], str) or not record["reason"]:
            raise _fail("it gives no reason")
        if not isinstance(record["changed_at"], str):
            raise _fail("it gives no time")
    except (ArtifactUnavailableError, RegistryError) as error:
        raise _fail(f"the lifecycle record of {model_version_id} cannot be used: {error}") from error
    return {key: record[key] for key in ("changed_at", "reason", "state")}


def set_lifecycle(roots: RegistryRoots, model_version_id: str, state: str, reason: str) -> dict[str, Any]:
    """Replaces the version's lifecycle record atomically. The version must
    exist and verify; its bytes are never touched."""
    if state not in LIFECYCLE_STATES:
        raise _fail(f"the state is one of {', '.join(LIFECYCLE_STATES)}")
    if not reason or len(reason) > 200 or any(ord(char) < 0x20 for char in reason):
        raise _fail("the reason is 1 to 200 printable characters")
    verified_model_version(roots, model_version_id)
    nanoseconds = time.time_ns()
    seconds, fraction = divmod(nanoseconds, 1_000_000_000)
    changed_at = datetime.fromtimestamp(seconds, UTC).strftime("%Y-%m-%dT%H:%M:%S") + f".{fraction:09d}Z"
    record = {"changed_at": changed_at, "reason": reason, "schema": LIFECYCLE_SCHEMA, "state": state}
    roots.lifecycle.mkdir(parents=True, exist_ok=True)
    temporary = roots.lifecycle / f".{model_version_id}.{secrets.token_hex(4)}.tmp"
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    try:
        os.write(descriptor, canonical_json(record))
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.replace(temporary, roots.lifecycle / f"{model_version_id}.json")
    directory = os.open(roots.lifecycle, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return {key: record[key] for key in ("changed_at", "reason", "state")}


# --- Provenance checks at read time ----------------------------------------------------------------------


def source_experiment_status(experiments: Path, version: ModelVersion) -> dict[str, str]:
    """Whether the experiment this version came from is still present, its
    manifest identity and model file still verify, and it still holds
    exactly this payload for this instrument and family. A version does not
    depend on it: this only traces where the version came from. Its
    predictions were re-derived when the version was registered; this check
    re-runs nothing."""
    experiment_id = version.manifest["provenance"]["experiment_id"]
    try:
        directory = artifact_directory(experiments, experiment_id)
    except ArtifactUnavailableError:
        return {"experiment_id": experiment_id, "state": "missing"}
    try:
        manifest = verified_manifest(directory, "experiment_id", with_newline=True)
        stored = json.loads(verified_file(directory, manifest["files"][MODEL]))
        entry = stored["instruments"][version.instrument][version.family]
        same = canonical_json(entry) == version.payload
    except (ArtifactUnavailableError, KeyError, TypeError, ValueError) as error:
        return {
            "detail": str(error) or type(error).__name__,
            "experiment_id": experiment_id,
            "state": "invalid",
        }
    if not same:
        return {
            "detail": "the experiment's stored model differs from this version's payload",
            "experiment_id": experiment_id,
            "state": "differs",
        }
    if manifest["sources"] != version.manifest["provenance"]["sources"]:
        return {
            "detail": "the experiment names different sources",
            "experiment_id": experiment_id,
            "state": "differs",
        }
    return {"experiment_id": experiment_id, "state": "matches"}
