"""The demo workspace's seed (demo_seed, ADR 0025): the production command,
run with only the production tools and without importing any test module,
publishes verified, audited artifacts and a navigation manifest; a second
run changes nothing; a corrupt artifact is refused, never repaired; and the
research service serves the guide's data only when configured to."""

import json
import stat
import subprocess
import sys
import threading
import urllib.error
import urllib.request
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

from chronos_research import demo_scenario, demo_seed
from chronos_research.encoding import canonical_json
from chronos_research.pipeline import Tools
from chronos_research.publication import publish
from chronos_research.service import DemoConfiguration, ServiceConfiguration, start
from tests import pipeline as built

PYTHON_ROOT = Path(__file__).resolve().parents[1]
# The production command, with a check that nothing from the tests package
# was imported on the way.
_SEED = (
    "import sys\n"
    "from chronos_research.cli import main\n"
    "code = main(sys.argv[1:])\n"
    "assert not [name for name in sys.modules if name == 'tests' or name.startswith('tests.')]\n"
    "sys.exit(code)\n"
)


def _production_tools(tools: built.Tools, directory: Path) -> Tools:
    """A tool directory with the production executables only: no test tool."""
    directory.mkdir()
    for name in ("chronos_dataset", "chronos_strategy"):
        (directory / name).symlink_to(tools.chronos_dataset.parent / name)
    return Tools(directory)


def _seed(tools: Tools, artifacts: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [
            sys.executable,
            "-c",
            _SEED,
            "seed-demo",
            "--tools",
            str(tools.directory),
            "--artifacts",
            str(artifacts),
        ],
        cwd=PYTHON_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr[-2000:]
    result: dict[str, Any] = json.loads(completed.stdout)
    return result


@pytest.fixture(scope="module")
def seeded(
    tools: built.Tools, tmp_path_factory: pytest.TempPathFactory
) -> tuple[Tools, Path, dict[str, Any]]:
    base = tmp_path_factory.mktemp("demo-seed")
    production = _production_tools(tools, base / "tools")
    artifacts = base / "artifacts"
    return production, artifacts, _seed(production, artifacts)


def _strings(document: Any) -> Iterator[str]:
    if isinstance(document, dict):
        for value in document.values():
            yield from _strings(value)
    elif isinstance(document, list):
        for value in document:
            yield from _strings(value)
    elif isinstance(document, str):
        yield document


def test_the_seed_publishes_audited_synthetic_artifacts_and_navigation(
    seeded: tuple[Tools, Path, dict[str, Any]],
) -> None:
    _, artifacts, result = seeded
    seed = result["seed"]
    assert result["outcome"] == "published"
    assert seed["schema"] == demo_seed.SCHEMA
    assert seed["seed_id"] == demo_seed.seed_identity(seed)
    assert demo_seed.verified_seed(artifacts, seed["seed_id"]) == seed
    assert seed["scenario"]["evidence"] == "synthetic"
    assert [session["name"] for session in seed["scenario"]["sessions"]] == ["h1", "h2", "h3", "playback"]
    # The playback session has a dataset and nothing derived from it.
    playback = seed["scenario"]["sessions"][-1]
    assert playback["feature_id"] is None
    assert playback["label_id"] is None
    assert seed["playback"]["dataset_id"] == playback["dataset_id"]
    assert seed["playback"]["stream"] == demo_scenario.PLAYBACK.stream
    # Every audit passed, and the chronology holds.
    assert seed["audits"]["strategy_diagnostics"]["verdict"] == "pass"
    assert {entry["verdict"] for entry in seed["audits"]["walk_forward"].values()} == {"pass"}
    chronology = seed["chronology"]
    assert int(chronology["history_ends_ns"]) < int(chronology["playback_starts_ns"])
    assert all(
        int(cutoff["at_ns"]) < int(chronology["playback_starts_ns"]) for cutoff in chronology["model_cutoffs"]
    )
    # Navigation only: routes and IDs, no machine path.
    assert (
        seed["routes"]["models"]["logistic"] == f"#/models/{seed['artifacts']['model_versions']['logistic']}"
    )
    assert not [text for text in _strings(seed) if text.startswith("/") or str(artifacts) in text]
    assert seed["fixed_in_advance"]["configuration"] == demo_seed.CONFIGURATION


def test_a_second_seed_changes_nothing(seeded: tuple[Tools, Path, dict[str, Any]]) -> None:
    tools, artifacts, first = seeded
    again = demo_seed.seed_demo(tools, artifacts)
    assert again["seed"] == first["seed"]
    assert {step["outcome"] for step in again["steps"]} == {"already_present"}
    assert [step["artifact_id"] for step in again["steps"]] == [
        step["artifact_id"] for step in first["steps"]
    ]
    assert (
        demo_seed.check_seed(tools, artifacts, first["seed"]["seed_id"])["seed_id"]
        == first["seed"]["seed_id"]
    )


def _tamper(path: Path) -> bytes:
    """Flips one digit of a published file, same size; returns the bytes."""
    path.chmod(path.stat().st_mode | stat.S_IWUSR)
    data = bytearray(path.read_bytes())
    at = next(index for index in range(len(data) - 1, 0, -1) if chr(data[index]).isdigit())
    data[at] = ord("1") if data[at] != ord("1") else ord("2")
    path.write_bytes(bytes(data))
    return bytes(data)


def test_a_corrupt_artifact_is_refused_and_left_untouched(
    seeded: tuple[Tools, Path, dict[str, Any]], tmp_path: Path
) -> None:
    tools, artifacts, result = seeded
    seed = result["seed"]
    copy = tmp_path / "artifacts"
    subprocess.run(["cp", "-R", str(artifacts), str(copy)], check=True)
    labels = next((copy / "labels" / seed["scenario"]["sessions"][0]["label_id"]).glob("*.ndjson"))
    tampered = _tamper(labels)
    completed = subprocess.run(
        [sys.executable, "-m", "chronos_research", "seed-demo", "--tools", str(tools.directory),
         "--artifacts", str(copy)],
        cwd=PYTHON_ROOT, capture_output=True, text=True, check=False,
    )  # fmt: skip
    assert completed.returncode == 1
    assert labels.read_bytes() == tampered
    # A start's check reads every playback event, so a corrupt one stops it.
    events = copy / "normalized" / seed["playback"]["dataset_id"] / "events.ndjson"
    _tamper(events)
    with pytest.raises(demo_seed.SeedError, match="does not verify"):
        demo_seed.check_seed(tools, copy, seed["seed_id"])


def _get(origin: str, path: str, method: str = "GET") -> tuple[int, dict[str, Any]]:
    request = urllib.request.Request(origin + path, method=method, data=b"{}" if method == "POST" else None)
    if method == "POST":
        request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def _serving(configuration: ServiceConfiguration) -> Iterator[str]:
    server, service = start(configuration)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        service.workers.stop()
        server.server_close()


def test_the_service_serves_the_guide_only_in_a_demo(
    seeded: tuple[Tools, Path, dict[str, Any]], tmp_path: Path
) -> None:
    tools, artifacts, result = seeded
    plain = ServiceConfiguration(tools=tools, artifacts=artifacts, state=tmp_path / "plain", port=0)
    for origin in _serving(plain):
        assert _get(origin, "/api/v1/research/demo")[0] == 404
        assert _get(origin, "/api/v1/research/demo")[1]["error"]["code"] == "route_not_found"
        assert _get(origin, "/api/v1/research/demo", "POST")[1]["error"]["code"] == "route_not_found"
    seed_id = result["seed"]["seed_id"]
    demo = ServiceConfiguration(
        tools=tools,
        artifacts=artifacts,
        state=tmp_path / "demo",
        port=0,
        demo=DemoConfiguration(seed_id=seed_id, session=2, mode="history"),
    )
    for origin in _serving(demo):
        status, document = _get(origin, "/api/v1/research/demo")
        assert status == 200
        assert document["session"] == {"mode": "history", "number": "2"}
        assert document["seed"]["seed_id"] == seed_id
        # Timestamps follow the service's representation rules.
        assert document["seed"]["playback"]["receive_first"].endswith("Z")
        assert "receive_first_ns" not in document["seed"]["playback"]
        assert _get(origin, "/api/v1/research/demo", "POST")[0] == 405
        assert _get(origin, "/api/v1/research/demo?verbose=1")[0] == 400


def test_a_seed_manifest_must_verify(tmp_path: Path) -> None:
    root = demo_seed.seeds_root(tmp_path)
    root.mkdir(parents=True)

    def never_present(directory: Path) -> bytes:
        raise AssertionError(f"{directory.name} is new")

    def published(document: dict[str, Any]) -> str:
        manifest = {**document, "seed_id": demo_seed.seed_identity(document)}
        files = {"manifest.json": canonical_json(manifest)}
        publish(root, manifest["seed_id"], files, "manifest.json", never_present)
        return str(manifest["seed_id"])

    good = published({"notice": "n", "schema": demo_seed.SCHEMA})
    assert demo_seed.verified_seed(tmp_path, good)["notice"] == "n"
    other = published({"notice": "n", "schema": {"name": "chronos.demo_seed", "version": "2"}})
    with pytest.raises(demo_seed.SeedError, match=r"is not chronos\.demo_seed/1"):
        demo_seed.verified_seed(tmp_path, other)
    manifest = root / good / "manifest.json"
    _tamper(manifest)
    with pytest.raises(demo_seed.SeedError, match="seed"):
        demo_seed.verified_seed(tmp_path, good)
    with pytest.raises(demo_seed.SeedError):
        demo_seed.verified_seed(tmp_path, "0" * 64)


def test_a_stored_table_must_be_the_scenarios(tmp_path: Path) -> None:
    contents = demo_scenario.event_table(demo_scenario.SESSIONS[0]).encode()
    assert demo_seed._write_table(tmp_path, "h1.csv", contents) == "published"
    assert demo_seed._write_table(tmp_path, "h1.csv", contents) == "already_present"
    # Read-only by its mode (os.access says writable to root, whatever the mode).
    writable = stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
    assert (tmp_path / "h1.csv").stat().st_mode & writable == 0
    tampered = _tamper(tmp_path / "h1.csv")
    with pytest.raises(demo_seed.SeedError, match="differs from the scenario"):
        demo_seed._write_table(tmp_path, "h1.csv", contents)
    assert (tmp_path / "h1.csv").read_bytes() == tampered
