"""A baseline experiment: models fitted per instrument on the training split,
evaluated on validation and test, and published as an immutable artifact:

    <experiment-root>/<experiment-id>/manifest.json
    <experiment-root>/<experiment-id>/model.json         fitted parameters
    <experiment-root>/<experiment-id>/metrics.json       metrics and exclusions
    <experiment-root>/<experiment-id>/predictions.ndjson one row per evaluated row

The experiment ID is the SHA-256 of the canonical manifest without the ID.
The manifest names every source artifact, the configuration, the software
environment, and each file's checksum, so the same sources, configuration,
and environment give the same ID and bytes.
"""

import platform
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

import numpy as np
import numpy.typing as npt
import scipy
import sklearn
import threadpoolctl

from chronos_research.artifacts import (
    CLASS_ORDER,
    FEATURE_SET,
    LABEL_SET,
    SPLIT_SET,
    RowIdentity,
    VerifiedSources,
    read_feature_rows,
    read_label_rows,
    read_split_rows,
)
from chronos_research.encoding import canonical_json, encode_float, encode_integer, sha256_hex
from chronos_research.metrics import classification_metrics, return_distribution
from chronos_research.model_data import (
    INPUT_DEFINITIONS,
    INPUT_SCALES,
    INPUTS,
    MODEL_FEATURE_SET,
    ModelData,
    ModelRow,
    join,
    to_floats,
)
from chronos_research.models import (
    INFERENCE_TOLERANCE,
    FittedLogistic,
    LogisticConfiguration,
    MajorityModel,
    Unevaluable,
    fit_logistic,
    fit_majority,
    require_parity,
)

SCHEMA = {"name": "chronos.baseline_experiment", "version": "1"}
MODELS_SCHEMA = {"name": "chronos.baseline_models", "version": "1"}
METRICS_SCHEMA = {"name": "chronos.baseline_metrics", "version": "1"}
PREDICTION_RECORD = {"name": "chronos.baseline_prediction", "version": "1"}
MANIFEST = "manifest.json"
MODEL = "model.json"
METRICS = "metrics.json"
PREDICTIONS = "predictions.ndjson"
EVALUATION_SPLITS = ("validation", "test")
MODELS = ("majority", "logistic")
SCOPE = (
    "predictive evaluation only: no trading strategy, position sizing, execution model, costs, or profit "
    "and loss; metrics from a short sample are a pipeline check, not evidence of skill or profitability"
)

type Classes = npt.NDArray[np.int64]
type Probabilities = npt.NDArray[np.float64]
type Predicted = tuple[Classes, Probabilities] | Unevaluable


@dataclass(frozen=True)
class ExperimentConfiguration:
    logistic: LogisticConfiguration = field(default_factory=LogisticConfiguration)

    def as_json(self) -> dict[str, Any]:
        return {
            "aggregate": "pooled over the evaluated rows of every instrument the model is evaluable for",
            "evaluation_splits": list(EVALUATION_SPLITS),
            "inference_tolerance": encode_float(INFERENCE_TOLERANCE),
            "logistic": self.logistic.as_json(),
            "majority": {
                "prediction": "most frequent training class; ties go to the earliest class in class_order",
                "probabilities": "training class frequencies",
            },
            "models_per": "instrument",
            "training_split": "train",
        }


@dataclass(frozen=True)
class Evaluated:
    """One instrument's rows in one evaluation split, with each model's
    predictions or why it has none."""

    rows: Sequence[ModelRow]
    models: Mapping[str, Predicted]

    @property
    def true_classes(self) -> Classes:
        return np.array([row.class_index for row in self.rows], dtype=np.int64)


def environment() -> dict[str, Any]:
    blas = np.__config__.CONFIG["Build Dependencies"]["blas"]
    return {
        "blas": f"{blas['name']} {blas['version']}",
        "machine": platform.machine(),
        "numpy": np.__version__,
        "platform": platform.platform(),
        "python": f"{platform.python_implementation()} {platform.python_version()}",
        "python_build": " ".join(platform.python_build()),
        "scikit_learn": sklearn.__version__,
        "scipy": scipy.__version__,
        "threadpoolctl": threadpoolctl.__version__,
        "training_threads": "1",
        "byteorder": sys.byteorder,
    }


def _source_json(sources: VerifiedSources) -> dict[str, Any]:
    return {
        "dataset_id": sources.dataset_id,
        "feature_artifact_id": sources.features.artifact_id,
        "feature_set": FEATURE_SET,
        "features_sha256": sources.features.rows_sha256,
        "horizon_ns": encode_integer(sources.horizon_ns),
        "label_artifact_id": sources.labels.artifact_id,
        "label_set": LABEL_SET,
        "labels_sha256": sources.labels.rows_sha256,
        "split_artifact_id": sources.splits.artifact_id,
        "split_set": SPLIT_SET,
        "splits_sha256": sources.splits.rows_sha256,
        "stream": sources.stream,
        "test_start_at_ns": encode_integer(sources.test_start_at_ns),
        "validation_start_at_ns": encode_integer(sources.validation_start_at_ns),
    }


def model_feature_set_json() -> dict[str, Any]:
    return {
        **MODEL_FEATURE_SET,
        "inputs": [
            {"definition": INPUT_DEFINITIONS[name], "name": name, "scale": encode_integer(scale)}
            for name, scale in zip(INPUTS, INPUT_SCALES, strict=True)
        ],
    }


def _predicted_json(predicted: Predicted, index: int) -> dict[str, Any] | None:
    if isinstance(predicted, Unevaluable):
        return None
    classes, probabilities = predicted
    return {
        "class": CLASS_ORDER[int(classes[index])],
        "probabilities": [encode_float(float(p)) for p in probabilities[index]],
    }


def prediction_record(row: ModelRow, models: Mapping[str, Predicted], index: int) -> dict[str, Any]:
    return {
        "identity": {
            "ingest_sequence": encode_integer(row.identity.ingest_sequence),
            "instrument": row.identity.instrument,
            "stream": row.identity.stream,
        },
        "inputs_raw": [encode_integer(value) for value in row.inputs],
        "predictions": {name: _predicted_json(models[name], index) for name in MODELS},
        "record": PREDICTION_RECORD,
        "return_raw": encode_integer(row.return_raw),
        "split": row.split,
        "true_class": CLASS_ORDER[row.class_index],
    }


def evaluation_metrics(evaluated: Mapping[str, Mapping[str, Evaluated]]) -> dict[str, Any]:
    """Per split, per instrument and pooled, for each model; shared by the
    experiment and by inspection, which recomputes it from predictions."""
    document: dict[str, Any] = {}
    for split in EVALUATION_SPLITS:
        instruments: dict[str, Any] = {}
        pooled: dict[str, list[tuple[Classes, Classes, Probabilities]]] = {name: [] for name in MODELS}
        pooled_instruments: dict[str, list[str]] = {name: [] for name in MODELS}
        for instrument, part in sorted(evaluated[split].items()):
            entry: dict[str, Any] = {
                "return_distribution": return_distribution(
                    [row.class_index for row in part.rows], [row.return_raw for row in part.rows]
                ),
            }
            for name in MODELS:
                predicted = part.models[name]
                if isinstance(predicted, Unevaluable):
                    entry[name] = {"unevaluable": predicted.reason}
                    continue
                classes, probabilities = predicted
                entry[name] = classification_metrics(part.true_classes, classes, probabilities)
                pooled[name].append((part.true_classes, classes, probabilities))
                pooled_instruments[name].append(instrument)
            instruments[instrument] = entry
        aggregate: dict[str, Any] = {}
        for name in MODELS:
            parts = pooled[name]
            truth = np.concatenate([p[0] for p in parts]) if parts else np.zeros(0, dtype=np.int64)
            classes = np.concatenate([p[1] for p in parts]) if parts else np.zeros(0, dtype=np.int64)
            probabilities = (
                np.concatenate([p[2] for p in parts]) if parts else np.zeros((0, len(CLASS_ORDER)))
            )
            aggregate[name] = {
                "instruments": pooled_instruments[name],
                "metrics": classification_metrics(truth, classes, probabilities),
            }
        document[split] = {"aggregate": aggregate, "instruments": instruments}
    return document


@dataclass(frozen=True)
class Experiment:
    experiment_id: str
    # Every file, the manifest included.
    files: Mapping[str, bytes]


def _by_instrument(rows: Sequence[ModelRow], split: str) -> dict[str, list[ModelRow]]:
    grouped: dict[str, list[ModelRow]] = {}
    for row in rows:
        if row.split == split:
            grouped.setdefault(row.identity.instrument, []).append(row)
    return grouped


def _logistic_predictions(logistic: FittedLogistic | Unevaluable, inputs: Probabilities) -> Predicted:
    if isinstance(logistic, Unevaluable):
        return logistic
    require_parity(logistic, inputs)
    return logistic.model.predict(inputs)


def build_experiment(
    sources: VerifiedSources, data: ModelData, configuration: ExperimentConfiguration
) -> Experiment:
    instruments = sorted(
        {row.identity.instrument for row in data.rows}
        | {instrument for period in data.excluded.values() for instrument in period}
    )
    training = _by_instrument(data.rows, "train")
    evaluation = {split: _by_instrument(data.rows, split) for split in EVALUATION_SPLITS}
    models_json: dict[str, Any] = {}
    training_json: dict[str, Any] = {}
    evaluated: dict[str, dict[str, Evaluated]] = {split: {} for split in EVALUATION_SPLITS}
    records: dict[RowIdentity, dict[str, Any]] = {}
    for instrument in instruments:
        train_rows = training.get(instrument, [])
        train_classes = np.array([row.class_index for row in train_rows], dtype=np.int64)
        majority: MajorityModel | Unevaluable
        logistic: FittedLogistic | Unevaluable
        if train_rows:
            majority = fit_majority(train_classes)
            logistic = fit_logistic(
                to_floats([row.inputs for row in train_rows]), train_classes, configuration.logistic
            )
        else:
            majority = logistic = Unevaluable(reason="no_training_rows", detail={})
        models_json[instrument] = {
            "logistic": logistic.model.as_json()
            if isinstance(logistic, FittedLogistic)
            else logistic.as_json(),
            "majority": majority.as_json(),
        }
        training_json[instrument] = {
            "rows": encode_integer(len(train_rows)),
            "true_distribution": {
                name: encode_integer(int(count))
                for name, count in zip(
                    CLASS_ORDER, np.bincount(train_classes, minlength=len(CLASS_ORDER)), strict=True
                )
            },
        }
        for split in EVALUATION_SPLITS:
            rows = evaluation[split].get(instrument, [])
            inputs = to_floats([row.inputs for row in rows])
            predicted: dict[str, Predicted] = {
                "majority": majority if isinstance(majority, Unevaluable) else majority.predict(len(rows)),
                "logistic": _logistic_predictions(logistic, inputs),
            }
            evaluated[split][instrument] = Evaluated(rows=rows, models=predicted)
            for index, row in enumerate(rows):
                records[row.identity] = prediction_record(row, predicted, index)

    predictions = b"".join(
        canonical_json(records[row.identity]) for row in data.rows if row.identity in records
    )
    excluded = {
        period: {instrument: dict(sorted(reasons.items())) for instrument, reasons in by_instrument.items()}
        for period, by_instrument in data.excluded.items()
    }
    files = {
        METRICS: canonical_json(
            {
                "excluded": {
                    period: {
                        instrument: {reason: encode_integer(count) for reason, count in reasons.items()}
                        for instrument, reasons in by_instrument.items()
                    }
                    for period, by_instrument in excluded.items()
                },
                "schema": METRICS_SCHEMA,
                "splits": evaluation_metrics(evaluated),
                "training": training_json,
            }
        ),
        MODEL: canonical_json(
            {
                "class_order": list(CLASS_ORDER),
                "instruments": models_json,
                "model_feature_set": model_feature_set_json(),
                "schema": MODELS_SCHEMA,
            }
        ),
        PREDICTIONS: predictions,
    }
    manifest = {
        "class_order": list(CLASS_ORDER),
        "configuration": configuration.as_json(),
        "counts": {
            "evaluated": {
                split: {
                    instrument: encode_integer(len(rows))
                    for instrument, rows in sorted(evaluation[split].items())
                }
                for split in EVALUATION_SPLITS
            },
            "excluded": {
                period: encode_integer(sum(sum(reasons.values()) for reasons in by_instrument.values()))
                for period, by_instrument in excluded.items()
            },
            "training": {
                instrument: encode_integer(len(rows)) for instrument, rows in sorted(training.items())
            },
        },
        "environment": environment(),
        "files": {
            name: {"name": name, "sha256": sha256_hex(contents), "size_bytes": encode_integer(len(contents))}
            for name, contents in files.items()
        },
        "model_feature_set": model_feature_set_json(),
        "schema": SCHEMA,
        "scope": SCOPE,
        "sources": _source_json(sources),
    }
    experiment_id = experiment_identity(manifest)
    return Experiment(
        experiment_id=experiment_id,
        files={**files, MANIFEST: canonical_json({**manifest, "experiment_id": experiment_id})},
    )


def experiment_identity(manifest: Mapping[str, Any]) -> str:
    """The SHA-256 of the canonical manifest without its experiment ID."""
    return sha256_hex(
        canonical_json({key: value for key, value in manifest.items() if key != "experiment_id"})
    )


def load_model_data(sources: VerifiedSources) -> ModelData:
    return join(
        read_feature_rows(sources.features),
        read_label_rows(sources.labels),
        read_split_rows(sources.splits),
        validation_start_at_ns=sources.validation_start_at_ns,
        test_start_at_ns=sources.test_start_at_ns,
    )
