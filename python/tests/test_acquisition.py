"""Quotes, authorizations, and acquisitions against fake provider tools.

Nothing here can reach a provider: the tools are the stand-ins in
tests/fake_databento.py, and every test counts their download invocations.
A refusal must leave that count at zero, and nothing that might have been
charged is ever attempted a second time.
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

import pytest

from chronos_research import acquisition, acquisition_cli, cli
from chronos_research.acquisition import (
    DETAIL_LIMIT,
    AcquisitionError,
    Ledger,
    acquire,
    authorize,
    quote_plan,
    read_receipt,
    recover,
)
from chronos_research.pipeline import Tools
from chronos_research.recording_plan import instant_ns, plan_identity, validated_plan
from tests import fake_databento
from tests.fake_databento import FAKE_KEY
from tests.recording_plans import plan, recording

PYTHON_ROOT = Path(__file__).resolve().parents[1]
STREAMS = [entry["stream_id"] for entry in plan()["recordings"]]


@pytest.fixture
def tools(tmp_path: Path) -> Path:
    return fake_databento.install(tmp_path / "tools")


@pytest.fixture
def root(tmp_path: Path) -> Path:
    directory = tmp_path / "state"
    directory.mkdir()
    return directory


@pytest.fixture(autouse=True)
def key(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("DATABENTO_API_KEY", FAKE_KEY)


def downloads(tools: Path) -> int:
    return len(fake_databento.invocations(tools, "download"))


def authorized(
    root: Path,
    document: dict[str, Any] | None = None,
    caps: dict[str, str] | None = None,
    allowance: str = "1.00",
) -> str:
    document = document or plan()
    caps = caps if caps is not None else dict.fromkeys(STREAMS, "0.30")
    result = authorize(document, plan_identity(validated_plan(document)), root, allowance, list(caps.items()))
    return str(result["authorization_id"])


def refused(reason: str, call: Any, *arguments: Any, **keywords: Any) -> AcquisitionError:
    with pytest.raises(AcquisitionError) as refusal:
        call(*arguments, **keywords)
    assert refusal.value.reason == reason, str(refusal.value)
    return refusal.value


def outcomes(result: dict[str, Any]) -> list[str]:
    return [entry["outcome"] for entry in result["recordings"]]


# --- Gate A: quotes and authorizations -------------------------------------------------------------------


def test_a_quote_asks_only_for_metadata_and_reads_estimates_exactly(tools: Path) -> None:
    fake_databento.set_behavior(tools, {"quote": {"cost": "1.2e-05"}})
    quote = quote_plan(Tools(tools), plan())
    assert [item["estimate"]["estimated_cost_usd"] for item in quote["items"]] == ["0.000012"] * 3
    assert [item["proposed_cap_usd"] for item in quote["items"]] == ["0.01"] * 3
    assert (quote["estimated_total_usd"], quote["proposed_allowance_usd"]) == ("0.000036", "0.03")
    assert quote["actual_cost"].startswith("unknown")
    assert [entry["command"] for entry in fake_databento.invocations(tools)] == ["quote"] * 3
    assert all(entry["api_key_present"] for entry in fake_databento.invocations(tools))


def test_a_quote_needs_a_key_and_a_window_that_has_passed(
    tools: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.delenv("DATABENTO_API_KEY")
    refused("missing_api_key", quote_plan, Tools(tools), plan())
    monkeypatch.setenv("DATABENTO_API_KEY", FAKE_KEY)
    future = plan(
        recordings=[
            recording(day, start=f"2099-01-{day:02d}T13:30:00Z", end=f"2099-01-{day:02d}T13:40:00Z")
            for day in (5, 6)
        ]
    )
    refused("window_not_complete", quote_plan, Tools(tools), future)
    assert fake_databento.invocations(tools) == []


@pytest.mark.parametrize("cost", ["-0.01", "NaN", "Infinity", '"0.01"', "true"])
def test_an_invalid_estimate_is_refused(tools: Path, cost: str) -> None:
    fake_databento.set_behavior(tools, {"quote": {"cost": cost}})
    refused("malformed_tool_output", quote_plan, Tools(tools), plan())
    assert downloads(tools) == 0


def test_a_quote_verifies_existing_recordings_offline_and_lists_them_apart(
    tools: Path, tmp_path: Path
) -> None:
    document = _with_existing(plan(), tmp_path)
    quote = quote_plan(Tools(tools), document)
    assert [entry["stream_id"] for entry in quote["existing_inputs"]] == [STREAMS[0]]
    assert [item["stream_id"] for item in quote["items"]] == STREAMS[1:]
    assert [entry["command"] for entry in fake_databento.invocations(tools)] == ["replay", "quote", "quote"]


def test_without_an_authorization_nothing_is_downloaded(tools: Path, root: Path) -> None:
    refused("unknown_authorization", acquire, Tools(tools), root, "a" * 64)
    assert downloads(tools) == 0


def test_an_authorization_binds_one_plan_root_allowance_and_caps(root: Path) -> None:
    document = plan()
    plan_id = plan_identity(validated_plan(document))
    refused("plan_mismatch", authorize, document, "b" * 64, root, "1.00", [])
    caps = [(STREAMS[0], "0.60"), (STREAMS[1], "0.50")]
    refused("invalid_authorization", authorize, document, plan_id, root, "1.00", caps)
    refused("invalid_authorization", authorize, document, plan_id, root, "1.00", [("not-in-plan", "0.10")])
    refused("invalid_authorization", authorize, document, plan_id, root, "1.00", [(STREAMS[0], "0.00")])
    refused("invalid_authorization", authorize, document, plan_id, root, "100.01", [])
    refused("invalid_authorization", authorize, document, plan_id, root, "1", [])
    refused("invalid_authorization", authorize, document, plan_id, root, "1.00", [(STREAMS[0], "0.10")] * 2)
    authorization_id = authorized(root, document)
    directory = root / "authorizations" / authorization_id
    # The mode, not os.access: root may write a read-only file.
    assert (directory / "authorization.json").stat().st_mode & 0o222 == 0
    stored = json.loads((directory / "authorization.json").read_bytes())
    assert (stored["plan_id"], stored["state_root"], stored["allowance_usd"]) == (
        plan_id,
        str(root.resolve()),
        "1.00",
    )
    assert (root / "ledgers" / f"{authorization_id}.ndjson").read_text().count("\n") == 1
    refused("already_authorized", authorize, document, plan_id, root, "0.90", [])


def test_a_changed_plan_needs_its_own_approval_and_a_moved_root_none(
    tools: Path, root: Path, tmp_path: Path
) -> None:
    authorization_id = authorized(root)
    changed = plan()
    changed["recordings"][0]["end"] = "2026-09-21T13:41:00Z"
    refused("plan_mismatch", authorize, changed, plan_identity(validated_plan(plan())), root, "1.00", [])
    moved = tmp_path / "moved"
    shutil.copytree(root, moved)
    refused("root_mismatch", acquire, Tools(tools), moved, authorization_id)
    assert downloads(tools) == 0


def test_the_help_separates_quoting_from_downloading() -> None:
    parser = cli._parser()
    subcommands = next(action for action in parser._actions if hasattr(action, "choices") and action.choices)
    choices: dict[str, Any] = subcommands.choices  # type: ignore[assignment]
    helps = {action.dest: action.help for action in subcommands._choices_actions}  # type: ignore[attr-defined]
    assert "never a download" in helps["quote-recordings"]
    assert "downloads nothing" in helps["authorize-acquisition"]
    assert "ONLY COMMAND THAT CAN SPEND MONEY" in helps["acquire-recordings"]
    assert "spends nothing" in helps["plan-recordings"]
    assert "never downloads" in choices["quote-recordings"].description
    assert "no default budget" in choices["authorize-acquisition"].description
    assert set(acquisition_cli.COMMANDS) <= set(choices)


def test_no_research_service_job_or_route_can_reach_the_provider_tool() -> None:
    from chronos_research import service

    assert not any("acqui" in kind or "download" in kind for kind in service.JOB_KINDS)
    source = Path(service.__file__).read_text()
    assert "acquisition" not in source
    assert "chronos_databento_historical" not in source
    assert "DATABENTO_HISTORICAL" not in source


# --- Gate B: attempts, reuse, and failure ------------------------------------------------------------------


def test_an_authorized_acquisition_downloads_each_capped_recording_once(tools: Path, root: Path) -> None:
    caps = {STREAMS[0]: "0.30", STREAMS[1]: "0.10"}
    authorization_id = authorized(root, caps=caps, allowance="0.40")
    first = acquire(Tools(tools), root, authorization_id)
    assert outcomes(first) == ["acquired", "acquired", "not_authorized"]
    assert first["reserved_usd"] == "0.40"
    arguments = [entry["argv"] for entry in fake_databento.invocations(tools, "download")]
    assert [argv[argv.index("--max-cost-usd") + 1] for argv in arguments] == ["0.3", "0.09999999999999999"]
    allowed = {
        "DATABENTO_API_KEY",
        "HOME",
        "LANG",
        "LC_ALL",
        "PATH",
        "TMPDIR",
        "SSL_CERT_DIR",
        "SSL_CERT_FILE",
    }
    assert set(acquisition.child_environment(FAKE_KEY)) <= allowed
    # What the fake child sees also holds what its own interpreter adds at
    # startup: macOS's __CF_USER_TEXT_ENCODING, and LC_CTYPE where Python
    # coerces a C locale (PEP 538).
    added_by_the_child = {"__CF_USER_TEXT_ENCODING", "LC_CTYPE"}
    for entry in fake_databento.invocations(tools, "download"):
        assert set(entry["environment"]) - added_by_the_child <= allowed
    receipt = read_receipt(root, authorization_id, STREAMS[0])
    assert receipt is not None
    assert (receipt["origin"], receipt["attempt"]) == ("acquired", "1")
    assert receipt["actual_cost"].startswith("unknown")
    assert receipt["quote_before_download"]["estimated_cost_usd"] == "0.0123"
    again = acquire(Tools(tools), root, authorization_id)
    assert outcomes(again) == ["already_acquired", "already_acquired", "not_authorized"]
    assert downloads(tools) == 2


def test_reservations_can_never_exceed_the_allowance(root: Path) -> None:
    authorization_id = authorized(root, caps={STREAMS[0]: "0.30"}, allowance="0.30")
    stored = json.loads((root / "authorizations" / authorization_id / "authorization.json").read_bytes())
    approved = validated_plan(plan())
    ledger = Ledger(root / "ledgers" / f"{authorization_id}.ndjson", stored, authorization_id, approved)
    destinations = [entry["destination"] for entry in approved["recordings"]]
    ledger.reserve(STREAMS[0], destinations[0], 30)
    refused("allowance_exhausted", ledger.reserve, STREAMS[1], destinations[1], 1)
    reloaded = Ledger(ledger.path, stored, authorization_id, approved)
    assert reloaded.reserved_cents == 30
    # A ledger edited to reserve past its allowance is refused as corrupt.
    line = json.dumps(
        {"attempt": "2", "cap_usd": "0.01", "destination": destinations[1], "entry": "reserved",
         "reserved_total_usd": "0.31", "stream_id": STREAMS[1]}, sort_keys=True, separators=(",", ":"),
    )  # fmt: skip
    os.chmod(ledger.path, 0o600)
    with ledger.path.open("a") as stream:
        stream.write(line + "\n")
    refused("state_corrupt", Ledger, ledger.path, stored, authorization_id, approved)


def test_a_concurrent_invocation_is_refused_while_one_downloads(tools: Path, root: Path) -> None:
    authorization_id = authorized(root, caps={STREAMS[0]: "0.30"}, allowance="0.30")
    fake_databento.set_behavior(tools, {"download": {"mode": "wait_for_release"}})
    results: list[Any] = []
    runner = threading.Thread(target=lambda: results.append(acquire(Tools(tools), root, authorization_id)))
    runner.start()
    _wait_for(tools / fake_databento.STARTED)
    refused("busy", acquire, Tools(tools), root, authorization_id)
    refused("busy", recover, root, authorization_id, 1, True)
    (tools / fake_databento.RELEASE).write_text("go")
    runner.join(30)
    assert outcomes(results[0])[0] == "acquired"
    assert downloads(tools) == 1


def test_a_parent_killed_after_reserving_blocks_the_root_until_explicit_recovery(
    tools: Path, root: Path
) -> None:
    authorization_id = authorized(root, caps={STREAMS[0]: "0.30"}, allowance="0.30")
    fake_databento.set_behavior(tools, {"download": {"mode": "sleep", "seconds": "60"}})
    parent = subprocess.Popen(
        [sys.executable, "-m", "chronos_research", "acquire-recordings", "--state-root", str(root),
         "--authorization", authorization_id, "--tools", str(tools)],
        cwd=PYTHON_ROOT, env={**os.environ}, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )  # fmt: skip
    child_pid = int(_wait_for(tools / fake_databento.STARTED))
    parent.kill()
    parent.wait()
    try:
        # The provider child survives its parent, and still holds the root.
        assert _alive(child_pid)
        refused("busy", acquire, Tools(tools), root, authorization_id)
        refused("busy", recover, root, authorization_id, 1, True)
    finally:
        os.kill(child_pid, signal.SIGKILL)
        _wait_until(lambda: not _alive(child_pid))
    refused("attempt_in_progress", acquire, Tools(tools), root, authorization_id)
    refused("confirmation_required", recover, root, authorization_id, 1, False)
    assert recover(root, authorization_id, 1, True)["outcome"] == "ambiguous"
    refused("invalid_request", recover, root, authorization_id, 1, True)
    after = acquire(Tools(tools), root, authorization_id)
    assert outcomes(after)[0] == "needs_reconciliation"
    assert after["reserved_usd"] == "0.30"
    assert downloads(tools) == 1


def test_a_download_past_its_timeout_is_killed_and_never_retried(tools: Path, root: Path) -> None:
    authorization_id = authorized(root)
    fake_databento.set_behavior(tools, {"download": {"mode": "sleep", "seconds": "60"}})
    result = acquire(Tools(tools), root, authorization_id, timeout_seconds=1)
    assert outcomes(result) == ["ambiguous", "not_attempted", "not_attempted"]
    assert "timed out" in result["recordings"][0]["detail"]
    child_pid = int((tools / fake_databento.STARTED).read_text())
    _wait_until(lambda: not _alive(child_pid))
    fake_databento.set_behavior(tools, {})
    again = acquire(Tools(tools), root, authorization_id)
    assert outcomes(again)[0] == "needs_reconciliation"
    assert downloads(tools) == 3  # the timed-out one, then the two not yet attempted
    attempted = [entry["argv"] for entry in fake_databento.invocations(tools, "download")]
    assert [argv[argv.index("--stream-id") + 1] for argv in attempted] == STREAMS


def test_a_download_reported_as_successful_is_still_verified_before_use(tools: Path, root: Path) -> None:
    authorization_id = authorized(root, caps={STREAMS[0]: "0.30", STREAMS[1]: "0.30"}, allowance="0.60")
    fake_databento.set_behavior(tools, {"download": {"mode": "garbage_stdout"}})
    first = acquire(Tools(tools), root, authorization_id)
    assert outcomes(first)[:2] == ["ambiguous", "not_attempted"]
    # The files it left verify on their own, so they are used, not downloaded again.
    fake_databento.set_behavior(tools, {"download": {"mode": "wrong_symbols"}})
    second = acquire(Tools(tools), root, authorization_id)
    assert outcomes(second)[:2] == ["verified_at_destination", "ambiguous"]
    receipt = read_receipt(root, authorization_id, STREAMS[0])
    assert receipt is not None
    assert (receipt["origin"], receipt["attempt"]) == ("acquired", "1")
    third = acquire(Tools(tools), root, authorization_id)
    assert outcomes(third)[:2] == ["already_acquired", "needs_reconciliation"]
    assert "requested" in third["recordings"][1]["detail"] or "symbols" in third["recordings"][1]["detail"]
    assert downloads(tools) == 2


def test_a_tool_refusal_is_recorded_stops_the_run_and_blocks_that_destination(
    tools: Path, root: Path
) -> None:
    authorization_id = authorized(root)
    fake_databento.set_behavior(tools, {"download": {"mode": "fail_code", "code": "cost_limit_exceeded"}})
    first = acquire(Tools(tools), root, authorization_id)
    assert outcomes(first) == ["failed", "not_attempted", "not_attempted"]
    assert "before the billable request" in first["recordings"][0]["detail"]
    fake_databento.set_behavior(tools, {})
    second = acquire(Tools(tools), root, authorization_id)
    assert outcomes(second) == ["needs_reconciliation", "acquired", "acquired"]
    assert downloads(tools) == 3


def test_missing_or_corrupt_state_fails_closed(tools: Path, root: Path) -> None:
    authorization_id = authorized(root)
    ledger = root / "ledgers" / f"{authorization_id}.ndjson"
    original = ledger.read_bytes()
    for damage, reason in (
        (lambda: ledger.unlink(), "state_missing"),
        (lambda: ledger.write_bytes(original + b"not json\n"), "state_corrupt"),
        (lambda: ledger.write_bytes(original + b'{"attempt":"1"'), "state_corrupt"),
        (lambda: ledger.write_bytes(b""), "state_corrupt"),
        (lambda: ledger.write_bytes(original.replace(authorization_id.encode(), b"c" * 64)), "state_corrupt"),
    ):
        damage()
        refused(reason, acquire, Tools(tools), root, authorization_id)
        ledger.write_bytes(original)
    authorization_file = root / "authorizations" / authorization_id / "authorization.json"
    os.chmod(authorization_file.parent, 0o755)
    os.chmod(authorization_file, 0o644)
    authorization_file.write_bytes(authorization_file.read_bytes().replace(b'"1.00"', b'"9.00"'))
    refused("state_corrupt", acquire, Tools(tools), root, authorization_id)
    assert downloads(tools) == 0


def test_a_tampered_receipt_fails_closed(tools: Path, root: Path) -> None:
    authorization_id = authorized(root, caps={STREAMS[0]: "0.30"}, allowance="0.30")
    acquire(Tools(tools), root, authorization_id)
    receipt = root / "receipts" / authorization_id / f"{STREAMS[0]}.json"
    os.chmod(receipt, 0o644)
    receipt.write_bytes(receipt.read_bytes().replace(b'"acquired"', b'"existing"', 1))
    refused("state_corrupt", acquire, Tools(tools), root, authorization_id)
    assert downloads(tools) == 1


def _with_existing(document: dict[str, Any], tmp_path: Path, sha256: str | None = None) -> dict[str, Any]:
    entry = document["recordings"][0]
    path = tmp_path / "existing" / "2026-09-21-aapl-spy.dbn.zst"
    actual = fake_databento.write_recording(
        path, instant_ns(entry["start"]), instant_ns(entry["end"]), entry["stream_id"], document["symbols"]
    )
    del entry["destination"]
    entry["existing"] = {
        "path": str(path),
        "sha256": sha256 or actual,
        "size_bytes": str(path.stat().st_size),
    }
    return document


def test_an_existing_recording_is_used_only_once_verified(tools: Path, root: Path, tmp_path: Path) -> None:
    good = _with_existing(plan(), tmp_path)
    authorization_id = authorized(root, good, caps={})
    result = acquire(Tools(tools), root, authorization_id)
    assert outcomes(result) == ["existing_verified", "not_authorized", "not_authorized"]
    receipt = read_receipt(root, authorization_id, STREAMS[0])
    assert receipt is not None
    assert (receipt["origin"], receipt["actual_cost"]) == (
        "existing",
        "none: no request was made for it here",
    )
    fake_databento.set_behavior(tools, {"replay": {"mode": "mismatch"}})
    assert outcomes(acquire(Tools(tools), root, authorization_id))[0] == "verification_failed"
    other_root = tmp_path / "other-state"
    other_root.mkdir()
    wrong = _with_existing(plan(), tmp_path / "wrong", sha256="d" * 64)
    other_id = authorized(other_root, wrong, caps={})
    assert outcomes(acquire(Tools(tools), other_root, other_id))[0] == "verification_failed"
    assert downloads(tools) == 0


def test_a_recording_already_at_its_destination_is_verified_never_downloaded(tools: Path, root: Path) -> None:
    authorization_id = authorized(root)
    first, second = plan()["recordings"][:2]
    for entry in (first, second):
        fake_databento.write_recording(
            root / "recordings" / entry["destination"], instant_ns(entry["start"]), instant_ns(entry["end"]),
            entry["stream_id"], ["AAPL", "SPY"],
        )  # fmt: skip
    damaged = root / "recordings" / second["destination"]
    damaged.write_bytes(damaged.read_bytes()[:-1] + b"2")
    result = acquire(Tools(tools), root, authorization_id)
    assert outcomes(result) == ["verified_at_destination", "needs_reconciliation", "acquired"]
    receipt = read_receipt(root, authorization_id, STREAMS[0])
    assert receipt is not None
    assert (receipt["origin"], receipt["attempt"]) == ("found_at_destination", None)
    assert downloads(tools) == 1


def test_interrupted_download_files_block_a_new_attempt(tools: Path, root: Path) -> None:
    authorization_id = authorized(root)
    entry = plan()["recordings"][0]
    destination = root / "recordings" / entry["destination"]
    destination.parent.mkdir(parents=True)
    Path(str(destination) + ".download.4242.0").write_bytes(b"partial")
    assert outcomes(acquire(Tools(tools), root, authorization_id))[0] == "needs_reconciliation"
    assert all(
        entry["argv"][entry["argv"].index("--stream-id") + 1] != STREAMS[0]
        for entry in fake_databento.invocations(tools, "download")
    )


@pytest.mark.parametrize(
    "behavior",
    [
        {"mode": "fail_code", "code": "provider_download_failed"},
        {"mode": "flood_stderr"},
        {"mode": "crash"},
    ],
    ids=["error-message", "oversized-stderr", "crash"],
)
def test_the_key_never_reaches_the_ledger_receipts_or_output(
    tools: Path, root: Path, behavior: dict[str, str]
) -> None:
    authorization_id = authorized(root)
    fake_databento.set_behavior(tools, {"download": behavior})
    result = acquire(Tools(tools), root, authorization_id)
    detail = result["recordings"][0]["detail"]
    assert FAKE_KEY not in json.dumps(result)
    assert len(detail) <= DETAIL_LIMIT + 200
    assert FAKE_KEY not in fake_databento.tree_text(root)
    expected = {"fail_code": "[redacted]", "flood_stderr": "output overflowed", "crash": "exited -9"}
    assert expected[behavior["mode"]] in detail
    assert downloads(tools) == 1


def test_the_command_line_runs_the_same_gates(tools: Path, root: Path, tmp_path: Path) -> None:
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan()))
    printed = _cli(["plan-recordings", "--plan", str(plan_file)])
    plan_id = printed["plan_id"]
    assert printed["spends"].startswith("nothing")
    authorized_id = _cli(
        ["authorize-acquisition", "--plan", str(plan_file), "--plan-id", plan_id, "--state-root", str(root),
         "--allowance-usd", "0.30", "--cap", f"{STREAMS[0]}=0.30"]
    )["authorization_id"]  # fmt: skip
    result = _cli(
        [
            "acquire-recordings",
            "--state-root",
            str(root),
            "--authorization",
            authorized_id,
            "--tools",
            str(tools),
        ]
    )
    assert outcomes(result) == ["acquired", "not_authorized", "not_authorized"]
    assert downloads(tools) == 1


def _cli(argv: list[str]) -> dict[str, Any]:
    completed = subprocess.run(
        [sys.executable, "-m", "chronos_research", *argv],
        cwd=PYTHON_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    document: dict[str, Any] = json.loads(completed.stdout)
    return document


def _wait_for(path: Path, seconds: float = 30) -> str:
    _wait_until(path.exists, seconds)
    return path.read_text()


def _wait_until(condition: Any, seconds: float = 30) -> None:
    deadline = time.monotonic() + seconds
    while not condition():
        if time.monotonic() > deadline:
            raise AssertionError("timed out waiting")
        time.sleep(0.02)


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie is not alive for these purposes.
    status = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True, check=False
    )
    return bool(status.stdout.strip()) and not status.stdout.strip().startswith("Z")
