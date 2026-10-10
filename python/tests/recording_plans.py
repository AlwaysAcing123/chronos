"""Request plans for tests: past windows, fake destinations, frozen settings."""

import copy
from typing import Any

CONFIGURATION = {
    "decision_interval_ns": "1000000000",
    "fee_per_fill": "0.01",
    "latency_ns": "1000000",
    "maximum_order_notional": "1000000",
    "maximum_quote_age_ns": "5000000000",
    "starting_cash": "10000",
    "target_quantity": "1",
    "threshold_ppb": "500000000",
}
SCENARIOS = [
    {"fee_per_fill": "0.01", "latency_ns": "1000000", "name": "base"},
    {"fee_per_fill": "0.01", "latency_ns": "50000000", "name": "slow"},
    {"fee_per_fill": "0.05", "latency_ns": "50000000", "name": "stress"},
]


def recording(day: int, **fields: Any) -> dict[str, Any]:
    entry = {
        "destination": f"databento/equs-mini/2026-09-{day:02d}-aapl-spy.dbn.zst",
        "end": f"2026-09-{day:02d}T13:40:00Z",
        "session_label": f"2026-09-{day:02d} 13:30-13:40 UTC (declared)",
        "start": f"2026-09-{day:02d}T13:30:00Z",
        "stream_id": f"databento-equs-mini-mbp1-202609{day:02d}-1330-1340",
    }
    entry.update(fields)
    return entry


def plan(*days: int, recordings: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {
        "evaluation": {
            "configuration": dict(CONFIGURATION),
            "horizon_ns": "1000000000",
            "instruments": ["AAPL", "SPY"],
            "minimum_training_sources": "1",
            "scenarios": copy.deepcopy(SCENARIOS),
        },
        "provider": {
            "dataset": "EQUS.MINI",
            "input_symbology": "raw_symbol",
            "name": "databento",
            "output_symbology": "instrument_id",
            "schema": "mbp-1",
        },
        "recordings": recordings
        if recordings is not None
        else [recording(day) for day in (days or (21, 22, 23))],
        "schema": {"name": "chronos.recording_request_plan", "version": "1"},
        "symbols": ["AAPL", "SPY"],
    }
