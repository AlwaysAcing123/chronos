import math
from fractions import Fraction
from typing import Any

import numpy as np
import pytest
from sklearn.metrics import (
    accuracy_score,
    balanced_accuracy_score,
    confusion_matrix,
    log_loss,
    precision_recall_fscore_support,
)

from chronos_research.metrics import classification_metrics, return_distribution


def value(metric: dict[str, str]) -> float:
    assert "value" in metric, metric
    return float(metric["value"])


def evaluate(true: list[int], predicted: list[int], probabilities: list[list[float]]) -> dict[str, Any]:
    return classification_metrics(
        np.array(true, dtype=np.int64), np.array(predicted, dtype=np.int64), np.array(probabilities)
    )


# down, down, flat, up, up, up predicted as down, flat, flat, up, up, down.
HAND = evaluate(
    [0, 0, 1, 2, 2, 2],
    [0, 1, 1, 2, 2, 0],
    [
        [0.5, 0.25, 0.25],
        [0.25, 0.5, 0.25],
        [0.25, 0.5, 0.25],
        [0.1, 0.2, 0.7],
        [0.2, 0.2, 0.6],
        [0.6, 0.2, 0.2],
    ],
)


def test_metrics_match_a_hand_calculation() -> None:
    assert HAND["rows"] == "6"
    assert value(HAND["accuracy"]) == pytest.approx(4 / 6)
    per_class = HAND["per_class"]
    assert value(per_class["down"]["precision"]) == pytest.approx(1 / 2)
    assert value(per_class["flat"]["precision"]) == pytest.approx(1 / 2)
    assert value(per_class["up"]["precision"]) == pytest.approx(1.0)
    assert value(per_class["down"]["recall"]) == pytest.approx(1 / 2)
    assert value(per_class["flat"]["recall"]) == pytest.approx(1.0)
    assert value(per_class["up"]["recall"]) == pytest.approx(2 / 3)
    assert value(per_class["down"]["f1"]) == pytest.approx(1 / 2)
    assert value(per_class["flat"]["f1"]) == pytest.approx(2 / 3)
    assert value(per_class["up"]["f1"]) == pytest.approx(4 / 5)
    assert value(HAND["balanced_accuracy"]) == pytest.approx((1 / 2 + 1 + 2 / 3) / 3)
    expected_log_loss = -sum(map(math.log, (0.5, 0.25, 0.5, 0.7, 0.6, 0.2))) / 6
    assert value(HAND["log_loss"]) == pytest.approx(expected_log_loss)
    # Row by row: 0.375, 0.875, 0.375, 0.14, 0.24, 1.04.
    assert value(HAND["brier_score"]) == pytest.approx(3.045 / 6)
    assert HAND["true_distribution"] == {"down": "2", "flat": "1", "up": "3"}
    assert HAND["predicted_distribution"] == {"down": "2", "flat": "2", "up": "2"}


def test_the_confusion_matrix_has_true_rows_and_predicted_columns() -> None:
    assert HAND["confusion_matrix"] == {
        "down": {"down": "1", "flat": "1", "up": "0"},
        "flat": {"down": "0", "flat": "1", "up": "0"},
        "up": {"down": "1", "flat": "0", "up": "2"},
    }


def test_undefined_metrics_have_reasons_never_zeros() -> None:
    absent = evaluate([0, 0], [0, 0], [[1.0, 0.0, 0.0], [0.5, 0.5, 0.0]])
    assert absent["per_class"]["flat"] == {
        "f1": {"undefined": "class_neither_occurs_nor_is_predicted"},
        "precision": {"undefined": "class_never_predicted"},
        "recall": {"undefined": "class_never_occurs"},
    }
    assert value(absent["balanced_accuracy"]) == 1.0
    assert value(absent["log_loss"]) == pytest.approx(-math.log(0.5) / 2)
    # A class predicted but never occurring has a precision of zero and no
    # recall; f1 is then a real zero.
    wrong = evaluate([0], [2], [[0.2, 0.2, 0.6]])
    assert value(wrong["per_class"]["up"]["precision"]) == 0.0
    assert wrong["per_class"]["up"]["recall"] == {"undefined": "class_never_occurs"}
    assert value(wrong["per_class"]["up"]["f1"]) == 0.0
    certain = evaluate([1], [0], [[1.0, 0.0, 0.0]])
    assert certain["log_loss"] == {"undefined": "zero_probability_for_a_true_class"}
    empty = classification_metrics(np.zeros(0, dtype=np.int64), np.zeros(0, dtype=np.int64), np.zeros((0, 3)))
    for name in ("accuracy", "balanced_accuracy", "brier_score", "log_loss"):
        assert empty[name] == {"undefined": "no_rows"}
    assert empty["rows"] == "0"


def test_metrics_agree_with_scikit_learn() -> None:
    generator = np.random.default_rng(11)
    for _ in range(20):
        rows = int(generator.integers(5, 200))
        true = generator.integers(0, 3, rows)
        predicted = generator.integers(0, 3, rows)
        probabilities = generator.dirichlet(np.ones(3), rows)
        metrics = classification_metrics(true, predicted, probabilities)
        assert value(metrics["accuracy"]) == pytest.approx(accuracy_score(true, predicted))
        assert value(metrics["balanced_accuracy"]) == pytest.approx(balanced_accuracy_score(true, predicted))
        assert value(metrics["log_loss"]) == pytest.approx(log_loss(true, probabilities, labels=[0, 1, 2]))
        precision, recall, f1, _ = precision_recall_fscore_support(
            true, predicted, labels=[0, 1, 2], zero_division=np.nan
        )
        for index, name in enumerate(("down", "flat", "up")):
            for metric, expected in (("precision", precision), ("recall", recall), ("f1", f1)):
                entry = metrics["per_class"][name][metric]
                if np.isnan(expected[index]):
                    assert "undefined" in entry
                else:
                    assert value(entry) == pytest.approx(expected[index])
        matrix = confusion_matrix(true, predicted, labels=[0, 1, 2])
        assert [
            [int(metrics["confusion_matrix"][t][p]) for p in ("down", "flat", "up")]
            for t in ("down", "flat", "up")
        ] == (matrix.tolist())


def test_return_distributions_are_exact() -> None:
    distribution = return_distribution([0, 0, 2], [-3, -5, 7])
    assert distribution["down"] == {
        "maximum_return_raw": "-3",
        "mean_return_raw": {"value": "-4.0"},
        "minimum_return_raw": "-5",
        "rows": "2",
    }
    assert distribution["flat"]["mean_return_raw"] == {"undefined": "no_rows"}
    assert distribution["flat"]["rows"] == "0"
    # The mean of large integers is rounded once, not accumulated in floats.
    big = 2**60
    assert distribution_mean(return_distribution([1, 1, 1], [big + 1, big + 2, big + 4])) == float(
        Fraction(3 * big + 7, 3)
    )


def distribution_mean(distribution: dict[str, Any]) -> float:
    return float(distribution["flat"]["mean_return_raw"]["value"])
