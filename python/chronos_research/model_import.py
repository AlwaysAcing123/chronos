"""Importing an ONNX model as a model version (ADR 0021).

An import is an uploader's manifest, `chronos.model_import/1`, and model
bytes. The manifest is strict JSON with no numbers. Everything it states is
recorded as declared by the uploader; what Chronos itself checked is
recorded separately, as verification statements.

The bytes are validated only inside the ONNX sandbox:
- the profile is checked and a session created;
- a fixed smoke corpus is run one row at a time, and its first row run
  again, with every output judged on the host.

Only then are those very bytes, still in memory, published as a model
version, and its name bound. An upload that fails any step never becomes a
version.

A declared information cutoff must say that it covers training,
preprocessing, tuning, and model selection. Without one, the version's
cutoff is unavailable, never zero, and no evaluation will treat it as
out-of-sample.
"""

from collections.abc import Mapping
from typing import Any

from chronos_research.artifacts import CLASS_ORDER, LABEL_SET
from chronos_research.encoding import canonical_json, decode_integer, sha256_hex
from chronos_research.onnx_profile import INPUT_NAME, INPUT_SHAPE, OUTPUT_NAME, OUTPUT_SHAPE, PROFILE
from chronos_research.onnx_sandbox import Sandbox, content_digest, context_files
from chronos_research.registered_inference import InvalidPredictionError, OnnxPredictor
from chronos_research.registry import (
    CUTOFF_SCOPE,
    DECLARED_CUTOFF_MEANING,
    FORMATS,
    INPUT_CONTRACT,
    INSTRUMENT,
    MODEL_NAME,
    MODEL_VERSION,
    ONNX_MODEL,
    OUTPUT_CONTRACT,
    RUNTIMES,
    SCHEMA,
    TARGET_DEFINITION,
    Registration,
    RegistryRoots,
    bind_name,
    publish_model_version,
    require_name_free,
    version_identity,
)

IMPORT_SCHEMA = {"name": "chronos.model_import", "version": "1"}
SMOKE_CORPUS = "chronos.onnx_smoke/1"
MAX_TRAINING_TEXT = 2000
MAX_SOURCE_TEXT = 500
_MANIFEST_KEYS = {
    "class_order", "declared", "input_tensor", "inputs", "instrument", "name", "output_tensor", "schema",
    "target", "version",
}  # fmt: skip

# Fixed rows of exact top_of_book_baseline/1 inputs (spread and price
# differences at 1e-9 dollars, imbalance at 1e-9, sizes at 1e-8 shares),
# covering tight and wide spreads, both imbalance extremes, and small and
# large books. Every one must give a valid prediction.
_SPREADS = (10_000_000, 20_000_000, 50_000_000, 1_000_000_000)
_IMBALANCES = (-1_000_000_000, -500_000_000, 0, 500_000_000, 1_000_000_000)
_MICROPRICE = (-5_000_000, 0, 2_500_000)
_SIZES = (100_000_000, 10_000_000_000, 1_000_000_000_000)
SMOKE_ROWS: tuple[tuple[int, ...], ...] = tuple(
    (_SPREADS[i % 4], _IMBALANCES[i % 5], _MICROPRICE[i % 3], _SIZES[i % 3], _SIZES[(i + 1) % 3])
    for i in range(24)
)


class ModelImportError(Exception):
    """An import is refused; why is its reason."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def _refuse(message: str, reason: str = "invalid_manifest") -> ModelImportError:
    return ModelImportError(reason, message)


def _exact(document: Any, keys: set[str], what: str) -> Mapping[str, Any]:
    if not isinstance(document, dict) or set(document) != keys:
        raise _refuse(f"{what} must have exactly the fields {', '.join(sorted(keys))}")
    return document


def _text(value: Any, what: str, maximum: int, empty: bool = False) -> str:
    if not isinstance(value, str) or (not value and not empty) or len(value) > maximum:
        raise _refuse(f"{what} must be text of {'0' if empty else '1'} to {maximum} characters")
    if any(ord(char) < 0x20 and char not in "\n\t" for char in value):
        raise _refuse(f"{what} must not contain control characters")
    return value


def _tensor(document: Any, name: str, shape: tuple[int, ...], what: str) -> None:
    tensor = _exact(document, {"dtype", "name", "shape"}, what)
    expected = {"dtype": "float64", "name": name, "shape": [str(dim) for dim in shape]}
    if dict(tensor) != expected:
        raise _refuse(f"{what} must be {expected}", "incompatible_contract")


def validated_import_manifest(document: Any) -> dict[str, Any]:
    """The uploader's manifest, checked and in canonical form."""
    manifest = _exact(document, _MANIFEST_KEYS, "the import manifest")
    if manifest["schema"] != IMPORT_SCHEMA:
        raise _refuse(f"the import manifest's schema is {IMPORT_SCHEMA}")
    name, version = manifest["name"], manifest["version"]
    if not isinstance(name, str) or not MODEL_NAME.fullmatch(name):
        raise _refuse("name is 1 to 64 characters from [a-z0-9._-], starting with a letter or digit")
    if not isinstance(version, str) or not MODEL_VERSION.fullmatch(version):
        raise _refuse("version is one to three dot-separated whole numbers")
    if not isinstance(manifest["instrument"], str) or not INSTRUMENT.fullmatch(manifest["instrument"]):
        raise _refuse("instrument must be one valid instrument identifier")
    if manifest["inputs"] != {"name": INPUT_CONTRACT["name"], "version": INPUT_CONTRACT["version"]}:
        raise _refuse("the inputs must be top_of_book_baseline version 1", "incompatible_contract")
    _tensor(manifest["input_tensor"], INPUT_NAME, INPUT_SHAPE, "input_tensor")
    _tensor(manifest["output_tensor"], OUTPUT_NAME, OUTPUT_SHAPE, "output_tensor")
    if manifest["class_order"] != list(CLASS_ORDER):
        raise _refuse("class_order must be down, flat, up", "incompatible_contract")
    target = _exact(manifest["target"], {"horizon_ns", "label_set"}, "target")
    try:
        horizon = decode_integer(target["horizon_ns"])
    except ValueError as error:
        raise _refuse("target.horizon_ns must be a canonical whole number") from error
    if target["label_set"] != LABEL_SET or not 0 < horizon <= 2**63 - 1:
        raise _refuse(f"the target must be {LABEL_SET} with a positive horizon", "incompatible_contract")
    declared = _exact(
        manifest["declared"], {"cutoff_covers", "information_cutoff_ns", "source", "training"}, "declared"
    )
    _text(declared["training"], "declared.training", MAX_TRAINING_TEXT)
    _text(declared["source"], "declared.source", MAX_SOURCE_TEXT, empty=True)
    cutoff = declared["information_cutoff_ns"]
    if cutoff is not None:
        try:
            value = decode_integer(cutoff)
        except ValueError as error:
            raise _refuse(
                "declared.information_cutoff_ns must be a canonical whole number or null"
            ) from error
        if not 0 < value <= 2**63 - 1:
            raise _refuse("declared.information_cutoff_ns must be a positive int64 nanosecond time")
        if declared["cutoff_covers"] != CUTOFF_SCOPE:
            raise _refuse(f"a declared cutoff must state that it covers exactly {CUTOFF_SCOPE!r}")
    elif declared["cutoff_covers"] is not None:
        raise _refuse("cutoff_covers is given only with a declared cutoff")
    return dict(manifest)


def _smoke(predictor: OnnxPredictor) -> tuple[list[list[str]], str]:
    outputs = []
    for row in SMOKE_ROWS:
        try:
            prediction = predictor.predict(row)
        except InvalidPredictionError as error:
            raise ModelImportError(error.reason, f"the smoke test failed on {list(row)}: {error}") from error
        outputs.append([repr(value) for value in prediction.probabilities])
    if [repr(value) for value in predictor.predict(SMOKE_ROWS[0]).probabilities] != outputs[0]:
        raise ModelImportError("nondeterministic_output", "the same row gave two different outputs")
    return outputs, sha256_hex(canonical_json(outputs))


def imported_version(request: Mapping[str, Any], model: bytes, sandbox: Sandbox) -> dict[str, Any]:
    """The model version of an import, after the sandbox accepted the bytes
    and every smoke row; raises otherwise."""
    predictor = OnnxPredictor(sandbox, model)
    try:
        _, outputs_sha256 = _smoke(predictor)
        report, runtime = predictor.session.report, predictor.session.onnxruntime
    finally:
        predictor.close()
    declared = request["declared"]
    cutoff: dict[str, Any] = (
        {"at_ns": declared["information_cutoff_ns"], "basis": "declared", "meaning": DECLARED_CUTOFF_MEANING}
        if declared["information_cutoff_ns"] is not None
        else {"basis": "unavailable", "reason": "not_declared"}
    )
    manifest: dict[str, Any] = {
        "family": "onnx",
        "format": FORMATS["onnx"],
        "inputs": INPUT_CONTRACT,
        "instrument": request["instrument"],
        "output": OUTPUT_CONTRACT,
        "payload": {"name": ONNX_MODEL, "sha256": sha256_hex(model), "size_bytes": str(len(model))},
        "provenance": {
            "declared": dict(declared),
            "manifest_sha256": sha256_hex(canonical_json(request)),
            "not_verified": {
                "cutoff": (
                    "the declared information cutoff: a file's checksum says nothing about when, or on what "
                    "data, it was trained, preprocessed, tuned, or selected"
                ),
                "target": "that the model was trained to predict this target at this horizon",
                "training": "how the model was trained, and on what data",
            },
            "origin": "import",
            "validation": {
                "onnxruntime": runtime,
                "profile": report,
                "sandbox_content_sha256": content_digest(context_files()),
                "smoke": {
                    "corpus": SMOKE_CORPUS,
                    "corpus_sha256": sha256_hex(
                        canonical_json([[str(v) for v in row] for row in SMOKE_ROWS])
                    ),
                    "outputs_sha256": outputs_sha256,
                    "rows": str(len(SMOKE_ROWS)),
                },
            },
        },
        "runtime": RUNTIMES["onnx"],
        "schema": SCHEMA,
        "target": {
            "definition": TARGET_DEFINITION,
            "horizon_ns": request["target"]["horizon_ns"],
            "label_set": LABEL_SET,
        },
        "training": {"information_cutoff": cutoff},
        "verification": {
            "artifact": "the payload is exactly the bytes the sandbox validated: their size and SHA-256",
            "isolation": (
                "the bytes were parsed, checked, and run only inside the restricted ONNX sandbox container: "
                "no network, a read-only filesystem with nothing of the host mounted, an unprivileged user, "
                "and bounded memory, CPU, processes, and time"
            ),
            "outputs": (
                "every smoke row gave exactly three finite probabilities from 0 to 1 summing to one within "
                "1e-12, judged on the host"
            ),
            "profile": (
                f"{PROFILE['name']}/{PROFILE['version']}: one double input [1, 5], one double output [1, 3], "
                "a small static graph of allowed default-domain operators, inline finite parameters, and no "
                "subgraph, function, or external data"
            ),
            "smoke_test": (
                f"{len(SMOKE_ROWS)} fixed rows of the top_of_book_baseline/1 inputs, one at a time, and the "
                "first again with identical output"
            ),
        },
    }
    return {**manifest, "model_version_id": version_identity(manifest)}


def import_model(
    roots: RegistryRoots, request: Mapping[str, Any], model: bytes, sandbox: Sandbox
) -> Registration:
    """Validates and publishes an import; nothing is published unless every
    check passed."""
    checked = validated_import_manifest(dict(request))
    name, version = checked["name"], checked["version"]
    manifest = imported_version(checked, model, sandbox)
    require_name_free(roots, name, version, manifest["model_version_id"])
    outcome, warning = publish_model_version(roots, manifest, model)
    binding_id, binding_outcome = bind_name(roots, name, version, manifest["model_version_id"])
    return Registration(
        model_version_id=manifest["model_version_id"],
        outcome=outcome.value,
        binding_id=binding_id,
        binding_outcome=binding_outcome.value,
        warning=warning,
    )
