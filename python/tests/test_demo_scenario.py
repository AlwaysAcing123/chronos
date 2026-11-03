"""The demo workspace's synthetic scenario (demo_scenario, ADR 0025): fixed,
versioned bytes, chronological sessions, and invented prices that say so."""

import hashlib
import itertools
import re
from decimal import Decimal

from chronos_research import demo_scenario

# Version 1's tables. A change to the generator is a new scenario version,
# never a silent change of these bytes.
VERSION_1_TABLES = {
    "h1": "695783ec26cf7fbb70b8e08a56c22102a91cb36614e5bd16937ad080cbaea4c0",
    "h2": "25613f08f07f5f74c00cf8d6d6e5ae5a813b4d0b215f978b42becf1c6f95b067",
    "h3": "0aeda7e2809304f26734d1e2ecfef8bd4bcd4729ad034fe41ea1eae2c613bd0a",
    "playback": "ccf0b10a41796876ca3f96799b108fa4690e51ff5beedecd36223f992295ec26",
}
QUOTE = re.compile(r"quote,(AAPL|SPY),([0-9]+),([0-9]+\.[0-9]{2}),([0-9]+),([0-9]+\.[0-9]{2}),([0-9]+)")


def test_version_1_is_fixed_bytes() -> None:
    assert demo_scenario.NAME == "chronos.demo_scenario"
    assert demo_scenario.VERSION == "1"
    for session in demo_scenario.SESSIONS:
        table = demo_scenario.event_table(session)
        assert table == demo_scenario.event_table(session)
        assert hashlib.sha256(table.encode()).hexdigest() == VERSION_1_TABLES[session.name]


def test_sessions_are_disjoint_and_playback_comes_last() -> None:
    names = [session.name for session in demo_scenario.SESSIONS]
    assert names == ["h1", "h2", "h3", "playback"]
    assert [session.role for session in demo_scenario.HISTORY] == ["history"] * 3
    assert demo_scenario.PLAYBACK.name == "playback"
    for earlier, later in itertools.pairwise(demo_scenario.SESSIONS):
        assert earlier.end_ns <= later.start_ns
    assert len({session.stream for session in demo_scenario.SESSIONS}) == 4
    # Long enough at 1x to read the screens and follow the guide.
    assert demo_scenario.PLAYBACK.duration_ns == 40 * 60 * 10**9


def test_every_row_is_an_invented_two_sided_quote_in_receive_order() -> None:
    for session in demo_scenario.SESSIONS:
        lines = demo_scenario.event_table(session).splitlines()
        assert lines[1].startswith("# Invented prices for a local demonstration: not market data")
        rows = [line for line in lines if not line.startswith("#")]
        times = []
        for row in rows:
            matched = QUOTE.fullmatch(row)
            assert matched is not None, row
            _, at, bid, bid_size, ask, ask_size = matched.groups()
            times.append(int(at))
            assert session.start_ns <= int(at) < session.end_ns
            assert Decimal(ask) - Decimal(bid) == Decimal("0.02")
            assert int(bid_size) >= 1
            assert int(ask_size) >= 1
        assert times == sorted(times)
        assert {row.split(",")[1] for row in rows} == {"AAPL", "SPY"}
        first = {
            instrument: next(row for row in rows if f",{instrument}," in row)
            for instrument in ("AAPL", "SPY")
        }
        assert abs(Decimal(first["AAPL"].split(",")[3]) - 100) < 1
        assert abs(Decimal(first["SPY"].split(",")[3]) - 500) < 1
