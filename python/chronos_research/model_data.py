"""Model-ready rows: feature, label, and split rows joined by identity.

Rows are joined by (stream, instrument, ingest sequence), never by line
number, and every row of each artifact must have exactly one partner in the
other two. Each joined row is either usable, with its split, its exact
model inputs, and its class, or excluded with a reason that is counted.

The model feature set `top_of_book_baseline` version 1 uses only what the
feature row knew at its prediction time:

    spread                      ask - bid, in price units
    imbalance                   (bid_size - ask_size) / (bid_size + ask_size)
    microprice_minus_midpoint   microprice - midpoint, in price units
    bid_size, ask_size          shares

Inputs stay exact integers until to_floats() divides each by its scale. No
label field, label time, future observation, or split assignment is ever an
input.
"""

from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
import numpy.typing as npt

from chronos_research.artifacts import (
    CLASS_ORDER,
    SPLITS,
    ArtifactError,
    FeatureRow,
    LabelRow,
    RowIdentity,
    SplitRow,
    Unavailable,
)

MODEL_FEATURE_SET = {"name": "top_of_book_baseline", "version": "1"}
_PRICE_SCALE = 10**9
_RATIO_SCALE = 10**9
_QUANTITY_SCALE = 10**8
INPUTS = ("spread", "imbalance", "microprice_minus_midpoint", "bid_size", "ask_size")
INPUT_SCALES = (_PRICE_SCALE, _RATIO_SCALE, _PRICE_SCALE, _QUANTITY_SCALE, _QUANTITY_SCALE)
INPUT_DEFINITIONS = {
    "spread": "feature spread",
    "imbalance": "feature imbalance",
    "microprice_minus_midpoint": "feature microprice minus feature midpoint, exact",
    "bid_size": "feature bid_size",
    "ask_size": "feature ask_size",
}


@dataclass(frozen=True, slots=True)
class ModelRow:
    identity: RowIdentity
    split: str
    # The exact inputs, as raw integers in INPUT_SCALES.
    inputs: tuple[int, ...]
    class_index: int
    return_raw: int


@dataclass(frozen=True)
class ModelData:
    # In the feature artifact's row order.
    rows: tuple[ModelRow, ...]
    # Excluded rows by period, instrument, and reason.
    excluded: dict[str, dict[str, Counter[str]]]


def exact_inputs(row: FeatureRow) -> tuple[int, ...] | Unavailable:
    """The model inputs as raw integers, or the first unavailable source
    feature, in INPUTS order."""
    raw: dict[str, int] = {}
    for name in ("spread", "imbalance", "microprice", "midpoint", "bid_size", "ask_size"):
        value = row.values[name]
        if isinstance(value, Unavailable):
            return value
        raw[name] = value
    return (
        raw["spread"],
        raw["imbalance"],
        raw["microprice"] - raw["midpoint"],
        raw["bid_size"],
        raw["ask_size"],
    )


def to_floats(inputs: Sequence[tuple[int, ...]]) -> npt.NDArray[np.float64]:
    """The deliberate conversion to binary floating point: each exact integer
    divided by its scale, correctly rounded."""
    matrix = np.array(
        [[value / scale for value, scale in zip(row, INPUT_SCALES, strict=True)] for row in inputs],
        dtype=np.float64,
    ).reshape(len(inputs), len(INPUTS))
    if not np.isfinite(matrix).all():
        raise ArtifactError("a model input is not finite")
    return matrix


def _index_by_identity[Row: (LabelRow, SplitRow)](rows: Sequence[Row], kind: str) -> dict[RowIdentity, Row]:
    indexed: dict[RowIdentity, Row] = {}
    for row in rows:
        if row.identity in indexed:
            raise ArtifactError(f"duplicate {kind} row identity {tuple(row.identity)}")
        indexed[row.identity] = row
    return indexed


def join(
    features: Sequence[FeatureRow],
    labels: Sequence[LabelRow],
    splits: Sequence[SplitRow],
    *,
    validation_start_at_ns: int,
    test_start_at_ns: int,
) -> ModelData:
    """Joins the three artifacts' rows and decides each row's fate."""
    labels_by_identity = _index_by_identity(labels, "label")
    splits_by_identity = _index_by_identity(splits, "split")
    seen: set[RowIdentity] = set()
    rows: list[ModelRow] = []
    excluded: dict[str, dict[str, Counter[str]]] = {split: {} for split in SPLITS}
    for feature in features:
        identity = feature.identity
        if identity in seen:
            raise ArtifactError(f"duplicate feature row identity {tuple(identity)}")
        seen.add(identity)
        label = labels_by_identity.get(identity)
        split = splits_by_identity.get(identity)
        if label is None or split is None:
            raise ArtifactError(
                f"feature row {tuple(identity)} has no {'label' if label is None else 'split'} row"
            )
        _check_consistency(feature, label, split, validation_start_at_ns, test_start_at_ns)
        reason = _exclusion(feature, label, split)
        if reason is not None:
            excluded[split.period].setdefault(identity.instrument, Counter())[reason] += 1
            continue
        inputs = exact_inputs(feature)
        if isinstance(inputs, Unavailable) or label.direction is None or label.return_raw is None:
            raise ArtifactError(f"row {tuple(identity)} is in a split without a label or inputs")
        rows.append(
            ModelRow(
                identity=identity,
                split=split.split,
                inputs=inputs,
                class_index=CLASS_ORDER.index(label.direction),
                return_raw=label.return_raw,
            )
        )
    for kind, indexed in (("label", labels_by_identity), ("split", splits_by_identity)):
        unmatched = indexed.keys() - seen
        if unmatched:
            raise ArtifactError(
                f"{len(unmatched)} {kind} rows have no feature row, such as {tuple(min(unmatched))}"
            )
    return ModelData(rows=tuple(rows), excluded=excluded)


def _check_consistency(
    feature: FeatureRow, label: LabelRow, split: SplitRow, validation_start_at_ns: int, test_start_at_ns: int
) -> None:
    identity = tuple(feature.identity)
    if not feature.available_at_ns == label.prediction_at_ns == split.prediction_at_ns:
        raise ArtifactError(f"row {identity}: prediction times differ between artifacts")
    if label.label_available_at_ns != split.label_available_at_ns:
        raise ArtifactError(f"row {identity}: label availability differs between label and split rows")
    if split.label_usable != (label.direction is not None):
        raise ArtifactError(f"row {identity}: label usability differs between label and split rows")
    if split.split not in (*SPLITS, "excluded") or split.period not in SPLITS:
        raise ArtifactError(f"row {identity}: unknown split {split.split!r} or period {split.period!r}")
    if split.split != "excluded" and (split.split != split.period or not split.label_usable):
        raise ArtifactError(
            f"row {identity}: assigned to {split.split} but falls in {split.period} or has no label"
        )
    if split.split in ("train", "validation"):
        boundary = validation_start_at_ns if split.split == "train" else test_start_at_ns
        if split.label_available_at_ns is None or split.label_available_at_ns >= boundary:
            raise ArtifactError(
                f"row {identity}: a usable {split.split} label is known after the next period begins"
            )
    if label.direction is not None and label.direction not in CLASS_ORDER:
        raise ArtifactError(f"row {identity}: unknown direction {label.direction!r}")


def _exclusion(feature: FeatureRow, label: LabelRow, split: SplitRow) -> str | None:
    if split.split == "excluded":
        if split.exclusion == "label_unavailable":
            return f"label_unavailable:{label.unavailable_reason}"
        return str(split.exclusion)
    inputs = exact_inputs(feature)
    if isinstance(inputs, Unavailable):
        return f"feature_unavailable:{inputs.reason}"
    return None
