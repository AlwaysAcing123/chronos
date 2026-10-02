"""Multiclass metrics over the fixed class order, each defined or not.

A metric that has no value, such as precision for a class never predicted,
is {"undefined": "<reason>"}, never a zero. Definitions:

    accuracy            correct / rows
    precision[k]        TP / (TP + FP)           undefined if k is never predicted
    recall[k]           TP / (TP + FN)           undefined if k never occurs
    f1[k]               2TP / (2TP + FP + FN)    undefined if k never occurs or is predicted
    balanced_accuracy   mean recall over the classes that occur
    log_loss            -mean(ln p[true class])  undefined if any p[true class] is 0
    brier               mean over rows of sum over classes (p - onehot)^2, in [0, 2]
    confusion[t][p]     rows of true class t predicted as p (rows true, columns predicted)
"""

from collections.abc import Sequence
from fractions import Fraction
from typing import Any

import numpy as np
import numpy.typing as npt

from chronos_research.artifacts import CLASS_ORDER
from chronos_research.encoding import encode_float, encode_integer

type Metric = dict[str, str]

_CLASSES = len(CLASS_ORDER)


def defined(value: float) -> Metric:
    return {"value": encode_float(value)}


def undefined(reason: str) -> Metric:
    return {"undefined": reason}


def _ratio(numerator: int, denominator: int, reason: str) -> Metric:
    return undefined(reason) if denominator == 0 else defined(numerator / denominator)


def classification_metrics(
    true_classes: npt.NDArray[np.int64],
    predicted_classes: npt.NDArray[np.int64],
    probabilities: npt.NDArray[np.float64],
) -> dict[str, Any]:
    rows = len(true_classes)
    if probabilities.shape != (rows, _CLASSES) or predicted_classes.shape != (rows,):
        raise ValueError("predictions and probabilities must have one row per evaluated row")
    confusion = np.zeros((_CLASSES, _CLASSES), dtype=np.int64)
    np.add.at(confusion, (true_classes, predicted_classes), 1)
    per_class: dict[str, Any] = {}
    recalls: list[float] = []
    for index, name in enumerate(CLASS_ORDER):
        true_positive = int(confusion[index, index])
        occurs = int(confusion[index, :].sum())
        predicted = int(confusion[:, index].sum())
        if occurs > 0:
            recalls.append(true_positive / occurs)
        per_class[name] = {
            "f1": _ratio(2 * true_positive, occurs + predicted, "class_neither_occurs_nor_is_predicted"),
            "precision": _ratio(true_positive, predicted, "class_never_predicted"),
            "recall": _ratio(true_positive, occurs, "class_never_occurs"),
        }
    if rows == 0:
        return {
            "accuracy": undefined("no_rows"),
            "balanced_accuracy": undefined("no_rows"),
            "brier_score": undefined("no_rows"),
            "confusion_matrix": _confusion_json(confusion),
            "log_loss": undefined("no_rows"),
            "per_class": per_class,
            "predicted_distribution": _distribution(predicted_classes),
            "rows": "0",
            "true_distribution": _distribution(true_classes),
        }
    true_probabilities = probabilities[np.arange(rows), true_classes]
    onehot = np.eye(_CLASSES)[true_classes]
    return {
        "accuracy": defined(int(np.trace(confusion)) / rows),
        "balanced_accuracy": defined(sum(recalls) / len(recalls)),
        "brier_score": defined(float(np.mean(np.sum((probabilities - onehot) ** 2, axis=1)))),
        "confusion_matrix": _confusion_json(confusion),
        "log_loss": (
            undefined("zero_probability_for_a_true_class")
            if np.any(true_probabilities <= 0.0)
            else defined(float(-np.mean(np.log(true_probabilities))))
        ),
        "per_class": per_class,
        "predicted_distribution": _distribution(predicted_classes),
        "rows": encode_integer(rows),
        "true_distribution": _distribution(true_classes),
    }


def _distribution(classes: npt.NDArray[np.int64]) -> dict[str, str]:
    counts = np.bincount(classes, minlength=_CLASSES)
    return {name: encode_integer(int(counts[index])) for index, name in enumerate(CLASS_ORDER)}


def _confusion_json(confusion: npt.NDArray[np.int64]) -> dict[str, dict[str, str]]:
    return {
        true_name: {
            predicted_name: encode_integer(int(confusion[true_index, predicted_index]))
            for predicted_index, predicted_name in enumerate(CLASS_ORDER)
        }
        for true_index, true_name in enumerate(CLASS_ORDER)
    }


def return_distribution(classes: Sequence[int], returns_raw: Sequence[int]) -> dict[str, Any]:
    """The future returns of each true class, exactly: count, minimum, and
    maximum raw returns, and their mean rounded once to a float."""
    distribution: dict[str, Any] = {}
    for index, name in enumerate(CLASS_ORDER):
        values = [value for value, cls in zip(returns_raw, classes, strict=True) if cls == index]
        distribution[name] = {
            "mean_return_raw": defined(float(Fraction(sum(values), len(values))))
            if values
            else undefined("no_rows"),
            "maximum_return_raw": encode_integer(max(values)) if values else None,
            "minimum_return_raw": encode_integer(min(values)) if values else None,
            "rows": encode_integer(len(values)),
        }
    return distribution
