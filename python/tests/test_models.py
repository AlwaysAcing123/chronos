import warnings

import numpy as np
import numpy.typing as npt
import pytest
from sklearn.pipeline import Pipeline

from chronos_research import models
from chronos_research.encoding import decode_float
from chronos_research.models import (
    INFERENCE_TOLERANCE,
    FittedLogistic,
    LogisticConfiguration,
    LogisticModel,
    Unevaluable,
    fit_logistic,
    fit_majority,
    require_parity,
)


def learnable(seed: int, rows: int = 600) -> tuple[npt.NDArray[np.float64], npt.NDArray[np.int64]]:
    """Five inputs on very different scales; the class follows the second."""
    generator = np.random.default_rng(seed)
    inputs = generator.normal(size=(rows, 5)) * np.array([0.01, 1.0, 0.001, 50.0, 30.0]) + np.array(
        [0.02, 0.0, 0.0, 100.0, 60.0]
    )
    signal = inputs[:, 1] + generator.normal(scale=0.7, size=rows)
    classes = np.digitize(signal, [-0.4, 0.4]).astype(np.int64)
    return inputs, classes


def fitted(inputs: npt.NDArray[np.float64], classes: npt.NDArray[np.int64]) -> FittedLogistic:
    result = fit_logistic(inputs, classes, LogisticConfiguration())
    assert isinstance(result, FittedLogistic), result
    return result


def test_the_majority_class_breaks_ties_by_class_order() -> None:
    assert fit_majority(np.array([0, 0, 1, 1, 2])).majority_class == 0
    assert fit_majority(np.array([1, 1, 2, 2, 0])).majority_class == 1
    assert fit_majority(np.array([2, 2, 1, 1, 2])).majority_class == 2
    model = fit_majority(np.array([2, 2, 1, 2]))
    assert model.class_counts == (0, 1, 3)
    classes, probabilities = model.predict(3)
    assert classes.tolist() == [2, 2, 2]
    assert probabilities.tolist() == [[0.0, 0.25, 0.75]] * 3
    assert model.as_json()["probabilities"] == ["0.0", "0.25", "0.75"]
    with pytest.raises(ValueError, match="at least one training row"):
        fit_majority(np.zeros(0, dtype=np.int64))


def test_stored_parameters_reproduce_scikit_learn() -> None:
    inputs, classes = learnable(1)
    result = fitted(inputs, classes)
    held_out, _ = learnable(2)
    predicted, probabilities = result.model.predict(held_out)
    assert np.array_equal(predicted, result.pipeline.predict(held_out))
    assert np.max(np.abs(probabilities - result.pipeline.predict_proba(held_out))) <= INFERENCE_TOLERANCE
    # The persisted spelling reads back to the same numbers bit for bit, and
    # so to the same predictions.
    stored = result.model.as_json()
    restored = LogisticModel(
        mean=np.array([decode_float(v) for v in stored["scaler"]["mean"]]),
        scale=np.array([decode_float(v) for v in stored["scaler"]["scale"]]),
        coefficients=np.array(
            [[decode_float(v) for v in stored["coefficients"][name]] for name in ("down", "flat", "up")]
        ),
        intercepts=np.array([decode_float(stored["intercepts"][name]) for name in ("down", "flat", "up")]),
        iterations=int(stored["iterations"]),
    )
    again, again_probabilities = restored.predict(held_out)
    assert np.array_equal(again, predicted)
    assert np.array_equal(again_probabilities, probabilities)
    assert stored["status"] == "fitted"


def test_fitting_is_deterministic_and_uses_only_its_rows() -> None:
    inputs, classes = learnable(3)
    first = fitted(inputs, classes)
    assert first.model.as_json() == fitted(inputs, classes).model.as_json()
    # The scaler is the training rows' own mean and standard deviation.
    assert np.allclose(first.model.mean, inputs.mean(axis=0), rtol=1e-15, atol=0)
    assert np.allclose(first.model.scale, inputs.std(axis=0), rtol=1e-12, atol=0)
    assert first.model.iterations < LogisticConfiguration().maximum_iterations


def test_too_little_class_support_is_unevaluable() -> None:
    inputs, classes = learnable(4)
    flat = np.flatnonzero(classes == 1)
    keep = np.concatenate([np.flatnonzero(classes != 1), flat[:9]])
    result = fit_logistic(inputs[keep], classes[keep], LogisticConfiguration())
    assert result == Unevaluable(
        reason="insufficient_class_support",
        detail={"down": str(int(np.sum(classes == 0))), "flat": "9", "up": str(int(np.sum(classes == 2)))},
    )
    # A class that never occurs is never fabricated or collapsed away.
    two_classes = fit_logistic(inputs[classes != 1], classes[classes != 1], LogisticConfiguration())
    assert isinstance(two_classes, Unevaluable)
    assert two_classes.detail["flat"] == "0"


def test_any_other_fit_warning_makes_the_model_unevaluable(monkeypatch: pytest.MonkeyPatch) -> None:
    class Warning(Pipeline):  # type: ignore[misc]
        def fit(self, *arguments: object) -> object:
            warnings.warn("overflow encountered", RuntimeWarning, stacklevel=1)
            return super().fit(*arguments)

    monkeypatch.setattr(models, "Pipeline", Warning)
    inputs, classes = learnable(6)
    result = fit_logistic(inputs, classes, LogisticConfiguration())
    assert result == Unevaluable(
        reason="fit_warning", detail={"category": "RuntimeWarning", "message": "overflow encountered"}
    )


def test_a_fit_that_does_not_converge_is_unevaluable() -> None:
    inputs, classes = learnable(5)
    result = fit_logistic(inputs, classes, LogisticConfiguration(maximum_iterations=2))
    assert isinstance(result, Unevaluable)
    assert result.reason == "did_not_converge"
    assert result.as_json() == {
        "detail": {"iterations": "2"},
        "reason": "did_not_converge",
        "status": "unevaluable",
    }


def test_finite_parameters_cannot_silently_produce_nonfinite_predictions() -> None:
    model = LogisticModel(
        mean=np.zeros(5),
        scale=np.full(5, np.nextafter(0.0, 1.0)),
        coefficients=np.zeros((3, 5)),
        intercepts=np.zeros(3),
        iterations=1,
    )
    # Every persisted parameter is finite and each scale is positive. Yet
    # standardization overflows, then inf * 0 produces NaN. A comparison
    # such as max(abs(actual - expected)) > tolerance cannot detect NaN.
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        with pytest.raises(ValueError, match="non-finite logistic inference"):
            model.predict(np.ones((1, 5)))


def test_parity_rejects_nonfinite_reference_probabilities(monkeypatch: pytest.MonkeyPatch) -> None:
    inputs, classes = learnable(9)
    result = fitted(inputs, classes)
    monkeypatch.setattr(result.pipeline, "predict_proba", lambda rows: np.full((len(rows), 3), np.nan))
    with pytest.raises(RuntimeError, match="scikit-learn produced non-finite probabilities"):
        require_parity(result, inputs)
