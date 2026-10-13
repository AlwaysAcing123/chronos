"""Decision cohorts (ADR 0024): label statuses under the plan's scoring rule,
fixed confidence bins, exact ratios, empty cohorts, and pooling that never
averages ratios."""

from fractions import Fraction
from typing import Any

import pytest

from chronos_research.artifacts import FeatureRow, LabelRow, RowIdentity, Unavailable
from chronos_research.decision_diagnostics import (
    CONFIDENCE_BINS,
    CohortCounts,
    LabelStatus,
    decision_cohorts,
    label_statuses,
    pooled,
    ratio,
    rounded,
    scorable_predictions,
    study_scores_agree,
)
from chronos_research.schedule import Window
from chronos_research.strategy_attribution import Decision, ScheduleRow
from chronos_research.walk_forward import LabelledRow

STREAM, INSTRUMENT = "s", "AAPL"
VALUES: dict[str, int | Unavailable] = {
    "spread": 1,
    "imbalance": 0,
    "microprice": 5,
    "midpoint": 5,
    "bid_size": 1,
    "ask_size": 1,
}


def labelled(
    sequence: int,
    at: int,
    direction: str | None,
    label_at: int | None,
    *,
    inputs: bool = True,
    return_raw: int = 1,
) -> LabelledRow:
    identity = RowIdentity(STREAM, INSTRUMENT, sequence)
    values: dict[str, int | Unavailable] = dict(VALUES)
    if not inputs:
        values["spread"] = Unavailable("missing_ask")
    return LabelledRow(
        FeatureRow(identity, at, values),
        LabelRow(
            identity,
            at,
            label_at,
            direction,
            None if direction is None else return_raw,
            "no_future_observation" if direction is None else None,
        ),
    )


def test_label_statuses_follow_the_plans_scoring_rule() -> None:
    window = Window("fold_1", 100, 200)
    statuses = label_statuses(
        [
            labelled(1, 99, "up", 150),  # before the window: not one of its rows
            labelled(2, 100, "up", 150, return_raw=338_207),
            labelled(3, 110, None, None),  # no future observation
            labelled(4, 120, "down", 200, return_raw=-12),  # known only at the exclusive cutoff
            labelled(5, 130, "flat", 199, return_raw=0),
            labelled(6, 140, "up", 150, inputs=False),
            labelled(7, 200, "up", 150),  # at the exclusive end
        ],
        window,
    )
    assert {identity.ingest_sequence: status for identity, status in statuses.items()} == {
        # A scored row carries its label's return_raw; no other row has one.
        2: LabelStatus("scored", 2, 338_207),
        3: LabelStatus("label_unavailable:no_future_observation"),
        4: LabelStatus("label_after_scoring_cutoff"),
        5: LabelStatus("scored", 1, 0),
        6: LabelStatus("feature_unavailable:missing_ask"),
    }


def test_confidence_bins_are_fixed_tenths_with_the_last_closed() -> None:
    assert [item["name"] for item in CONFIDENCE_BINS] == [
        f"{n / 10:.1f}-{(n + 1) / 10:.1f}" for n in range(10)
    ]
    cohort = CohortCounts()
    for confidence in (0, 99_999_999, 100_000_000, 499_999_999, 500_000_000, 999_999_999, 1_000_000_000):
        cohort.add("up", confidence, LabelStatus("scored", 2, 0))
    assert [bucket[0] for bucket in cohort.bins] == [2, 1, 0, 0, 1, 1, 0, 0, 0, 2]
    with pytest.raises(ValueError, match="outside"):
        cohort.add("up", 1_000_000_001, LabelStatus("scored", 2, 0))


def test_ratios_are_exact_fractions_rounded_once_half_away_from_zero() -> None:
    assert ratio(1, 3, "none") == {"denominator": "3", "numerator": "1", "rounded": "0.333333"}
    assert ratio(2, 4, "none") == {"denominator": "2", "numerator": "1", "rounded": "0.5"}
    # Exactly half a unit in the sixth place rounds away from zero.
    assert rounded(Fraction(1, 2_000_000), 6) == "0.000001"
    assert rounded(Fraction(-1, 2_000_000), 6) == "-0.000001"
    assert rounded(Fraction(-1, 3_000_000), 6) == "0"
    assert ratio(0, 0, "no_scored_items") == {"undefined": "no_scored_items"}


def test_an_empty_cohort_is_undefined_never_zero() -> None:
    document = CohortCounts().document()
    assert document["accuracy"] == {"undefined": "no_scored_items"}
    assert document["balanced_accuracy"] == {"undefined": "no_scored_items"}
    assert document["items"] == "0"
    assert all(
        bucket["accuracy"] == {"undefined": "no_scored_items"} for bucket in document["confidence_bins"]
    )
    # Items without a usable label are counted but never scored.
    unlabelled = CohortCounts()
    unlabelled.add("up", 600_000_000, LabelStatus("label_after_scoring_cutoff"))
    document = unlabelled.document()
    assert (document["items"], document["scored"]) == ("1", "0")
    assert document["labels"] == {"label_after_scoring_cutoff": "1"}
    assert document["accuracy"] == {"undefined": "no_scored_items"}


def test_balanced_accuracy_averages_recall_over_realized_classes_only() -> None:
    cohort = CohortCounts()
    for _ in range(3):
        cohort.add("up", 600_000_000, LabelStatus("scored", 2, 0))
    cohort.add("down", 600_000_000, LabelStatus("scored", 2, 0))
    cohort.add("down", 600_000_000, LabelStatus("scored", 0, 0))
    # Recall of up is 3/4 and of down 1/1; flat never occurs.
    assert cohort.balanced_accuracy() == Fraction(7, 8)
    assert cohort.document()["accuracy"]["rounded"] == "0.8"


def test_pooling_sums_counts_and_never_averages_ratios() -> None:
    first, second = CohortCounts(), CohortCounts()
    first.add("up", 600_000_000, LabelStatus("scored", 2, 0))
    for _ in range(3):
        second.add("up", 600_000_000, LabelStatus("scored", 0, 0))
    second.add("up", 600_000_000, LabelStatus("scored", 2, 0))
    pooled_counts = pooled([first, second])
    # Fold accuracies are 1 and 1/4, whose mean is 5/8; pooled it is 2/5.
    assert pooled_counts.document()["accuracy"] == {"denominator": "5", "numerator": "2", "rounded": "0.4"}
    assert pooled_counts.size == 5
    assert pooled([]).document()["items"] == "0"


def test_decision_cohorts_nest_and_carry_reasons_and_outcomes() -> None:
    statuses = {RowIdentity(STREAM, INSTRUMENT, n): LabelStatus("scored", 2, 0) for n in range(4)}
    decisions = [
        Decision(1, 0, "up", 600_000_000, "order_scheduled", "filled"),
        Decision(2, 1, "up", 600_000_000, "already_at_target", None),
        Decision(3, 2, "down", 700_000_000, "order_scheduled", "rejected"),
        Decision(4, 3, "up", 400_000_000, "hold_below_threshold", None),
    ]
    cohorts = decision_cohorts(decisions, STREAM, INSTRUMENT, statuses)
    assert [
        cohorts[name].size for name in ("sampled_decisions", "order_scheduling_decisions", "filled_orders")
    ] == [4, 2, 1]
    assert dict(cohorts["order_scheduling_decisions"].reasons) == {"filled": 1, "rejected": 1}
    assert dict(cohorts["sampled_decisions"].reasons)["hold_below_threshold"] == 1
    with pytest.raises(ValueError, match="not a predicted row"):
        decision_cohorts([Decision(1, 9, "up", 1, "order_scheduled", "filled")], STREAM, INSTRUMENT, statuses)


def test_scorable_predictions_must_be_the_windows_rows_and_agree_with_the_study() -> None:
    statuses = {
        RowIdentity(STREAM, INSTRUMENT, 1): LabelStatus("scored", 2, 0),
        RowIdentity(STREAM, INSTRUMENT, 2): LabelStatus("feature_unavailable:missing_ask"),
        RowIdentity(STREAM, INSTRUMENT, 3): LabelStatus("label_unavailable:no_future_observation"),
    }
    rows = [
        ScheduleRow(1, 1, "up", 600_000_000),
        ScheduleRow(2, 2, None, None),
        ScheduleRow(3, 3, "down", 550_000_000),
    ]
    predictions = scorable_predictions(rows, STREAM, INSTRUMENT, statuses)
    assert (predictions.cohort.size, predictions.scheduled) == (2, 3)
    study: dict[str, Any] = {
        "metrics": {
            "accuracy": {"value": "1.0"},
            "balanced_accuracy": {"value": "1.0"},
            "confusion_matrix": {
                true: {
                    predicted: "1" if (true, predicted) == ("up", "up") else "0"
                    for predicted in ("down", "flat", "up")
                }
                for true in ("down", "flat", "up")
            },
        },
        "rows": {
            "not_scored": {
                "feature_unavailable:missing_ask": "1",
                "label_unavailable:no_future_observation": "1",
            },
            "scheduled": "3",
            "scored": "1",
        },
    }
    assert study_scores_agree(predictions, study) is None
    study["rows"]["scored"] = "2"
    assert study_scores_agree(predictions, study) == "the scheduled or scored row counts"
    with pytest.raises(ValueError, match="not a feature row"):
        scorable_predictions([*rows, ScheduleRow(4, 4, "up", 1)], STREAM, INSTRUMENT, statuses)
    with pytest.raises(ValueError, match="no prediction but available inputs"):
        scorable_predictions([ScheduleRow(1, 1, None, None)], STREAM, INSTRUMENT, statuses)
    with pytest.raises(ValueError, match="not the window's rows"):
        scorable_predictions(rows[:2], STREAM, INSTRUMENT, statuses)


def test_future_returns_are_exact_in_the_labels_unit_and_pool_by_sums() -> None:
    first, second = CohortCounts(), CohortCounts()
    first.add("up", 650_000_000, LabelStatus("scored", 2, 338_207))
    first.add("up", 950_000_000, LabelStatus("scored", 0, -26_176))
    first.add("down", 650_000_000, LabelStatus("label_unavailable:no_future_observation"))
    second.add("down", 1_000_000_000, LabelStatus("scored", 0, -1))
    second.add("up", 600_000_000, LabelStatus("label_after_scoring_cutoff"))
    document = first.document()["future_returns"]
    assert document["all"] == {
        "maximum_raw": {"value": "338207"},
        # (338207 - 26176) / 2 = 312031 / 2, rounded once.
        "mean_raw": {"denominator": "2", "numerator": "312031", "rounded": "156015.5"},
        "minimum_raw": {"value": "-26176"},
        "rows": "2",
        "sum_raw": "312031",
    }
    # Items without a scored label are counted, never given a return.
    assert document["not_scored"] == "1"
    assert document["by_predicted_class"]["down"] == {
        "maximum_raw": {"undefined": "no_scored_items"},
        "mean_raw": {"undefined": "no_scored_items"},
        "minimum_raw": {"undefined": "no_scored_items"},
        "rows": "0",
        "sum_raw": "0",
    }
    bins = {item["name"]: item for item in document["by_confidence_bin"]}
    assert [item["name"] for item in document["by_confidence_bin"]] == [b["name"] for b in CONFIDENCE_BINS]
    assert (bins["0.6-0.7"]["sum_raw"], bins["0.9-1.0"]["sum_raw"]) == ("338207", "-26176")
    # Pooled from sums and counts: (312031 - 1) / 3, not a mean of means.
    pooled_returns = pooled([first, second]).document()["future_returns"]
    assert pooled_returns["all"]["mean_raw"] == {
        "denominator": "1",
        "numerator": "104010",
        "rounded": "104010",
    }
    assert (pooled_returns["all"]["minimum_raw"], pooled_returns["not_scored"]) == ({"value": "-26176"}, "2")
    assert pooled_returns["by_predicted_class"]["down"]["rows"] == "1"
    assert {item["name"]: item["rows"] for item in pooled_returns["by_confidence_bin"]}["0.9-1.0"] == "2"
    assert CohortCounts().document()["future_returns"]["all"]["mean_raw"] == {"undefined": "no_scored_items"}


def test_a_scored_label_without_a_return_is_refused() -> None:
    row = labelled(2, 100, "up", 150)
    broken = LabelledRow(row.feature, LabelRow(row.label.identity, 100, 150, "up", None, None))
    with pytest.raises(ValueError, match="no return"):
        label_statuses([broken], Window("fold_1", 100, 200))
