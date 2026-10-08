"""ONNX models for tests, built at test time with the optional onnx package,
so no model binary is kept in the repository. Import this only from a test
that requested the onnx_available fixture."""

from collections.abc import Sequence
from typing import Any

import numpy as np
from onnx import ModelProto, TensorProto, helper, numpy_helper

# A small fixed classifier: standardize, a linear layer, and Softmax.
MEAN = [0.02, 0.0, 0.0, 100.0, 100.0]
SCALE = [0.01, 0.5, 0.005, 50.0, 50.0]
WEIGHTS = [[0.4, -0.1, -0.3], [-1.2, 0.0, 1.2], [-0.8, 0.1, 0.7], [0.01, 0.0, -0.01], [-0.01, 0.0, 0.01]]
BIAS = [-0.2, 0.4, -0.2]


def tensor(name: str, values: Any) -> Any:
    return numpy_helper.from_array(np.asarray(values, dtype=np.float64), name)


def value(name: str, shape: Sequence[int | str], element: int = TensorProto.DOUBLE) -> Any:
    return helper.make_tensor_value_info(name, element, list(shape))


def classifier(
    *,
    nodes: Sequence[Any] | None = None,
    initializers: Sequence[Any] | None = None,
    inputs: Sequence[Any] | None = None,
    outputs: Sequence[Any] | None = None,
    opset: int = 17,
    ir_version: int = 8,
) -> ModelProto:
    """The fixed classifier, or a variant with any part replaced."""
    graph = helper.make_graph(
        list(
            nodes
            if nodes is not None
            else [
                helper.make_node("Sub", ["inputs", "mean"], ["centered"]),
                helper.make_node("Div", ["centered", "scale"], ["standardized"]),
                helper.make_node("MatMul", ["standardized", "weights"], ["products"]),
                helper.make_node("Add", ["products", "bias"], ["logits"]),
                helper.make_node("Softmax", ["logits"], ["probabilities"], axis=-1),
            ]
        ),
        "test_classifier",
        list(inputs if inputs is not None else [value("inputs", [1, 5])]),
        list(outputs if outputs is not None else [value("probabilities", [1, 3])]),
        list(
            initializers
            if initializers is not None
            else [
                tensor("mean", MEAN),
                tensor("scale", SCALE),
                tensor("weights", WEIGHTS),
                tensor("bias", BIAS),
            ]
        ),
    )
    return helper.make_model(graph, opset_imports=[helper.make_opsetid("", opset)], ir_version=ir_version)


def serialized(model: ModelProto) -> bytes:
    return bytes(model.SerializeToString(deterministic=True))


def valid() -> bytes:
    return serialized(classifier())


def without_softmax() -> bytes:
    """Logits, not probabilities: valid ONNX that breaks the output contract."""
    return serialized(
        classifier(
            nodes=[
                helper.make_node("Sub", ["inputs", "mean"], ["centered"]),
                helper.make_node("Div", ["centered", "scale"], ["standardized"]),
                helper.make_node("MatMul", ["standardized", "weights"], ["products"]),
                helper.make_node("Add", ["products", "bias"], ["probabilities"]),
            ]
        )
    )


def non_finite_output() -> bytes:
    """Division by a zero scale: finite parameters, NaN probabilities."""
    return serialized(
        classifier(
            initializers=[
                tensor("mean", MEAN),
                tensor("scale", [0.0] * 5),
                tensor("weights", WEIGHTS),
                tensor("bias", BIAS),
            ]
        )
    )


def with_operator(op_type: str, domain: str = "", import_domain: bool = True, **attributes: Any) -> bytes:
    """The classifier with one more node, of this operator, before Softmax;
    a custom domain is also imported unless asked not to."""
    nodes = [
        helper.make_node("Sub", ["inputs", "mean"], ["centered"]),
        helper.make_node("Div", ["centered", "scale"], ["standardized"]),
        helper.make_node("MatMul", ["standardized", "weights"], ["products"]),
        helper.make_node("Add", ["products", "bias"], ["linear"]),
        helper.make_node(op_type, ["linear"], ["logits"], domain=domain, **attributes),
        helper.make_node("Softmax", ["logits"], ["probabilities"], axis=-1),
    ]
    model = classifier(nodes=nodes)
    if domain and import_domain:
        model.opset_import.append(helper.make_opsetid(domain, 1))
    return serialized(model)


def with_external_data(location: str = "/etc/passwd") -> bytes:
    model = classifier()
    weights = model.graph.initializer[2]
    weights.ClearField("raw_data")
    weights.data_location = TensorProto.EXTERNAL
    entry = weights.external_data.add()
    entry.key, entry.value = "location", location
    return serialized(model)


def with_identity_chain(length: int) -> bytes:
    """A graph of `length` Identity nodes before the classifier's Softmax."""
    nodes = [
        helper.make_node("Sub", ["inputs", "mean"], ["centered"]),
        helper.make_node("Div", ["centered", "scale"], ["standardized"]),
        helper.make_node("MatMul", ["standardized", "weights"], ["products"]),
        helper.make_node("Add", ["products", "bias"], ["step0"]),
    ]
    nodes += [helper.make_node("Identity", [f"step{i}"], [f"step{i + 1}"]) for i in range(length)]
    nodes.append(helper.make_node("Softmax", [f"step{length}"], ["probabilities"], axis=-1))
    return serialized(classifier(nodes=nodes))
