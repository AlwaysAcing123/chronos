"""In-memory feature, label, and split rows for tests that need no C++.

`market(...)` describes rows compactly and produces the three artifacts'
rows exactly as the readers would, consistent with each other unless a test
changes them.
"""

from dataclasses import dataclass, replace
from pathlib import Path

from chronos_research.artifacts import (
    FeatureRow,
    LabelRow,
    RowIdentity,
    SplitRow,
    Unavailable,
    VerifiedArtifact,
    VerifiedSources,
)
from chronos_research.model_data import ModelData, join

STREAM = "test-stream"
PRICE = 10**9
QUANTITY = 10**8


@dataclass(frozen=True)
class Spec:
    """One row: when it predicts, its split, its direction (or None for an
    unavailable label), and its imbalance."""

    instrument: str
    at: int
    split: str
    direction: str | None = "up"
    imbalance: int = 0
    halted: bool = False


@dataclass(frozen=True)
class Market:
    features: list[FeatureRow]
    labels: list[LabelRow]
    splits: list[SplitRow]
    validation_start_at_ns: int
    test_start_at_ns: int

    def joined(self) -> ModelData:
        return join(
            self.features,
            self.labels,
            self.splits,
            validation_start_at_ns=self.validation_start_at_ns,
            test_start_at_ns=self.test_start_at_ns,
        )


def feature_row(spec: Spec, sequence: int) -> FeatureRow:
    if spec.halted:
        values: dict[str, int | Unavailable] = {
            name: Unavailable("trading_halted")
            for name in (
                "ask_size",
                "best_ask",
                "best_bid",
                "bid_size",
                "imbalance",
                "microprice",
                "midpoint",
                "spread",
            )
        }
    else:
        values = {
            "ask_size": 3 * QUANTITY,
            "best_ask": 101 * PRICE,
            "best_bid": 100 * PRICE,
            "bid_size": 5 * QUANTITY,
            "imbalance": spec.imbalance,
            "microprice": 100 * PRICE + 625_000_001,
            "midpoint": 100 * PRICE + 500_000_000,
            "spread": PRICE,
        }
    return FeatureRow(
        identity=RowIdentity(STREAM, spec.instrument, sequence), available_at_ns=spec.at, values=values
    )


def market(
    specs: list[Spec], *, delay: int = 1, validation_start_at_ns: int, test_start_at_ns: int
) -> Market:
    features: list[FeatureRow] = []
    labels: list[LabelRow] = []
    splits: list[SplitRow] = []
    for sequence, spec in enumerate(specs):
        feature = feature_row(spec, sequence)
        usable = spec.direction is not None and not spec.halted
        period = spec.split
        features.append(feature)
        labels.append(
            LabelRow(
                identity=feature.identity,
                prediction_at_ns=spec.at,
                label_available_at_ns=spec.at + delay,
                direction=spec.direction if usable else None,
                return_raw={"down": -5, "flat": 0, "up": 5}[spec.direction]
                if usable and spec.direction
                else None,
                unavailable_reason=None if usable else "current_midpoint_unavailable",
            )
        )
        splits.append(
            SplitRow(
                identity=feature.identity,
                prediction_at_ns=spec.at,
                label_available_at_ns=spec.at + delay,
                label_usable=usable,
                period=period,
                split=period if usable else "excluded",
                exclusion=None if usable else "label_unavailable",
            )
        )
    return Market(features, labels, splits, validation_start_at_ns, test_start_at_ns)


def sources(validation_start_at_ns: int = 0, test_start_at_ns: int = 0) -> VerifiedSources:
    def artifact(name: str) -> VerifiedArtifact:
        return VerifiedArtifact(
            artifact_id=name * 64,
            rows_path=Path("/nonexistent"),
            rows_sha256=name * 64,
            rows=0,
            summary={},
            reproduced="recomputed",
        )

    return VerifiedSources(
        dataset_id="d" * 64,
        stream=STREAM,
        horizon_ns=1,
        features=artifact("f"),
        labels=artifact("a"),
        splits=artifact("b"),
        validation_start_at_ns=validation_start_at_ns,
        test_start_at_ns=test_start_at_ns,
    )


__all__ = ["Market", "Spec", "feature_row", "market", "replace", "sources"]
