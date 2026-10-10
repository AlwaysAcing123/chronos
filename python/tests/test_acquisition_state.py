"""Acquisition state that is not what it claims: paths redirected out of the
state root, ledgers whose entries disagree with their authorization and plan,
and tool output that is not the tool's own.

Each refusal leaves the fake download count, every ledger, and everything
outside the state root as they were. Fake tools only (tests/fake_databento.py);
nothing here can reach a provider.
"""

import json
import re
import shutil
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from chronos_research import acquisition
from chronos_research.acquisition import (
    MAX_LEDGER_LINE_BYTES,
    AcquisitionError,
    acquire,
    authorize,
    recover,
)
from chronos_research.encoding import canonical_json
from chronos_research.pipeline import Tools
from chronos_research.recording_pilot import prepare_plan
from chronos_research.recording_plan import plan_identity, validated_plan
from tests import fake_databento
from tests.fake_databento import FAKE_KEY
from tests.recording_plans import plan

STREAMS = [entry["stream_id"] for entry in plan()["recordings"]]
DESTINATIONS = [entry["destination"] for entry in plan()["recordings"]]


@pytest.fixture
def tools(tmp_path: Path) -> Path:
    return fake_databento.install(tmp_path / "tools")


@pytest.fixture
def root(tmp_path: Path) -> Path:
    directory = tmp_path / "state"
    directory.mkdir()
    return directory


@pytest.fixture
def outside(tmp_path: Path) -> Path:
    directory = tmp_path / "outside-the-approved-root"
    directory.mkdir()
    return directory


@pytest.fixture(autouse=True)
def key(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("DATABENTO_API_KEY", FAKE_KEY)


def authorized(root: Path, caps: dict[str, str] | None = None, allowance: str = "0.30") -> str:
    document = validated_plan(plan())
    caps = caps if caps is not None else {STREAMS[0]: "0.30"}
    result = authorize(document, plan_identity(document), root, allowance, list(caps.items()))
    return str(result["authorization_id"])


def downloads(tools: Path) -> int:
    return len(fake_databento.invocations(tools, "download"))


def state_of(root: Path, outside: Path) -> tuple[dict[str, bytes], list[str]]:
    """Every ledger's bytes and everything outside the root, to compare."""
    ledgers = root / "ledgers"
    entries = sorted(ledgers.iterdir()) if ledgers.is_dir() and not ledgers.is_symlink() else []
    files = {path.name: path.read_bytes() for path in entries}
    return files, sorted(str(path.relative_to(outside)) for path in outside.rglob("*"))


def refused(reason: str, call: Callable[..., Any], *arguments: Any) -> AcquisitionError:
    with pytest.raises(AcquisitionError) as refusal:
        call(*arguments)
    assert refusal.value.reason == reason, str(refusal.value)
    return refusal.value


# --- Paths that leave the state root ----------------------------------------------------------------------


def _moved_out_and_linked(root: Path, outside: Path, name: str) -> None:
    """A namespace moved outside the root, with a link left in its place."""
    shutil.move(str(root / name), str(outside / name))
    (root / name).symlink_to(outside / name, target_is_directory=True)


def _destination(root: Path) -> Path:
    return root / "recordings" / str(DESTINATIONS[0])


def _linked_parent(root: Path, outside: Path) -> None:
    (root / "recordings" / "databento").symlink_to(outside, target_is_directory=True)


def _dangling_parent(root: Path, outside: Path) -> None:
    (root / "recordings" / "databento").symlink_to(outside / "missing", target_is_directory=True)


def _file_as_parent(root: Path, outside: Path) -> None:
    (root / "recordings" / "databento").write_text("not a directory")


def _linked_recording(root: Path, outside: Path) -> None:
    _destination(root).parent.mkdir(parents=True)
    _destination(root).symlink_to(outside / "elsewhere.dbn.zst")


def _linked_manifest(root: Path, outside: Path) -> None:
    _destination(root).parent.mkdir(parents=True)
    Path(f"{_destination(root)}.manifest.json").symlink_to(outside / "elsewhere.json")


def _linked_lock(root: Path, outside: Path) -> None:
    (root / "acquisition.lock").unlink()
    (root / "acquisition.lock").symlink_to(outside / "lock")


REDIRECTS: dict[str, Callable[[Path, Path], None]] = {
    "recordings is a link": lambda root, outside: _moved_out_and_linked(root, outside, "recordings"),
    "an intermediate parent is a link": _linked_parent,
    "a parent is a dangling link": _dangling_parent,
    "a parent is a file": _file_as_parent,
    "the recording is a link": _linked_recording,
    "the manifest is a link": _linked_manifest,
    "the ledgers are a link": lambda root, outside: _moved_out_and_linked(root, outside, "ledgers"),
    "the receipts are a link": lambda root, outside: _moved_out_and_linked(root, outside, "receipts"),
    "the authorizations are a link": lambda root, outside: _moved_out_and_linked(
        root, outside, "authorizations"
    ),
    "the lock is a link": _linked_lock,
}


@pytest.mark.parametrize("redirect", REDIRECTS.values(), ids=REDIRECTS.keys())
def test_a_path_leaving_the_root_is_refused_before_anything_is_reserved(
    tools: Path, root: Path, outside: Path, redirect: Callable[[Path, Path], None]
) -> None:
    authorization_id = authorized(root)
    redirect(root, outside)
    ledgers, outside_files = state_of(root, outside)
    refused("unsafe_path", acquire, Tools(tools), root, authorization_id)
    assert downloads(tools) == 0
    assert state_of(root, outside) == (ledgers, outside_files)


def test_a_verified_recording_behind_a_linked_parent_is_not_reused(
    tools: Path, root: Path, outside: Path, tmp_path: Path
) -> None:
    authorization_id = authorized(root)
    assert acquire(Tools(tools), root, authorization_id)["recordings"][0]["outcome"] == "acquired"
    _moved_out_and_linked(root, outside, "recordings")
    refused("unsafe_path", acquire, Tools(tools), root, authorization_id)
    report = prepare_plan(Tools(tools), plan(), tmp_path / "artifacts", root)
    assert report["recordings"] == []
    assert report["pending"][0]["reason"].startswith("refused (unsafe_path): ")
    assert downloads(tools) == 1


def test_authorizing_and_recovering_refuse_redirected_state(tools: Path, root: Path, outside: Path) -> None:
    (outside / "authorizations").mkdir()
    (root / "authorizations").symlink_to(outside / "authorizations", target_is_directory=True)
    document = validated_plan(plan())
    refused("unsafe_path", authorize, document, plan_identity(document), root, "0.30", [(STREAMS[0], "0.30")])
    assert list(outside.rglob("*")) == [outside / "authorizations"]
    (root / "authorizations").unlink()
    authorization_id = authorized(root)
    _moved_out_and_linked(root, outside, "ledgers")
    refused("unsafe_path", recover, root, authorization_id, 1, True)


def test_nested_destination_directories_inside_the_root_are_still_created(tools: Path, root: Path) -> None:
    authorization_id = authorized(root, caps={STREAMS[0]: "0.30", STREAMS[1]: "0.30"}, allowance="0.60")
    (root / "recordings" / "databento").mkdir(parents=True)
    result = acquire(Tools(tools), root, authorization_id)
    assert [entry["outcome"] for entry in result["recordings"][:2]] == ["acquired", "acquired"]
    assert _destination(root).is_file()
    assert not any(path.is_symlink() for path in root.rglob("*"))


# --- Ledgers that disagree with their authorization and plan ----------------------------------------------


def _failed_attempt(tools: Path, root: Path) -> tuple[str, Path]:
    """One capped recording, allowance room for a second, and one failed attempt."""
    authorization_id = authorized(root, allowance="0.60")
    fake_databento.set_behavior(
        tools, {"download": {"mode": "fail_code", "code": "provider_download_failed"}}
    )
    assert acquire(Tools(tools), root, authorization_id)["recordings"][0]["outcome"] == "failed"
    fake_databento.set_behavior(tools, {})
    return authorization_id, root / "ledgers" / f"{authorization_id}.ndjson"


def _rewritten(ledger: Path, change: Callable[[list[dict[str, Any]]], None]) -> None:
    entries = [json.loads(line) for line in ledger.read_bytes().splitlines()]
    change(entries)
    ledger.write_bytes(b"".join(canonical_json(entry) for entry in entries))


def _second_attempt(entries: list[dict[str, Any]]) -> None:
    entries += [
        {**entries[1], "attempt": "2", "reserved_total_usd": "0.60"},
        {**entries[2], "attempt": "2"},
    ]


MUTATIONS: dict[str, Callable[[list[dict[str, Any]]], None]] = {
    "another destination": lambda entries: entries[1].update(destination="wrong-location.dbn.zst"),
    "another item's destination": lambda entries: entries[1].update(destination=DESTINATIONS[1]),
    "another item's stream": lambda entries: entries[1].update(stream_id=STREAMS[1]),
    "a stream not in the plan": lambda entries: entries[1].update(stream_id="not-in-the-plan"),
    "a cap that is not the item's": lambda entries: entries[1].update(
        cap_usd="0.20", reserved_total_usd="0.20"
    ),
    "a second attempt at the same item": _second_attempt,
    "an outcome no attempt can have": lambda entries: entries[2].update(outcome="refunded"),
    "an error code no tool writes": lambda entries: entries[2].update(error_code="Not A Code!"),
    "a detail over its bound": lambda entries: entries[2].update(detail="x" * 2_000),
}


@pytest.mark.parametrize("mutation", MUTATIONS.values(), ids=MUTATIONS.keys())
def test_a_ledger_entry_that_disagrees_with_the_authorization_or_plan_is_refused(
    tools: Path, root: Path, tmp_path: Path, mutation: Callable[[list[dict[str, Any]]], None]
) -> None:
    authorization_id, ledger = _failed_attempt(tools, root)
    _rewritten(ledger, mutation)
    damaged = ledger.read_bytes()
    refused("state_corrupt", acquire, Tools(tools), root, authorization_id)
    refused("state_corrupt", recover, root, authorization_id, 1, True)
    report = prepare_plan(Tools(tools), plan(), tmp_path / "artifacts", root)
    assert all(entry["reason"].startswith("refused (state_corrupt): ") for entry in report["pending"])
    assert downloads(tools) == 1
    assert ledger.read_bytes() == damaged


def test_the_review_reproduction_stays_at_one_attempt(tools: Path, root: Path) -> None:
    """Codex's probe: only the reservation's destination changed, canonically."""
    authorization_id, ledger = _failed_attempt(tools, root)
    _rewritten(ledger, MUTATIONS["another destination"])
    refused("state_corrupt", acquire, Tools(tools), root, authorization_id)
    assert downloads(tools) == 1
    assert ledger.read_bytes().count(b'"entry":"reserved"') == 1


# --- Tool output that is not the tool's -------------------------------------------------------------------


@pytest.mark.parametrize(
    "code", [FAKE_KEY, "x" * 10_000, "Not a code", ""], ids=["the-key", "oversized", "spaces", "empty"]
)
def test_an_error_code_the_tool_never_writes_is_ambiguous_and_never_echoed(
    tools: Path, root: Path, code: str
) -> None:
    authorization_id = authorized(root)
    fake_databento.set_behavior(tools, {"download": {"mode": "fail_code", "code": code}})
    result = acquire(Tools(tools), root, authorization_id)
    entry = result["recordings"][0]
    assert (entry["outcome"], downloads(tools)) == ("ambiguous", 1)
    assert FAKE_KEY not in json.dumps(result)
    assert FAKE_KEY not in fake_databento.tree_text(root)
    if code:
        assert code not in json.dumps(result)
        assert code not in fake_databento.tree_text(root)
    ledger = root / "ledgers" / f"{authorization_id}.ndjson"
    assert all(len(line) + 1 <= MAX_LEDGER_LINE_BYTES for line in ledger.read_bytes().splitlines())
    finished = json.loads(ledger.read_bytes().splitlines()[-1])
    assert finished["error_code"] == "unrecognized_error_code"
    # The attempt stays blocked, as any ambiguous one.
    assert acquire(Tools(tools), root, authorization_id)["recordings"][0]["outcome"] == "needs_reconciliation"
    assert downloads(tools) == 1


def test_the_accepted_error_codes_are_exactly_the_ones_the_tool_writes() -> None:
    """The whitelist is the C++ tool's to_string(HistoricalRecordingErrorCode)."""
    source = (
        Path(__file__).resolve().parents[2] / "adapters/databento/src/historical_recording_acquisition.cpp"
    )
    body = source.read_text().split(
        "std::string_view to_string(HistoricalRecordingErrorCode code) noexcept {"
    )[1]
    written = set(re.findall(r'return "([a-z_]+)";', body.split('return "unknown"')[0]))
    assert written == set(acquisition._TOOL_ERROR_CODES)
    assert len(written) == 12
