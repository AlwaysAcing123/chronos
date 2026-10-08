"""Scoring a schedule's stored predictions against its window's realized
labels: only rows the split assigns to the window with a usable label, the
experiment's metric definitions, undefined metrics with their reasons, and
no scoring of a schedule whose rows are not the window's."""

import json
import math
from typing import Any

import numpy as np
import pytest

from chronos_research.artifacts import RowIdentity
from chronos_research.encoding import encode_float
from chronos_research.metrics import classification_metrics
from chronos_research.schedule import ROW_RECORD
from chronos_research.scoring import ScoringError, WindowLabels, score_schedule

DOWN, FLAT, UP = 0, 1, 2


def identity(sequence: int) -> RowIdentity:
    return RowIdentity("stream", "AAPL", sequence)


def row(sequence: int, predicted: str | None, probabilities: tuple[float, float, float] = (0, 0, 0)) -> bytes:
    prediction: dict[str, Any] = (
        {"unavailable": "feature_unavailable:no_quote"}
        if predicted is None
        else {
            "class": predicted,
            "confidence_ppb": "0",
            "probabilities": [encode_float(p) for p in probabilities],
        }
    )
    record = {
        "available_at_ns": str(1000 + sequence),
        "identity": {"ingest_sequence": str(sequence), "instrument": "AAPL", "stream": "stream"},
        "inputs": {"unavailable": "no_quote"} if predicted is None else {"raw": ["1", "0", "0", "1", "1"]},
        "prediction": prediction,
        "record": ROW_RECORD,
    }
    return json.dumps(record, sort_keys=True).encode() + b"\n"


ROWS = b"".join(
    [
        row(1, "up", (0.1, 0.2, 0.7)),
        row(2, "up", (0.3, 0.3, 0.4)),
        row(3, "flat", (0.2, 0.6, 0.2)),
        # Predicted, but its label was never observed.
        row(4, "down", (0.5, 0.25, 0.25)),
        row(5, None),
    ]
)
LABELS = WindowLabels(
    truth={identity(1): UP, identity(2): DOWN, identity(3): FLAT},
    not_scored={"feature_unavailable:no_quote": 1, "label_unavailable:no_future_observation": 1},
)


def test_only_the_windows_labelled_rows_are_scored_with_the_experiments_definitions() -> None:
    scored = score_schedule(ROWS, LABELS)
    assert scored["rows"] == {
        "not_scored": {"feature_unavailable:no_quote": "1", "label_unavailable:no_future_observation": "1"},
        "scheduled": "5",
        "scored": "3",
    }
    expected = classification_metrics(
        np.array([UP, DOWN, FLAT]),
        np.array([UP, UP, FLAT]),
        np.array([[0.1, 0.2, 0.7], [0.3, 0.3, 0.4], [0.2, 0.6, 0.2]]),
    )
    assert scored["metrics"] == expected
    # By hand: two of three right; the log loss of the true classes' probabilities.
    assert scored["metrics"]["accuracy"] == {"value": encode_float(2 / 3)}
    assert scored["metrics"]["log_loss"] == {
        "value": encode_float(-(math.log(0.7) + math.log(0.3) + math.log(0.6)) / 3)
    }
    assert scored["metrics"]["per_class"]["down"]["recall"] == {"value": encode_float(0.0)}
    assert scored["metrics"]["per_class"]["down"]["precision"] == {"undefined": "class_never_predicted"}


def test_without_a_scorable_row_every_metric_is_undefined_not_zero() -> None:
    scored = score_schedule(
        row(4, "down", (0.5, 0.25, 0.25)) + row(5, None),
        WindowLabels(
            truth={},
            not_scored={"feature_unavailable:no_quote": 1, "label_unavailable:no_future_observation": 1},
        ),
    )
    assert scored["rows"]["scored"] == "0"
    for name in ("accuracy", "balanced_accuracy", "brier_score", "log_loss"):
        assert scored["metrics"][name] == {"undefined": "no_rows"}


@pytest.mark.parametrize(
    ("rows", "labels", "message"),
    [
        # A labelled row the schedule has no prediction for.
        (row(1, None), WindowLabels({identity(1): UP}, {}), "no prediction"),
        # A labelled row missing from the schedule.
        (row(1, "up", (0.1, 0.2, 0.7)), WindowLabels({identity(1): UP, identity(2): DOWN}, {}), "not in"),
        # A schedule row that is neither scored nor left out by the split.
        (
            row(1, "up", (0.1, 0.2, 0.7)) + row(9, "up", (0.1, 0.2, 0.7)),
            WindowLabels({identity(1): UP}, {}),
            "rows",
        ),
    ],
)
def test_a_schedule_whose_rows_are_not_the_windows_is_not_scored(
    rows: bytes, labels: WindowLabels, message: str
) -> None:
    with pytest.raises(ScoringError, match=message):
        score_schedule(rows, labels)
