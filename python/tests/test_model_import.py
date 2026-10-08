"""Imports: the uploader's manifest, what an imported version records as
declared versus verified, and that nothing is published unless every check
passed. The sandbox is replaced by a fake here; test_onnx_sandbox and
test_import_routes run the real one."""

import copy
import json
from pathlib import Path
from typing import Any, ClassVar

import pytest

from chronos_research import model_import
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.model_import import (
    IMPORT_SCHEMA,
    SMOKE_ROWS,
    ModelImportError,
    import_model,
    validated_import_manifest,
)
from chronos_research.onnx_profile import PROFILE
from chronos_research.onnx_sandbox import Sandbox
from chronos_research.registered_inference import InvalidPredictionError, RowPrediction, judged
from chronos_research.registry import (
    CUTOFF_SCOPE,
    NameVersionTakenError,
    RegistryError,
    RegistryRoots,
    bind_name,
    model_version_ids,
    names_by_version,
    verified_model_version,
)

MODEL = b"\x08\x08 a stand-in for model bytes the fake sandbox accepts"


def manifest(**changes: Any) -> dict[str, Any]:
    document: dict[str, Any] = {
        "class_order": ["down", "flat", "up"],
        "declared": {
            "cutoff_covers": CUTOFF_SCOPE,
            "information_cutoff_ns": "1048000000",
            "source": "a notebook outside Chronos",
            "training": "multinomial logistic regression on the first 900 rows",
        },
        "input_tensor": {"dtype": "float64", "name": "inputs", "shape": ["1", "5"]},
        "inputs": {"name": "top_of_book_baseline", "version": "1"},
        "instrument": "AAPL",
        "name": "aapl-imported",
        "output_tensor": {"dtype": "float64", "name": "probabilities", "shape": ["1", "3"]},
        "schema": IMPORT_SCHEMA,
        "target": {"horizon_ns": "150000", "label_set": "future_midpoint_return_v1"},
        "version": "1",
    }
    for path, value in changes.items():
        *parents, leaf = path.split("__")
        target = document
        for parent in parents:
            target = target[parent]
        target[leaf] = value
    return document


class FakeSession:
    report: ClassVar[dict[str, Any]] = {"profile": PROFILE, "nodes": "5"}
    onnxruntime = "1.30.0"


class FakePredictor:
    """Answers like the sandbox would for a model that outputs `outputs`."""

    outputs: tuple[float, ...] = (0.25, 0.5, 0.25)
    closed: ClassVar[list[bool]] = []

    def __init__(self, sandbox: Sandbox, payload: bytes) -> None:
        assert payload == MODEL
        self.session = FakeSession()

    def predict(self, inputs: tuple[int, ...]) -> RowPrediction:
        return judged(self.outputs)

    def close(self) -> None:
        FakePredictor.closed.append(True)


@pytest.fixture
def fake_sandbox(monkeypatch: pytest.MonkeyPatch) -> type[FakePredictor]:
    monkeypatch.setattr(model_import, "OnnxPredictor", FakePredictor)
    monkeypatch.setattr(FakePredictor, "outputs", (0.25, 0.5, 0.25))
    FakePredictor.closed = []
    return FakePredictor


def refusal(document: Any) -> tuple[str, str]:
    with pytest.raises(ModelImportError) as refused:
        validated_import_manifest(document)
    return refused.value.reason, str(refused.value)


def test_a_complete_manifest_is_accepted_unchanged() -> None:
    assert validated_import_manifest(manifest()) == manifest()
    undeclared = manifest(declared__information_cutoff_ns=None, declared__cutoff_covers=None)
    assert validated_import_manifest(undeclared)["declared"]["information_cutoff_ns"] is None


@pytest.mark.parametrize(
    ("changes", "reason"),
    [
        ({"schema": {"name": "chronos.model_import", "version": "2"}}, "invalid_manifest"),
        ({"name": "Not Allowed"}, "invalid_manifest"),
        ({"version": "01"}, "invalid_manifest"),
        ({"instrument": "../AAPL"}, "invalid_manifest"),
        ({"inputs": {"name": "top_of_book_baseline", "version": "2"}}, "incompatible_contract"),
        ({"input_tensor__shape": ["batch", "5"]}, "incompatible_contract"),
        ({"input_tensor__dtype": "float32"}, "incompatible_contract"),
        ({"output_tensor__name": "logits"}, "incompatible_contract"),
        ({"class_order": ["up", "flat", "down"]}, "incompatible_contract"),
        ({"target__horizon_ns": "0"}, "incompatible_contract"),
        ({"target__horizon_ns": 150000}, "invalid_manifest"),
        ({"declared__information_cutoff_ns": "0"}, "invalid_manifest"),
        ({"declared__information_cutoff_ns": "-1"}, "invalid_manifest"),
        ({"declared__cutoff_covers": "training"}, "invalid_manifest"),
        ({"declared__information_cutoff_ns": None}, "invalid_manifest"),
        ({"declared__training": ""}, "invalid_manifest"),
        ({"declared__training": "x" * 2001}, "invalid_manifest"),
        ({"declared__source": "line\x00break"}, "invalid_manifest"),
    ],
)
def test_an_incomplete_or_incompatible_manifest_is_refused(changes: dict[str, Any], reason: str) -> None:
    assert refusal(manifest(**changes))[0] == reason


def test_unknown_and_missing_fields_are_refused() -> None:
    extra = manifest()
    extra["weights_url"] = "https://example.com/model.onnx"
    missing = manifest()
    del missing["declared"]["source"]
    assert refusal(extra)[0] == "invalid_manifest"
    assert refusal(missing)[0] == "invalid_manifest"
    assert refusal([manifest()])[0] == "invalid_manifest"


def test_an_import_records_declarations_separately_from_what_was_verified(
    tmp_path: Path, fake_sandbox: type[FakePredictor]
) -> None:
    roots = RegistryRoots.under(tmp_path)
    registration = import_model(roots, manifest(), MODEL, Sandbox())
    assert (registration.outcome, registration.binding_outcome) == ("published", "published")
    version = verified_model_version(roots, registration.model_version_id)
    recorded = version.manifest
    assert recorded["family"] == "onnx"
    assert recorded["training"]["information_cutoff"]["basis"] == "declared"
    assert recorded["training"]["information_cutoff"]["at_ns"] == "1048000000"
    assert recorded["provenance"]["declared"] == manifest()["declared"]
    assert recorded["provenance"]["manifest_sha256"] == sha256_hex(canonical_json(manifest()))
    assert set(recorded["provenance"]["not_verified"]) == {"cutoff", "target", "training"}
    assert recorded["provenance"]["validation"]["smoke"]["rows"] == str(len(SMOKE_ROWS))
    assert "experiment_id" not in recorded["provenance"]
    assert version.payload == MODEL
    assert version.information_cutoff_ns == 1_048_000_000
    assert names_by_version(roots)[version.model_version_id] == [{"name": "aapl-imported", "version": "1"}]
    assert fake_sandbox.closed == [True]
    again = import_model(roots, manifest(), MODEL, Sandbox())
    assert (again.model_version_id, again.outcome) == (registration.model_version_id, "already_present")


def test_without_a_declared_cutoff_the_cutoff_is_unavailable_never_zero(
    tmp_path: Path, fake_sandbox: type[FakePredictor]
) -> None:
    roots = RegistryRoots.under(tmp_path)
    undeclared = manifest(declared__information_cutoff_ns=None, declared__cutoff_covers=None)
    version = verified_model_version(
        roots, import_model(roots, undeclared, MODEL, Sandbox()).model_version_id
    )
    assert version.manifest["training"]["information_cutoff"] == {
        "basis": "unavailable",
        "reason": "not_declared",
    }
    assert version.information_cutoff_ns is None


@pytest.mark.parametrize(
    ("outputs", "reason"),
    [
        ((float("nan"), 0.5, 0.5), "non_finite_output"),
        ((0.2, 0.2, 0.2), "probabilities_do_not_sum_to_one"),
        ((1.5, -0.25, -0.25), "probability_out_of_range"),
        ((0.5, 0.5), "wrong_output"),
    ],
)
def test_an_invalid_smoke_output_publishes_nothing(
    tmp_path: Path, fake_sandbox: type[FakePredictor], outputs: tuple[float, ...], reason: str
) -> None:
    roots = RegistryRoots.under(tmp_path)
    fake_sandbox.outputs = outputs
    with pytest.raises(ModelImportError) as refused:
        import_model(roots, manifest(), MODEL, Sandbox())
    assert refused.value.reason == reason
    assert model_version_ids(roots) == []
    assert fake_sandbox.closed == [True]


def test_a_nondeterministic_model_publishes_nothing(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    class Drifting(FakePredictor):
        calls = 0

        def predict(self, inputs: tuple[int, ...]) -> RowPrediction:
            Drifting.calls += 1
            return judged((0.25, 0.5, 0.25) if Drifting.calls <= len(SMOKE_ROWS) else (0.5, 0.25, 0.25))

    monkeypatch.setattr(model_import, "OnnxPredictor", Drifting)
    roots = RegistryRoots.under(tmp_path)
    with pytest.raises(ModelImportError) as refused:
        import_model(roots, manifest(), MODEL, Sandbox())
    assert refused.value.reason == "nondeterministic_output"
    assert model_version_ids(roots) == []


def test_a_taken_name_publishes_no_version(tmp_path: Path, fake_sandbox: type[FakePredictor]) -> None:
    roots = RegistryRoots.under(tmp_path)
    bind_name(roots, "aapl-imported", "1", "e" * 64)
    with pytest.raises(NameVersionTakenError):
        import_model(roots, manifest(), MODEL, Sandbox())
    assert model_version_ids(roots) == []


def _resigned(roots: RegistryRoots, version_id: str, edit: Any) -> str:
    """The version with its manifest edited and its ID recomputed, as a
    forger with write access to the catalog could; returns the new ID."""
    from chronos_research.registry import version_identity

    directory = roots.models / version_id
    document = json.loads((directory / "manifest.json").read_bytes())
    edit(document)
    del document["model_version_id"]
    document["model_version_id"] = version_identity(document)
    target = roots.models / document["model_version_id"]
    target.mkdir()
    (target / "manifest.json").write_bytes(canonical_json(document))
    (target / "model.onnx").write_bytes((directory / "model.onnx").read_bytes())
    return str(document["model_version_id"])


@pytest.mark.parametrize(
    "edit",
    [
        lambda d: d["training"]["information_cutoff"].update(basis="verified"),
        lambda d: d["training"]["information_cutoff"].update(at_ns="999"),
        lambda d: d["training"].update(information_cutoff={"basis": "unavailable", "reason": "not_declared"}),
        lambda d: d["provenance"]["declared"].update(cutoff_covers="training"),
        lambda d: d["provenance"].update(origin="experiment"),
        lambda d: d["provenance"]["validation"].update(
            profile={"profile": {"name": "other", "version": "1"}}
        ),
        lambda d: d.update(family="logistic"),
    ],
)
def test_an_import_cannot_claim_more_than_it_declared(
    tmp_path: Path, fake_sandbox: type[FakePredictor], edit: Any
) -> None:
    roots = RegistryRoots.under(tmp_path)
    version_id = import_model(roots, manifest(), MODEL, Sandbox()).model_version_id
    forged = _resigned(roots, version_id, edit)
    with pytest.raises(RegistryError):
        verified_model_version(roots, forged)


def test_a_judged_prediction_takes_the_first_largest_probability() -> None:
    tie = judged((0.4, 0.4, 0.2))
    assert (tie.class_index, tie.confidence_ppb) == (0, 400_000_000)
    with pytest.raises(InvalidPredictionError):
        judged((0.4, 0.4, 0.2 + 1e-11))
    assert copy.deepcopy(judged((0.0, 0.0, 1.0))).confidence_ppb == 1_000_000_000


def test_an_import_without_a_cutoff_cannot_be_deployed(
    tmp_path: Path, fake_sandbox: type[FakePredictor]
) -> None:
    from chronos_research import model_library

    roots = RegistryRoots.under(tmp_path)
    available = {"content_sha256": "0" * 64, "image": "image", "limits": {}, "status": "available"}
    undeclared = manifest(declared__information_cutoff_ns=None, declared__cutoff_covers=None)
    version = verified_model_version(
        roots, import_model(roots, undeclared, MODEL, Sandbox()).model_version_id
    )
    detail = model_library.detail(roots, tmp_path / "experiments", version, [], available)
    assert detail["compatibility"]["deployment"]["reason"] == "cutoff_unavailable"
    assert detail["source_check"] == {
        "reason": "imported_model_has_no_source_experiment",
        "state": "not_applicable",
    }
    declared = verified_model_version(
        roots, import_model(roots, manifest(name="declared"), MODEL, Sandbox()).model_version_id
    )
    assert model_library.detail(roots, tmp_path, declared, [], available)["compatibility"]["deployment"][
        "status"
    ] == ("compatible")
