"""Prediction quality of a schedule against its window's realized labels
(ADR 0021), read only after inference.

A comparison scores each completed member's stored predictions; the model is
not run again and its inputs never include a label. Rows are joined by
identity (stream, instrument, ingest sequence) with model_data.join, as the
experiment joins them. A schedule row is scored when the split assigns it to
the evaluation window with a usable label. Every other row of the window is
counted under the split's reason for leaving it out, such as
`label_unavailable:no_future_observation` or `feature_unavailable:<reason>`.

The metrics are metrics.classification_metrics, the experiment's own
definitions, in binary floating point as documented there; a metric without
a value is undefined with its reason, never zero. This is classification
quality, kept apart from the strategy's trading results.
"""

import json
from collections import Counter
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any

import numpy as np

from chronos_research.artifacts import (
    CLASS_ORDER,
    RowIdentity,
    VerifiedSources,
    read_feature_rows,
    read_label_rows,
    read_split_rows,
)
from chronos_research.encoding import encode_integer
from chronos_research.metrics import classification_metrics
from chronos_research.model_data import join


class ScoringError(Exception):
    """A schedule's rows are not the window's rows, so it cannot be scored."""


@dataclass(frozen=True)
class WindowLabels:
    """The realized class of each scorable row of one instrument's window,
    and how many of its other rows the split left out, by reason."""

    truth: Mapping[RowIdentity, int]
    not_scored: Mapping[str, int]


def window_labels(sources: VerifiedSources, instrument: str, window_name: str) -> WindowLabels:
    data = join(
        read_feature_rows(sources.features),
        read_label_rows(sources.labels),
        read_split_rows(sources.splits),
        validation_start_at_ns=sources.validation_start_at_ns,
        test_start_at_ns=sources.test_start_at_ns,
    )
    truth = {
        row.identity: row.class_index
        for row in data.rows
        if row.split == window_name and row.identity.instrument == instrument
    }
    return WindowLabels(truth, dict(data.excluded[window_name].get(instrument, Counter())))


@dataclass(frozen=True)
class ScoredRows:
    """A schedule's scored rows, in schedule order, and how many of its
    rows were scheduled and left out, by reason."""

    truth: list[int]
    predicted: list[int]
    probabilities: list[list[float]]
    scheduled: int
    not_scored: Mapping[str, int]


def scored_rows(rows: bytes, labels: WindowLabels) -> ScoredRows:
    """Joins a schedule's rows to the window's labels by identity, after
    checking they are exactly the window's rows."""
    scheduled: set[RowIdentity] = set()
    truth: list[int] = []
    predicted: list[int] = []
    probabilities: list[list[float]] = []
    for line in rows.splitlines():
        record = json.loads(line)
        identity = RowIdentity(
            record["identity"]["stream"],
            record["identity"]["instrument"],
            int(record["identity"]["ingest_sequence"]),
        )
        scheduled.add(identity)
        true_class = labels.truth.get(identity)
        if true_class is None:
            continue
        prediction = record["prediction"]
        if "unavailable" in prediction:
            raise ScoringError(f"row {tuple(identity)} has a usable label but no prediction")
        truth.append(true_class)
        predicted.append(CLASS_ORDER.index(prediction["class"]))
        probabilities.append([float(value) for value in prediction["probabilities"]])
    missing = labels.truth.keys() - scheduled
    if missing:
        raise ScoringError(f"{len(missing)} labelled rows of the window are not in the schedule")
    if len(scheduled) != len(truth) + sum(labels.not_scored.values()):
        raise ScoringError("the schedule's rows are not the window's rows")
    return ScoredRows(truth, predicted, probabilities, len(scheduled), dict(labels.not_scored))


def scores(scored: ScoredRows) -> dict[str, Any]:
    """The classification metrics of scored rows, and every row's count."""
    rows = len(scored.truth)
    return {
        "metrics": classification_metrics(
            np.array(scored.truth, dtype=np.int64),
            np.array(scored.predicted, dtype=np.int64),
            np.array(scored.probabilities, dtype=np.float64).reshape(rows, len(CLASS_ORDER)),
        ),
        "rows": {
            "not_scored": {
                reason: encode_integer(count) for reason, count in sorted(scored.not_scored.items())
            },
            "scheduled": encode_integer(scored.scheduled),
            "scored": encode_integer(rows),
        },
    }


def pooled(parts: Sequence[ScoredRows]) -> ScoredRows:
    """Several schedules' scored rows as one sample, in order: metrics of
    it weight every scored row equally, never every schedule."""
    not_scored: Counter[str] = Counter()
    for part in parts:
        not_scored.update(part.not_scored)
    return ScoredRows(
        truth=[value for part in parts for value in part.truth],
        predicted=[value for part in parts for value in part.predicted],
        probabilities=[value for part in parts for value in part.probabilities],
        scheduled=sum(part.scheduled for part in parts),
        not_scored=dict(not_scored),
    )


def score_schedule(rows: bytes, labels: WindowLabels) -> dict[str, Any]:
    """The classification metrics of a schedule's scorable rows, and the
    count of every row by whether and why it was scored."""
    return scores(scored_rows(rows, labels))
