"""`python -m chronos_research inference-worker`: one deployment's long-lived
model inference, for the paper-trading server (ADR 0018).

Protocol `chronos.inference_worker/1`: one JSON object per line on stdout,
requests on stdin, diagnostics on stderr.

    -> {"type": "ready", "protocol", "experiment_id", "instrument", "model",
        "parameters_sha256", "model_feature_set", "training_cutoff_ns"}
    <- {"id", "stream", "instrument", "ingest_sequence", "inputs": [5 integers]}
    -> {"type": "prediction", "id", "stream", "instrument", "ingest_sequence",
        "class", "probabilities": [3], "confidence_ppb"}
    -> {"type": "error", "id": <id or null>, "message"}

The experiment is verified once, at start: its identity, checksums, and
every stored prediction re-derived from its parameters. Inference then uses
only those verified parameters in memory; the files are never read again.
The parameters' checksum is computed from the verified model itself, so it
names exactly the bytes in use. Inference is the row-pure arithmetic the
historical schedules use, so a row gets the same prediction live and
historically. Integers stay exact strings until `to_floats`.

`--describe` prints the ready line and exits; the server uses it to record a
deployment's model identity before anything runs.
"""

import json
import sys
from pathlib import Path
from typing import IO, Any

from chronos_research.artifacts import CLASS_ORDER
from chronos_research.encoding import canonical_json, encode_float, sha256_hex
from chronos_research.experiment import model_feature_set_json
from chronos_research.inspection import verified_experiment
from chronos_research.models import LogisticModel, MajorityModel, Unevaluable
from chronos_research.schedule import (
    SCHEDULE_MODELS,
    ScheduleError,
    confidence_ppb,
    logistic_predictions,
)

PROTOCOL = "chronos.inference_worker/1"
MAXIMUM_LINE_BYTES = 8192
_INPUTS = len(model_feature_set_json()["inputs"])
_INT64_MIN, _INT64_MAX = -(2**63), 2**63 - 1


class WorkerError(Exception):
    """The worker cannot serve this model at all."""


def _canonical_integer(text: object, signed: bool) -> int:
    if not isinstance(text, str) or not text or len(text) > 20:
        raise ValueError("an integer is a canonical decimal string")
    digits = text[1:] if signed and text.startswith("-") else text
    if not digits.isascii() or not digits.isdigit() or (digits != "0" and digits.startswith("0")):
        raise ValueError("an integer is a canonical decimal string")
    if text == "-0":
        raise ValueError("an integer is a canonical decimal string")
    value = int(text)
    if not _INT64_MIN <= value <= _INT64_MAX:
        raise ValueError("an integer is out of range")
    return value


class LoadedModel:
    """One verified instrument model, held in memory."""

    def __init__(self, experiments: Path, experiment_id: str, instrument: str, name: str) -> None:
        if name not in SCHEDULE_MODELS:
            raise WorkerError(f"the model is one of {', '.join(SCHEDULE_MODELS)}")
        manifest, models = verified_experiment(experiments / experiment_id)
        if instrument not in models:
            raise WorkerError(f"experiment {experiment_id} has no models for {instrument!r}")
        model = models[instrument][name]
        if isinstance(model, Unevaluable):
            raise WorkerError(f"the {name} model for {instrument} is unevaluable ({model.reason})")
        self.model: LogisticModel | MajorityModel = model
        self.instrument = instrument
        self.ready: dict[str, Any] = {
            "experiment_id": manifest["experiment_id"],
            "instrument": instrument,
            "model": name,
            "model_feature_set": {
                "name": model_feature_set_json()["name"],
                "version": model_feature_set_json()["version"],
            },
            # The verified model's own canonical form: what a schedule's
            # parameters_sha256 names, since verification requires the
            # stored entry to equal it.
            "parameters_sha256": sha256_hex(canonical_json(model.as_json())),
            "protocol": PROTOCOL,
            # Training rows end before validation begins, and every training
            # label was known before it.
            "training_cutoff_ns": manifest["sources"]["validation_start_at_ns"],
            "type": "ready",
        }

    def predict(self, request: Any) -> dict[str, Any]:
        if not isinstance(request, dict) or set(request) != {
            "id",
            "ingest_sequence",
            "inputs",
            "instrument",
            "stream",
        }:
            raise ValueError("a request has exactly id, stream, instrument, ingest_sequence, and inputs")
        _canonical_integer(request["id"], signed=False)
        _canonical_integer(request["ingest_sequence"], signed=False)
        stream = request["stream"]
        if not isinstance(stream, str) or not stream or len(stream) > 256:
            raise ValueError("the stream is a nonempty string")
        if request["instrument"] != self.instrument:
            raise ValueError(f"this worker serves {self.instrument}")
        inputs = request["inputs"]
        if not isinstance(inputs, list) or len(inputs) != _INPUTS:
            raise ValueError(f"a request has {_INPUTS} inputs")
        values = tuple(_canonical_integer(value, signed=True) for value in inputs)
        if isinstance(self.model, LogisticModel):
            classes, probabilities = logistic_predictions(self.model, [values])
        else:
            classes, probabilities = self.model.predict(1)
        index = int(classes[0])
        row = [float(p) for p in probabilities[0]]
        return {
            "class": CLASS_ORDER[index],
            "confidence_ppb": str(confidence_ppb(row[index])),
            "id": request["id"],
            "ingest_sequence": request["ingest_sequence"],
            "instrument": request["instrument"],
            "probabilities": [encode_float(p) for p in row],
            "stream": stream,
            "type": "prediction",
        }


def _write(output: IO[str], document: dict[str, Any]) -> None:
    output.write(json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n")
    output.flush()


def run_worker(
    experiments: Path,
    experiment_id: str,
    instrument: str,
    model: str,
    describe: bool,
    requests: IO[bytes],
    output: IO[str],
) -> int:
    try:
        loaded = LoadedModel(experiments, experiment_id, instrument, model)
    except Exception as error:  # Any failure to verify fails closed.
        _write(output, {"id": None, "message": f"{type(error).__name__}: {error}", "type": "error"})
        return 1
    _write(output, loaded.ready)
    if describe:
        return 0
    while True:
        line = requests.readline(MAXIMUM_LINE_BYTES + 1)
        if not line:
            return 0
        if len(line) > MAXIMUM_LINE_BYTES or not line.endswith(b"\n"):
            _write(output, {"id": None, "message": "a request line is too long", "type": "error"})
            return 1
        request: Any = None
        try:
            request = json.loads(line)
            _write(output, loaded.predict(request))
        except (ValueError, ScheduleError) as error:
            identifier = (
                request.get("id")
                if isinstance(request, dict) and isinstance(request.get("id"), str)
                else None
            )
            _write(output, {"id": identifier, "message": str(error), "type": "error"})


def main(arguments: Any) -> int:
    return run_worker(
        arguments.artifacts / "experiments",
        arguments.experiment,
        arguments.instrument,
        arguments.model,
        arguments.describe,
        sys.stdin.buffer,
        sys.stdout,
    )
