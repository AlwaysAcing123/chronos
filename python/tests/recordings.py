"""Independent synthetic recordings for walk-forward tests: no licensed data.

Each recording is its own dataset and stream on its own day, so ingest
sequences overlap across recordings (every one starts at 0), row counts
differ, and each has its own regime: how often the midpoint rises after a
bid-heavy quote. Within a recording, quotes follow irregular gaps, with an
occasional long pause, and AAPL and SPY sometimes quote at the same receive
time. Everything is labelled synthetic by the dataset tool itself.
"""

from dataclasses import dataclass, field
from datetime import UTC, datetime
from pathlib import Path

import numpy as np

from chronos_research import pipeline as research_pipeline
from chronos_research.pipeline import ArtifactRoots

DAY_NS = 86_400 * 10**9
# 13:30 UTC on Monday 2026-09-21, the first recording's day.
FIRST_SESSION_NS = int(datetime(2026, 9, 21, 13, 30, tzinfo=UTC).timestamp()) * 10**9
HORIZON_NS = 1_000_000


@dataclass(frozen=True)
class Recording:
    stream: str
    day: int
    quotes: int
    seed: int
    up_bias: float = 0.4
    instruments: tuple[str, ...] = ("AAPL", "SPY")
    # Nanoseconds after the session start at which it begins.
    offset_ns: int = 0
    # Quotes of the first instrument alone, 100 ms apart, after the others stop.
    tail_quotes: int = 0


@dataclass(frozen=True)
class Prepared:
    recording: Recording
    dataset_id: str
    feature_id: str
    label_id: str


@dataclass
class Recordings:
    """Recordings prepared into one artifact root."""

    artifacts: Path
    tools: research_pipeline.Tools
    prepared: dict[str, Prepared] = field(default_factory=dict)

    def label_ids(self, *streams: str) -> list[str]:
        return [self.prepared[stream].label_id for stream in streams]


def recording_events(recording: Recording) -> str:
    """CSV for chronos_synthetic_dataset, in receive order."""
    generator = np.random.default_rng(recording.seed)
    midpoints = {
        instrument: {"AAPL": 10_000, "SPY": 50_000}.get(instrument, 20_000)
        for instrument in recording.instruments
    }
    imbalance = dict.fromkeys(recording.instruments, 0.0)
    at = FIRST_SESSION_NS + recording.day * DAY_NS + recording.offset_ns
    # Spacing is spread so the last instrument still quotes before the next step.
    gaps = max(len(recording.instruments) - 1, 1)
    lines: list[str] = []
    for step in range(recording.quotes):
        at += int(generator.integers(50_000, 400_000))
        if step and step % 97 == 0:
            at += 20_000_000
        # Every eleventh step, the instruments quote at the same receive time.
        spacing = 0 if step % 11 == 0 else int(generator.integers(1, 40_000)) // gaps
        for position, instrument in enumerate(recording.instruments):
            draw = generator.random()
            up = recording.up_bias + 0.3 * imbalance[instrument]
            move = 1 if draw < up else (0 if draw < up + 0.2 else -1)
            midpoints[instrument] += move
            bid_size = int(generator.integers(1, 20))
            ask_size = int(generator.integers(1, 20))
            imbalance[instrument] = (bid_size - ask_size) / (bid_size + ask_size)
            bid, ask = (midpoints[instrument] - 1) / 100, (midpoints[instrument] + 1) / 100
            lines.append(
                f"quote,{instrument},{at + position * spacing},{bid:.2f},{bid_size},{ask:.2f},{ask_size}"
            )
    first = recording.instruments[0]
    for _ in range(recording.tail_quotes):
        at += 100_000_000
        bid, ask = (midpoints[first] - 1) / 100, (midpoints[first] + 1) / 100
        lines.append(f"quote,{first},{at},{bid:.2f},10,{ask:.2f},10")
    return "\n".join(lines) + "\n"


def prepare(recordings: Recordings, recording: Recording, horizon_ns: int = HORIZON_NS) -> Prepared:
    """The recording as a dataset, its features, and its labels, published
    into the root like any recording's."""
    roots = ArtifactRoots.under(recordings.artifacts)
    events = recordings.artifacts / f"{recording.stream}-{recording.seed}.csv"
    events.write_text(recording_events(recording))
    dataset = research_pipeline.synthesize_dataset(recordings.tools, events, recording.stream, roots.catalog)
    features = research_pipeline.extract_features(recordings.tools, roots, dataset.artifact_id)
    labels = research_pipeline.extract_labels(recordings.tools, roots, features.artifact_id, horizon_ns)
    prepared = Prepared(recording, dataset.artifact_id, features.artifact_id, labels.artifact_id)
    recordings.prepared.setdefault(recording.stream, prepared)
    return prepared


# Three trading days with different regimes and sizes, as the studies use.
WEEK = (
    Recording("synthetic-2026-09-21", day=0, quotes=420, seed=11, up_bias=0.45),
    Recording("synthetic-2026-09-22", day=1, quotes=360, seed=12, up_bias=0.30),
    Recording("synthetic-2026-09-23", day=2, quotes=500, seed=13, up_bias=0.40),
)
