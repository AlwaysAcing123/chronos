"""The program inside the ONNX sandbox container (ADR 0021).

It runs only there: no network, a read-only filesystem with nothing of the
host mounted, an unprivileged user, and bounded memory, CPU, and processes.
It speaks `chronos.onnx_sandbox/1`, one JSON object per line on stdout:

    <- {"type": "load", "size", "sha256"} and then exactly `size` model bytes
    -> {"type": "ready", "report", "onnxruntime", "providers"}
       or {"type": "rejected", "reason", "message"}, and it exits
    <- {"type": "predict", "id", "inputs": [5 binary64 values as strings]}
    -> {"type": "prediction", "id", "dtype", "shape", "values": [...]}
       or {"type": "error", "id", "message"}

The model bytes are checked against the profile (onnx_profile) before any
session exists, and the session is created with one thread, no graph
optimization, no memory arena, released opsets only, and strict shape
inference. Outputs are reported exactly as computed, as shortest
round-trip strings; the host, not this program, judges them.
"""

import hashlib
import json
import sys
from typing import IO, Any

from chronos_research.onnx_profile import (
    INPUT_NAME,
    INPUT_SHAPE,
    MAX_MODEL_BYTES,
    OUTPUT_NAME,
    OUTPUT_SHAPE,
    ProfileError,
    check_model,
)

PROTOCOL = "chronos.onnx_sandbox/1"
MAXIMUM_LINE_BYTES = 4096


def _write(output: IO[str], document: dict[str, Any]) -> None:
    output.write(json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n")
    output.flush()


def _session(data: bytes) -> Any:
    import onnxruntime as ort

    # The container has no network; telemetry is off regardless.
    ort.disable_telemetry_events()
    options = ort.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
    options.enable_cpu_mem_arena = False
    options.enable_mem_pattern = False
    options.log_severity_level = 3
    for key, value in (
        ("session.allow_released_opsets_only", "1"),
        ("session.strict_shape_type_inference", "1"),
        ("session.intra_op.allow_spinning", "0"),
        ("session.inter_op.allow_spinning", "0"),
        ("session.model_external_initializers_file_folder_path", "/nonexistent"),
    ):
        options.add_session_config_entry(key, value)
    session = ort.InferenceSession(data, sess_options=options, providers=["CPUExecutionProvider"])
    if session.get_providers() != ["CPUExecutionProvider"]:
        raise ProfileError("unsupported_provider", "the session must run on the CPU provider alone")
    (single_input,) = session.get_inputs()
    (single_output,) = session.get_outputs()
    if (single_input.name, tuple(single_input.shape), single_input.type) != (
        INPUT_NAME,
        INPUT_SHAPE,
        "tensor(double)",
    ) or (single_output.name, tuple(single_output.shape), single_output.type) != (
        OUTPUT_NAME,
        OUTPUT_SHAPE,
        "tensor(double)",
    ):
        raise ProfileError("session_contract", "the session's input or output differs from the profile")
    return session


def _load(requests: IO[bytes], output: IO[str]) -> Any | None:
    line = requests.readline(MAXIMUM_LINE_BYTES + 1)
    try:
        header = json.loads(line)
        if set(header) != {"sha256", "size", "type"} or header["type"] != "load":
            raise ValueError("the first request loads a model")
        size = int(header["size"])
        if not 0 < size <= MAX_MODEL_BYTES or str(size) != header["size"]:
            raise ValueError("the model size is out of range")
    except (ValueError, TypeError, KeyError) as error:
        _write(output, {"message": str(error), "reason": "malformed_request", "type": "rejected"})
        return None
    data = requests.read(size)
    if len(data) != size or hashlib.sha256(data).hexdigest() != header["sha256"]:
        _write(
            output,
            {
                "message": "the model bytes are incomplete or altered",
                "reason": "corrupt_upload",
                "type": "rejected",
            },
        )
        return None
    try:
        _, report = check_model(data)
        session = _session(data)
    except ProfileError as error:
        _write(output, {"message": str(error), "reason": error.reason, "type": "rejected"})
        return None
    except Exception as error:  # A model the runtime cannot load is refused, never served.
        _write(
            output,
            {
                "message": f"{type(error).__name__}: {error}"[:1000],
                "reason": "runtime_refused",
                "type": "rejected",
            },
        )
        return None
    import onnxruntime as ort

    _write(
        output,
        {
            "onnxruntime": ort.__version__,
            "protocol": PROTOCOL,
            "providers": session.get_providers(),
            "report": report.as_json(),
            "type": "ready",
        },
    )
    return session


def _predict(session: Any, request: Any) -> dict[str, Any]:
    import numpy as np

    if (
        not isinstance(request, dict)
        or set(request) != {"id", "inputs", "type"}
        or request["type"] != "predict"
    ):
        raise ValueError("a request has exactly type, id, and inputs")
    if not isinstance(request["id"], str) or not request["id"].isdigit() or len(request["id"]) > 20:
        raise ValueError("a request ID is a decimal string")
    values = request["inputs"]
    if (
        not isinstance(values, list)
        or len(values) != INPUT_SHAPE[1]
        or not all(isinstance(v, str) for v in values)
    ):
        raise ValueError(f"a request has {INPUT_SHAPE[1]} inputs as strings")
    row = np.array([[float(value) for value in values]], dtype=np.float64)
    (result,) = session.run([OUTPUT_NAME], {INPUT_NAME: row})
    result = np.asarray(result)
    return {
        "dtype": str(result.dtype),
        "id": request["id"],
        "shape": [str(dim) for dim in result.shape],
        "type": "prediction",
        "values": [repr(float(value)) for value in result.reshape(-1)[:16]],
    }


def run(requests: IO[bytes], output: IO[str]) -> int:
    session = _load(requests, output)
    if session is None:
        return 2
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
            _write(output, _predict(session, request))
        except Exception as error:  # One bad request is answered; the session stays.
            identifier = (
                request.get("id")
                if isinstance(request, dict) and isinstance(request.get("id"), str)
                else None
            )
            _write(
                output,
                {"id": identifier, "message": f"{type(error).__name__}: {error}"[:1000], "type": "error"},
            )


if __name__ == "__main__":
    sys.exit(run(sys.stdin.buffer, sys.stdout))
