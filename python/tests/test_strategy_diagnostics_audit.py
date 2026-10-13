"""The independent diagnostics audit (tools/strategy_diagnostics_audit), run
as `python -I -S`: it passes genuine diagnostics and refuses every planted
error, including ones whose checksums and identity were made consistent."""

import hashlib
import json
import runpy
import shutil
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from chronos_research.strategy_diagnostics import diagnostics_root, generate_diagnostics, request_for
from tests import attribution_runs
from tests.attribution_runs import Script, tie, ties_toward_zero
from tests.test_strategy_attribution import round_trip
from tests.test_strategy_diagnostics import Studies, studies  # noqa: F401  (the module fixture)

AUDITOR = Path(__file__).resolve().parents[2] / "tools" / "strategy_diagnostics_audit" / "audit.py"


def audit(artifacts: Path, diagnostic_id: str) -> tuple[int, dict[str, Any]]:
    completed = subprocess.run(
        [sys.executable, "-I", "-S", str(AUDITOR), str(artifacts), diagnostic_id],
        capture_output=True,
        text=True,
        check=False,
    )
    return completed.returncode, json.loads(completed.stdout)


@pytest.fixture(scope="module")
def published(studies: Studies) -> dict[str, str]:  # noqa: F811
    ids = studies.ids

    def published_id(members: list[str]) -> str:
        generated = generate_diagnostics(studies.tools, studies.artifacts, request_for(members))
        return str(generated["diagnostics"]["diagnostic_id"])

    return {
        name: published_id(members)
        for name, members in (
            ("week", [ids["week"]]),
            ("rounding", [ids["rounding"]]),
            ("stale", [ids["stale"]]),
            ("partial", [ids["partial"], ids["failed"]]),
            ("across", [ids["week"], ids["rounding"]]),
        )
    }


@pytest.mark.parametrize("name", ["week", "rounding", "stale", "partial", "across"])
def test_the_audit_passes_genuine_diagnostics(studies: Studies, published: dict[str, str], name: str) -> None:  # noqa: F811
    code, report = audit(studies.artifacts, published[name])
    assert (code, report["verdict"], report["problems"]) == (0, "pass", [])
    assert report["checks"]["fills"] > 0
    assert report["checks"]["identities"] > 0
    assert report["checks_total"] > 1000


def _canonical(document: Any) -> bytes:
    return json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode() + b"\n"


type Rows = list[dict[str, Any]]
type Change = Callable[[dict[str, Any], Rows, Rows], object]


def plant(artifacts: Path, diagnostic_id: str, change: Change) -> str:
    """The artifact changed and rewritten consistently: rows, checksums, and
    identity all agree, so only recomputation can find the change."""
    root = diagnostics_root(artifacts)
    source = root / diagnostic_id
    manifest = json.loads((source / "manifest.json").read_bytes())
    fills = [json.loads(line) for line in (source / "fills.ndjson").read_bytes().splitlines()]
    episodes = [json.loads(line) for line in (source / "episodes.ndjson").read_bytes().splitlines()]
    change(manifest, fills, episodes)
    files = {
        "fills.ndjson": b"".join(map(_canonical, fills)),
        "episodes.ndjson": b"".join(map(_canonical, episodes)),
    }
    for key, name, rows in (("fills", "fills.ndjson", fills), ("episodes", "episodes.ndjson", episodes)):
        manifest["files"][key] = {
            "name": name,
            "rows": str(len(rows)),
            "sha256": hashlib.sha256(files[name]).hexdigest(),
            "size_bytes": str(len(files[name])),
        }
    body = {key: value for key, value in manifest.items() if key != "diagnostic_id"}
    manifest["diagnostic_id"] = hashlib.sha256(_canonical(body)).hexdigest()
    target = root / manifest["diagnostic_id"]
    target.mkdir()
    for name, data in {**files, "manifest.json": _canonical(manifest)}.items():
        (target / name).write_bytes(data)
    return str(manifest["diagnostic_id"])


def _run(manifest: dict[str, Any], model: str = "logistic", fold: int = 1) -> dict[str, Any]:
    runs = manifest["studies"][0]["folds"][fold]["runs"]
    run: dict[str, Any] = next(run for run in runs if run["model"] == model and "accounts" in run)
    return run


def _bump(figure: dict[str, str], by: str = "0.000000000000000005") -> None:
    from fractions import Fraction

    value = Fraction(figure["value"]) + Fraction(by)
    whole, rest = divmod(abs(value.numerator * 10**18 // value.denominator), 10**18)
    text = str(whole) + ("." + str(rest).rjust(18, "0").rstrip("0") if rest else "")
    figure["value"] = ("-" if value < 0 else "") + text


def _account(manifest: dict[str, Any], account: str) -> dict[str, Any]:
    document: dict[str, Any] = _run(manifest)["accounts"][account]
    return document


PLANTS: dict[str, tuple[str, Change]] = {
    "a fill's spread cost": ("fills", lambda m, f, e: _bump(f[0]["contributions"]["execution_spread_cost"])),
    "a fill's signal midpoint": ("fills", lambda m, f, e: _bump(f[0]["signal"]["midpoint"], "0.01")),
    "a dropped fill": ("fills", lambda m, f, e: f.pop()),
    "an account's markdown": (
        "accounts",
        lambda m, f, e: _bump(_run(m)["accounts"]["strategy"]["components"]["terminal_bid_markdown"]),
    ),
    "a rounding adjustment made a residual": (
        "accounts",
        lambda m, f, e: _bump(_account(m, "buy_and_hold")["components"]["rounding_adjustment"], "0.00000001"),
    ),
    "a borrowed ask": (
        "accounts",
        lambda m, f, e: _run(m)["accounts"]["buy_and_hold"]["final_mark"].__setitem__("ask", "1000"),
    ),
    "an episode's PnL": ("episodes", lambda m, f, e: _bump(e[0]["pnl"])),
    "a cohort's correct count": (
        "cohorts",
        lambda m, f, e: _run(m)["decisions"]["cohorts"]["sampled_decisions"].__setitem__("correct", "999"),
    ),
    "a label moved past the cutoff": (
        "cohorts",
        lambda m, f, e: _run(m)["decisions"]["cohorts"]["sampled_decisions"]["labels"].__setitem__(
            "label_after_scoring_cutoff", "1"
        ),
    ),
    "a prediction cohort": (
        "predictions",
        lambda m, f, e: m["studies"][0]["folds"][0]["models"]["logistic"]["predictions"]["cohort"].update(
            items="1"
        ),
    ),
    "a pooled sum": (
        "pools",
        lambda m, f, e: _bump(
            m["studies"][0]["pooled"]["trading"]["logistic"]["base"]["accounts"]["strategy"]["fees"], "0.01"
        ),
    ),
    "a delta": ("deltas", lambda m, f, e: _bump(_run(m)["deltas"]["cash"]["booked_net_pnl"], "0.01")),
    "an order count": (
        "accounts",
        lambda m, f, e: _run(m)["accounts"]["strategy"]["orders"]["outcomes"].__setitem__("rejected", "1"),
    ),
    "an observed duration": (
        "accounts",
        lambda m, f, e: _account(m, "strategy")["timing"]["signal_to_execution_duration_ns"].update(sum="1"),
    ),
    "a source checksum": (
        "sources",
        lambda m, f, e: m["studies"][0]["folds"][0].update(events_sha256="0" * 64),
    ),
    "a reconciliation claimed": (
        "accounts",
        lambda m, f, e: _run(m)["accounts"]["cash"]["reconciliation"].__setitem__(
            "booked_net_pnl", {"state": "reconciled_at_bid"}
        ),
    ),
    "the counts": ("manifest", lambda m, f, e: m["counts"].__setitem__("fills", "0")),
}


@pytest.mark.parametrize("planted", sorted(PLANTS))
def test_the_audit_refuses_every_planted_error(
    studies: Studies,  # noqa: F811
    published: dict[str, str],
    tmp_path: Path,
    planted: str,
) -> None:
    artifacts = tmp_path / "artifacts"
    shutil.copytree(studies.artifacts, artifacts, symlinks=True)
    category, change = PLANTS[planted]
    identifier = plant(artifacts, published["week"], change)
    code, report = audit(artifacts, identifier)
    assert (code, report["verdict"]) == (1, "fail")
    assert any(problem.startswith(f"{category}:") for problem in report["problems"]), report["problems"]


def test_the_audit_refuses_corrupt_files_and_changed_sources(
    studies: Studies,  # noqa: F811
    published: dict[str, str],
    tmp_path: Path,
) -> None:
    artifacts = tmp_path / "artifacts"
    shutil.copytree(studies.artifacts, artifacts, symlinks=True)
    directory = diagnostics_root(artifacts) / published["week"]
    directory.chmod(0o755)
    fills = directory / "fills.ndjson"
    fills.chmod(0o644)
    original = fills.read_bytes()
    fills.write_bytes(original + b"{}\n")
    code, report = audit(artifacts, published["week"])
    assert code == 1
    assert any(problem.startswith("artifact:") for problem in report["problems"])
    fills.write_bytes(original)
    manifest = directory / "manifest.json"
    manifest.chmod(0o644)
    manifest.write_bytes(b" " + manifest.read_bytes())
    code, report = audit(artifacts, published["week"])
    assert code == 1
    assert report["problems"][0].startswith("unreadable:")
    # A journal changed after the diagnostics were generated.
    run_id = json.loads(original.splitlines()[0])["run_id"]
    journal = artifacts / "strategies" / run_id / "journal.ndjson"
    journal.parent.chmod(0o755)
    journal.chmod(0o644)
    journal.write_bytes(journal.read_bytes().replace(b"buy", b"sell", 1))
    code, report = audit(artifacts, published["across"])
    assert (code, report["verdict"]) == (1, "fail")
    assert audit(artifacts, "0" * 64)[0] == 1


# --- Execution rules, on hand-built runs ------------------------------------------------------------------


def _run_problems(script: Script, manifest: dict[str, Any] | None = None) -> list[str]:
    """The auditor's own run checks of a hand-built run, in this process."""
    auditor = runpy.run_path(str(AUDITOR))
    audit = auditor["Audit"]()
    built_manifest, journal = script.run()
    book = auditor["Book"](script.events, attribution_runs.STREAM, attribution_runs.INSTRUMENT)
    rows = {int(item["identity"]["ingest_sequence"]): item for item in script.rows}
    auditor["run_figures"](audit, "run", manifest or built_manifest, journal, book, rows)
    return list(audit.problems)


@pytest.mark.parametrize("mode", ["buy", "sell", "market_value"])
def test_the_audit_refuses_a_tie_rounded_toward_zero(mode: str) -> None:
    script = tie(mode)
    assert _run_problems(script) == []
    if mode == "market_value":
        script.value_rounding = ties_toward_zero
    else:
        script.notional_rounding = ties_toward_zero
    problems = _run_problems(script)
    expected = "valuation:" if mode == "market_value" else "execution:"
    assert any(problem.startswith(expected) and "half away from zero" in problem for problem in problems), (
        problems
    )


@pytest.mark.parametrize(("configured", "booked"), [("0.01", "0.02"), ("0.01", "0"), ("0", "0.01")])
def test_the_audit_refuses_a_fee_other_than_the_runs_fee_per_fill(configured: str, booked: str) -> None:
    script = round_trip()
    script.fee = configured
    assert _run_problems(script) == []
    script.booked_fee = booked
    problems = _run_problems(script)
    assert any(problem.startswith("execution:") and "fee_per_fill" in problem for problem in problems), (
        problems
    )


# --- Sources, authenticated before use --------------------------------------------------------------------


def _writable(path: Path) -> Path:
    path.parent.chmod(0o755)
    if path.exists():
        path.chmod(0o644)
    return path


def _evaluation(artifacts: Path, study_id: str, fold: int = 0) -> dict[str, Any]:
    study = json.loads((artifacts / "walk_forward_studies" / study_id / "manifest.json").read_bytes())
    plan = json.loads((artifacts / "walk_forward_plans" / study["plan_id"] / "manifest.json").read_bytes())
    evaluation: dict[str, Any] = plan["folds"][fold]["evaluation"]
    return evaluation


def _change_feature_row(path: Path) -> None:
    records = [json.loads(line) for line in path.read_bytes().splitlines()]
    changed = next(record for record in records if "raw" in record["features"]["spread"])
    changed["features"]["spread"]["raw"] = str(int(changed["features"]["spread"]["raw"]) + 1)
    _writable(path).write_bytes(b"".join(_canonical(record) for record in records))


def _reidentify(path: Path, key: str, newline: bool, change: Callable[[dict[str, Any]], object]) -> str:
    """A manifest changed and given the identity of its new content, in a
    directory of that name: a consistent forgery of one artifact."""
    document = json.loads(path.read_bytes())
    change(document)
    body = {name: value for name, value in document.items() if name != key}
    spelled = _canonical(body)
    document[key] = hashlib.sha256(spelled if newline else spelled[:-1]).hexdigest()
    target = path.parent.parent / document[key]
    shutil.copytree(path.parent, target)
    _writable(target / "manifest.json").write_bytes(_canonical(document))
    return str(document[key])


def _forge_labels(artifacts: Path, study_id: str, diagnostic_id: str) -> str:
    """Changed labels, with their label manifest, the plan, the study, and
    the diagnostics all re-identified to match: only the collection, which
    names every member's label artifact, still disagrees."""
    evaluation = _evaluation(artifacts, study_id)
    labels = artifacts / "labels" / evaluation["label_artifact_id"]
    rows = [json.loads(line) for line in (labels / "labels.ndjson").read_bytes().splitlines()]
    scored = next(row for row in rows if "direction" in row["label"])
    scored["label"]["return_raw"] = str(int(scored["label"]["return_raw"]) + 1)
    data = b"".join(_canonical(row) for row in rows)
    _writable(labels / "labels.ndjson").write_bytes(data)
    sha = hashlib.sha256(data).hexdigest()
    label_id = _reidentify(
        labels / "manifest.json",
        "artifact_id",
        False,
        lambda document: document["files"]["labels"].update(sha256=sha, size_bytes=str(len(data))),
    )
    study_path = artifacts / "walk_forward_studies" / study_id / "manifest.json"
    plan_id = _reidentify(
        artifacts / "walk_forward_plans" / json.loads(study_path.read_bytes())["plan_id"] / "manifest.json",
        "plan_id",
        True,
        lambda plan: plan["folds"][0]["evaluation"].update(label_artifact_id=label_id),
    )
    forged = _reidentify(study_path, "study_id", True, lambda study: study.update(plan_id=plan_id))

    def renamed(manifest: dict[str, Any], fills: Rows, episodes: Rows) -> None:
        manifest["request"]["study_ids"] = [forged]
        section = manifest["studies"][0]
        section.update(study_id=forged, plan_id=plan_id)
        section["folds"][0].update(label_artifact_id=label_id, labels_sha256=sha)
        for rows_of in (fills, episodes):
            for item in rows_of:
                item["study_id"] = forged

    return plant(artifacts, diagnostic_id, renamed)


def _features(artifacts: Path, study_id: str, name: str) -> Path:
    feature_id: str = _evaluation(artifacts, study_id)["feature_artifact_id"]
    return artifacts / "features" / feature_id / name


def _missing(path: Path) -> None:
    _writable(path).unlink()


def _rewritten(path: Path, change: Callable[[bytes], bytes]) -> None:
    _writable(path).write_bytes(change(path.read_bytes()))


SOURCES: dict[str, tuple[Callable[[Path, str, str], object], str]] = {
    "changed feature bytes": (
        lambda a, s, d: _change_feature_row(_features(a, s, "features.ndjson")),
        "features.ndjson has its manifest's checksum",
    ),
    "a missing feature file": (
        lambda a, s, d: _missing(_features(a, s, "features.ndjson")),
        "features.ndjson cannot be read",
    ),
    "a missing label artifact": (
        lambda a, s, d: shutil.rmtree(
            _writable(a / "labels" / _evaluation(a, s)["label_artifact_id"] / "manifest.json").parent
        ),
        "labels",
    ),
    "a changed feature manifest": (
        lambda a, s, d: _rewritten(
            _features(a, s, "manifest.json"), lambda data: data.replace(b'"rows":"', b'"rows":"1', 1)
        ),
        "identity does not match",
    ),
    "a changed dataset manifest": (
        lambda a, s, d: _rewritten(
            a / "normalized" / _evaluation(a, s)["dataset_id"] / "manifest.json", lambda data: b" " + data
        ),
        "not canonical",
    ),
    "a forged label artifact": (_forge_labels, "collection member"),
}


@pytest.mark.parametrize("case", sorted(SOURCES))
def test_the_audit_authenticates_every_source_before_using_it(
    studies: Studies,  # noqa: F811
    published: dict[str, str],
    tmp_path: Path,
    case: str,
) -> None:
    artifacts = tmp_path / "artifacts"
    shutil.copytree(studies.artifacts, artifacts, symlinks=True)
    change, named = SOURCES[case]
    planted = change(artifacts, studies.ids["week"], published["week"])
    code, report = audit(artifacts, planted if isinstance(planted, str) else published["week"])
    assert (code, report["verdict"]) == (1, "fail")
    assert any(problem.startswith("sources:") and named in problem for problem in report["problems"]), report[
        "problems"
    ]


def test_the_audit_refuses_a_zero_where_no_run_was_evaluated(
    studies: Studies,  # noqa: F811
    published: dict[str, str],
    tmp_path: Path,
) -> None:
    artifacts = tmp_path / "artifacts"
    shutil.copytree(studies.artifacts, artifacts, symlinks=True)

    def zero(manifest: dict[str, Any], fills: Rows, episodes: Rows) -> None:
        failed = next(
            section for section in manifest["studies"] if section["study_id"] == studies.ids["failed"]
        )
        stress = failed["pooled"]["trading"]["logistic"]["stress"]
        assert stress["accounts"]["strategy"]["booked_net_pnl"] == {"unavailable": "no_completed_runs"}
        stress["accounts"]["strategy"]["booked_net_pnl"] = {"value": "0"}

    identifier = plant(artifacts, published["partial"], zero)
    code, report = audit(artifacts, identifier)
    assert (code, report["verdict"]) == (1, "fail")
    assert any(problem.startswith("pools:") for problem in report["problems"]), report["problems"]
