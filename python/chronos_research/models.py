"""The two baselines, fitted on one instrument's training rows only.

No-skill: the training class frequencies are its probabilities, and the
most frequent class, with ties going to the earliest class in CLASS_ORDER,
is its prediction. It establishes the floor any model must beat.

Logistic regression: a scikit-learn pipeline of StandardScaler and
multinomial LogisticRegression (L2, C = 1, lbfgs), single-threaded, with
explicit convergence limits and no shuffling. It is evaluable only with
every class present in training at least `minimum_training_rows_per_class`
times and a converged fit; otherwise it is unevaluable with a reason.

A fitted model is kept as plain numbers, and predictions are always made
from those numbers, never from a pickled object. They must reproduce the
pipeline's classes exactly and its probabilities within
INFERENCE_TOLERANCE on every row they predict.
"""

import warnings
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

import numpy as np
import numpy.typing as npt
import sklearn
from sklearn.exceptions import ConvergenceWarning
from sklearn.linear_model import LogisticRegression
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from threadpoolctl import threadpool_limits

from chronos_research.artifacts import CLASS_ORDER
from chronos_research.encoding import encode_float, encode_integer

INFERENCE_TOLERANCE = 1e-12
_CLASSES = len(CLASS_ORDER)


@dataclass(frozen=True)
class LogisticConfiguration:
    regularization_c: float = 1.0
    tolerance: float = 1e-6
    maximum_iterations: int = 1000
    minimum_training_rows_per_class: int = 10
    random_state: int = 0

    def as_json(self) -> dict[str, Any]:
        return {
            "class_weight": None,
            "estimator": f"sklearn.linear_model.LogisticRegression {sklearn.__version__}",
            "fit_intercept": True,
            "l1_ratio": encode_float(0.0),
            "maximum_iterations": encode_integer(self.maximum_iterations),
            "minimum_training_rows_per_class": encode_integer(self.minimum_training_rows_per_class),
            "multinomial": True,
            "random_state": encode_integer(self.random_state),
            "regularization_c": encode_float(self.regularization_c),
            "scaler": "sklearn.preprocessing.StandardScaler, fitted on training rows",
            "solver": "lbfgs",
            "threads": "1",
            "tolerance": encode_float(self.tolerance),
        }


@dataclass(frozen=True)
class MajorityModel:
    class_counts: tuple[int, ...]

    @property
    def probabilities(self) -> npt.NDArray[np.float64]:
        counts = np.array(self.class_counts, dtype=np.float64)
        return counts / counts.sum()

    @property
    def majority_class(self) -> int:
        # argmax returns the first maximum: the earliest class in order.
        return int(np.argmax(np.array(self.class_counts)))

    def predict(self, rows: int) -> tuple[npt.NDArray[np.int64], npt.NDArray[np.float64]]:
        return (
            np.full(rows, self.majority_class, dtype=np.int64),
            np.tile(self.probabilities, (rows, 1)),
        )

    def as_json(self) -> dict[str, Any]:
        return {
            "class_counts": dict(zip(CLASS_ORDER, map(encode_integer, self.class_counts), strict=True)),
            "majority_class": CLASS_ORDER[self.majority_class],
            "probabilities": [encode_float(float(p)) for p in self.probabilities],
        }


def fit_majority(training_classes: npt.NDArray[np.int64]) -> MajorityModel:
    if len(training_classes) == 0:
        raise ValueError("the majority baseline needs at least one training row")
    counts = np.bincount(training_classes, minlength=_CLASSES)
    return MajorityModel(class_counts=tuple(int(count) for count in counts))


@dataclass(frozen=True)
class LogisticModel:
    mean: npt.NDArray[np.float64]
    scale: npt.NDArray[np.float64]
    coefficients: npt.NDArray[np.float64]
    intercepts: npt.NDArray[np.float64]
    iterations: int

    def logits(self, inputs: npt.NDArray[np.float64]) -> npt.NDArray[np.float64]:
        # One thread, as in training, so predictions do not depend on BLAS
        # threading. Finite parameters can still overflow during inference;
        # NaN must never reach a tolerance comparison, where it evades `>`.
        try:
            with threadpool_limits(limits=1), np.errstate(over="raise", invalid="raise", divide="raise"):
                standardized = (inputs - self.mean) / self.scale
                logits = np.asarray(standardized @ self.coefficients.T + self.intercepts, dtype=np.float64)
        except FloatingPointError as error:
            raise ValueError("non-finite logistic inference") from error
        if not np.isfinite(logits).all():
            raise ValueError("non-finite logistic inference")
        return logits

    def predict(
        self, inputs: npt.NDArray[np.float64]
    ) -> tuple[npt.NDArray[np.int64], npt.NDArray[np.float64]]:
        logits = self.logits(inputs)
        try:
            with np.errstate(over="raise", invalid="raise", divide="raise"):
                shifted = np.exp(logits - logits.max(axis=1, keepdims=True))
                probabilities = shifted / shifted.sum(axis=1, keepdims=True)
        except FloatingPointError as error:
            raise ValueError("non-finite logistic inference") from error
        if not np.isfinite(probabilities).all():
            raise ValueError("non-finite logistic inference")
        return np.argmax(logits, axis=1).astype(np.int64), probabilities

    def as_json(self) -> dict[str, Any]:
        def floats(values: npt.NDArray[np.float64]) -> list[str]:
            return [encode_float(float(value)) for value in values]

        return {
            "coefficients": {
                name: floats(row) for name, row in zip(CLASS_ORDER, self.coefficients, strict=True)
            },
            "intercepts": dict(zip(CLASS_ORDER, floats(self.intercepts), strict=True)),
            "iterations": encode_integer(self.iterations),
            "scaler": {"mean": floats(self.mean), "scale": floats(self.scale)},
            "status": "fitted",
        }


@dataclass(frozen=True)
class Unevaluable:
    reason: str
    detail: Mapping[str, str]

    def as_json(self) -> dict[str, Any]:
        return {"detail": dict(self.detail), "reason": self.reason, "status": "unevaluable"}


@dataclass(frozen=True)
class FittedLogistic:
    model: LogisticModel
    # Kept only to prove the stored numbers reproduce it.
    pipeline: Pipeline


def fit_logistic(
    inputs: npt.NDArray[np.float64],
    training_classes: npt.NDArray[np.int64],
    configuration: LogisticConfiguration,
) -> FittedLogistic | Unevaluable:
    counts = np.bincount(training_classes, minlength=_CLASSES)
    if int(counts.min()) < configuration.minimum_training_rows_per_class:
        return Unevaluable(
            reason="insufficient_class_support",
            detail={
                name: encode_integer(int(count)) for name, count in zip(CLASS_ORDER, counts, strict=True)
            },
        )
    pipeline = Pipeline(
        [
            ("scaler", StandardScaler()),
            (
                "classifier",
                LogisticRegression(
                    C=configuration.regularization_c,
                    tol=configuration.tolerance,
                    max_iter=configuration.maximum_iterations,
                    random_state=configuration.random_state,
                    solver="lbfgs",
                ),
            ),
        ]
    )
    with threadpool_limits(limits=1), warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        pipeline.fit(inputs, training_classes)
    classifier = pipeline.named_steps["classifier"]
    iterations = int(np.max(classifier.n_iter_))
    if any(issubclass(warning.category, ConvergenceWarning) for warning in caught) or (
        iterations >= configuration.maximum_iterations
    ):
        return Unevaluable(reason="did_not_converge", detail={"iterations": encode_integer(iterations)})
    if caught:
        # Any other warning, such as a numerical one, is not ignored either.
        return Unevaluable(
            reason="fit_warning",
            detail={"category": caught[0].category.__name__, "message": str(caught[0].message)},
        )
    if list(classifier.classes_) != list(range(_CLASSES)) or classifier.coef_.shape != (
        _CLASSES,
        inputs.shape[1],
    ):
        raise RuntimeError("the fitted classifier does not have the fixed class order and shape")
    scaler = pipeline.named_steps["scaler"]
    model = LogisticModel(
        mean=np.array(scaler.mean_, dtype=np.float64),
        scale=np.array(scaler.scale_, dtype=np.float64),
        coefficients=np.array(classifier.coef_, dtype=np.float64),
        intercepts=np.array(classifier.intercept_, dtype=np.float64),
        iterations=iterations,
    )
    fitted = FittedLogistic(model=model, pipeline=pipeline)
    require_parity(fitted, inputs)
    return fitted


def require_parity(fitted: FittedLogistic, inputs: npt.NDArray[np.float64]) -> None:
    """The stored numbers must reproduce the pipeline on these rows."""
    if len(inputs) == 0:
        return
    classes, probabilities = fitted.model.predict(inputs)
    with threadpool_limits(limits=1):
        expected_classes = fitted.pipeline.predict(inputs)
        expected_probabilities = fitted.pipeline.predict_proba(inputs)
    if not np.isfinite(expected_probabilities).all():
        raise RuntimeError("scikit-learn produced non-finite probabilities")
    difference = float(np.max(np.abs(probabilities - expected_probabilities)))
    if not np.array_equal(classes, expected_classes) or difference > INFERENCE_TOLERANCE:
        raise RuntimeError(
            "inference from the stored parameters differs from scikit-learn "
            f"(largest probability difference {difference})"
        )
