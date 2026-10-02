"""End to end through the C++ verifier, on synthetic artifacts."""

import json
import os
import shutil
import stat
from collections.abc import Callable, Mapping
from pathlib import Path
from typing import Any

import pytest

from chronos_research import artifacts
from chronos_research.artifacts import ArtifactError, verify_sources
from chronos_research.cli import main
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.experiment import (
    MANIFEST,
    METRICS,
    MODEL,
    PREDICTIONS,
    ExperimentConfiguration,
    build_experiment,
    experiment_identity,
    load_model_data,
)
from chronos_research.inspection import InspectionError, verify_experiment
from chronos_research.models import LogisticConfiguration
from tests.pipeline import Pipeline
from tests.synthetic import synthetic_events


def evaluate(
    pipeline: Pipeline, output: Path, capsys: pytest.CaptureFixture[str], *extra: str
) -> dict[str, Any]:
    status = main([
        "evaluate",
        "--chronos-dataset", str(pipeline.tools.chronos_dataset),
        "--features", str(pipeline.features),
        "--labels", str(pipeline.labels),
        "--splits", str(pipeline.splits),
        "--split-artifact", pipeline.split_id,
        "--output", str(output),
        *extra,
    ])  # fmt: skip
    captured = capsys.readouterr()
    assert status == 0, captured.err
    result: dict[str, Any] = json.loads(captured.out)
    return result


def writable_copy(directory: Path, destination: Path) -> Path:
    shutil.copytree(directory, destination)
    destination.chmod(stat.S_IRWXU)
    for path in destination.iterdir():
        path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    return destination


def test_an_experiment_is_published_verified_and_reproducible(
    pipeline: Pipeline, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    first = evaluate(pipeline, tmp_path / "one", capsys, "--catalog", str(pipeline.catalog))
    assert first["outcome"] == "published"
    directory = Path(first["directory"])
    assert sorted(os.listdir(directory)) == sorted([MANIFEST, METRICS, MODEL, PREDICTIONS])
    assert stat.S_IMODE(directory.stat().st_mode) == 0o555
    manifest = verify_experiment(directory)
    assert manifest["sources"]["split_artifact_id"] == pipeline.split_id
    assert manifest["sources"]["label_artifact_id"] == pipeline.label_id
    assert manifest["sources"]["feature_artifact_id"] == pipeline.feature_id
    assert manifest["sources"]["dataset_id"] == pipeline.dataset_id
    assert manifest["environment"]["scikit_learn"]
    assert manifest["environment"]["numpy"]
    assert "not evidence of skill or profitability" in manifest["scope"]
    assert manifest["class_order"] == ["down", "flat", "up"]

    # Identical inputs in the same environment give identical bytes, and
    # publishing again recognizes the existing experiment.
    second = evaluate(pipeline, tmp_path / "two", capsys)
    assert second["experiment_id"] == first["experiment_id"]
    for name in (MANIFEST, METRICS, MODEL, PREDICTIONS):
        assert (Path(second["directory"]) / name).read_bytes() == (directory / name).read_bytes()
    assert evaluate(pipeline, tmp_path / "one", capsys)["outcome"] == "already_present"

    assert main(["inspect", "--output", str(tmp_path / "one"), "--experiment", first["experiment_id"]]) == 0
    inspected = json.loads(capsys.readouterr().out)
    assert inspected["experiment_id"] == first["experiment_id"]

    # Both baselines were evaluable for both instruments, and the synthetic
    # signal in imbalance is learnable.
    models = json.loads((directory / MODEL).read_bytes())["instruments"]
    assert sorted(models) == ["AAPL", "SPY"]
    assert all(entry["logistic"]["status"] == "fitted" for entry in models.values())
    headline = first["headline"]["test"]
    assert float(headline["logistic"]["accuracy"]["value"]) > float(headline["majority"]["accuracy"]["value"])


def test_every_row_is_accounted_for(pipeline: Pipeline) -> None:
    sources = verify_sources(
        pipeline.tools.chronos_dataset,
        features_root=pipeline.features,
        labels_root=pipeline.labels,
        splits_root=pipeline.splits,
        split_artifact_id=pipeline.split_id,
    )
    data = load_model_data(sources)
    excluded = sum(sum(reasons.values()) for period in data.excluded.values() for reasons in period.values())
    assert len(data.rows) + excluded == sources.features.rows == 800
    split = sources.splits.summary
    for name in ("train", "validation", "test"):
        assert sum(row.split == name for row in data.rows) == int(split["usable"][name]["rows"])
    # Each instrument's last quote has no future observation, and each
    # period's last label crosses into the next period.
    assert data.excluded["test"] == {
        "AAPL": {"label_unavailable:no_future_observation": 1},
        "SPY": {"label_unavailable:no_future_observation": 1},
    }
    assert sum(data.excluded["train"]["AAPL"].values()) + sum(data.excluded["train"]["SPY"].values()) == int(
        split["counts"]["excluded"]["label_crosses_validation_start"]
    )


def test_changed_sources_or_configuration_change_the_id(
    pipeline: Pipeline, make_pipeline: Callable[..., Pipeline]
) -> None:
    def experiment_id(source: Pipeline, configuration: ExperimentConfiguration) -> str:
        sources = verify_sources(
            source.tools.chronos_dataset,
            features_root=source.features,
            labels_root=source.labels,
            splits_root=source.splits,
            split_artifact_id=source.split_id,
        )
        return build_experiment(sources, load_model_data(sources), configuration).experiment_id

    base = experiment_id(pipeline, ExperimentConfiguration())
    assert experiment_id(pipeline, ExperimentConfiguration()) == base
    regularized = ExperimentConfiguration(logistic=LogisticConfiguration(regularization_c=0.5))
    assert experiment_id(pipeline, regularized) != base
    assert (
        experiment_id(
            make_pipeline(synthetic_events(seed=7), percentages=(50, 25, 25)), ExperimentConfiguration()
        )
        != base
    )
    assert experiment_id(make_pipeline(synthetic_events(seed=8)), ExperimentConfiguration()) != base


def test_corrupted_experiments_are_detected(
    pipeline: Pipeline, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    published = Path(evaluate(pipeline, tmp_path / "published", capsys)["directory"])

    def corrupted(edit: Callable[[Path], object], resign: bool = False) -> Path:
        copy = writable_copy(published, tmp_path / f"copy-{len(os.listdir(tmp_path))}" / published.name)
        edit(copy)
        if resign:
            # Rewrite the checksums and ID so that only the content is wrong.
            manifest = json.loads((copy / MANIFEST).read_bytes())
            for name in (METRICS, MODEL, PREDICTIONS):
                data = (copy / name).read_bytes()
                manifest["files"][name] = {
                    "name": name,
                    "sha256": sha256_hex(data),
                    "size_bytes": str(len(data)),
                }
            manifest["experiment_id"] = experiment_identity(manifest)
            (copy / MANIFEST).write_bytes(canonical_json(manifest))
            copy = copy.rename(copy.parent / manifest["experiment_id"])
        return copy

    def replace_in(name: str, old: bytes, new: bytes) -> Callable[[Path], None]:
        def edit(directory: Path) -> None:
            data = (directory / name).read_bytes()
            assert old in data
            (directory / name).write_bytes(data.replace(old, new, 1))

        return edit

    def edit_json(
        name: str, change: Callable[[Any], None], line: int | None = None
    ) -> Callable[[Path], None]:
        """Changes a document, or one prediction line, keeping it canonical."""

        def edit(directory: Path) -> None:
            data = (directory / name).read_bytes()
            if line is None:
                document = json.loads(data)
                change(document)
                (directory / name).write_bytes(canonical_json(document))
                return
            lines = data.splitlines(keepends=True)
            record = json.loads(lines[line])
            change(record)
            lines[line] = canonical_json(record)
            (directory / name).write_bytes(b"".join(lines))

        return edit

    def first_instrument(document: Any) -> Any:
        return document["training"][sorted(document["training"])[0]]

    model = (published / MODEL).read_bytes()
    # The imbalance coefficient: the synthetic spread is constant, so its
    # standardized value, and any change to its coefficient, is negligible.
    first_coefficient = json.loads(model)["instruments"]["AAPL"]["logistic"]["coefficients"]["up"][1]
    metrics = (published / METRICS).read_bytes()
    accuracy = json.loads(metrics)["splits"]["test"]["aggregate"]["logistic"]["metrics"]["accuracy"]["value"]
    cases: list[tuple[Callable[[Path], object], bool, str]] = [
        (replace_in(PREDICTIONS, b'"class":"up"', b'"class":"down"'), False, "differs from the manifest"),
        (replace_in(PREDICTIONS, b'"class":"up"', b'"class":"down"'), True, "persisted predictions differ"),
        (
            replace_in(MODEL, f'"{first_coefficient}"'.encode(), b'"0.5"'),
            True,
            "persisted predictions differ",
        ),
        (replace_in(METRICS, f'"{accuracy}"'.encode(), b'"0.99"'), True, "but the predictions give"),
        (replace_in(MANIFEST, b'"training_threads":"1"', b'"training_threads":"2"'), False, "experiment ID"),
        (replace_in(PREDICTIONS, b'{"identity"', b'{ "identity"'), True, "not in canonical form"),
        (lambda directory: (directory / "extra").write_bytes(b""), False, "holds exactly"),
        (lambda directory: (directory / METRICS).unlink(), False, "holds exactly"),
    ]
    for edit, resign, message in cases:
        with pytest.raises(InspectionError, match=message):
            verify_experiment(corrupted(edit, resign))
    # Self-consistent but wrong summaries, structure, and counts are refused
    # as malformed experiments, never as crashes.
    malformed: list[tuple[Callable[[Path], object], str]] = [
        (edit_json(MANIFEST, lambda m: m.pop("scope")), "must have exactly the keys"),
        (edit_json(MANIFEST, lambda m: m.update(sources={})), "sources must have exactly the keys"),
        (
            edit_json(MANIFEST, lambda m: m.update(configuration={})),
            "configuration must have exactly the keys",
        ),
        (
            edit_json(MANIFEST, lambda m: m["configuration"].update(training_split="test")),
            "unsupported experiment configuration",
        ),
        (edit_json(MANIFEST, lambda m: m["sources"].update(horizon_ns="0")), "horizon_ns must be positive"),
        (
            edit_json(MANIFEST, lambda m: m["sources"].update(dataset_id="invalid")),
            "dataset_id must be a 64-digit lowercase",
        ),
        (edit_json(MANIFEST, lambda m: m.update(environment={})), "environment must have exactly the keys"),
        (
            edit_json(MANIFEST, lambda m: m["configuration"]["logistic"].update(maximum_iterations="0")),
            "invalid logistic configuration",
        ),
        (edit_json(MANIFEST, lambda m: m["counts"]["evaluated"].update(train={})), "evaluated counts"),
        (edit_json(METRICS, lambda m: first_instrument(m).update(rows="1")), "training counts differ"),
        (edit_json(MANIFEST, lambda m: m["counts"]["excluded"].update(test="0")), "excluded counts differ"),
        (edit_json(PREDICTIONS, lambda r: r.update(extra="1"), line=0), "must have exactly the keys"),
        (
            edit_json(PREDICTIONS, lambda r: r["identity"].update(stream="unrelated-stream"), line=0),
            "differs from the source stream",
        ),
        (
            edit_json(MODEL, lambda m: m["instruments"]["AAPL"]["logistic"].update(iterations="0")),
            "parameters are malformed",
        ),
        (
            edit_json(
                MODEL, lambda m: m["instruments"]["AAPL"]["majority"]["class_counts"].update(down="-1")
            ),
            "must not be negative",
        ),
        (
            edit_json(MODEL, lambda m: m["instruments"]["AAPL"].update(logistic=[])),
            "not a well-formed experiment",
        ),
    ]
    for edit, message in malformed:
        with pytest.raises(InspectionError, match=message):
            verify_experiment(corrupted(edit, resign=True))
    # A persisted infinity cannot even be written down.
    with pytest.raises(InspectionError, match="not a canonical finite float"):
        verify_experiment(corrupted(replace_in(MODEL, f'"{first_coefficient}"'.encode(), b'"inf"'), True))

    # Inspection can run under another library version. The declared
    # estimator must match the artifact's declared environment.
    def older_environment(manifest: Any) -> None:
        manifest["environment"]["scikit_learn"] = "1.9.0"
        manifest["configuration"]["logistic"]["estimator"] = "sklearn.linear_model.LogisticRegression 1.9.0"

    verify_experiment(corrupted(edit_json(MANIFEST, older_environment), resign=True))


def test_mismatched_ancestry_is_refused(
    pipeline: Pipeline, make_pipeline: Callable[..., Pipeline], monkeypatch: pytest.MonkeyPatch
) -> None:
    other = make_pipeline(synthetic_events(seed=9))
    # The split's label artifact is not in the other label root.
    with pytest.raises(ArtifactError, match="refused the artifact"):
        verify_sources(
            pipeline.tools.chronos_dataset,
            features_root=pipeline.features,
            labels_root=other.labels,
            splits_root=pipeline.splits,
            split_artifact_id=pipeline.split_id,
        )
    # A verifier that vouched for an unrelated feature artifact is still
    # caught by the cross-checks.
    real = artifacts._inspect

    def lying(tool: Path, arguments: list[str]) -> Any:
        document: Mapping[str, Any] = real(tool, arguments)
        if arguments[0] == "features":
            document = real(
                tool, ["features", "inspect", "--output", str(other.features), "--artifact", other.feature_id]
            )
        return document

    monkeypatch.setattr(artifacts, "_inspect", lying)
    with pytest.raises(ArtifactError, match="artifact ancestry does not match"):
        verify_sources(
            pipeline.tools.chronos_dataset,
            features_root=pipeline.features,
            labels_root=pipeline.labels,
            splits_root=pipeline.splits,
            split_artifact_id=pipeline.split_id,
        )


def test_rows_changed_after_verification_are_refused(make_pipeline: Callable[..., Pipeline]) -> None:
    pipeline = make_pipeline(synthetic_events(seed=7, quotes_per_instrument=60))
    sources = verify_sources(
        pipeline.tools.chronos_dataset,
        features_root=pipeline.features,
        labels_root=pipeline.labels,
        splits_root=pipeline.splits,
        split_artifact_id=pipeline.split_id,
    )
    rows = sources.labels.rows_path
    rows.parent.chmod(stat.S_IRWXU)
    rows.chmod(stat.S_IRUSR | stat.S_IWUSR)
    data = rows.read_bytes()
    rows.write_bytes(data.replace(b'"direction":"up"', b'"direction":"down"', 1))
    with pytest.raises(ArtifactError, match="changed after it was verified"):
        load_model_data(sources)


def test_a_halted_instrument_is_excluded_with_reasons(
    make_pipeline: Callable[..., Pipeline], tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    pipeline = make_pipeline(synthetic_events(seed=7, halted_spy_quotes=30))
    directory = Path(evaluate(pipeline, tmp_path / "out", capsys)["directory"])
    excluded = json.loads((directory / METRICS).read_bytes())["excluded"]
    reasons = {reason for period in excluded.values() for by in period.values() for reason in by}
    assert "label_unavailable:current_midpoint_unavailable" in reasons
    assert "label_unavailable:future_midpoint_unavailable" in reasons
    verify_experiment(directory)


def test_too_little_data_makes_the_logistic_model_unevaluable(
    make_pipeline: Callable[..., Pipeline], tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    pipeline = make_pipeline(synthetic_events(seed=7, quotes_per_instrument=30))
    result = evaluate(pipeline, tmp_path / "out", capsys)
    directory = Path(result["directory"])
    models = json.loads((directory / MODEL).read_bytes())["instruments"]
    assert {entry["logistic"]["reason"] for entry in models.values()} == {"insufficient_class_support"}
    metrics = json.loads((directory / METRICS).read_bytes())["splits"]["test"]
    assert metrics["aggregate"]["logistic"]["instruments"] == []
    assert metrics["aggregate"]["logistic"]["metrics"]["accuracy"] == {"undefined": "no_rows"}
    assert all(
        entry["logistic"] == {"unevaluable": "insufficient_class_support"}
        for entry in metrics["instruments"].values()
    )
    assert metrics["aggregate"]["majority"]["instruments"] == ["AAPL", "SPY"]
    verify_experiment(directory)


def test_refused_inputs_exit_with_a_json_error(
    pipeline: Pipeline, tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    status = main([
        "evaluate",
        "--chronos-dataset", str(tmp_path / "missing-tool"),
        "--features", str(pipeline.features),
        "--labels", str(pipeline.labels),
        "--splits", str(pipeline.splits),
        "--split-artifact", pipeline.split_id,
        "--output", str(tmp_path / "out"),
    ])  # fmt: skip
    assert status == 1
    error = json.loads(capsys.readouterr().err)["error"]
    assert error["kind"] == "ArtifactError"
    assert "cannot run" in error["message"]
    assert main(["inspect", "--output", str(tmp_path), "--experiment", "f" * 64]) == 1
    assert "not an experiment directory" in json.loads(capsys.readouterr().err)["error"]["message"]
