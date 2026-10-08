"""The one supported ONNX contract, `chronos.onnx_classifier/1` (ADR 0021).

An imported model is untrusted bytes. This module decides, mechanically,
whether those bytes are a small, static, stateless three-class classifier
of the `top_of_book_baseline/1` inputs, and it runs only inside the ONNX
sandbox container: the host never parses an imported model.

The bytes are parsed with ModelProto.ParseFromString, which never follows
external data. Every rule below is checked on the whole model, including
every node's attributes, so no subgraph, function, external tensor, or
dynamic shape can pass. The checker and shape inference then run in their
strict modes, and every value must come out static, double, and small.

The module imports onnx lazily, so the host can read the profile's limits
without the optional dependency.
"""

import math
from dataclasses import dataclass
from typing import Any

PROFILE = {"name": "chronos.onnx_classifier", "version": "1"}
INPUT_NAME = "inputs"
OUTPUT_NAME = "probabilities"
INPUT_SHAPE = (1, 5)
OUTPUT_SHAPE = (1, 3)
MAX_MODEL_BYTES = 1024 * 1024
IR_VERSIONS = (7, 10)
OPSET_VERSIONS = (13, 21)
OPERATORS = ("Add", "Div", "Gemm", "Identity", "MatMul", "Mul", "Softmax", "Sub")
MAX_NODES = 64
MAX_INITIALIZERS = 32
MAX_TENSOR_ELEMENTS = 4096
MAX_INITIALIZER_ELEMENTS = 16384
MAX_NAME_LENGTH = 128
# Every attribute an allowed operator may carry, by its AttributeProto type
# name. Graph-valued attributes are never allowed, so no node has a subgraph.
ATTRIBUTES: dict[str, dict[str, str]] = {
    "Softmax": {"axis": "INT"},
    "Gemm": {"alpha": "FLOAT", "beta": "FLOAT", "transA": "INT", "transB": "INT"},
}


class ProfileError(Exception):
    """The model is not a chronos.onnx_classifier/1 model."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


@dataclass(frozen=True)
class ProfileReport:
    ir_version: int
    opset: int
    nodes: int
    initializers: int
    initializer_elements: int
    operators: tuple[str, ...]

    def as_json(self) -> dict[str, Any]:
        return {
            "initializer_elements": str(self.initializer_elements),
            "initializers": str(self.initializers),
            "ir_version": str(self.ir_version),
            "nodes": str(self.nodes),
            "opset": str(self.opset),
            "operators": list(self.operators),
            "profile": PROFILE,
        }


def limits() -> dict[str, Any]:
    """The profile's rules, for manifests and the UI."""
    return {
        "attributes": {op: dict(names) for op, names in sorted(ATTRIBUTES.items())},
        "input": {"dtype": "float64", "name": INPUT_NAME, "shape": [str(d) for d in INPUT_SHAPE]},
        "ir_versions": [str(IR_VERSIONS[0]), str(IR_VERSIONS[1])],
        "max_initializer_elements": str(MAX_INITIALIZER_ELEMENTS),
        "max_initializers": str(MAX_INITIALIZERS),
        "max_model_bytes": str(MAX_MODEL_BYTES),
        "max_nodes": str(MAX_NODES),
        "max_tensor_elements": str(MAX_TENSOR_ELEMENTS),
        "opsets": [str(OPSET_VERSIONS[0]), str(OPSET_VERSIONS[1])],
        "operators": list(OPERATORS),
        "output": {"dtype": "float64", "name": OUTPUT_NAME, "shape": [str(d) for d in OUTPUT_SHAPE]},
        "profile": PROFILE,
    }


def _refuse(reason: str, message: str) -> ProfileError:
    return ProfileError(reason, message)


def _name(text: str, what: str) -> None:
    if not text or len(text) > MAX_NAME_LENGTH:
        raise _refuse("invalid_name", f"{what} must have a name of 1 to {MAX_NAME_LENGTH} characters")


def _static_shape(value: Any, what: str) -> tuple[int, ...]:
    """A double tensor's shape, every dimension a fixed number."""
    from onnx import TensorProto

    if value.type.WhichOneof("value") != "tensor_type":
        raise _refuse("unsupported_type", f"{what} is not a tensor")
    tensor = value.type.tensor_type
    if tensor.elem_type != TensorProto.DOUBLE:
        raise _refuse("unsupported_dtype", f"{what} is not double")
    if not tensor.HasField("shape"):
        raise _refuse("dynamic_shape", f"{what} has no shape")
    dims = []
    for dim in tensor.shape.dim:
        if dim.WhichOneof("value") != "dim_value" or dim.dim_value < 0:
            raise _refuse("dynamic_shape", f"{what} has a dimension that is not a fixed number")
        dims.append(int(dim.dim_value))
    return tuple(dims)


def _elements(dims: tuple[int, ...] | list[int], what: str) -> int:
    count = 1
    for dim in dims:
        if dim < 0 or dim > MAX_TENSOR_ELEMENTS:
            raise _refuse("tensor_too_large", f"{what} has a dimension out of range")
        count *= dim
        if count > MAX_TENSOR_ELEMENTS:
            raise _refuse("tensor_too_large", f"{what} has more than {MAX_TENSOR_ELEMENTS} elements")
    return count


def _initializer(tensor: Any, graph_inputs: set[str]) -> int:
    from onnx import TensorProto, numpy_helper

    what = f"initializer {tensor.name!r}"
    _name(tensor.name, "an initializer")
    if tensor.name in graph_inputs:
        raise _refuse("overridable_initializer", f"{what} is also a graph input, so it could be overridden")
    if tensor.data_location == TensorProto.EXTERNAL or len(tensor.external_data) > 0:
        raise _refuse("external_data", f"{what} keeps its data outside the model")
    if tensor.data_type != TensorProto.DOUBLE:
        raise _refuse("unsupported_dtype", f"{what} is not double")
    if tensor.HasField("segment"):
        raise _refuse("unsupported_tensor", f"{what} is segmented")
    elements = _elements(list(tensor.dims), what)
    try:
        values = numpy_helper.to_array(tensor)
    except Exception as error:
        raise _refuse("malformed_tensor", f"{what} cannot be read: {error}") from error
    if values.size != elements or not all(math.isfinite(float(v)) for v in values.reshape(-1)):
        raise _refuse("non_finite_parameter", f"{what} has a value that is missing or not finite")
    return elements


def _node(node: Any, index: int, known: set[str]) -> None:
    from onnx import AttributeProto

    what = f"node {index} ({node.op_type})"
    if node.domain not in ("", "ai.onnx"):
        raise _refuse("custom_domain", f"{what} is in the custom domain {node.domain!r}")
    if node.op_type not in OPERATORS:
        raise _refuse("unsupported_operator", f"{what}: {node.op_type} is not one of {', '.join(OPERATORS)}")
    allowed = ATTRIBUTES.get(node.op_type, {})
    for attribute in node.attribute:
        kind = AttributeProto.AttributeType.Name(attribute.type)
        if attribute.ref_attr_name:
            raise _refuse("attribute_reference", f"{what} refers to a function attribute")
        if kind in ("GRAPH", "GRAPHS", "TENSOR", "TENSORS", "SPARSE_TENSOR", "SPARSE_TENSORS", "TYPE_PROTO"):
            raise _refuse("subgraph", f"{what} has a {kind.lower()} attribute")
        if allowed.get(attribute.name) != kind:
            raise _refuse("unsupported_attribute", f"{what} has an unsupported attribute {attribute.name!r}")
        if node.op_type == "Softmax" and attribute.i not in (-1, 1):
            raise _refuse("unsupported_attribute", f"{what} must apply Softmax over the class axis")
    for name in node.input:
        if name and name not in known:
            raise _refuse("undefined_value", f"{what} reads {name!r} before anything defines it")
    for name in node.output:
        _name(name, f"an output of {what}")
        if name in known:
            raise _refuse("redefined_value", f"{what} redefines {name!r}")
        known.add(name)


def check_model(data: bytes) -> tuple[Any, ProfileReport]:
    """The parsed model and its report, or a ProfileError with its reason."""
    from onnx import ModelProto, checker, shape_inference

    if len(data) > MAX_MODEL_BYTES:
        raise _refuse("model_too_large", f"the model has more than {MAX_MODEL_BYTES} bytes")
    model = ModelProto()
    try:
        model.ParseFromString(data)
    except Exception as error:
        raise _refuse("malformed_model", f"the bytes are not an ONNX model: {error}") from error
    if not IR_VERSIONS[0] <= model.ir_version <= IR_VERSIONS[1]:
        raise _refuse("unsupported_ir_version", f"IR version {model.ir_version} is outside {IR_VERSIONS}")
    if len(model.opset_import) != 1 or model.opset_import[0].domain not in ("", "ai.onnx"):
        raise _refuse("unsupported_opset", "the model must import exactly the default operator set")
    opset = int(model.opset_import[0].version)
    if not OPSET_VERSIONS[0] <= opset <= OPSET_VERSIONS[1]:
        raise _refuse("unsupported_opset", f"opset {opset} is outside {OPSET_VERSIONS}")
    for field, reason in (
        ("functions", "model_functions"),
        ("training_info", "training_info"),
        ("metadata_props", "metadata_properties"),
    ):
        if len(getattr(model, field)) > 0:
            raise _refuse(reason, f"the model has {field.replace('_', ' ')}")
    graph = model.graph
    if len(graph.sparse_initializer) > 0 or len(graph.quantization_annotation) > 0:
        raise _refuse("unsupported_tensor", "the graph has sparse initializers or quantization annotations")
    if len(graph.input) != 1 or graph.input[0].name != INPUT_NAME:
        raise _refuse("wrong_input", f"the graph has exactly one input, {INPUT_NAME!r}")
    if _static_shape(graph.input[0], "the input") != INPUT_SHAPE:
        raise _refuse("wrong_input", f"the input's shape is {list(INPUT_SHAPE)}")
    if len(graph.output) != 1 or graph.output[0].name != OUTPUT_NAME:
        raise _refuse("wrong_output", f"the graph has exactly one output, {OUTPUT_NAME!r}")
    if _static_shape(graph.output[0], "the output") != OUTPUT_SHAPE:
        raise _refuse("wrong_output", f"the output's shape is {list(OUTPUT_SHAPE)}")
    if len(graph.node) > MAX_NODES:
        raise _refuse("graph_too_large", f"the graph has more than {MAX_NODES} nodes")
    if len(graph.initializer) > MAX_INITIALIZERS:
        raise _refuse("graph_too_large", f"the graph has more than {MAX_INITIALIZERS} initializers")
    known = {INPUT_NAME}
    total = 0
    for tensor in graph.initializer:
        if tensor.name in known:
            raise _refuse("redefined_value", f"initializer {tensor.name!r} is defined twice")
        total += _initializer(tensor, {INPUT_NAME})
        known.add(tensor.name)
    if total > MAX_INITIALIZER_ELEMENTS:
        raise _refuse(
            "graph_too_large", f"the initializers hold more than {MAX_INITIALIZER_ELEMENTS} elements"
        )
    for index, node in enumerate(graph.node):
        _node(node, index, known)
    if OUTPUT_NAME not in known:
        raise _refuse("wrong_output", f"no node produces {OUTPUT_NAME!r}")
    try:
        checker.check_model(model, full_check=True)
        inferred = shape_inference.infer_shapes(model, check_type=True, strict_mode=True, data_prop=False)
    except Exception as error:
        raise _refuse("invalid_graph", f"the ONNX checker refused the model: {error}") from error
    values = {value.name: value for value in [*inferred.graph.value_info, *inferred.graph.output]}
    for node in graph.node:
        for name in node.output:
            if name not in values:
                raise _refuse("dynamic_shape", f"the shape of {name!r} cannot be inferred")
            _elements(list(_static_shape(values[name], f"value {name!r}")), f"value {name!r}")
    return model, ProfileReport(
        ir_version=int(model.ir_version),
        opset=opset,
        nodes=len(graph.node),
        initializers=len(graph.initializer),
        initializer_elements=total,
        operators=tuple(sorted({node.op_type for node in graph.node})),
    )
