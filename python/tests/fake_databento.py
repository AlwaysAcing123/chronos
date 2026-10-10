"""Fake Databento tools for acquisition tests: nothing here can reach a provider.

`install(directory)` writes executable stand-ins for chronos_databento_historical
and chronos_databento_replay. They read their behavior from `behavior.json`
beside them (rewritten by tests between runs) and append every invocation to
`invocations.ndjson`, recording whether a key was present but never its value.

A fake recording is a small JSON header (query and stream) padded with
bytes; its manifest has the C++ manifest's shape, so the real verification
code checks it like any other. Fake recordings are labelled `fake` in their
bytes and exist only in test directories.
"""

import hashlib
import json
import os
import stat
import sys
from pathlib import Path
from typing import Any

HISTORICAL = "chronos_databento_historical"
REPLAY = "chronos_databento_replay"
BEHAVIOR = "behavior.json"
INVOCATIONS = "invocations.ndjson"
STARTED = "started"
RELEASE = "release"
FAKE_KEY = "fake-databento-key-0123456789abcdef"

_COMMON = r"""
import hashlib, json, os, sys, time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
BEHAVIOR = json.loads((HERE / "behavior.json").read_text())


def log(entry):
    with (HERE / "invocations.ndjson").open("a") as stream:
        stream.write(json.dumps(entry, sort_keys=True) + "\n")


def options(argv):
    found, symbols = {}, []
    for name, value in zip(argv[0::2], argv[1::2]):
        if name == "--symbol":
            symbols.append(value)
        else:
            found[name] = value
    return found, symbols


def nanoseconds(text):
    return int(datetime.strptime(text, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp()) * 10**9


def manifest_for(path, header):
    data = path.read_bytes()
    return {
        "counts": {"normalized_events": 4, "provider_records": 3},
        "manifest_version": 1,
        "normalization": {"contract_version": 1, "first_ingest_sequence": 0, "last_ingest_sequence": 3,
                          "price_scale": 1000000000, "quantity_scale": 100000000, "stream": header["stream"]},
        "provider": {"dataset": "EQUS.MINI", "dbn_version": 3, "name": "databento", "schema": "mbp-1",
                     "sdk_source_commit": "fake", "sdk_version": "fake"},
        "query": {"end_nanoseconds": header["end_ns"], "includes_send_timestamps": False,
                  "input_symbology": "raw_symbol", "output_symbology": "instrument_id",
                  "partially_resolved_symbols": [], "requested_symbols": header["symbols"],
                  "start_nanoseconds": header["start_ns"], "unresolved_symbols": []},
        "source": {"file_name": path.name, "sha256": hashlib.sha256(data).hexdigest(),
                   "size_bytes": len(data)},
    }


def header_of(path):
    data = path.read_bytes()
    return json.loads(data[: data.index(b"\n")])
"""

_HISTORICAL = r"""
command, found_symbols = sys.argv[1], None
found, symbols = options(sys.argv[2:])
key = os.environ.get("DATABENTO_API_KEY", "")
log({"command": command, "argv": sys.argv[1:], "api_key_present": bool(key),
     "environment": sorted(os.environ)})
query = {"dataset": "EQUS.MINI", "end": found["--end"], "input_symbology": "raw_symbol",
         "output_symbology": "instrument_id", "schema": "mbp-1", "start": found["--start"],
         "symbols": symbols}
behavior = BEHAVIOR.get(command, {})
mode = behavior.get("mode", "ok")


def fail(code, message):
    sys.stderr.write(json.dumps({"error": {"code": code, "field": "query", "message": message}}) + "\n")
    sys.exit(1)


if command == "quote":
    if mode == "fail":
        fail("provider_quote_failed", "quote refused " + key)
    cost = behavior.get("cost", "0.0123")
    estimate = ('{"estimated_billable_size_bytes": 2048, "estimated_cost_usd": %s, '
                '"estimated_record_count": 120}' % cost)
    sys.stdout.write('{"query": %s, "quote": %s}\n' % (json.dumps(query), estimate))
    sys.exit(0)

output = Path(found["--output"])
(HERE / "started").write_text(str(os.getpid()))
if mode == "sleep":
    time.sleep(float(behavior.get("seconds", "30")))
if mode == "wait_for_release":
    for _ in range(600):
        if (HERE / "release").exists():
            break
        time.sleep(0.05)
if mode == "fail_code":
    fail(behavior.get("code", "provider_download_failed"), "download refused for key " + key)
if mode == "flood_stderr":
    sys.stderr.write(("key=" + key + " ") * 20000)
    sys.exit(1)
if mode == "crash":
    sys.stderr.write("Segmentation fault while holding " + key + "\n")
    os.kill(os.getpid(), 9)
header = {"end_ns": nanoseconds(found["--end"]), "start_ns": nanoseconds(found["--start"]),
          "stream": found["--stream-id"], "symbols": symbols if mode != "wrong_symbols" else symbols[:1]}
body = b"\nfake recording, not market data\n" + b"0" * 512
output.write_bytes(json.dumps(header, sort_keys=True).encode() + body)
manifest = manifest_for(output, header)
manifest_path = Path(str(output) + ".manifest.json")
manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
for path in (output, manifest_path):
    path.chmod(0o444)
if mode == "garbage_stdout":
    sys.stdout.write("this is not the tool's JSON\n")
    sys.exit(0)
sys.stdout.write(json.dumps({"manifest": manifest, "manifest_path": str(manifest_path), "query": query,
                             "quote": {"estimated_billable_size_bytes": 2048, "estimated_cost_usd": 0.0123,
                                       "estimated_record_count": 120},
                             "recording_path": str(output)}) + "\n")
"""

_REPLAY = r"""
found, _ = options(sys.argv[1:])
log({"command": "replay", "argv": sys.argv[1:]})
path = Path(found["--input"])
manifest = manifest_for(path, header_of(path))
if BEHAVIOR.get("replay", {}).get("mode") == "mismatch":
    manifest["counts"]["normalized_events"] += 1
sys.stdout.write(json.dumps({"manifest": manifest, "recording_path": str(path), "replay": {}}) + "\n")
"""


def install(directory: Path, behavior: dict[str, Any] | None = None) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    for name, body in ((HISTORICAL, _HISTORICAL), (REPLAY, _REPLAY)):
        script = directory / name
        script.write_text(f"#!{sys.executable}\n{_COMMON}\n{body}")
        script.chmod(script.stat().st_mode | stat.S_IXUSR)
    set_behavior(directory, behavior or {})
    return directory


def set_behavior(directory: Path, behavior: dict[str, Any]) -> None:
    (directory / BEHAVIOR).write_text(json.dumps(behavior))
    for marker in (STARTED, RELEASE):
        (directory / marker).unlink(missing_ok=True)


def invocations(directory: Path, command: str | None = None) -> list[dict[str, Any]]:
    path = directory / INVOCATIONS
    if not path.exists():
        return []
    entries = [json.loads(line) for line in path.read_text().splitlines()]
    return [entry for entry in entries if command is None or entry["command"] == command]


def write_recording(path: Path, start_ns: int, end_ns: int, stream: str, symbols: list[str]) -> str:
    """A fake recording and manifest on disk, as an earlier acquisition left
    them; returns its SHA-256."""
    path.parent.mkdir(parents=True, exist_ok=True)
    header = {"end_ns": end_ns, "start_ns": start_ns, "stream": stream, "symbols": symbols}
    path.write_bytes(
        json.dumps(header, sort_keys=True).encode() + b"\nfake recording, not market data\n" + b"1" * 256
    )
    data = path.read_bytes()
    manifest = {
        "counts": {"normalized_events": 4, "provider_records": 3},
        "manifest_version": 1,
        "normalization": {
            "contract_version": 1,
            "first_ingest_sequence": 0,
            "last_ingest_sequence": 3,
            "price_scale": 1000000000,
            "quantity_scale": 100000000,
            "stream": stream,
        },
        "provider": {
            "dataset": "EQUS.MINI",
            "dbn_version": 3,
            "name": "databento",
            "schema": "mbp-1",
            "sdk_source_commit": "fake",
            "sdk_version": "fake",
        },
        "query": {
            "end_nanoseconds": end_ns,
            "includes_send_timestamps": False,
            "input_symbology": "raw_symbol",
            "output_symbology": "instrument_id",
            "partially_resolved_symbols": [],
            "requested_symbols": symbols,
            "start_nanoseconds": start_ns,
            "unresolved_symbols": [],
        },
        "source": {
            "file_name": path.name,
            "sha256": hashlib.sha256(data).hexdigest(),
            "size_bytes": len(data),
        },
    }
    Path(str(path) + ".manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return hashlib.sha256(data).hexdigest()


def tree_text(root: Path) -> str:
    """Every file under a state root as text, to search for a secret."""
    parts = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and not os.path.islink(path):
            parts.append(path.read_bytes().decode("utf-8", "replace"))
    return "\n".join(parts)
