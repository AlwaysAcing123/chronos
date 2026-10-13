"""Decision cohorts of a strategy run, described after the fact (ADR 0024).

Four cohorts, each a subset of the one before:

    scorable_predictions        schedule rows with a prediction
    sampled_decisions           the strategy's journaled decisions
    order_scheduling_decisions  those whose reason is order_scheduled
    filled_orders               those whose order filled, at least in part

Each item has its predicted class, its confidence, and its label status:
`scored` with its realized class, `label_unavailable:<reason>`, or
`label_after_scoring_cutoff`, by the plan's scoring rule (the fold's
`fold_labels`). Labels are read only to describe what happened: nothing
here reaches a model, a policy, an order, or a choice of what to run.

Counts are kept as integers so that cohorts pool exactly: pooled ratios
come from pooled counts, never from averaging ratios. Accuracy is correct /
scored; balanced accuracy is the mean recall over the realized classes that
occur. Each is an exact reduced fraction with a 6-place decimal rounded
half away from zero, or undefined with its reason. Confidence bins are
fixed: ten tenths of [0, 1] in parts per billion, the last closed.

Each cohort also summarizes its scored items' future returns in the label
artifact's own unit, `return_raw`: (future midpoint - current midpoint) /
current midpoint in parts per billion, rounded half away from zero by the
label generator. Rows, the exact sum, the extremes, and the exact mean, for
the cohort, each predicted class, and each confidence bin; items without a
scored label are counted, never given a return. Sums and counts pool, and
means come from pooled sums.
"""

from collections import Counter
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from typing import Any

from chronos_research.artifacts import CLASS_ORDER, RowIdentity, Unavailable
from chronos_research.model_data import exact_inputs
from chronos_research.schedule import Window
from chronos_research.strategy_attribution import FILLED, Decision, ScheduleRow
from chronos_research.walk_forward import LabelledRow

CONFIDENCE_SCALE = 1_000_000_000
BIN_WIDTH = CONFIDENCE_SCALE // 10
CONFIDENCE_BINS = tuple(
    {
        "lower_ppb": str(index * BIN_WIDTH),
        "name": f"{index / 10:.1f}-{(index + 1) / 10:.1f}",
        "upper": "inclusive" if index == 9 else "exclusive",
        "upper_ppb": str((index + 1) * BIN_WIDTH),
    }
    for index in range(10)
)
COHORTS = ("scorable_predictions", "sampled_decisions", "order_scheduling_decisions", "filled_orders")
SCORED = "scored"
RATIO_PLACES = 6


@dataclass(frozen=True, slots=True)
class LabelStatus:
    status: str
    true_class: int | None = None
    # The label's return_raw, for a scored item only.
    return_raw: int | None = None


def label_statuses(rows: Sequence[LabelledRow], window: Window) -> dict[RowIdentity, LabelStatus]:
    """Every feature row of the window by identity, with its status under the
    plan's scoring rule, exactly as fold_labels classifies it."""
    assert window.end_at_ns is not None
    statuses: dict[RowIdentity, LabelStatus] = {}
    for row in rows:
        if not window.contains(row.feature.available_at_ns):
            continue
        label = row.label
        inputs = exact_inputs(row.feature)
        if isinstance(inputs, Unavailable):
            status = LabelStatus(f"feature_unavailable:{inputs.reason}")
        elif label.direction is None:
            status = LabelStatus(f"label_unavailable:{label.unavailable_reason}")
        elif label.label_available_at_ns is None or label.label_available_at_ns >= window.end_at_ns:
            status = LabelStatus("label_after_scoring_cutoff")
        elif label.return_raw is None:
            raise ValueError(
                f"label row {row.feature.identity.ingest_sequence} has a direction but no return"
            )
        else:
            status = LabelStatus(SCORED, CLASS_ORDER.index(label.direction), label.return_raw)
        statuses[row.feature.identity] = status
    return statuses


def ratio(numerator: int, denominator: int, undefined: str) -> dict[str, str]:
    if denominator == 0:
        return {"undefined": undefined}
    return fraction_json(Fraction(numerator, denominator))


def fraction_json(value: Fraction) -> dict[str, str]:
    return {
        "denominator": str(value.denominator),
        "numerator": str(value.numerator),
        "rounded": rounded(value, RATIO_PLACES),
    }


def rounded(value: Fraction, places: int) -> str:
    """Half away from zero, canonical: no trailing zeros, no negative zero."""
    scaled = abs(value) * 10**places
    whole = scaled.numerator // scaled.denominator
    if scaled - whole >= Fraction(1, 2):
        whole += 1
    units, fraction = divmod(whole, 10**places)
    text = str(units)
    if fraction:
        text += "." + str(fraction).rjust(places, "0").rstrip("0")
    return f"-{text}" if value < 0 and whole else text


def _bin(confidence_ppb: int) -> int:
    if not 0 <= confidence_ppb <= CONFIDENCE_SCALE:
        raise ValueError(f"confidence {confidence_ppb} ppb is outside [0, 10^9]")
    return min(confidence_ppb // BIN_WIDTH, 9)


@dataclass
class Returns:
    """Future returns in return_raw: rows, the exact sum, and the extremes."""

    rows: int = 0
    total: int = 0
    minimum: int | None = None
    maximum: int | None = None

    def add(self, value: int) -> None:
        self.rows += 1
        self.total += value
        self.minimum = value if self.minimum is None else min(self.minimum, value)
        self.maximum = value if self.maximum is None else max(self.maximum, value)

    def merge(self, other: "Returns") -> None:
        if other.minimum is not None:
            self.minimum = other.minimum if self.minimum is None else min(self.minimum, other.minimum)
        if other.maximum is not None:
            self.maximum = other.maximum if self.maximum is None else max(self.maximum, other.maximum)
        self.rows += other.rows
        self.total += other.total

    def document(self) -> dict[str, Any]:
        def extreme(value: int | None) -> dict[str, str]:
            return {"undefined": "no_scored_items"} if value is None else {"value": str(value)}

        return {
            "maximum_raw": extreme(self.maximum),
            "mean_raw": ratio(self.total, self.rows, "no_scored_items"),
            "minimum_raw": extreme(self.minimum),
            "rows": str(self.rows),
            "sum_raw": str(self.total),
        }


def _returns_by(count: int) -> list[Returns]:
    return [Returns() for _ in range(count)]


@dataclass
class CohortCounts:
    """A cohort's counts, additive across runs and folds."""

    size: int = 0
    labels: Counter[str] = field(default_factory=Counter)
    predicted: list[int] = field(default_factory=lambda: [0] * len(CLASS_ORDER))
    confusion: list[list[int]] = field(default_factory=lambda: [[0] * len(CLASS_ORDER) for _ in CLASS_ORDER])
    # Per bin: items, scored items, correct items.
    bins: list[list[int]] = field(default_factory=lambda: [[0, 0, 0] for _ in range(10)])
    # Decision reasons or order outcomes, when the cohort has them.
    reasons: Counter[str] = field(default_factory=Counter)
    # Scored items' future returns: all of them, by predicted class, by bin.
    returns: Returns = field(default_factory=Returns)
    returns_by_class: list[Returns] = field(default_factory=lambda: _returns_by(len(CLASS_ORDER)))
    returns_by_bin: list[Returns] = field(default_factory=lambda: _returns_by(10))

    def add(
        self, predicted_class: str, confidence_ppb: int, status: LabelStatus, reason: str | None = None
    ) -> None:
        predicted = CLASS_ORDER.index(predicted_class)
        self.size += 1
        self.labels[status.status] += 1
        self.predicted[predicted] += 1
        bucket = self.bins[_bin(confidence_ppb)]
        bucket[0] += 1
        if reason is not None:
            self.reasons[reason] += 1
        if status.status == SCORED:
            assert status.true_class is not None
            assert status.return_raw is not None
            self.confusion[status.true_class][predicted] += 1
            bucket[1] += 1
            bucket[2] += int(status.true_class == predicted)
            self.returns.add(status.return_raw)
            self.returns_by_class[predicted].add(status.return_raw)
            self.returns_by_bin[_bin(confidence_ppb)].add(status.return_raw)

    def merge(self, other: "CohortCounts") -> None:
        self.size += other.size
        self.labels.update(other.labels)
        self.reasons.update(other.reasons)
        for index in range(len(CLASS_ORDER)):
            self.predicted[index] += other.predicted[index]
            for column in range(len(CLASS_ORDER)):
                self.confusion[index][column] += other.confusion[index][column]
        for mine, theirs in zip(self.bins, other.bins, strict=True):
            for position in range(3):
                mine[position] += theirs[position]
        self.returns.merge(other.returns)
        for own, their in zip(
            [*self.returns_by_class, *self.returns_by_bin],
            [*other.returns_by_class, *other.returns_by_bin],
            strict=True,
        ):
            own.merge(their)

    @property
    def scored(self) -> int:
        return sum(sum(row) for row in self.confusion)

    @property
    def correct(self) -> int:
        return sum(self.confusion[index][index] for index in range(len(CLASS_ORDER)))

    def balanced_accuracy(self) -> Fraction | None:
        recalls = [Fraction(row[index], sum(row)) for index, row in enumerate(self.confusion) if sum(row) > 0]
        return sum(recalls, Fraction(0)) / len(recalls) if recalls else None

    def document(self) -> dict[str, Any]:
        balanced = self.balanced_accuracy()
        return {
            "accuracy": ratio(self.correct, self.scored, "no_scored_items"),
            "balanced_accuracy": (
                {"undefined": "no_scored_items"} if balanced is None else fraction_json(balanced)
            ),
            "confidence_bins": [
                {
                    "accuracy": ratio(correct, scored, "no_scored_items"),
                    "correct": str(correct),
                    "items": str(items),
                    "name": CONFIDENCE_BINS[index]["name"],
                    "scored": str(scored),
                }
                for index, (items, scored, correct) in enumerate(self.bins)
            ],
            "confusion": {
                true_name: {
                    predicted_name: str(self.confusion[true_index][predicted_index])
                    for predicted_index, predicted_name in enumerate(CLASS_ORDER)
                }
                for true_index, true_name in enumerate(CLASS_ORDER)
            },
            "correct": str(self.correct),
            "future_returns": {
                "all": self.returns.document(),
                "by_confidence_bin": [
                    {"name": CONFIDENCE_BINS[index]["name"], **returns.document()}
                    for index, returns in enumerate(self.returns_by_bin)
                ],
                "by_predicted_class": {
                    name: self.returns_by_class[index].document() for index, name in enumerate(CLASS_ORDER)
                },
                "not_scored": str(self.size - self.scored),
            },
            "items": str(self.size),
            "labels": {status: str(count) for status, count in sorted(self.labels.items())},
            "predicted": {name: str(self.predicted[index]) for index, name in enumerate(CLASS_ORDER)},
            "realized": {name: str(sum(self.confusion[index])) for index, name in enumerate(CLASS_ORDER)},
            "reasons": {reason: str(count) for reason, count in sorted(self.reasons.items())},
            "scored": str(self.scored),
        }


def pooled(parts: Iterable[CohortCounts]) -> CohortCounts:
    result = CohortCounts()
    for part in parts:
        result.merge(part)
    return result


@dataclass(frozen=True)
class Predictions:
    """A schedule's scorable predictions, and its rows without one."""

    cohort: CohortCounts
    scheduled: int
    feature_unavailable: Mapping[str, int]


def scorable_predictions(
    rows: Sequence[ScheduleRow], stream: str, instrument: str, statuses: Mapping[RowIdentity, LabelStatus]
) -> Predictions:
    """Cohort 1. Every schedule row must be a row of the window with a status,
    and every scheduled row's inputs must agree with it about availability."""
    cohort = CohortCounts()
    unavailable: Counter[str] = Counter()
    for row in rows:
        status = statuses.get(RowIdentity(stream, instrument, row.ingest_sequence))
        if status is None:
            raise ValueError(f"schedule row {row.ingest_sequence} is not a feature row of the window")
        if row.predicted_class is None:
            if not status.status.startswith("feature_unavailable:"):
                raise ValueError(f"schedule row {row.ingest_sequence} has no prediction but available inputs")
            unavailable[status.status] += 1
            continue
        if status.status.startswith("feature_unavailable:"):
            raise ValueError(f"schedule row {row.ingest_sequence} predicts from unavailable inputs")
        assert row.confidence_ppb is not None
        cohort.add(row.predicted_class, row.confidence_ppb, status)
    if len(rows) != sum(1 for status in statuses.values()):
        raise ValueError("the schedule's rows are not the window's rows")
    return Predictions(cohort, len(rows), dict(unavailable))


def predictions_document(predictions: Predictions) -> dict[str, Any]:
    return {
        "cohort": predictions.cohort.document(),
        "feature_unavailable": {
            reason: str(count) for reason, count in sorted(predictions.feature_unavailable.items())
        },
        "scheduled": str(predictions.scheduled),
    }


def decision_cohorts(
    decisions: Sequence[Decision], stream: str, instrument: str, statuses: Mapping[RowIdentity, LabelStatus]
) -> dict[str, CohortCounts]:
    """Cohorts 2 to 4 of one run's strategy decisions."""
    sampled, scheduling, filled = CohortCounts(), CohortCounts(), CohortCounts()
    for decision in decisions:
        status = statuses.get(RowIdentity(stream, instrument, decision.ingest_sequence))
        if status is None or status.status.startswith("feature_unavailable:"):
            raise ValueError(f"decision {decision.number}'s signal is not a predicted row of the window")
        sampled.add(decision.predicted_class, decision.confidence_ppb, status, decision.reason)
        if decision.reason == "order_scheduled":
            assert decision.order_outcome is not None
            scheduling.add(decision.predicted_class, decision.confidence_ppb, status, decision.order_outcome)
            if decision.order_outcome in FILLED:
                filled.add(decision.predicted_class, decision.confidence_ppb, status, decision.order_outcome)
    return {"sampled_decisions": sampled, "order_scheduling_decisions": scheduling, "filled_orders": filled}


def study_scores_agree(predictions: Predictions, predictive: Mapping[str, Any]) -> str | None:
    """Where cohort 1 differs from the study's own scores of the same
    schedule, if it does: every count exactly, accuracy to the bit."""
    cohort = predictions.cohort
    rows = predictive.get("rows", {})
    not_scored = {
        **predictions.feature_unavailable,
        **{status: count for status, count in cohort.labels.items() if status != SCORED},
    }
    if rows.get("scheduled") != str(predictions.scheduled) or rows.get("scored") != str(cohort.scored):
        return "the scheduled or scored row counts"
    if rows.get("not_scored") != {reason: str(count) for reason, count in sorted(not_scored.items())}:
        return "the rows not scored, by reason"
    metrics = predictive.get("metrics", {})
    confusion = {
        true_name: {
            predicted_name: str(cohort.confusion[true_index][predicted_index])
            for predicted_index, predicted_name in enumerate(CLASS_ORDER)
        }
        for true_index, true_name in enumerate(CLASS_ORDER)
    }
    if metrics.get("confusion_matrix") != confusion:
        return "the confusion counts"
    accuracy = metrics.get("accuracy", {})
    if cohort.scored == 0:
        return None if "undefined" in accuracy else "the accuracy"
    if accuracy.get("value") != repr(cohort.correct / cohort.scored):
        return "the accuracy"
    balanced = cohort.balanced_accuracy()
    stored = metrics.get("balanced_accuracy", {}).get("value")
    if balanced is None or stored is None or abs(float(stored) - float(balanced)) > 1e-12:
        return "the balanced accuracy"
    return None
