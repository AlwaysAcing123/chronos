"""Offline preparation and frozen evaluation of a request plan's recordings.

    python -m chronos_research prepare-recordings --plan <plan.json>
        --tools <build dir> --artifacts <artifacts root> [--state-root <root>]
        [--markdown <report.md>]

For each recording in the plan it finds a raw file it can trust (an
`existing` input, or a destination with a receipt under the plan's
authorization in the state root), verifies it again, normalizes it, and
extracts features and labels with the plan's horizon. It then builds one
collection, and for each planned instrument publishes the walk-forward plan
the request plan froze, runs its study, and has the independent auditor
check it. A recording that is not available is reported as pending with
its reason, and a study the planner refuses is reported with the refusal.

It never contacts a provider or reads a credential. Its report copies
every figure from the published study, with each unavailable value's
reason, and computes no winner, significance, or annualized figure.
"""

import json
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from chronos_research import walk_forward
from chronos_research.acquisition import (
    AcquisitionError,
    check_receipt,
    plan_authorization,
    read_receipt,
    safe_destination,
)
from chronos_research.bounded_process import EXITED, run_bounded
from chronos_research.collection import create_collection
from chronos_research.pipeline import (
    ArtifactRoots,
    Tools,
    extract_features,
    extract_labels,
    normalize_recording,
)
from chronos_research.recording_plan import plan_identity, validated_plan, walk_forward_request
from chronos_research.recording_verification import (
    RecordingVerificationError,
    VerifiedRecordingFile,
    verify_recording_file,
)
from chronos_research.walk_forward_study import run_study

REPORT_SCHEMA = {"name": "chronos.recording_pilot_report", "version": "1"}
AUDITOR = Path(__file__).resolve().parents[2] / "tools" / "walk_forward_audit" / "audit.py"
_AUDIT_TIMEOUT_SECONDS = 3_600
LIMITATIONS = (
    "A pilot of a few short recordings is partial coverage of a few sessions, not a sample of markets: "
    "it cannot show that a model generalizes, that a strategy is profitable, or that a difference is "
    "significant. Each recording's coverage is partial and its session label a declaration. Databento "
    "EQUS.MINI is a derived composite mini-BBO, not SIP, not the NBBO, and not full depth. Trading "
    "figures are paper simulations against recorded top-of-book quotes, one share at a time, with no "
    "queue position or market impact."
)


@dataclass(frozen=True)
class PreparedRecording:
    """A recording's derived artifacts, and the raw file they came from
    (None for the synthetic test generator's recordings)."""

    stream_id: str
    session_label: str
    dataset_id: str
    feature_id: str
    label_id: str
    raw: dict[str, str] | None = None

    def as_json(self) -> dict[str, Any]:
        return {
            "dataset_id": self.dataset_id,
            "feature_id": self.feature_id,
            "label_id": self.label_id,
            "raw": self.raw if self.raw is not None else {"unavailable": "synthetic_recording"},
            "session_label": self.session_label,
            "stream_id": self.stream_id,
        }


# --- Finding and verifying the recordings ----------------------------------------------------------------


def located_recording(
    tools: Tools, plan: Mapping[str, Any], recording: Mapping[str, Any], state_root: Path | None
) -> tuple[VerifiedRecordingFile | None, str]:
    """The recording's verified raw file and its origin, or None and why not."""
    if "existing" in recording:
        return verify_recording_file(tools, plan, recording, Path(recording["existing"]["path"])), "existing"
    if state_root is None:
        return None, "not acquired: no state root was given"
    root = Path(state_root).resolve()
    found = plan_authorization(root, plan_identity(plan))
    if found is None:
        return None, "not acquired: the state root holds no authorization for this plan"
    authorization_id, ledger = found
    receipt = read_receipt(root, authorization_id, recording["stream_id"])
    if receipt is None:
        return None, "not acquired: no receipt for it under the plan's authorization"
    verified = verify_recording_file(tools, plan, recording, safe_destination(root, recording))
    check_receipt(receipt, authorization_id, plan, recording, ledger.attempts, verified)
    return verified, str(receipt["origin"])


def prepare_recording(
    tools: Tools,
    artifacts: Path,
    recording: Mapping[str, Any],
    verified: VerifiedRecordingFile,
    horizon_ns: int,
) -> PreparedRecording:
    roots = ArtifactRoots.under(artifacts)
    for directory in (roots.catalog, roots.features, roots.labels):
        directory.mkdir(parents=True, exist_ok=True)
    dataset = normalize_recording(tools, verified.path, recording["stream_id"], roots.catalog)
    features = extract_features(tools, roots, dataset.artifact_id)
    labels = extract_labels(tools, roots, features.artifact_id, horizon_ns)
    return PreparedRecording(
        stream_id=recording["stream_id"],
        session_label=recording["session_label"],
        dataset_id=dataset.artifact_id,
        feature_id=features.artifact_id,
        label_id=labels.artifact_id,
        raw={
            "manifest_sha256": verified.manifest_sha256,
            "recording_sha256": verified.sha256,
            "size_bytes": str(verified.size_bytes),
        },
    )


# --- The frozen evaluation -------------------------------------------------------------------------------


def run_auditor(artifacts: Path, study_id: str, auditor: Path = AUDITOR) -> dict[str, Any]:
    """The independent auditor's verdict: the standard library alone (-I -S)."""
    finished = run_bounded(
        [sys.executable, "-I", "-S", str(auditor), str(artifacts), study_id],
        {"PATH": "/usr/bin:/bin"},
        _AUDIT_TIMEOUT_SECONDS,
        64 * 1024 * 1024,
        1024 * 1024,
    )
    if finished.outcome != EXITED or finished.returncode not in (0, 1):
        detail = finished.stderr.decode("utf-8", "replace")[:2_000]
        return {
            "verdict": "not_run",
            "detail": f"the auditor {finished.outcome} ({finished.returncode}): {detail}",
        }
    report = json.loads(finished.stdout)
    return {
        "checks_total": report["checks_total"],
        "not_audited": report["not_audited"],
        "problems": report["problems"][:20],
        "problems_total": str(len(report["problems"])),
        "verdict": report["verdict"],
    }


def _study_plan(
    tools: Tools, artifacts: Path, evaluation: Mapping[str, Any], collection_id: str, instrument: str
) -> dict[str, Any]:
    """The published plan for one instrument, held to the frozen settings."""
    request = walk_forward_request(evaluation, collection_id, instrument)
    plan = walk_forward.create_plan(tools, artifacts, request)["plan"]
    if (
        plan["request"] != walk_forward.validated_request(request)
        or plan["horizon_ns"] != evaluation["horizon_ns"]
    ):
        raise walk_forward.PlanError(
            "not_frozen", f"the {instrument} plan is not the request plan's frozen settings"
        )
    return dict(plan)


def evaluate_collection(
    tools: Tools,
    artifacts: Path,
    prepared: Sequence[PreparedRecording],
    evaluation: Mapping[str, Any],
    auditor: Path = AUDITOR,
) -> dict[str, Any]:
    """One collection of the prepared recordings, then each instrument's
    frozen plan, study, and independent audit."""
    if not prepared:
        return {"collection": {"unavailable": "no_recordings"}, "studies": []}
    created = create_collection(
        tools,
        artifacts,
        [recording.label_id for recording in prepared],
        {recording.label_id: recording.session_label for recording in prepared},
    )["collection"]
    studies: list[dict[str, Any]] = []
    for instrument in evaluation["instruments"]:
        try:
            plan = _study_plan(tools, artifacts, evaluation, created["collection_id"], instrument)
        except walk_forward.PlanError as error:
            studies.append(
                {"instrument": instrument, "refused": {"message": str(error), "reason": error.reason}}
            )
            continue
        study = run_study(tools, artifacts, plan["plan_id"])["study"]
        studies.append(
            {
                "audit": run_auditor(artifacts, study["study_id"], auditor),
                "instrument": instrument,
                "plan_id": plan["plan_id"],
                **summarize_study(study),
            }
        )
    return {
        "collection": {
            "collection_id": created["collection_id"],
            "evidence": created["evidence"],
            "members": str(len(created["members"])),
        },
        "studies": studies,
    }


def prepare_plan(
    tools: Tools, document: Any, artifacts: Path, state_root: Path | None, auditor: Path = AUDITOR
) -> dict[str, Any]:
    """Every available recording verified and prepared, the rest pending
    with reasons, and the frozen evaluation of what is there."""
    plan = validated_plan(document)
    evaluation = plan["evaluation"]
    prepared: list[PreparedRecording] = []
    pending: list[dict[str, str]] = []
    origins: dict[str, str] = {}
    for recording in plan["recordings"]:
        try:
            verified, origin = located_recording(tools, plan, recording, state_root)
        except (AcquisitionError, RecordingVerificationError) as error:
            verified, origin = None, f"refused ({error.reason}): {error}"
        if verified is None:
            pending.append({"reason": origin, "stream_id": recording["stream_id"]})
            continue
        origins[recording["stream_id"]] = origin
        prepared.append(
            prepare_recording(tools, artifacts, recording, verified, int(evaluation["horizon_ns"]))
        )
    evaluated = evaluate_collection(tools, artifacts, prepared, evaluation, auditor)
    return {
        "evaluation": evaluation,
        "limitations": LIMITATIONS,
        "pending": pending,
        "plan_id": plan_identity(plan),
        "recordings": [
            {**recording.as_json(), "origin": origins[recording.stream_id]} for recording in prepared
        ],
        "schema": REPORT_SCHEMA,
        **evaluated,
    }


# --- The study, summarized with its reasons --------------------------------------------------------------


def _value(metric: Any) -> str:
    """A metric as the study wrote it: its value, or why it is undefined."""
    if isinstance(metric, dict) and "value" in metric:
        return str(metric["value"])
    if isinstance(metric, dict) and "undefined" in metric:
        return f"undefined: {metric['undefined']}"
    return "unavailable"


def _predictive(entry: Mapping[str, Any]) -> dict[str, Any]:
    predictive = entry.get("predictive")
    if not isinstance(predictive, dict) or "metrics" not in predictive:
        reason = predictive.get("unavailable") if isinstance(predictive, dict) else None
        return {"unavailable": str(reason or entry.get("reason") or "not_scored")}
    metrics = predictive["metrics"]
    return {
        "accuracy": _value(metrics["accuracy"]),
        "balanced_accuracy": _value(metrics["balanced_accuracy"]),
        "log_loss": _value(metrics["log_loss"]),
        "rows": predictive["rows"],
    }


def _account(account: Mapping[str, Any]) -> dict[str, Any]:
    valuation = account["valuation"]
    summary: dict[str, Any] = {
        "fills": account["fills"],
        "maximum_drawdown": account["drawdown"]["maximum"],
        "orders": account["orders"],
        "turnover": account["turnover"]["notional"],
    }
    if "unavailable" in valuation:
        summary["valuation"] = {"unavailable": valuation["unavailable"]}
    else:
        summary["valuation"] = {
            key: valuation[key] for key in ("fees_paid", "net_total_pnl", "return_percent")
        }
    return summary


def _cell(cell: Mapping[str, Any]) -> dict[str, Any]:
    summary: dict[str, Any] = {
        "fee_per_fill": cell["fee_per_fill"],
        "latency_ns": cell["latency_ns"],
        "scenario": cell["scenario"],
        "state": cell["state"],
    }
    if cell["state"] != "completed":
        summary["reason"] = cell.get("reason", "unknown")
        if "detail" in cell:
            summary["detail"] = cell["detail"]
        return summary
    summary["accounts"] = {name: _account(account) for name, account in cell["accounts"].items()}
    summary["deltas"] = cell["deltas"]
    summary["run_id"] = cell["run"]["artifact_id"]
    return summary


def _model(entry: Mapping[str, Any]) -> dict[str, Any]:
    if entry["state"] != "completed":
        return {
            "reason": entry.get("reason", "unknown"),
            "state": entry["state"],
            "detail": entry.get("detail"),
        }
    return {
        "cells": [_cell(cell) for cell in entry["cells"]],
        "predictive": _predictive(entry),
        "schedule_id": entry["schedule"]["artifact_id"],
        "state": "completed",
    }


def summarize_study(study: Mapping[str, Any]) -> dict[str, Any]:
    folds = []
    for fold in study["folds"]:
        folds.append(
            {
                "evaluation": {
                    "feature_rows": fold["evaluation"]["feature_rows"],
                    "stream": fold["evaluation"]["stream"],
                    "window": fold["evaluation"]["window"],
                },
                "fold": fold["index"],
                "fold_model": fold["fold_model"]["state"],
                "models": {name: _model(entry) for name, entry in fold.get("models", {}).items()},
                "training": {key: fold["training"][key] for key in ("class_support", "rows")},
            }
        )
    return {
        "aggregates": study["aggregates"],
        "folds": folds,
        "missing": study["missing"],
        "outcomes": study["outcomes"],
        "status": study["status"],
        "study_id": study["study_id"],
    }


# --- A reader's rendering --------------------------------------------------------------------------------


def _money(account: Mapping[str, Any]) -> tuple[str, str, str]:
    """Net PnL, return, and fees, or each the reason the valuation is unavailable."""
    valuation = account["valuation"]
    if "unavailable" in valuation:
        reason = valuation["unavailable"]
        text = f"unavailable: {reason.get('code', reason) if isinstance(reason, dict) else reason}"
        return text, text, text
    return valuation["net_total_pnl"], valuation["return_percent"], valuation["fees_paid"]


def _short(identifier: str) -> str:
    return f"`{identifier[:12]}…`"


def _row(*cells: object) -> str:
    return "| " + " | ".join(str(cell) for cell in cells) + " |"


def _recording_rows(report: Mapping[str, Any]) -> list[str]:
    lines = [
        _row("Stream", "Origin", "Raw SHA-256", "Dataset", "Features", "Labels"),
        _row(*["---"] * 6),
    ]
    for recording in report["recordings"]:
        raw = recording["raw"]
        lines.append(
            _row(
                recording["stream_id"],
                recording["origin"],
                _short(raw["recording_sha256"])
                if "recording_sha256" in raw
                else f"none: {raw['unavailable']}",
                _short(recording["dataset_id"]),
                _short(recording["feature_id"]),
                _short(recording["label_id"]),
            )
        )
    lines += [
        _row(entry["stream_id"], f"**pending**: {entry['reason']}", "", "", "", "")
        for entry in report["pending"]
    ]
    return lines


def _versus(deltas: Mapping[str, Any], benchmark: str) -> str:
    delta = deltas[benchmark]
    return (
        str(delta["net_total_pnl"])
        if "net_total_pnl" in delta
        else f"unavailable: {delta.get('unavailable')}"
    )


def _trading_rows(study: Mapping[str, Any]) -> list[str]:
    heading = ("Fold", "Model", "Scenario", "State", "Net PnL", "Return %", "Fees", "Fills", "Max drawdown",
               "vs buy and hold", "vs cash")  # fmt: skip
    lines = [_row(*heading), _row(*["---"] * len(heading))]
    blank = [""] * 7
    for fold in study["folds"]:
        for name, model in fold["models"].items():
            if model["state"] != "completed":
                lines.append(_row(fold["fold"], name, "all", f"{model['state']}: {model['reason']}", *blank))
                continue
            for cell in model["cells"]:
                if cell["state"] != "completed":
                    state = f"{cell['state']}: {cell['reason']}"
                    lines.append(_row(fold["fold"], name, cell["scenario"], state, *blank))
                    continue
                strategy = cell["accounts"]["strategy"]
                pnl, percent, fees = _money(strategy)
                lines.append(
                    _row(
                        fold["fold"],
                        name,
                        cell["scenario"],
                        "completed",
                        pnl,
                        percent,
                        fees,
                        strategy["fills"],
                        strategy["maximum_drawdown"],
                        _versus(cell["deltas"], "buy_and_hold"),
                        _versus(cell["deltas"], "cash"),
                    )
                )
    return lines


def _predictive_rows(study: Mapping[str, Any]) -> list[str]:
    heading = ("Fold", "Model", "Rows scored", "Not scored", "Accuracy", "Balanced accuracy", "Log loss")
    lines = [_row(*heading), _row(*["---"] * len(heading))]
    for fold in study["folds"]:
        for name, model in fold["models"].items():
            predictive = model.get("predictive", {"unavailable": model.get("reason")})
            if "unavailable" in predictive:
                lines.append(
                    _row(fold["fold"], name, f"unavailable: {predictive['unavailable']}", "", "", "", "")
                )
                continue
            rows = predictive["rows"]
            skipped = ", ".join(f"{reason} {count}" for reason, count in rows["not_scored"].items()) or "none"
            scored = f"{rows['scored']} of {rows['scheduled']}"
            metrics = (predictive["accuracy"], predictive["balanced_accuracy"], predictive["log_loss"])
            lines.append(_row(fold["fold"], name, scored, skipped, *metrics))
    return lines


def _total_lines(study: Mapping[str, Any]) -> list[str]:
    lines = []
    for name, scenarios in study["aggregates"]["trading"].items():
        for scenario, aggregate in scenarios.items():
            complete = aggregate["complete"]
            if "unavailable" in complete:
                missing = "; ".join(f"fold {item['fold']}: {item['reason']}" for item in complete["missing"])
                lines.append(f"- {name} {scenario}: complete total unavailable ({missing})")
                if "partial" in aggregate:
                    partial = aggregate["partial"]["accounts"]["strategy"]
                    included = ", ".join(aggregate["folds"]["included"])
                    lines.append(f"  - partial, folds {included} only: net PnL {partial['net_total_pnl']}")
                continue
            strategy, deltas = complete["accounts"]["strategy"], complete["deltas"]
            lines.append(
                f"- {name} {scenario}: net PnL {strategy['net_total_pnl']} on {complete['starting_capital']} "
                f"({strategy['return_percent']} %), fees {strategy['fees_paid']}; "
                f"vs buy and hold {deltas['buy_and_hold']['net_total_pnl']}, "
                f"vs cash {deltas['cash']['net_total_pnl']}"
            )
    return lines


def _study_lines(study: Mapping[str, Any]) -> list[str]:
    lines = [f"## {study['instrument']}", ""]
    if "refused" in study:
        refusal = study["refused"]
        return [*lines, f"Planning refused, `{refusal['reason']}`: {refusal['message']}", ""]
    audit = study["audit"]
    lines += [
        f"Plan {_short(study['plan_id'])}, study {_short(study['study_id'])}: **{study['status']}**; "
        f"independent audit **{audit['verdict']}** ({audit.get('checks_total', '?')} checks).",
        "",
        *_trading_rows(study),
        "",
        *_predictive_rows(study),
        "",
        "Totals over reset accounts, as the study computed them:",
        "",
        *_total_lines(study),
        "",
    ]
    return lines


def render_markdown(report: Mapping[str, Any]) -> str:
    """The report for a reader: every figure as the study wrote it."""
    collection = report["collection"]
    title = f"plan {_short(report['plan_id'])}" if report["plan_id"] else "synthetic fixture, no request plan"
    lines = [
        f"# Recording pilot report: {title}",
        "",
        f"Evidence: **{collection.get('evidence', 'none')}**. {report['limitations']}",
        "",
        "## Recordings",
        "",
        *_recording_rows(report),
        "",
        f"Collection: `{collection.get('collection_id', collection.get('unavailable'))}`",
        "",
    ]
    for study in report["studies"]:
        lines += _study_lines(study)
    return "\n".join(lines) + "\n"
