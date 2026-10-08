"""One way to run a registered model version on one row (ADR 0021).

Historical schedules and live deployment workers both predict through
open_predictor, so a row gets the same prediction in both. Every call is
one row, so neither batch size nor order can change a result.

What a model outputs is judged here, never repaired: exactly three finite
binary64 probabilities, each from 0 to 1, together within 1e-12 of one (the
live checker's tolerance). No value is clipped, renormalized, or defaulted.
The class is the family's rule; for an imported model it is the first
largest probability, so ties go to the earliest class. confidence_ppb is
floor(1e9 x p), exactly.
"""

import math
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Protocol

from chronos_research.artifacts import CLASS_ORDER
from chronos_research.model_data import to_floats
from chronos_research.models import LogisticModel, MajorityModel
from chronos_research.onnx_sandbox import (
    Sandbox,
    SandboxRejectedError,
    SandboxSession,
    SandboxUnavailableError,
    content_digest,
    context_files,
)
from chronos_research.registry import ModelVersion, OnnxPayload
from chronos_research.schedule import ScheduleError, confidence_ppb, logistic_predictions

PROBABILITY_SUM_TOLERANCE = 1e-12


class InvalidPredictionError(Exception):
    """A row has no valid prediction; why is its reason."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


@dataclass(frozen=True)
class RowPrediction:
    class_index: int
    probabilities: tuple[float, ...]
    confidence_ppb: int


def judged(probabilities: Sequence[float], class_index: int | None = None) -> RowPrediction:
    """A valid prediction from these probabilities, or InvalidPredictionError.
    Without a class, the first largest probability is the class."""
    values = tuple(float(value) for value in probabilities)
    if len(values) != len(CLASS_ORDER):
        raise InvalidPredictionError("wrong_output", f"{len(values)} probabilities, not {len(CLASS_ORDER)}")
    if not all(math.isfinite(value) for value in values):
        raise InvalidPredictionError("non_finite_output", "a probability is not finite")
    if not all(0.0 <= value <= 1.0 for value in values):
        raise InvalidPredictionError("probability_out_of_range", "a probability is outside [0, 1]")
    if abs(math.fsum(values) - 1.0) > PROBABILITY_SUM_TOLERANCE:
        raise InvalidPredictionError(
            "probabilities_do_not_sum_to_one", f"the probabilities sum to {math.fsum(values)!r}"
        )
    index = values.index(max(values)) if class_index is None else class_index
    return RowPrediction(index, values, confidence_ppb(values[index]))


class RowPredictor(Protocol):
    def predict(self, inputs: tuple[int, ...]) -> RowPrediction: ...

    # What, beyond the host's own packages, the predictions depend on.
    def runtime_environment(self) -> dict[str, str]: ...

    def close(self) -> None: ...


class BaselinePredictor:
    """The row-pure arithmetic historical schedules already use."""

    def __init__(self, model: MajorityModel | LogisticModel) -> None:
        self.model = model

    def predict(self, inputs: tuple[int, ...]) -> RowPrediction:
        try:
            if isinstance(self.model, LogisticModel):
                classes, probabilities = logistic_predictions(self.model, [inputs])
            else:
                classes, probabilities = self.model.predict(1)
        except ScheduleError as error:
            raise InvalidPredictionError("non_finite_output", str(error)) from error
        return judged([float(p) for p in probabilities[0]], int(classes[0]))

    def runtime_environment(self) -> dict[str, str]:
        return {}

    def close(self) -> None:
        return None


class OnnxPredictor:
    """An imported model, in its own sandbox container for its lifetime."""

    def __init__(self, sandbox: Sandbox, payload: bytes) -> None:
        self.session = SandboxSession(sandbox, payload)

    def predict(self, inputs: tuple[int, ...]) -> RowPrediction:
        row = to_floats([inputs])[0]
        try:
            values = self.session.predict([float(value) for value in row])
        except SandboxRejectedError as error:
            raise InvalidPredictionError(error.reason, str(error)) from error
        try:
            decoded = [float(value) for value in values]
        except ValueError as error:
            raise InvalidPredictionError("wrong_output", "an output is not a number") from error
        return judged(decoded)

    def runtime_environment(self) -> dict[str, str]:
        return {
            "onnx_sandbox_content_sha256": content_digest(context_files()),
            "onnxruntime": self.session.onnxruntime,
        }

    def close(self) -> None:
        self.session.close()


def open_predictor(version: ModelVersion, sandbox: Sandbox | None) -> RowPredictor:
    """The version's row predictor. An imported model runs only in the
    sandbox; without one this raises SandboxUnavailableError."""
    if isinstance(version.model, OnnxPayload):
        if sandbox is None:
            raise SandboxUnavailableError(
                "sandbox_not_configured", "imported models run only in the ONNX sandbox"
            )
        return OnnxPredictor(sandbox, version.payload)
    return BaselinePredictor(version.model)
