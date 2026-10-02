"""Verifies a published experiment without retraining anything.

Checks the directory and file set; that every document is canonical with
exactly its expected keys; the experiment ID and every file's checksum; the
model parameters' structure and finiteness; every prediction row; that the
stored parameters reproduce every persisted logistic prediction within
INFERENCE_TOLERANCE and every majority prediction exactly; that the metrics
recomputed from the persisted predictions are the published ones; and that
the training and exclusion counts agree across the manifest, the models,
and the metrics. Anything malformed is an InspectionError, never a crash.
"""

import json
import math
import os
import stat
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import numpy as np

from chronos_research.artifacts import (
    CLASS_ORDER,
    FEATURE_SET,
    LABEL_SET,
    SPLIT_SET,
    SPLITS,
    RowIdentity,
    is_artifact_id,
)
from chronos_research.encoding import canonical_json, decode_float, decode_integer, sha256_hex
from chronos_research.experiment import (
    EVALUATION_SPLITS,
    MANIFEST,
    METRICS,
    METRICS_SCHEMA,
    MODEL,
    MODELS,
    MODELS_SCHEMA,
    PREDICTION_RECORD,
    PREDICTIONS,
    SCHEMA,
    SCOPE,
    Evaluated,
    ExperimentConfiguration,
    Predicted,
    evaluation_metrics,
    experiment_identity,
    model_feature_set_json,
)
from chronos_research.model_data import INPUTS, ModelRow, to_floats
from chronos_research.models import (
    INFERENCE_TOLERANCE,
    LogisticConfiguration,
    LogisticModel,
    MajorityModel,
    Unevaluable,
)

_FILES = (MANIFEST, METRICS, MODEL, PREDICTIONS)
_METRIC_TOLERANCE = 1e-12


class InspectionError(Exception):
    pass


def _fail(message: str) -> InspectionError:
    return InspectionError(message)


def _keys(document: Any, expected: set[str], what: str) -> Any:
    if not isinstance(document, dict) or document.keys() != expected:
        raise _fail(f"{what} must have exactly the keys {sorted(expected)}")
    return document


def _count(text: str, what: str) -> int:
    value = decode_integer(text)
    if value < 0 or value > 2**64 - 1:
        raise _fail(f"{what} must not be negative or exceed uint64")
    return value


def _metadata(manifest: Mapping[str, Any]) -> LogisticConfiguration:
    """Validate the declared experiment contract, even when checksums and
    the content-derived ID are internally consistent."""
    sources = _keys(
        manifest["sources"],
        {
            "dataset_id", "feature_artifact_id", "feature_set", "features_sha256", "horizon_ns",
            "label_artifact_id", "label_set", "labels_sha256", "split_artifact_id", "split_set",
            "splits_sha256", "stream", "test_start_at_ns", "validation_start_at_ns",
        },
        "sources",
    )  # fmt: skip
    for key in (
        "dataset_id",
        "feature_artifact_id",
        "features_sha256",
        "label_artifact_id",
        "labels_sha256",
        "split_artifact_id",
        "splits_sha256",
    ):
        if not isinstance(sources[key], str) or not is_artifact_id(sources[key]):
            raise _fail(f"sources.{key} must be a 64-digit lowercase ID or checksum")
    for key, expected in (("feature_set", FEATURE_SET), ("label_set", LABEL_SET), ("split_set", SPLIT_SET)):
        if sources[key] != expected:
            raise _fail(f"unsupported sources.{key}")
    if not isinstance(sources["stream"], str) or not sources["stream"]:
        raise _fail("sources.stream must be a nonempty string")
    horizon = _count(sources["horizon_ns"], "sources.horizon_ns")
    validation_start = _count(sources["validation_start_at_ns"], "sources.validation_start_at_ns")
    test_start = _count(sources["test_start_at_ns"], "sources.test_start_at_ns")
    if horizon == 0 or horizon > 2**63 - 1:
        raise _fail("sources.horizon_ns must be positive and fit int64")
    if not validation_start < test_start <= 2**63 - 1:
        raise _fail("sources split boundaries must increase and fit int64")
    environment = _keys(
        manifest["environment"],
        {
            "blas", "byteorder", "machine", "numpy", "platform", "python", "python_build",
            "scikit_learn", "scipy", "threadpoolctl", "training_threads",
        },
        "environment",
    )  # fmt: skip
    if any(not isinstance(value, str) or not value for value in environment.values()):
        raise _fail("environment fields must be nonempty strings")
    if environment["byteorder"] not in ("little", "big") or environment["training_threads"] != "1":
        raise _fail("unsupported experiment environment")
    expected_config = ExperimentConfiguration().as_json()
    config = _keys(manifest["configuration"], set(expected_config), "configuration")
    logistic = _keys(config["logistic"], set(expected_config["logistic"]), "configuration.logistic")
    configuration = LogisticConfiguration(
        regularization_c=decode_float(logistic["regularization_c"]),
        tolerance=decode_float(logistic["tolerance"]),
        maximum_iterations=_count(logistic["maximum_iterations"], "maximum_iterations"),
        minimum_training_rows_per_class=_count(
            logistic["minimum_training_rows_per_class"], "minimum_training_rows_per_class"
        ),
        random_state=_count(logistic["random_state"], "random_state"),
    )
    if (
        configuration.regularization_c <= 0
        or configuration.tolerance <= 0
        or configuration.maximum_iterations == 0
        or configuration.minimum_training_rows_per_class == 0
        or configuration.random_state > 2**32 - 1
    ):
        raise _fail("invalid logistic configuration parameters")
    expected_config = ExperimentConfiguration(logistic=configuration).as_json()
    # Inspection can run under another library version; validate the stored
    # estimator against the stored environment rather than today's version.
    expected_config["logistic"]["estimator"] = (
        f"sklearn.linear_model.LogisticRegression {environment['scikit_learn']}"
    )
    if config != expected_config:
        raise _fail("unsupported experiment configuration")
    if manifest["scope"] != SCOPE:
        raise _fail("unsupported experiment scope")
    return configuration


def _canonical_document(data: bytes, name: str) -> Any:
    try:
        document = json.loads(data)
    except ValueError as error:
        raise _fail(f"{name} is not JSON: {error}") from error
    if canonical_json(document) != data:
        raise _fail(f"{name} is not in canonical form")
    return document


def _floats(values: Sequence[str], length: int, what: str) -> np.ndarray[Any, np.dtype[np.float64]]:
    if not isinstance(values, list) or len(values) != length:
        raise _fail(f"{what} must have {length} values")
    try:
        return np.array([decode_float(value) for value in values], dtype=np.float64)
    except (TypeError, ValueError) as error:
        raise _fail(f"{what}: {error}") from error


def _unevaluable(document: Any, what: str) -> Unevaluable:
    _keys(document, {"detail", "reason", "status"}, what)
    if (
        document["status"] != "unevaluable"
        or not isinstance(document["reason"], str)
        or not document["reason"]
        or not isinstance(document["detail"], dict)
        or any(not isinstance(value, str) for value in document["detail"].values())
    ):
        raise _fail(f"{what} must name its reason")
    return Unevaluable(reason=document["reason"], detail=document["detail"])


def _models(
    document: Mapping[str, Any], configuration: LogisticConfiguration
) -> dict[str, dict[str, MajorityModel | LogisticModel | Unevaluable]]:
    _keys(document, {"class_order", "instruments", "model_feature_set", "schema"}, MODEL)
    if document.get("schema") != MODELS_SCHEMA or document.get("class_order") != list(CLASS_ORDER):
        raise _fail("model.json has an unexpected schema or class order")
    if document.get("model_feature_set") != model_feature_set_json():
        raise _fail("model.json has an unexpected model feature set")
    models: dict[str, dict[str, MajorityModel | LogisticModel | Unevaluable]] = {}
    for instrument, entry in document["instruments"].items():
        if not instrument:
            raise _fail("a model instrument must not be empty")
        _keys(entry, {"logistic", "majority"}, f"{MODEL} {instrument}")
        majority_json = entry["majority"]
        majority: MajorityModel | Unevaluable
        if majority_json.get("status") == "unevaluable":
            majority = _unevaluable(majority_json, f"{instrument} majority")
        else:
            _keys(
                majority_json, {"class_counts", "majority_class", "probabilities"}, f"{instrument} majority"
            )
            _keys(majority_json["class_counts"], set(CLASS_ORDER), f"{instrument} majority class counts")
            counts = tuple(
                _count(majority_json["class_counts"][name], "a class count") for name in CLASS_ORDER
            )
            if sum(counts) == 0:
                raise _fail(f"{instrument}: a fitted majority model has training rows")
            majority = MajorityModel(counts)
            if majority.as_json() != majority_json:
                raise _fail(
                    f"{instrument}: the majority model's probabilities or class do not follow from its counts"
                )
        logistic_json = entry["logistic"]
        logistic: LogisticModel | Unevaluable
        if logistic_json["status"] == "unevaluable":
            logistic = _unevaluable(logistic_json, f"{instrument} logistic")
        elif logistic_json["status"] == "fitted":
            classes, inputs = len(CLASS_ORDER), len(INPUTS)
            logistic = LogisticModel(
                mean=_floats(logistic_json["scaler"]["mean"], inputs, f"{instrument} scaler mean"),
                scale=_floats(logistic_json["scaler"]["scale"], inputs, f"{instrument} scaler scale"),
                coefficients=np.stack(
                    [
                        _floats(
                            logistic_json["coefficients"][name], inputs, f"{instrument} {name} coefficients"
                        )
                        for name in CLASS_ORDER
                    ]
                ),
                intercepts=_floats(
                    [logistic_json["intercepts"][name] for name in CLASS_ORDER], classes, "intercepts"
                ),
                iterations=decode_integer(logistic_json["iterations"]),
            )
            if (
                not np.all(logistic.scale > 0)
                or logistic.as_json() != logistic_json
                or not 0 < logistic.iterations < configuration.maximum_iterations
            ):
                raise _fail(f"{instrument}: the logistic model's parameters are malformed")
            if (
                not isinstance(majority, MajorityModel)
                or min(majority.class_counts) < configuration.minimum_training_rows_per_class
            ):
                raise _fail(f"{instrument}: a fitted logistic model lacks training class support")
        else:
            raise _fail(f"{instrument}: unknown logistic status {logistic_json['status']!r}")
        models[instrument] = {"majority": majority, "logistic": logistic}
    return models


def _prediction(record: Mapping[str, Any] | None, model: MajorityModel | LogisticModel | Unevaluable) -> Any:
    if isinstance(model, Unevaluable):
        if record is not None:
            raise _fail("a prediction exists for an unevaluable model")
        return None
    if record is None:
        raise _fail("a prediction is missing for an evaluable model")
    _keys(record, {"class", "probabilities"}, "a prediction")
    if record["class"] not in CLASS_ORDER:
        raise _fail("a prediction names an unknown class")
    probabilities = _floats(record["probabilities"], len(CLASS_ORDER), "probabilities")
    if np.any(probabilities < 0) or abs(float(probabilities.sum()) - 1.0) > 1e-9:
        raise _fail("predicted probabilities must be nonnegative and add up to one")
    return CLASS_ORDER.index(record["class"]), probabilities


def _same(published: Any, recomputed: Any, path: str = "metrics") -> None:
    if isinstance(published, dict) and isinstance(recomputed, dict):
        if published.keys() != recomputed.keys():
            raise _fail(f"{path} has different entries than its recomputation")
        for key in published:
            if key == "value":
                expected, actual = decode_float(recomputed[key]), decode_float(published[key])
                if not math.isclose(expected, actual, rel_tol=_METRIC_TOLERANCE, abs_tol=_METRIC_TOLERANCE):
                    raise _fail(f"{path} is {actual}, but the predictions give {expected}")
            else:
                _same(published[key], recomputed[key], f"{path}.{key}")
    elif published != recomputed:
        raise _fail(f"{path} differs from its recomputation")


def _checked_predictions(
    model: MajorityModel | LogisticModel | Unevaluable, rows: Sequence[ModelRow], persisted: Sequence[Any]
) -> Predicted:
    """The persisted predictions, once the stored model reproduces them:
    exactly for the majority model, within INFERENCE_TOLERANCE for the
    logistic one."""
    if isinstance(model, Unevaluable):
        return model
    classes = np.array([entry[0] for entry in persisted], dtype=np.int64)
    probabilities = (
        np.stack([entry[1] for entry in persisted]) if persisted else np.zeros((0, len(CLASS_ORDER)))
    )
    if isinstance(model, MajorityModel):
        expected_classes, expected_probabilities = model.predict(len(rows))
        tolerance = 0.0
    else:
        expected_classes, expected_probabilities = model.predict(to_floats([row.inputs for row in rows]))
        tolerance = INFERENCE_TOLERANCE
    if not np.array_equal(classes, expected_classes) or (
        len(rows) and float(np.max(np.abs(probabilities - expected_probabilities))) > tolerance
    ):
        raise _fail("persisted predictions differ from the stored model's")
    return classes, probabilities


def verify_experiment(directory: Path) -> dict[str, Any]:
    """Returns the manifest of a valid experiment directory."""
    try:
        return _verify(directory)
    except InspectionError:
        raise
    except (
        AttributeError,
        IndexError,
        KeyError,
        TypeError,
        ValueError,
        OverflowError,
        ZeroDivisionError,
    ) as error:
        raise _fail(
            f"{directory.name} is not a well-formed experiment ({type(error).__name__}: {error})"
        ) from error


def _verify(directory: Path) -> dict[str, Any]:
    if directory.is_symlink() or not directory.is_dir():
        raise _fail(f"{directory} is not an experiment directory")
    if not is_artifact_id(directory.name):
        raise _fail("an experiment directory is named by its 64-digit lowercase experiment ID")
    if sorted(os.listdir(directory)) != sorted(_FILES):
        raise _fail(f"an experiment holds exactly {', '.join(_FILES)}")
    contents: dict[str, bytes] = {}
    for name in _FILES:
        if not stat.S_ISREG(os.lstat(directory / name).st_mode):
            raise _fail(f"{name} must be a regular file")
        contents[name] = (directory / name).read_bytes()

    manifest = _keys(
        _canonical_document(contents[MANIFEST], MANIFEST),
        {
            "class_order", "configuration", "counts", "environment", "experiment_id", "files",
            "model_feature_set", "schema", "scope", "sources",
        },
        MANIFEST,
    )  # fmt: skip
    if manifest.get("schema") != SCHEMA:
        raise _fail(f"unsupported experiment schema {manifest.get('schema')}")
    if (
        experiment_identity(manifest) != manifest["experiment_id"]
        or manifest["experiment_id"] != directory.name
    ):
        raise _fail("the experiment ID does not match the manifest's content or the directory name")
    if (
        manifest["class_order"] != list(CLASS_ORDER)
        or manifest["model_feature_set"] != model_feature_set_json()
    ):
        raise _fail("the manifest has an unexpected class order or model feature set")
    if sorted(manifest["files"]) != sorted([METRICS, MODEL, PREDICTIONS]):
        raise _fail("the manifest must name exactly the experiment's three data files")
    for name in (METRICS, MODEL, PREDICTIONS):
        digest = {"name": name, "sha256": sha256_hex(contents[name]), "size_bytes": str(len(contents[name]))}
        if manifest["files"][name] != digest:
            raise _fail(f"{name} differs from the manifest's checksum or size")

    configuration = _metadata(manifest)
    models = _models(_canonical_document(contents[MODEL], MODEL), configuration)
    rows: dict[str, dict[str, list[ModelRow]]] = {
        split: {instrument: [] for instrument in models} for split in EVALUATION_SPLITS
    }
    persisted: dict[str, dict[str, dict[str, list[Any]]]] = {
        split: {instrument: {model: [] for model in MODELS} for instrument in models}
        for split in EVALUATION_SPLITS
    }
    seen: set[RowIdentity] = set()
    for number, line in enumerate(contents[PREDICTIONS].splitlines(keepends=True), start=1):
        record = _keys(
            _canonical_document(line, f"{PREDICTIONS} line {number}"),
            {"identity", "inputs_raw", "predictions", "record", "return_raw", "split", "true_class"},
            f"{PREDICTIONS} line {number}",
        )
        _keys(
            record["identity"],
            {"ingest_sequence", "instrument", "stream"},
            f"{PREDICTIONS} line {number} identity",
        )
        _keys(record["predictions"], set(MODELS), f"{PREDICTIONS} line {number} predictions")
        identity = RowIdentity(
            record["identity"]["stream"],
            record["identity"]["instrument"],
            _count(record["identity"]["ingest_sequence"], "ingest_sequence"),
        )
        if identity.stream != manifest["sources"]["stream"]:
            raise _fail(f"{PREDICTIONS} line {number} differs from the source stream")
        if (
            record["record"] != PREDICTION_RECORD
            or record["split"] not in EVALUATION_SPLITS
            or identity in seen
        ):
            raise _fail(f"{PREDICTIONS} line {number} is not a prediction of a new evaluated row")
        if identity.instrument not in models or record["true_class"] not in CLASS_ORDER:
            raise _fail(f"{PREDICTIONS} line {number} names an unknown instrument or class")
        seen.add(identity)
        row = ModelRow(
            identity=identity,
            split=record["split"],
            inputs=tuple(decode_integer(value) for value in record["inputs_raw"]),
            class_index=CLASS_ORDER.index(record["true_class"]),
            return_raw=decode_integer(record["return_raw"]),
        )
        if len(row.inputs) != len(INPUTS):
            raise _fail(f"{PREDICTIONS} line {number} has the wrong number of inputs")
        rows[row.split][identity.instrument].append(row)
        for model_name in MODELS:
            persisted[row.split][identity.instrument][model_name].append(
                _prediction(record["predictions"][model_name], models[identity.instrument][model_name])
            )

    evaluated: dict[str, dict[str, Evaluated]] = {
        split: {
            instrument: Evaluated(
                rows=rows[split][instrument],
                models={
                    model_name: _checked_predictions(
                        model, rows[split][instrument], persisted[split][instrument][model_name]
                    )
                    for model_name, model in instrument_models.items()
                },
            )
            for instrument, instrument_models in models.items()
        }
        for split in EVALUATION_SPLITS
    }

    metrics = _keys(
        _canonical_document(contents[METRICS], METRICS), {"excluded", "schema", "splits", "training"}, METRICS
    )
    if metrics.get("schema") != METRICS_SCHEMA:
        raise _fail("metrics.json has an unexpected schema")
    _same(metrics["splits"], evaluation_metrics(evaluated))
    counts = _keys(manifest["counts"], {"evaluated", "excluded", "training"}, "the manifest's counts")
    _keys(counts["evaluated"], set(EVALUATION_SPLITS), "evaluated counts")
    for split in EVALUATION_SPLITS:
        published = {instrument: _count(n, "a count") for instrument, n in counts["evaluated"][split].items()}
        actual = {instrument: len(part) for instrument, part in rows[split].items() if part}
        if published != actual:
            raise _fail(f"the manifest's {split} counts differ from the predictions")
    _check_training_and_exclusions(models, metrics, counts)
    return dict(manifest)


def _check_training_and_exclusions(
    models: Mapping[str, Mapping[str, MajorityModel | LogisticModel | Unevaluable]],
    metrics: Mapping[str, Any],
    counts: Mapping[str, Any],
) -> None:
    """Training rows are what the majority models counted; exclusions add up
    to the manifest's per-period totals."""
    training: dict[str, Any] = {}
    for instrument, instrument_models in models.items():
        majority = instrument_models["majority"]
        class_counts = (
            majority.class_counts if isinstance(majority, MajorityModel) else (0,) * len(CLASS_ORDER)
        )
        training[instrument] = {
            "rows": str(sum(class_counts)),
            "true_distribution": dict(zip(CLASS_ORDER, map(str, class_counts), strict=True)),
        }
    if metrics["training"] != training:
        raise _fail("metrics.json's training counts differ from the majority models' class counts")
    if counts["training"] != {
        name: entry["rows"] for name, entry in training.items() if entry["rows"] != "0"
    }:
        raise _fail("the manifest's training counts differ from the models'")
    excluded = _keys(metrics["excluded"], set(SPLITS), "metrics.json's exclusions")
    totals: dict[str, str] = {}
    for period, by_instrument in excluded.items():
        total = 0
        for instrument, reasons in by_instrument.items():
            if instrument not in models or not isinstance(reasons, dict):
                raise _fail(f"metrics.json excludes rows of an unknown instrument {instrument!r}")
            for reason, count in reasons.items():
                if not isinstance(reason, str) or _count(count, "an excluded count") == 0:
                    raise _fail("excluded counts must be positive")
                total += int(count)
        totals[period] = str(total)
    if counts["excluded"] != totals:
        raise _fail("the manifest's excluded counts differ from metrics.json's")
