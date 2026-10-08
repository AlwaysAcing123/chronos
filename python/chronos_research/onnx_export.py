"""Exporting a registered logistic version as an ONNX import (ADR 0021).

The export builds a chronos.onnx_classifier/1 graph from the version's
verified parameters, in the same order of operations as the row-pure
logistic runtime:

    probabilities = Softmax(((inputs - mean) / scale) @ coefficients^T + intercepts)

It writes a new directory holding model.onnx, an import manifest whose
declared cutoff is the source version's verified one, and a parity report.
The report runs every one of the source experiment's feature rows for the
instrument through both the row-pure runtime on the host and the exported
model in the ONNX sandbox, and reports three things separately: the largest
probability difference against a stated tolerance, class agreement, and
agreement of the long_flat_threshold policy's signal at the default
threshold. Floating-point summation inside the runtime may differ from the
host's; the report measures that, it does not assume it away.

Building the graph needs the optional `onnx` package on the host; parsing
and running it happens only in the sandbox, as for any import.
"""

import json
import os
import secrets
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from chronos_research.artifacts import CLASS_ORDER, LABEL_SET, Unavailable, read_feature_rows
from chronos_research.encoding import canonical_json, encode_float, sha256_hex
from chronos_research.model_data import exact_inputs
from chronos_research.model_import import IMPORT_SCHEMA, ModelImportError, validated_import_manifest
from chronos_research.models import LogisticModel
from chronos_research.onnx_profile import INPUT_NAME, INPUT_SHAPE, OUTPUT_NAME, OUTPUT_SHAPE
from chronos_research.onnx_sandbox import Sandbox
from chronos_research.pipeline import ArtifactRoots, Tools, verified_sources
from chronos_research.registered_inference import BaselinePredictor, OnnxPredictor, RowPrediction
from chronos_research.registry import CUTOFF_SCOPE, INPUT_CONTRACT, ModelVersion

PARITY_SCHEMA = {"name": "chronos.onnx_export_parity", "version": "1"}
# The live checker's and the experiment's inference tolerance.
PARITY_PROBABILITY_TOLERANCE = 1e-12
DEFAULT_THRESHOLD_PPB = 500_000_000
IR_VERSION = 8
OPSET = 17


def logistic_graph(model: LogisticModel) -> bytes:
    """The serialized ONNX model of these parameters."""
    try:
        import numpy as np
        from onnx import TensorProto, helper, numpy_helper
    except ImportError as error:
        raise ModelImportError(
            "onnx_unavailable",
            "exporting builds the graph with the optional onnx package: pip install .[onnx]",
        ) from error

    def initializer(name: str, values: Any) -> Any:
        return numpy_helper.from_array(np.ascontiguousarray(values, dtype=np.float64), name)

    graph = helper.make_graph(
        [
            helper.make_node("Sub", [INPUT_NAME, "mean"], ["centered"]),
            helper.make_node("Div", ["centered", "scale"], ["standardized"]),
            helper.make_node("MatMul", ["standardized", "coefficients"], ["products"]),
            helper.make_node("Add", ["products", "intercepts"], ["logits"]),
            helper.make_node("Softmax", ["logits"], [OUTPUT_NAME], axis=-1),
        ],
        "chronos_logistic",
        [helper.make_tensor_value_info(INPUT_NAME, TensorProto.DOUBLE, list(INPUT_SHAPE))],
        [helper.make_tensor_value_info(OUTPUT_NAME, TensorProto.DOUBLE, list(OUTPUT_SHAPE))],
        [
            initializer("mean", model.mean),
            initializer("scale", model.scale),
            initializer("coefficients", model.coefficients.T),
            initializer("intercepts", model.intercepts),
        ],
    )
    exported = helper.make_model(
        graph,
        opset_imports=[helper.make_opsetid("", OPSET)],
        ir_version=IR_VERSION,
        producer_name="chronos-research export-onnx",
    )
    return bytes(exported.SerializeToString(deterministic=True))


def _signal(prediction: RowPrediction, threshold_ppb: int) -> str:
    """The long_flat_threshold policy's signal for one prediction."""
    if prediction.confidence_ppb < threshold_ppb:
        return "hold"
    return {"up": "target_long", "down": "target_flat"}.get(CLASS_ORDER[prediction.class_index], "hold")


def parity_report(
    reference: BaselinePredictor, candidate: OnnxPredictor, rows: Sequence[tuple[int, ...]]
) -> dict[str, Any]:
    largest = 0.0
    class_disagreements, signal_disagreements = [], []
    for index, row in enumerate(rows):
        expected, actual = reference.predict(row), candidate.predict(row)
        difference = max(
            abs(a - b) for a, b in zip(expected.probabilities, actual.probabilities, strict=True)
        )
        largest = max(largest, difference)
        if expected.class_index != actual.class_index:
            class_disagreements.append(index)
        if _signal(expected, DEFAULT_THRESHOLD_PPB) != _signal(actual, DEFAULT_THRESHOLD_PPB):
            signal_disagreements.append(index)
    within = largest <= PARITY_PROBABILITY_TOLERANCE
    return {
        "classes": {
            "agree": str(len(rows) - len(class_disagreements)),
            "disagreeing_rows": [str(i) for i in class_disagreements[:20]],
            "rows": str(len(rows)),
        },
        "policy": {
            "agree": str(len(rows) - len(signal_disagreements)),
            "disagreeing_rows": [str(i) for i in signal_disagreements[:20]],
            "policy": "long_flat_threshold/1",
            "threshold_ppb": str(DEFAULT_THRESHOLD_PPB),
        },
        "probabilities": {
            "largest_difference": encode_float(largest),
            "tolerance": encode_float(PARITY_PROBABILITY_TOLERANCE),
            "within_tolerance": within,
        },
        "rows": str(len(rows)),
        "schema": PARITY_SCHEMA,
        "status": "agrees" if within and not class_disagreements and not signal_disagreements else "differs",
    }


def _source_rows(tools: Tools, artifacts: Path, version: ModelVersion) -> list[tuple[int, ...]]:
    """Every available feature row of the version's instrument in the
    sources it was trained from, verified again with chronos_dataset."""
    split_id = version.manifest["provenance"]["sources"]["split_artifact_id"]
    sources = verified_sources(tools, ArtifactRoots.under(artifacts), split_id, reproduce_features=False)
    inputs = [
        exact_inputs(row)
        for row in read_feature_rows(sources.features)
        if row.identity.instrument == version.instrument
    ]
    return [value for value in inputs if not isinstance(value, Unavailable)]


def import_manifest(version: ModelVersion, name: str, version_label: str) -> dict[str, Any]:
    cutoff = version.manifest["training"]["information_cutoff"]
    experiment_id = version.manifest["provenance"]["experiment_id"]
    return validated_import_manifest(
        {
            "class_order": list(CLASS_ORDER),
            "declared": {
                "cutoff_covers": CUTOFF_SCOPE,
                "information_cutoff_ns": cutoff["at_ns"],
                "source": f"chronos model version {version.model_version_id}",
                "training": (
                    "exported by chronos-research export-onnx from the registered logistic version "
                    f"{version.model_version_id} of experiment {experiment_id}, whose cutoff Chronos "
                    "verified as the split's validation start"
                ),
            },
            "input_tensor": {"dtype": "float64", "name": INPUT_NAME, "shape": [str(d) for d in INPUT_SHAPE]},
            "inputs": {"name": INPUT_CONTRACT["name"], "version": INPUT_CONTRACT["version"]},
            "instrument": version.instrument,
            "name": name,
            "output_tensor": {
                "dtype": "float64",
                "name": OUTPUT_NAME,
                "shape": [str(d) for d in OUTPUT_SHAPE],
            },
            "schema": IMPORT_SCHEMA,
            "target": {"horizon_ns": version.manifest["target"]["horizon_ns"], "label_set": LABEL_SET},
            "version": version_label,
        }
    )


def export_logistic(
    tools: Tools,
    artifacts: Path,
    version: ModelVersion,
    output: Path,
    name: str,
    version_label: str,
    sandbox: Sandbox,
) -> dict[str, Any]:
    """Writes the export into `output`, which must not exist yet."""
    if not isinstance(version.model, LogisticModel):
        raise ModelImportError("not_exportable", "only a registered logistic version can be exported")
    manifest = import_manifest(version, name, version_label)
    model = logistic_graph(version.model)
    rows = _source_rows(tools, artifacts, version)
    reference, candidate = BaselinePredictor(version.model), OnnxPredictor(sandbox, model)
    try:
        report = parity_report(reference, candidate, rows)
    finally:
        candidate.close()
    report["source_model_version_id"] = version.model_version_id
    report["model_sha256"] = sha256_hex(model)
    staging = output.parent / f".{output.name}.{secrets.token_hex(4)}.tmp"
    staging.mkdir()
    files = {
        "model.onnx": model,
        "import-manifest.json": json.dumps(manifest, indent=2, sort_keys=True).encode() + b"\n",
        "parity.json": canonical_json(report),
    }
    for file_name, data in files.items():
        (staging / file_name).write_bytes(data)
    try:
        if output.exists():
            raise FileExistsError(output)
        os.rename(staging, output)
    except OSError as error:
        for file_name in files:
            (staging / file_name).unlink()
        staging.rmdir()
        raise ModelImportError("output_exists", f"{output} already exists") from error
    return {
        "manifest": manifest,
        "model_sha256": report["model_sha256"],
        "output": str(output),
        "parity": report,
    }
