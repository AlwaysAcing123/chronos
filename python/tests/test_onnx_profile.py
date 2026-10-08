"""The chronos.onnx_classifier/1 profile, checked mechanically on models
built here. In production the profile runs only inside the sandbox; these
tests run the same module on the host, on models they built themselves."""

from collections.abc import Callable
from typing import Any

import pytest

from chronos_research.onnx_profile import MAX_MODEL_BYTES, MAX_NODES, ProfileError, check_model, limits

pytestmark = pytest.mark.usefixtures("onnx_available")


def refused(data: bytes) -> str:
    with pytest.raises(ProfileError) as refusal:
        check_model(data)
    return refusal.value.reason


def test_the_reference_classifier_passes_with_its_report() -> None:
    from tests import onnx_models

    _, report = check_model(onnx_models.valid())
    assert report.as_json() == {
        "initializer_elements": "28",
        "initializers": "4",
        "ir_version": "8",
        "nodes": "5",
        "opset": "17",
        "operators": ["Add", "Div", "MatMul", "Softmax", "Sub"],
        "profile": {"name": "chronos.onnx_classifier", "version": "1"},
    }
    assert limits()["max_model_bytes"] == str(MAX_MODEL_BYTES)


def _build(change: Callable[[Any], None]) -> bytes:
    from tests import onnx_models

    model = onnx_models.classifier()
    change(model)
    return onnx_models.serialized(model)


def test_malformed_oversized_and_out_of_range_models_are_refused() -> None:
    from tests import onnx_models

    assert refused(b"\x00\xffnot a model") == "malformed_model"
    assert refused(onnx_models.valid()[:-7]) in ("malformed_model", "invalid_graph", "wrong_output")
    assert refused(b"\x00" * (MAX_MODEL_BYTES + 1)) == "model_too_large"
    assert refused(onnx_models.serialized(onnx_models.classifier(ir_version=11))) == "unsupported_ir_version"
    assert refused(onnx_models.serialized(onnx_models.classifier(opset=12))) == "unsupported_opset"
    assert refused(onnx_models.serialized(onnx_models.classifier(opset=22))) == "unsupported_opset"


@pytest.mark.parametrize(
    ("operator", "domain", "reason"),
    [
        ("Relu", "", "unsupported_operator"),
        ("If", "", "unsupported_operator"),
        ("Loop", "", "unsupported_operator"),
        ("RandomNormalLike", "", "unsupported_operator"),
        ("Identity", "com.example.custom", "unsupported_opset"),
    ],
)
def test_control_flow_randomness_and_custom_domains_are_refused(
    operator: str, domain: str, reason: str
) -> None:
    from tests import onnx_models

    assert refused(onnx_models.with_operator(operator, domain)) == reason


def test_a_custom_domain_node_is_refused_even_without_its_opset() -> None:
    from tests import onnx_models

    assert refused(onnx_models.with_operator("Identity", "com.example.custom", import_domain=False)) == (
        "custom_domain"
    )


def test_a_subgraph_hidden_in_an_allowed_operator_is_refused() -> None:
    from onnx import helper

    branch = helper.make_graph([], "branch", [], [])

    def hide(model: Any) -> None:
        model.graph.node[4].attribute.append(helper.make_attribute("body", branch))

    assert refused(_build(hide)) == "subgraph"


def test_external_data_is_refused_before_anything_reads_it() -> None:
    from tests import onnx_models

    assert refused(onnx_models.with_external_data("/etc/passwd")) == "external_data"
    assert refused(onnx_models.with_external_data("../../secrets.bin")) == "external_data"


def test_functions_metadata_and_overridable_or_non_finite_parameters_are_refused() -> None:
    from onnx import helper

    from tests import onnx_models

    def add_function(model: Any) -> None:
        model.functions.append(
            helper.make_function("custom", "f", ["x"], ["y"], [], [helper.make_opsetid("", 17)])
        )

    def add_metadata(model: Any) -> None:
        helper.set_model_props(model, {"hint": "x"})

    def overridable(model: Any) -> None:
        model.graph.initializer.append(onnx_models.tensor("inputs", [0.0] * 5))

    def not_finite(model: Any) -> None:
        model.graph.initializer[3].CopyFrom(onnx_models.tensor("bias", [float("nan"), 0.0, 0.0]))

    assert refused(_build(add_function)) == "model_functions"
    assert refused(_build(add_metadata)) == "metadata_properties"
    assert refused(_build(overridable)) in ("overridable_initializer", "redefined_value")
    assert refused(_build(not_finite)) == "non_finite_parameter"


def test_uncontrolled_shapes_and_types_are_refused() -> None:
    from onnx import TensorProto

    from tests import onnx_models

    dynamic = onnx_models.classifier(inputs=[onnx_models.value("inputs", ["batch", 5])])
    single = onnx_models.classifier(inputs=[onnx_models.value("inputs", [1, 5], TensorProto.FLOAT)])
    wide = onnx_models.classifier(outputs=[onnx_models.value("probabilities", [1, 4])])
    renamed = onnx_models.classifier(outputs=[onnx_models.value("scores", [1, 3])])
    assert refused(onnx_models.serialized(dynamic)) == "dynamic_shape"
    assert refused(onnx_models.serialized(single)) == "unsupported_dtype"
    assert refused(onnx_models.serialized(wide)) == "wrong_output"
    assert refused(onnx_models.serialized(renamed)) == "wrong_output"


def test_oversized_graphs_and_tensors_are_refused() -> None:
    from onnx import helper

    from tests import onnx_models

    assert check_model(onnx_models.with_identity_chain(MAX_NODES - 5))[1].nodes == MAX_NODES
    assert refused(onnx_models.with_identity_chain(MAX_NODES - 4)) == "graph_too_large"

    def huge_initializer(model: Any) -> None:
        model.graph.initializer.append(onnx_models.tensor("unused", [0.0] * 5000))

    def softmax_axis(model: Any) -> None:
        del model.graph.node[4].attribute[:]
        model.graph.node[4].attribute.append(helper.make_attribute("axis", 0))

    assert refused(_build(huge_initializer)) == "tensor_too_large"
    assert refused(_build(softmax_axis)) == "unsupported_attribute"
