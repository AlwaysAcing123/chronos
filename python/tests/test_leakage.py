"""Adversarial tests: a model fitted with any information beyond the
training rows' inputs and classes would change when that information does.

Each test mutates something a leaky implementation might use (validation and
test labels, inputs, and returns; label availability times; the size of a
return behind a training label) and requires the fitted models to stay byte
for byte the same. A positive control proves the comparison is sensitive.
"""

import json
from dataclasses import replace

import numpy as np

from chronos_research.experiment import MODEL, ExperimentConfiguration, build_experiment
from chronos_research.model_data import INPUT_SCALES, to_floats
from tests.rows import Market, Spec, market, sources

ROWS = 300
TRAIN, VALIDATION = 180, 240


def world(seed: int = 5) -> Market:
    generator = np.random.default_rng(seed)
    specs = []
    for index in range(ROWS):
        imbalance = int(generator.integers(-900_000_000, 900_000_000))
        signal = imbalance / 1e9 + generator.normal(scale=0.4)
        direction = "down" if signal < -0.25 else ("up" if signal > 0.25 else "flat")
        split = "train" if index < TRAIN else ("validation" if index < VALIDATION else "test")
        specs.append(Spec("AAPL", index * 10, split, direction, imbalance=imbalance))
    return market(specs, delay=1, validation_start_at_ns=TRAIN * 10, test_start_at_ns=VALIDATION * 10)


def model_bytes(market_rows: Market) -> bytes:
    experiment = build_experiment(sources(), market_rows.joined(), ExperimentConfiguration())
    return experiment.files[MODEL]


def evaluated(market_rows: Market) -> list[int]:
    return [i for i, row in enumerate(market_rows.splits) if row.split in ("validation", "test")]


def test_the_models_are_fitted_from_training_rows_only() -> None:
    base = world()
    models = json.loads(model_bytes(base))["instruments"]["AAPL"]
    assert models["logistic"]["status"] == "fitted"
    training = [row for row in base.joined().rows if row.split == "train"]
    expected_mean = to_floats([row.inputs for row in training]).mean(axis=0)
    assert np.allclose(
        [float(v) for v in models["logistic"]["scaler"]["mean"]], expected_mean, rtol=1e-15, atol=0
    )
    counts = np.bincount([row.class_index for row in training], minlength=3)
    assert models["majority"]["class_counts"] == {
        "down": str(counts[0]),
        "flat": str(counts[1]),
        "up": str(counts[2]),
    }


def test_evaluation_rows_cannot_change_the_models() -> None:
    base = world()
    labels = list(base.labels)
    features = list(base.features)
    rotate = {"down": "flat", "flat": "up", "up": "down"}
    for index in evaluated(base):
        label = labels[index]
        assert label.direction is not None
        # Another class, an extreme return, and the inputs of a different
        # market: a fit that saw any of it would differ.
        labels[index] = replace(label, direction=rotate[label.direction], return_raw=-(10**12))
        values = dict(features[index].values)
        imbalance = values["imbalance"]
        assert isinstance(imbalance, int)
        values["imbalance"] = -imbalance
        values["bid_size"] = 10**6 * INPUT_SCALES[3]
        features[index] = replace(features[index], values=values)
    mutated = replace(base, labels=labels, features=features)
    assert model_bytes(mutated) == model_bytes(base)
    # The evaluation itself does see the change.
    base_metrics = build_experiment(sources(), base.joined(), ExperimentConfiguration()).files["metrics.json"]
    mutated_metrics = build_experiment(sources(), mutated.joined(), ExperimentConfiguration()).files[
        "metrics.json"
    ]
    assert base_metrics != mutated_metrics


def test_label_timing_and_return_size_cannot_change_the_models() -> None:
    base = world()
    labels = list(base.labels)
    splits = list(base.splits)
    for index, label in enumerate(labels):
        assert label.return_raw is not None
        assert label.label_available_at_ns is not None
        later = label.label_available_at_ns + (5 if splits[index].split != "test" else 10**6)
        # Same direction, much larger return; a later but still valid
        # availability time.
        labels[index] = replace(label, return_raw=label.return_raw * 977, label_available_at_ns=later)
        splits[index] = replace(splits[index], label_available_at_ns=later)
    assert model_bytes(replace(base, labels=labels, splits=splits)) == model_bytes(base)


def test_the_comparison_is_sensitive_to_training_labels() -> None:
    base = world()
    labels = list(base.labels)
    labels[0] = replace(labels[0], direction="down" if labels[0].direction != "down" else "up")
    assert model_bytes(replace(base, labels=labels)) != model_bytes(base)
