"""A deterministic synthetic market for tests: no licensed or downloaded data.

Two instruments quote alternately every 100 microseconds. Each quote's
midpoint moves by one cent from the previous one, up more often when the
previous quote's bid size exceeds its ask size, and sometimes not at all,
so a model of imbalance has something to learn and every class occurs.
"""

import numpy as np


def synthetic_events(seed: int, quotes_per_instrument: int = 400, halted_spy_quotes: int = 0) -> str:
    """CSV for chronos_synthetic_dataset. With `halted_spy_quotes`, SPY is
    halted for that many quotes in the middle of the session."""
    generator = np.random.default_rng(seed)
    midpoints = {"AAPL": 10_000, "SPY": 50_000}
    imbalance = {"AAPL": 0.0, "SPY": 0.0}
    lines: list[str] = []
    halt_start = quotes_per_instrument // 2
    for step in range(quotes_per_instrument):
        for offset, instrument in enumerate(("AAPL", "SPY")):
            at = 1_000_000_000 + step * 200_000 + offset * 100_000
            if instrument == "SPY" and halted_spy_quotes and step == halt_start:
                lines.append(f"status,SPY,{at - 1},halted")
            if instrument == "SPY" and halted_spy_quotes and step == halt_start + halted_spy_quotes:
                lines.append(f"status,SPY,{at - 1},open")
            draw = generator.random()
            up = 0.4 + 0.3 * imbalance[instrument]
            move = 1 if draw < up else (0 if draw < up + 0.2 else -1)
            midpoints[instrument] += move
            bid_size = int(generator.integers(1, 20))
            ask_size = int(generator.integers(1, 20))
            imbalance[instrument] = (bid_size - ask_size) / (bid_size + ask_size)
            bid, ask = (midpoints[instrument] - 1) / 100, (midpoints[instrument] + 1) / 100
            lines.append(f"quote,{instrument},{at},{bid:.2f},{bid_size},{ask:.2f},{ask_size}")
    return "\n".join(lines) + "\n"
