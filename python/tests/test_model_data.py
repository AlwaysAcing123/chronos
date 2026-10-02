from dataclasses import replace

import pytest

from chronos_research.artifacts import ArtifactError, RowIdentity, Unavailable
from chronos_research.model_data import INPUTS, exact_inputs, to_floats
from tests.rows import PRICE, QUANTITY, STREAM, Spec, feature_row, market


def small_market() -> list[Spec]:
    return [
        Spec("AAPL", 10, "train", "up", imbalance=250_000_000),
        Spec("SPY", 10, "train", "down"),
        Spec("AAPL", 20, "train", None),
        Spec("SPY", 30, "train", "flat", halted=True),
        Spec("AAPL", 40, "validation", "down"),
        Spec("SPY", 50, "test", "up"),
    ]


def test_rows_join_by_identity_and_keep_feature_order() -> None:
    data = market(small_market(), validation_start_at_ns=40, test_start_at_ns=50).joined()
    assert [row.identity for row in data.rows] == [
        RowIdentity(STREAM, "AAPL", 0),
        RowIdentity(STREAM, "SPY", 1),
        RowIdentity(STREAM, "AAPL", 4),
        RowIdentity(STREAM, "SPY", 5),
    ]
    assert [row.split for row in data.rows] == ["train", "train", "validation", "test"]
    assert [row.class_index for row in data.rows] == [2, 0, 0, 2]
    # Every exclusion is counted with its reason, by period and instrument.
    assert data.excluded["train"] == {
        "AAPL": {"label_unavailable:current_midpoint_unavailable": 1},
        "SPY": {"label_unavailable:current_midpoint_unavailable": 1},
    }
    assert data.excluded["validation"] == {}
    assert data.excluded["test"] == {}


def test_order_of_artifact_rows_does_not_matter_but_identity_does() -> None:
    world = market(small_market(), validation_start_at_ns=40, test_start_at_ns=50)
    shuffled = replace(world, labels=world.labels[::-1], splits=world.splits[1:] + world.splits[:1])
    assert shuffled.joined() == world.joined()


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda w: replace(w, labels=w.labels[:-1]), "has no label row"),
        (lambda w: replace(w, splits=w.splits[1:]), "has no split row"),
        (lambda w: replace(w, labels=[*w.labels, w.labels[0]]), "duplicate label row identity"),
        # One row assigned to two splits is a duplicate identity.
        (
            lambda w: replace(w, splits=[*w.splits, replace(w.splits[0], split="test", period="test")]),
            "duplicate split",
        ),
        (lambda w: replace(w, features=[*w.features, w.features[0]]), "duplicate feature row identity"),
        (
            lambda w: replace(
                w, labels=[*w.labels, replace(w.labels[0], identity=RowIdentity(STREAM, "QQQ", 9))]
            ),
            "1 label rows have no feature row",
        ),
        (
            lambda w: replace(w, labels=[replace(w.labels[0], prediction_at_ns=11), *w.labels[1:]]),
            "prediction times",
        ),
        (
            lambda w: replace(w, labels=[replace(w.labels[0], label_available_at_ns=12), *w.labels[1:]]),
            "label availability differs",
        ),
        (lambda w: replace(w, splits=[replace(w.splits[0], label_usable=False), *w.splits[1:]]), "usability"),
        (
            lambda w: replace(w, splits=[replace(w.splits[0], split="validation"), *w.splits[1:]]),
            "falls in train",
        ),
        (
            lambda w: replace(w, splits=[replace(w.splits[0], split="holdout"), *w.splits[1:]]),
            "unknown split",
        ),
        (
            lambda w: replace(w, labels=[replace(w.labels[0], direction="sideways"), *w.labels[1:]]),
            "direction",
        ),
    ],
)
def test_inconsistent_artifacts_are_refused(change: object, message: str) -> None:
    world = market(small_market(), validation_start_at_ns=40, test_start_at_ns=50)
    with pytest.raises(ArtifactError, match=message):
        change(world).joined()  # type: ignore[operator]


def test_a_label_known_after_the_next_period_begins_is_refused() -> None:
    # The C++ verifier refuses this too; the join checks it again.
    world = market(small_market(), delay=30, validation_start_at_ns=40, test_start_at_ns=50)
    with pytest.raises(ArtifactError, match="known after the next period begins"):
        world.joined()
    # Train labels are known at 40: usable once validation starts at 41.
    # The validation label is known at 70, exactly when test starts.
    exact = market(small_market(), delay=30, validation_start_at_ns=41, test_start_at_ns=70)
    with pytest.raises(ArtifactError, match="usable validation label is known after the next period begins"):
        exact.joined()
    assert market(small_market(), delay=30, validation_start_at_ns=41, test_start_at_ns=71).joined().rows


def test_inputs_are_exact_until_the_model_boundary() -> None:
    row = feature_row(Spec("AAPL", 1, "train", imbalance=-333_333_333), 0)
    inputs = exact_inputs(row)
    # microprice - midpoint is formed from the raw integers: 125000001.
    assert inputs == (PRICE, -333_333_333, 125_000_001, 5 * QUANTITY, 3 * QUANTITY)
    assert len(INPUTS) == 5
    assert not isinstance(inputs, Unavailable)
    matrix = to_floats([inputs])
    assert matrix.tolist() == [[1.0, -0.333333333, 0.125000001, 5.0, 3.0]]
    assert exact_inputs(feature_row(Spec("AAPL", 1, "train", halted=True), 0)) == Unavailable(
        "trading_halted"
    )


def test_a_usable_label_with_an_unavailable_input_is_excluded_by_reason() -> None:
    world = market(small_market(), validation_start_at_ns=40, test_start_at_ns=50)
    broken = replace(
        world.features[0],
        values={**world.features[0].values, "microprice": Unavailable("arithmetic_overflow")},
    )
    data = replace(world, features=[broken, *world.features[1:]]).joined()
    assert data.excluded["train"]["AAPL"] == {
        "feature_unavailable:arithmetic_overflow": 1,
        "label_unavailable:current_midpoint_unavailable": 1,
    }
    assert RowIdentity(STREAM, "AAPL", 0) not in {row.identity for row in data.rows}
