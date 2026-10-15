"""The demo workspace's synthetic market: scenario `chronos.demo_scenario`,
version 1 (ADR 0025).

Invented quotes for two symbols, AAPL and SPY, whose prices start at 100.00
and 500.00. The prices are examples: they are neither company's nor fund's
real prices, and nothing here was observed on an exchange. Each session is
an event table for `chronos_dataset synthetic write`, whose datasets say
they are synthetic.

Four sessions on consecutive days, each with its own stream:

    h1, h2, h3   ten minutes each, from 14:30 UTC: history for research
    playback     forty minutes, the next day: the interactive replay only

Each instrument quotes every 100 to 300 ms, with a two-cent spread. A
persistent pressure regime (buy, sell, or neutral, lasting about five
seconds) sets both the displayed sizes and the next ticks, so the book's
imbalance carries information about the next second. The data is built that
way so that the machinery has something to find; whatever a model or
strategy does on it says nothing about any market.

The generator is SplitMix64 in integer arithmetic, so the bytes depend on no
library, platform, or floating point; a version change is a new scenario.
"""

from collections.abc import Iterator
from dataclasses import dataclass
from datetime import UTC, datetime

NAME = "chronos.demo_scenario"
VERSION = "1"
INSTRUMENTS = ("AAPL", "SPY")
# Invented starting midpoints, in cents.
_START_CENTS = {"AAPL": 10_000, "SPY": 50_000}
_MASK = (1 << 64) - 1
_SECOND_NS = 1_000_000_000
_MINUTE_NS = 60 * _SECOND_NS


@dataclass(frozen=True)
class _Regime:
    """Displayed bid and ask sizes (inclusive ranges), and how often the
    midpoint moves up or down a cent on a step; otherwise it stays."""

    bid: tuple[int, int]
    ask: tuple[int, int]
    up_percent: int
    down_percent: int


_REGIMES = {
    "buy": _Regime(bid=(12, 20), ask=(1, 6), up_percent=55, down_percent=10),
    "neutral": _Regime(bid=(1, 20), ask=(1, 20), up_percent=20, down_percent=20),
    "sell": _Regime(bid=(1, 6), ask=(12, 20), up_percent=10, down_percent=55),
}
# Percent chance on each step of switching to one of the other regimes.
_SWITCH_PERCENT = 4
_GAP_NS = (100_000_000, 300_000_000)


@dataclass(frozen=True)
class Session:
    name: str
    stream: str
    role: str
    day: str
    start_ns: int
    duration_ns: int
    seed: int

    @property
    def end_ns(self) -> int:
        return self.start_ns + self.duration_ns

    @property
    def label(self) -> str:
        """The session label a collection records as a declaration."""
        return f"synthetic {self.name} ({self.day}), invented prices"


def _at(day: str, hour: int, minute: int) -> int:
    moment = datetime.fromisoformat(day).replace(hour=hour, minute=minute, tzinfo=UTC)
    return int(moment.timestamp()) * _SECOND_NS


def _session(name: str, role: str, day: str, minutes: int, seed: int) -> Session:
    return Session(
        name=name,
        stream=f"chronos-demo-v{VERSION}-{name}",
        role=role,
        day=day,
        start_ns=_at(day, 14, 30),
        duration_ns=minutes * _MINUTE_NS,
        seed=seed,
    )


SESSIONS = (
    _session("h1", "history", "2026-03-02", 10, 0xC0FFEE01),
    _session("h2", "history", "2026-03-03", 10, 0xC0FFEE02),
    _session("h3", "history", "2026-03-04", 10, 0xC0FFEE03),
    _session("playback", "playback", "2026-03-05", 40, 0xC0FFEE04),
)
HISTORY = tuple(session for session in SESSIONS if session.role == "history")
PLAYBACK = next(session for session in SESSIONS if session.role == "playback")


class _SplitMix64:
    def __init__(self, seed: int) -> None:
        self.state = seed & _MASK

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & _MASK
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _MASK
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _MASK
        return value ^ (value >> 31)

    def below(self, bound: int) -> int:
        """Uniform in [0, bound), without modulo bias."""
        limit = (1 << 64) - (1 << 64) % bound
        while True:
            value = self.next()
            if value < limit:
                return value % bound

    def between(self, low: int, high: int) -> int:
        """Uniform in [low, high], both included."""
        return low + self.below(high - low + 1)


def _cents(value: int) -> str:
    return f"{value // 100}.{value % 100:02d}"


def _quotes(session: Session, instrument: str, index: int) -> Iterator[tuple[int, int, str]]:
    """(receive time, instrument index, CSV line) for one instrument."""
    random = _SplitMix64(session.seed * 2 + index)
    regimes = list(_REGIMES)
    regime = "neutral"
    mid = _START_CENTS[instrument]
    at = session.start_ns + random.between(0, _GAP_NS[0])
    while at < session.end_ns:
        if random.below(100) < _SWITCH_PERCENT:
            regime = [other for other in regimes if other != regime][random.below(2)]
        shape = _REGIMES[regime]
        draw = random.below(100)
        if draw < shape.up_percent:
            mid += 1
        elif draw < shape.up_percent + shape.down_percent:
            mid = max(mid - 1, 2)
        bid_size = random.between(*shape.bid)
        ask_size = random.between(*shape.ask)
        yield at, index, f"quote,{instrument},{at},{_cents(mid - 1)},{bid_size},{_cents(mid + 1)},{ask_size}"
        at += random.between(*_GAP_NS)


def event_table(session: Session) -> str:
    """The session's event table, in receive order (AAPL first at a tie)."""
    rows = sorted(
        row for index, instrument in enumerate(INSTRUMENTS) for row in _quotes(session, instrument, index)
    )
    header = [
        f"# {NAME} version {VERSION}, session {session.name} ({session.day}, {session.role})",
        "# Invented prices for a local demonstration: not market data, not any real security's price.",
    ]
    return "\n".join([*header, *(line for _, _, line in rows)]) + "\n"
