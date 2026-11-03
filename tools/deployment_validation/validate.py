#!/usr/bin/env python3
"""Validate a model deployment against a running paper-trading server.

Simulated trading only: the server never sends an order anywhere. This tool
reads the server's API and, read-only, its database, and writes sanitized
evidence. It holds no credential and never asks for one.

    preflight       Is this database safe for deployments? Read-only.
    run             An opt-in, bounded live session: refuses unless the
                    source is live and a quote is fresh, creates the
                    deployment stopped, starts it explicitly, observes,
                    stops it, and checks nothing happens after Stop.
    audit           Independent accounting: decisions, orders, fills,
                    timing, positions, and exact cash, fees, and PnL.
    verify-stopped  After a restart: the deployment is stopped and decides
                    nothing while quotes arrive.
    verify-continuity
                    After a restart: the first fresh quote continues the
                    live stream after every durable sequence (ADR 0020),
                    and values an account's open position. A ledger that
                    reconciles is not this: an audit can pass while
                    valuation is unavailable.

Standard library only; Python 3.10 or later, such as python/.venv/bin/python.
See docs/model-deployments.md ("Validating on live data") for the runbook.
"""

import argparse
import datetime
import json
import math
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from decimal import Decimal
from pathlib import Path
from typing import Any, NamedTuple

PRICE_SCALE = 10**9
QUANTITY_SCALE = 10**8  # also money

# The guards migration 005 must have installed. A version-5 row alone is
# not enough: an older, unmerged 005 recorded the version without the
# deferred resolution guard, and rerunning the idempotent file adds nothing.
REQUIRED_TRIGGERS = {
    "paper_order_records_deployment_ownership": "paper_order_respects_deployment_ownership",
    "paper_order_records_deployment_resolution": "paper_deployment_order_is_resolved",
}


# Every durable record of a stream's ingest sequences (ADR 0020), as the
# server's live-stream store reads them
# (adapters/postgresql/src/postgresql_live_stream_sequences.cpp).
DURABLE_REFERENCES = (
    ("paper_execution_states", "market_stream_id", "market_ingest_sequence"),
    ("paper_fills", "market_stream_id", "market_ingest_sequence"),
    (
        "paper_order_records",
        "market_stream_id",
        "greatest(market_ingest_sequence, execution_market_ingest_sequence)",
    ),
    ("paper_orders", "market_stream_id", "accepted_market_ingest_sequence"),
    ("model_deployments", "stream_id", "watermark_ingest_sequence"),
    (
        "model_deployment_decisions",
        "stream_id",
        "greatest(observation_ingest_sequence, execution_ingest_sequence)",
    ),
)


class Fill(NamedTuple):
    client_order_id: str
    side: str
    price: int
    quantity: int
    fee: int
    notional: int


class ValidationError(Exception):
    """A prerequisite or check failed; the message says which."""


# ---------------------------------------------------------------------------
# Access


def database_environment(url: str) -> dict[str, str]:
    """libpq settings for `url`, passed in the environment so no password
    appears in a process's arguments, with every session read-only."""
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in ("postgresql", "postgres"):
        raise ValidationError("the database URL must be postgresql://")
    environment = {
        key: value
        for key, value in {
            "PGHOST": parsed.hostname or "",
            "PGPORT": str(parsed.port or 5432),
            "PGUSER": urllib.parse.unquote(parsed.username or ""),
            "PGPASSWORD": urllib.parse.unquote(parsed.password or ""),
            "PGDATABASE": parsed.path.lstrip("/"),
        }.items()
        if value
    }
    environment["PGOPTIONS"] = "-c default_transaction_read_only=on"
    environment["PGCONNECT_TIMEOUT"] = "5"
    environment["PATH"] = os.environ.get("PATH", "/usr/bin:/bin")
    return environment


def query(database: dict[str, str], psql: str, sql: str) -> list[list[str]]:
    """Rows of a read-only query, as text; NULL is the empty string."""
    result = subprocess.run(
        [psql, "-X", "-q", "-At", "-F", "\x1f", "-v", "ON_ERROR_STOP=1", "-c", sql],
        env=database,
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )
    if result.returncode != 0:
        raise ValidationError(
            "a read-only query failed: " + result.stderr.strip().splitlines()[-1]
        )
    return [line.split("\x1f") for line in result.stdout.splitlines()]


class Server:
    def __init__(self, base: str) -> None:
        parsed = urllib.parse.urlsplit(base)
        if parsed.hostname not in ("127.0.0.1", "localhost", "::1"):
            raise ValidationError("the server must be on this machine (loopback only)")
        self.base = base.rstrip("/")

    def request(self, method: str, path: str, body: Any = None) -> tuple[int, Any]:
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            self.base + path,
            data=data,
            method=method,
            headers={
                "Accept": "application/json",
                **({"Content-Type": "application/json"} if data else {}),
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            try:
                return error.code, json.load(error)
            except (ValueError, OSError) as failure:
                raise ValidationError(
                    f"{method} {path}: invalid error response"
                ) from failure
        except (OSError, ValueError) as failure:
            # A lost POST reply may follow a commit. Do not assume it failed.
            raise ValidationError(
                f"{method} {path}: response unavailable or invalid; outcome unknown"
            ) from failure

    def get(self, path: str) -> Any:
        status, body = self.request("GET", path)
        if status != 200:
            raise ValidationError(
                f"GET {path} returned {status}: {body.get('error', body)}"
            )
        return body


# ---------------------------------------------------------------------------
# preflight


def preflight(database: dict[str, str], psql: str) -> dict[str, Any]:
    exists = (
        query(
            database,
            psql,
            "SELECT to_regclass('chronos_schema_migrations') IS NOT NULL",
        )[0][0]
        == "t"
    )
    versions = (
        [
            row[0]
            for row in query(
                database,
                psql,
                "SELECT version FROM chronos_schema_migrations ORDER BY version",
            )
        ]
        if exists
        else []
    )
    report: dict[str, Any] = {"migrations": versions, "problems": []}
    if versions != [str(version) for version in range(1, 9)]:
        report["problems"].append(
            f"migrations are {','.join(versions) or 'absent'}, not 1 to 8"
        )
    if versions == [str(version) for version in range(1, 6)]:
        report["problems"].append(
            "apply migrations/006_live_stream_sequences.sql: without it a live "
            "stream cannot continue across a restart (ADR 0020)"
        )
    if versions == [str(version) for version in range(1, 7)]:
        report["problems"].append(
            "apply migrations/007_live_stream_writer_fence.sql, with every live "
            "server on the database stopped: without it a current server takes "
            "no live stream, and a deposed writer could not be refused (ADR 0020)"
        )
    if versions == [str(version) for version in range(1, 8)]:
        report["problems"].append(
            "apply migrations/008_registered_model_deployments.sql: without it the "
            "server cannot read or create a deployment"
        )
    if "6" in versions:
        report["live_streams"], report["unregistered_streams"] = live_streams(
            database, psql, report["problems"], "7" in versions
        )
    rows = (
        query(
            database,
            psql,
            "SELECT t.tgname, p.proname, t.tgdeferrable, t.tginitdeferred, t.tgenabled "
            "FROM pg_trigger t JOIN pg_proc p ON p.oid = t.tgfoid "
            "WHERE t.tgrelid = to_regclass('paper_order_records') AND NOT t.tgisinternal",
        )
        if "5" in versions
        else []
    )
    triggers = {
        row[0]: {
            "function": row[1],
            "deferrable": row[2],
            "initially_deferred": row[3],
            "enabled": row[4],
        }
        for row in rows
    }
    report["triggers"] = triggers
    # An early 005 recorded its version without the resolution guard's
    # function as well as its trigger: each is checked on its own.
    functions = (
        {
            row[0]
            for row in query(
                database,
                psql,
                "SELECT proname FROM pg_proc WHERE pg_function_is_visible(oid) AND proname IN ("
                + ", ".join(
                    f"'{sql_text(function)}'" for function in REQUIRED_TRIGGERS.values()
                )
                + ")",
            )
        }
        if "5" in versions
        else set()
    )
    report["functions"] = sorted(functions)
    if "5" in versions:
        for function in REQUIRED_TRIGGERS.values():
            if function not in functions:
                report["problems"].append(f"function {function} is missing")
    for name, function in REQUIRED_TRIGGERS.items():
        found = triggers.get(name)
        if found is None:
            report["problems"].append(f"trigger {name} is missing")
        elif found["function"] != function or found["enabled"] not in ("O", "A"):
            report["problems"].append(f"trigger {name} does not run {function}")
    resolution = triggers.get("paper_order_records_deployment_resolution")
    if resolution is not None and (
        resolution["deferrable"] != "t" or resolution["initially_deferred"] != "t"
    ):
        report["problems"].append("the resolution guard is not deferred to commit")
    if "5" in versions:
        report["deployments"] = int(
            query(database, psql, "SELECT count(*) FROM model_deployments")[0][0]
        )
    report["safe"] = not report["problems"]
    return report


def durable_maxima(database: dict[str, str], psql: str) -> dict[str, int]:
    """The largest sequence each stream's durable rows reference."""
    union = " UNION ALL ".join(
        f"SELECT {stream} AS stream, max({sequence}) AS reference "
        f"FROM {table} GROUP BY {stream}"
        for table, stream, sequence in DURABLE_REFERENCES
    )
    rows = query(
        database,
        psql,
        f"SELECT stream, max(reference)::text FROM ({union}) AS durable "
        "WHERE reference IS NOT NULL GROUP BY stream ORDER BY stream",
    )
    return {row[0]: int(row[1]) for row in rows}


def live_streams(
    database: dict[str, str], psql: str, problems: list[str], generations: bool
) -> tuple[dict[str, Any], dict[str, str]]:
    """Each live stream's reservations, writer lease, and, from migration 007,
    the first sequence of its writer's generation, and every stream with
    durable history but no record: a replay, or a live stream that needs one
    explicit adoption (--adopt-live-stream-history)."""
    rows = query(
        database,
        psql,
        "SELECT stream_id, next_sequence::text, origin, "
        "coalesce(adopted_maximum::text, ''), coalesce(writer_label, ''), "
        "coalesce(to_char(lease_expires_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS.MS\"Z\"'), ''), "
        "coalesce((lease_expires_at > clock_timestamp())::text, 'false'), "
        + ("coalesce(writer_first_sequence::text, '')" if generations else "''")
        + " FROM live_stream_sequences ORDER BY stream_id",
    )
    maxima = durable_maxima(database, psql)
    streams: dict[str, Any] = {}
    for stream, next_sequence, origin, adopted, writer, expires, held, first in rows:
        maximum = maxima.get(stream)
        streams[stream] = {
            "next_sequence": next_sequence,
            "origin": origin,
            "adopted_maximum": adopted or None,
            "writer": writer or None,
            "lease_expires_at": expires or None,
            "lease_held": held == "true",
            "writer_first_sequence": first or None,
            "durable_maximum": None if maximum is None else str(maximum),
        }
        if maximum is not None and maximum >= int(next_sequence):
            problems.append(
                f"live stream {stream} has durable history up to {maximum}, at or "
                f"beyond its next reservable sequence {next_sequence}"
            )
    unregistered = {
        stream: str(maximum)
        for stream, maximum in maxima.items()
        if stream not in streams
    }
    return streams, unregistered


# ---------------------------------------------------------------------------
# run


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def nanoseconds(timestamp: str) -> int:
    """RFC 3339 with nine fractional digits, exactly."""
    whole, fraction = timestamp.rstrip("Z").split(".")
    seconds = (
        datetime.datetime.fromisoformat(whole)
        .replace(tzinfo=datetime.timezone.utc)
        .timestamp()
    )
    return int(seconds) * 10**9 + int(fraction)


def fresh_market(
    server: Server, instrument: str, maximum_age_ns: int
) -> tuple[dict[str, Any] | None, str]:
    """One /api/v1/market-data snapshot in which the instrument's quote is
    executable and fresh, or None and why not. Whatever is judged about that
    quote must come from this snapshot: another read can see a later quote,
    or none."""
    health = server.get("/health")
    source = health.get("market_data_source") or {}
    if source.get("mode") != "live":
        return None, f"the source is {source.get('mode')}, not live"
    if source.get("state") != "live":
        return None, f"the live source is {source.get('state')}"
    market = server.get("/api/v1/market-data")["market_data"]
    entry = next(
        (item for item in market["instruments"] if item["instrument"] == instrument),
        None,
    )
    if entry is None or not entry["execution_quote_available"]:
        reason = None if entry is None else entry["unavailable_reason"]
        return (
            None,
            f"no executable {instrument} quote ({reason}); a closed market or an idle feed is inconclusive",
        )
    age = nanoseconds(market["stream"]["market_time"]) - nanoseconds(
        entry["quote"]["receive_timestamp"]
    )
    if age > maximum_age_ns:
        return (
            None,
            f"the latest {instrument} quote is {age / 1e9:.1f} s old; inconclusive",
        )
    return market, f"live, {instrument} quote {age / 1e6:.0f} ms old"


def live_and_fresh(
    server: Server, instrument: str, maximum_age_ns: int
) -> tuple[bool, str]:
    market, why = fresh_market(server, instrument, maximum_age_ns)
    return market is not None, why


def run(arguments: argparse.Namespace, database: dict[str, str]) -> dict[str, Any]:
    if not arguments.i_understand_this_uses_live_iex_market_data:
        raise ValidationError(
            "live mode needs --i-understand-this-uses-live-iex-market-data"
        )
    if not 1 <= arguments.minutes <= 60:
        raise ValidationError("--minutes must be 1 to 60")
    if (
        not math.isfinite(arguments.poll_seconds)
        or not 0.1 <= arguments.poll_seconds <= 30
    ):
        raise ValidationError("--poll-seconds must be finite and 0.1 to 30")
    if not 1 <= arguments.quiet_seconds <= 60:
        raise ValidationError("--quiet-seconds must be 1 to 60")
    if not 1 <= arguments.max_orders <= 20:
        raise ValidationError("--max-orders must be 1 to 20")
    checks = preflight(database, arguments.psql)
    if not checks["safe"]:
        raise ValidationError(
            "the database is not safe for deployments: " + "; ".join(checks["problems"])
        )
    server = Server(arguments.server)
    evidence: dict[str, Any] = {
        "started_at": utc_now(),
        "preflight": checks,
        "timeline": [],
    }
    listing = server.get("/api/v1/deployments")
    if not listing["enabled"]:
        raise ValidationError("the server was not started with --model-deployments")
    fresh, why = live_and_fresh(
        server, arguments.instrument, arguments.fresh_quote_ms * 10**6
    )
    evidence["source_check"] = why
    if not fresh:
        evidence["verdict"] = "inconclusive"
        evidence["verdict_reason"] = why
        evidence["passed"] = False
        return evidence

    request = {
        "deployment_id": arguments.deployment_id,
        "account_id": arguments.account_id,
        "starting_cash": arguments.starting_cash,
        "experiment_id": arguments.experiment,
        "model": arguments.model,
        "instrument": arguments.instrument,
        "threshold": arguments.threshold,
        "target_quantity": "1",
        "decision_interval_ms": "1000",
        "latency_ms": arguments.latency_ms,
        "max_signal_age_ms": arguments.max_signal_age_ms,
        "max_orders_per_run": str(arguments.max_orders),
        "max_consecutive_rejections": "2",
    }
    status, created = server.request("POST", "/api/v1/deployments", request)
    if status not in (200, 201):
        raise ValidationError(f"create returned {status}: {created.get('error')}")
    if not created.get("created"):
        raise ValidationError(
            "use new deployment/account IDs; this validation never restarts an existing deployment"
        )
    deployment = created["deployment"]
    if deployment["state"] != "stopped":
        raise ValidationError(
            f"a created deployment must be stopped, not {deployment['state']}"
        )
    expected = arguments.expected_parameters_sha256
    if expected and deployment["model"]["parameters_sha256"] != expected:
        raise ValidationError("the verified model differs from the expected parameters")
    evidence["model"] = deployment["model"]
    # Arm cleanup before sending Start: a missing reply is not proof that
    # it did not commit. Exceptions and interrupts also take this path.
    try:
        status, started = server.request(
            "POST", f"/api/v1/deployments/{arguments.deployment_id}/start", {}
        )
        if status != 200:
            raise ValidationError(f"start returned {status}: {started.get('error')}")
        evidence["timeline"].append(
            {
                "at": utc_now(),
                "event": "started",
                "run": started["deployment"]["run_generation"],
            }
        )
        deadline = time.monotonic() + arguments.minutes * 60
        last = None
        while time.monotonic() < deadline:
            current = server.get(f"/api/v1/deployments/{arguments.deployment_id}")[
                "deployment"
            ]
            condition = current.get("condition") or {}
            key = (condition.get("code"), condition.get("reason"), current["state"])
            if key != last:
                evidence["timeline"].append(
                    {"at": utc_now(), "state": current["state"], "condition": condition}
                )
                last = key
            if current["state"] != "running":
                break
            time.sleep(
                min(arguments.poll_seconds, max(0.0, deadline - time.monotonic()))
            )
    finally:
        stop_validation_deployment(server, arguments.deployment_id)
        evidence["timeline"].append({"at": utc_now(), "event": "stop_acknowledged"})
    evidence["after_stop"] = quiet_after_stop(
        server, arguments.deployment_id, arguments.quiet_seconds
    )
    evidence["decisions"] = all_decisions(server, arguments.deployment_id)
    evidence["account"] = server.get(f"/api/v1/accounts/{arguments.account_id}")[
        "account"
    ]
    evidence["verdict"] = verdict(evidence)
    evidence["passed"] = evidence["after_stop"]["unchanged"]
    return evidence


def stop_validation_deployment(server: Server, deployment_id: str) -> None:
    """Idempotent bounded cleanup, including an unknown Start/Stop reply."""
    path = f"/api/v1/deployments/{deployment_id}"
    for _ in range(3):
        try:
            status, reply = server.request("POST", path + "/stop", {})
            deployment = reply.get("deployment", {})
            if (
                status == 200
                and deployment.get("state") in ("stopped", "faulted")
                and deployment.get("active") is False
            ):
                return
        except ValidationError:
            continue
    raise ValidationError(
        f"Stop outcome unknown for {deployment_id}; it may still be trading. "
        "Stop this deployment or stop the isolated validation server before proceeding."
    )


def all_decisions(server: Server, deployment_id: str) -> list[dict[str, Any]]:
    """Read every page after Stop, with a bound and cycle detection.

    A live run decides at most once per second for at most an hour. Older
    fills must not disappear from its verdict behind the newest 200 holds.
    """
    path = f"/api/v1/deployments/{deployment_id}/decisions?limit=200"
    cursor: str | None = None
    seen: set[str] = set()
    rows: list[dict[str, Any]] = []
    for _ in range(100):
        suffix = (
            "" if cursor is None else "&cursor=" + urllib.parse.quote(cursor, safe="")
        )
        page = server.get(path + suffix)
        rows.extend(page["decisions"])
        cursor = page["next_cursor"]
        if cursor is None:
            return rows
        if cursor in seen:
            raise ValidationError(
                "decision pagination repeated a cursor; evidence is incomplete"
            )
        seen.add(cursor)
    raise ValidationError(
        "decision pagination exceeded the bounded review limit; evidence is incomplete"
    )


def quiet_after_stop(
    server: Server, deployment_id: str, seconds: int
) -> dict[str, Any]:
    """No decision or order may appear after an acknowledged Stop."""

    if not 1 <= seconds <= 60:
        raise ValidationError("--quiet-seconds must be 1 to 60")

    def snapshot() -> str:
        deployment = server.get(f"/api/v1/deployments/{deployment_id}")["deployment"]
        rows = server.get(f"/api/v1/deployments/{deployment_id}/decisions?limit=1")[
            "decisions"
        ]
        account = server.get(f"/api/v1/accounts/{deployment['account_id']}")["account"]
        # A pending decision can resolve with no new sequence. Include its
        # state/outcome and the account version, not just the newest ID.
        return json.dumps(
            {
                "state": deployment["state"],
                "active": deployment["active"],
                "run_generation": deployment["run_generation"],
                "pending_decision": deployment.get("pending_decision"),
                "latest_decision": rows,
                "account_version": account["version"],
            },
            sort_keys=True,
        )

    before = snapshot()
    time.sleep(seconds)
    return {"waited_seconds": seconds, "unchanged": before == snapshot()}


def verdict(evidence: dict[str, Any]) -> str:
    decisions = evidence.get("decisions", [])
    orders = [
        row
        for row in decisions
        if row["order"] is not None and row["state"] == "recorded"
    ]
    fills = [row for row in decisions if row.get("fill")]
    if not evidence["after_stop"]["unchanged"]:
        return "failed: activity after Stop"
    if fills:
        return "fill observed"
    if orders:
        return "orders observed, no fill"
    if decisions:
        return "inference observed, no order"
    return "inconclusive: no decision (no eligible quote reached the deployment)"


# ---------------------------------------------------------------------------
# audit


def audit(arguments: argparse.Namespace, database: dict[str, str]) -> dict[str, Any]:
    server = Server(arguments.server)
    deployment = server.get(f"/api/v1/deployments/{arguments.deployment_id}")[
        "deployment"
    ]
    account_id = deployment["account_id"]
    rows = query(
        database,
        arguments.psql,
        "SELECT decision_sequence, state, client_order_id, side, quantity_raw, observation_ingest_sequence, "
        "observation_receive_ns, not_before_receive_ns, expires_at_market_ns, execution_ingest_sequence, "
        "execution_receive_ns, resolved_at_market_ns, inputs_raw "
        f"FROM model_deployment_decisions WHERE deployment_id = '{sql_text(arguments.deployment_id)}' "
        "ORDER BY decision_sequence",
    )
    columns = (
        "sequence",
        "state",
        "client_order_id",
        "side",
        "quantity",
        "observation_sequence",
        "observation_receive",
        "not_before",
        "expires",
        "execution_sequence",
        "execution_receive",
        "resolved_at",
        "inputs",
    )
    decisions = [dict(zip(columns, row, strict=True)) for row in rows]
    orders = {
        row[0]: {
            "side": row[1],
            "quantity": int(row[2]),
            "submitted_at": row[3],
            "sequence": row[4],
            "bid": row[5],
            "ask": row[6],
            "source": row[7],
        }
        for row in query(
            database,
            arguments.psql,
            "SELECT client_order_id, side, quantity_raw, submitted_at_ns, market_ingest_sequence, "
            "market_bid_price_raw, market_ask_price_raw, source_kind || ':' || source_id "
            f"FROM paper_order_records WHERE account_id = '{sql_text(account_id)}'",
        )
    }
    fills = [
        Fill(row[0], row[1], int(row[2]), int(row[3]), int(row[4]), int(row[5]))
        for row in query(
            database,
            arguments.psql,
            "SELECT client_order_id, side, price_raw, quantity_raw, fee_raw, notional_raw FROM paper_fills "
            f"WHERE account_id = '{sql_text(account_id)}' ORDER BY fill_record_id",
        )
    ]
    starting_cash = int(
        query(
            database,
            arguments.psql,
            f"SELECT starting_cash_raw FROM paper_accounts WHERE account_id = '{sql_text(account_id)}'",
        )[0][0]
    )
    latency = int(deployment["timing"]["latency_ms"]) * 10**6
    cutoff = nanoseconds(deployment["model"]["training_cutoff"])
    target = int(Decimal(deployment["policy"]["target_quantity"]) * QUANTITY_SCALE)
    problems: list[str] = []

    recorded = [d for d in decisions if d["state"] == "recorded"]
    if [d for d in decisions if d["state"] == "pending"] and deployment[
        "state"
    ] != "running":
        problems.append("a pending decision outlived its run")
    if set(orders) != {d["client_order_id"] for d in recorded}:
        problems.append("orders and recorded decisions differ")
    for d in decisions:
        if int(d["observation_receive"]) < cutoff:
            problems.append(
                f"decision {d['sequence']} used data from before the training cutoff"
            )
    for d in recorded:
        order = orders.get(d["client_order_id"])
        if order is None:
            continue
        if int(d["execution_sequence"]) <= int(d["observation_sequence"]):
            problems.append(
                f"decision {d['sequence']} executed on its signal's quote or an earlier one"
            )
        if int(d["execution_receive"]) < int(d["observation_receive"]) + latency:
            problems.append(
                f"decision {d['sequence']} executed sooner than the latency"
            )
        if int(d["resolved_at"]) > int(d["expires"]):
            problems.append(
                f"decision {d['sequence']} executed after its signal expired"
            )
        if (
            order["submitted_at"] != d["resolved_at"]
            or order["sequence"] != d["execution_sequence"]
        ):
            problems.append(
                f"decision {d['sequence']} and its order disagree on the execution quote"
            )
        if order["source"] != f"model:{arguments.deployment_id}":
            problems.append(f"order {d['client_order_id']} is not the deployment's")
    position, fees, cash, realized, open_cost = 0, 0, starting_cash, 0, 0
    for fill in fills:
        order = orders[fill.client_order_id]
        expected_price = order["ask"] if fill.side == "buy" else order["bid"]
        if str(fill.price) != expected_price:
            problems.append(
                f"fill of {fill.client_order_id} is not at the opposite best of its execution quote"
            )
        exact = fill.price * fill.quantity
        if half_away(exact, PRICE_SCALE) != fill.notional:
            problems.append(
                f"fill of {fill.client_order_id} has an incorrect rounded notional"
            )
        fees += fill.fee
        if fill.side == "buy":
            cash -= fill.notional + fill.fee
            position += fill.quantity
            open_cost += fill.notional
        else:
            if fill.quantity > position or fill.quantity <= 0:
                problems.append(
                    f"fill of {fill.client_order_id} sells beyond the available position"
                )
                continue
            # Independent implementation of the account's weighted-average
            # open-cost contract, with half-away proportional removal.
            removed = (
                open_cost
                if fill.quantity == position
                else half_away(open_cost * fill.quantity, position)
            )
            cash += fill.notional - fill.fee
            realized += fill.notional - removed
            open_cost -= removed
            position -= fill.quantity
        if not 0 <= position <= target:
            problems.append(
                f"the position left 0 to {target} after {fill.client_order_id}"
            )
    account = server.get(f"/api/v1/accounts/{account_id}")["account"]

    # Money and quantities share one scale.
    def money(raw: int) -> str:
        return str((Decimal(raw) / QUANTITY_SCALE).normalize()) if raw else "0"

    api_position = sum(
        int(Decimal(p["quantity"]) * QUANTITY_SCALE) for p in account["positions"]
    )
    for name, mine, theirs in (
        ("cash", money(cash), account["cash"]),
        ("fees", money(fees), account["fees_paid"]),
        ("realized PnL", money(realized), account["gross_realized_pnl"]),
    ):
        if Decimal(mine) != Decimal(theirs):
            problems.append(f"{name}: independent {mine}, API {theirs}")
    if api_position != position:
        problems.append(
            f"position: independent {money(position)}, API {money(api_position)}"
        )
    api_open_cost = sum(
        Decimal(p.get("open_cost_basis", "0")) for p in account["positions"]
    )
    if api_open_cost != Decimal(money(open_cost)):
        problems.append(
            f"open cost: independent {money(open_cost)}, API {api_open_cost}"
        )
    features = "not re-derived: live triggering quotes are not retained, so only replay evidence permits it"
    if arguments.recording:
        features = rederive_features(Path(arguments.recording), decisions, problems)
    status, portfolio = server.request(
        "GET", f"/api/v1/accounts/{account_id}/portfolio"
    )
    independent_equity: str | None = None
    if status == 200:
        valuation = portfolio["portfolio"]
        marked_positions = valuation["positions"]
        marked_quantity = sum(
            int(Decimal(p["quantity"]) * QUANTITY_SCALE) for p in marked_positions
        )
        if marked_quantity != position:
            problems.append(
                "portfolio position differs from the independently booked position"
            )
        market_value = 0
        for marked in marked_positions:
            quantity = int(Decimal(marked["quantity"]) * QUANTITY_SCALE)
            price = int(Decimal(marked["mark_price"]) * PRICE_SCALE)
            bid = marked["market"]["bid"]["price"]
            if Decimal(marked["mark_price"]) != Decimal(bid):
                problems.append("portfolio mark is not its quoted best bid")
            market_value += half_away(price * quantity, PRICE_SCALE)
        equity = cash + market_value
        independent_equity = money(equity)
        for name, raw in (
            ("open_cost_basis", open_cost),
            ("market_value", market_value),
            ("unrealized_pnl", market_value - open_cost),
            ("equity", equity),
            ("total_pnl", equity - starting_cash),
        ):
            if Decimal(valuation[name]) != Decimal(money(raw)):
                problems.append(
                    f"portfolio {name}: independent {money(raw)}, API {valuation[name]}"
                )
    valuation_problems = [p for p in problems if p.startswith("portfolio")]
    return {
        "deployment": arguments.deployment_id,
        # The ledger reconciling says nothing about restart continuity or
        # valuation; verify-continuity checks those.
        "valuation_verified": status == 200 and not valuation_problems,
        "decisions": len(decisions),
        "recorded": len(recorded),
        "fills": len(fills),
        "independent": {
            "cash": money(cash),
            "fees": money(fees),
            "realized": money(realized),
            "position": str(Decimal(position) / QUANTITY_SCALE),
            "open_cost_basis": money(open_cost),
            "equity": independent_equity,
        },
        "equity": portfolio["portfolio"]["equity"]
        if status == 200
        else f"unavailable ({portfolio['error']['code']})",
        "features": features,
        "problems": problems,
        "passed": not problems,
    }


def sql_text(value: str) -> str:
    if not value.replace("-", "").replace(".", "").replace("_", "").isalnum():
        raise ValidationError("identifiers are letters, digits, '.', '-', and '_'")
    return value


def half_away(numerator: int, denominator: int) -> int:
    quotient, remainder = divmod(abs(numerator), denominator)
    if 2 * remainder >= denominator:
        quotient += 1
    return quotient if (numerator >= 0) == (denominator > 0) else -quotient


def rederive_features(
    recording: Path, decisions: list[dict[str, str]], problems: list[str]
) -> str:
    """Each decision's inputs from the normalized recording's quote."""
    wanted = {int(d["observation_sequence"]): d for d in decisions}
    checked: set[int] = set()
    with recording.open() as lines:
        for line in lines:
            event = json.loads(line)
            if (
                event["type"] != "top_of_book"
                or int(event["ingest_sequence"]) not in wanted
            ):
                continue
            bid, ask = int(event["bid"]["price_raw"]), int(event["ask"]["price_raw"])
            bid_size, ask_size = (
                int(event["bid"]["quantity_raw"]),
                int(event["ask"]["quantity_raw"]),
            )
            expected = [
                ask - bid,
                half_away((bid_size - ask_size) * 10**9, bid_size + ask_size),
                half_away(ask * bid_size + bid * ask_size, bid_size + ask_size)
                - half_away(bid + ask, 2),
                bid_size,
                ask_size,
            ]
            decision = wanted[int(event["ingest_sequence"])]
            if [int(v) for v in decision["inputs"].strip("{}").split(",")] != expected:
                problems.append(
                    f"decision {decision['sequence']}: inputs differ from the recording"
                )
            checked.add(int(event["ingest_sequence"]))
    missing = set(wanted) - checked
    if missing:
        problems.append(
            f"recording is missing {len(missing)} triggering observations; features are not fully verified"
        )
    return f"re-derived from the recording for {len(checked)} of {len(decisions)} decisions"


# ---------------------------------------------------------------------------
# verify-stopped


def verify_stopped(arguments: argparse.Namespace) -> dict[str, Any]:
    server = Server(arguments.server)
    deployment = server.get(f"/api/v1/deployments/{arguments.deployment_id}")[
        "deployment"
    ]
    quiet = quiet_after_stop(server, arguments.deployment_id, arguments.quiet_seconds)
    return {
        "state": deployment["state"],
        "reason": deployment["state_reason"],
        "active": deployment["active"],
        "quiet": quiet,
        "passed": deployment["state"] == "stopped"
        and not deployment["active"]
        and quiet["unchanged"],
    }


# ---------------------------------------------------------------------------
# verify-continuity


def verify_continuity(
    arguments: argparse.Namespace, database: dict[str, str]
) -> dict[str, Any]:
    """After a restart, the first genuinely fresh quote for the account's
    instrument must continue its stream after every durable sequence, and the
    account's open position must be valued on it. No fresh quote is
    inconclusive, never a pass.

    The quote judged is the one in the snapshot found fresh, and the first
    such quote decides: a counter that restarted low and later passed the
    durable history fails, it is not polled until it looks right. When the
    server reports the first sequence it accepted since it started, that is
    checked too, which covers every quote before the one observed; without
    it the result says the proof was sampled."""
    server = Server(arguments.server)
    account_id = sql_text(arguments.account_id)
    instrument = sql_text(arguments.instrument)
    account = server.get(f"/api/v1/accounts/{account_id}")["account"]
    position = next(
        (
            p
            for p in account["positions"]
            if p["instrument"] == instrument and Decimal(p["quantity"]) != 0
        ),
        None,
    )
    if position is None:
        raise ValidationError(
            f"{account_id} holds no {instrument}; continuity needs an open position"
        )
    stream = account.get("market_stream_id")
    executions = query(
        database,
        arguments.psql,
        "SELECT market_stream_id, market_ingest_sequence::text FROM "
        f"paper_execution_states WHERE account_id = '{account_id}' "
        f"AND instrument = '{instrument}'",
    )
    last_fill = query(
        database,
        arguments.psql,
        "SELECT coalesce(max(filled_at_ns)::text, '') FROM paper_fills "
        f"WHERE account_id = '{account_id}'",
    )[0][0]
    if not executions or not last_fill:
        raise ValidationError(f"{account_id} has no durable {instrument} execution")
    execution_sequence = int(executions[0][1])
    stream_maximum = durable_maxima(database, arguments.psql).get(executions[0][0])
    evidence: dict[str, Any] = {
        "account": account_id,
        "instrument": instrument,
        "stream": stream,
        "position": position["quantity"],
        "execution_sequence": str(execution_sequence),
        "stream_durable_maximum": None
        if stream_maximum is None
        else str(stream_maximum),
        "last_fill_ns": last_fill,
    }

    deadline = time.monotonic() + arguments.timeout_seconds
    market: dict[str, Any] | None = None
    first: dict[str, Any] | None = None
    why = "not checked"
    while first is None and time.monotonic() < deadline:
        market, why = fresh_market(
            server, instrument, arguments.fresh_quote_ms * 1_000_000
        )
        if market is not None:
            entry = next(
                item
                for item in market["instruments"]
                if item["instrument"] == instrument
            )
            # Received after the account last traded: not a quote the
            # account could have known before the restart.
            if nanoseconds(entry["quote"]["receive_timestamp"]) > int(last_fill):
                first = entry["quote"]
                break
            why = "the latest quote is not newer than the account's last fill"
        time.sleep(arguments.poll_seconds)
    if first is None or market is None:
        evidence["verdict"] = f"inconclusive: no fresh quote after the restart ({why})"
        evidence["passed"] = False
        return evidence

    sequence = int(first["ingest_sequence"])
    evidence["first_fresh_quote"] = {
        "stream_id": first["stream_id"],
        "ingest_sequence": first["ingest_sequence"],
        "receive_timestamp": first["receive_timestamp"],
        "bid": first["bid"],
    }
    problems: list[str] = []
    if first["stream_id"] != stream:
        problems.append(
            f"the quote is on {first['stream_id']}, the account on {stream}"
        )
    if sequence <= execution_sequence:
        problems.append(
            f"the first fresh quote, sequence {sequence}, does not follow the "
            f"account's execution quote, sequence {execution_sequence}"
        )
    if stream_maximum is not None and sequence <= stream_maximum:
        problems.append(
            f"the first fresh quote, sequence {sequence}, does not follow the "
            f"stream's durable history, up to {stream_maximum}"
        )
    started = (market.get("stream") or {}).get("first_ingest_sequence")
    if started is not None:
        evidence["proof"] = "server_first_sequence"
        evidence["server_first_ingest_sequence"] = started
        evidence["proof_limitation"] = (
            "durable history is read once, when the check begins; a restarted "
            "process that already wrote durable rows fails the check, never "
            "passes it"
        )
        start = int(started)
        if start > sequence:
            problems.append(
                f"the server's first sequence since start, {start}, is after "
                f"the quote it serves, sequence {sequence}"
            )
        if start <= execution_sequence:
            problems.append(
                f"the server's first sequence since start, {start}, does not "
                f"follow the account's execution quote, sequence {execution_sequence}"
            )
        if stream_maximum is not None and start <= stream_maximum:
            problems.append(
                f"the server's first sequence since start, {start}, does not "
                f"follow the stream's durable history, up to {stream_maximum}"
            )
    else:
        evidence["proof"] = "first_observed_quote"
        evidence["proof_limitation"] = (
            "the server does not report its first sequence since start, so "
            "only the first fresh quote this check observed was compared; "
            "quotes the server accepted before it were sampled, not proven"
        )
    status, body = server.request("GET", f"/api/v1/accounts/{account_id}/portfolio")
    if status != 200:
        error = body.get("error", {})
        reasons = "; ".join(
            f"{item.get('instrument')}: {item.get('reason')}"
            for item in error.get("instruments") or []
        )
        evidence["portfolio"] = {
            "status": status,
            "code": error.get("code"),
            "reason": error.get("reason"),
            "instruments": reasons or None,
        }
        problems.append(
            f"the open position could not be valued: {status} {error.get('code')}"
            + (f" ({reasons})" if reasons else "")
        )
    else:
        valuation = body["portfolio"]
        marked = next(
            (p for p in valuation["positions"] if p["instrument"] == instrument),
            None,
        )
        if marked is None:
            problems.append("the portfolio has no mark for the open position")
        else:
            evidence["portfolio"] = {
                "status": status,
                "mark_price": marked["mark_price"],
                "mark_sequence": marked["market"]["ingest_sequence"],
                "equity": valuation["equity"],
            }
            if int(marked["market"]["ingest_sequence"]) < sequence:
                problems.append("the mark is older than the first fresh quote")
            if Decimal(marked["mark_price"]) != Decimal(
                marked["market"]["bid"]["price"]
            ):
                problems.append("the mark is not its quote's best bid")
    evidence["problems"] = problems
    evidence["verdict"] = (
        "continuity verified: the first fresh quote values the open position"
        if not problems
        else "failed: " + problems[0]
    )
    evidence["passed"] = not problems
    return evidence


# ---------------------------------------------------------------------------


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("preflight", "run", "audit", "verify-stopped", "verify-continuity"):
        command = commands.add_parser(name)
        if name != "verify-stopped":
            command.add_argument("--psql", default="psql")
        if name in ("run", "audit", "verify-stopped", "verify-continuity"):
            command.add_argument("--server", required=True)
        if name in ("run", "audit", "verify-stopped"):
            command.add_argument("--deployment-id", required=True)
        if name in ("run", "verify-stopped"):
            command.add_argument("--quiet-seconds", type=int, default=15)
        command.add_argument("--evidence", help="also write the JSON here")
    run_command = commands.choices["run"]
    run_command.add_argument("--account-id", required=True)
    run_command.add_argument("--experiment", required=True)
    run_command.add_argument("--instrument", required=True)
    run_command.add_argument(
        "--model", choices=("logistic", "majority"), default="logistic"
    )
    run_command.add_argument("--threshold", default="0.5")
    run_command.add_argument("--starting-cash", default="10000")
    run_command.add_argument("--latency-ms", default="1")
    run_command.add_argument("--max-signal-age-ms", default="5000")
    run_command.add_argument("--max-orders", type=int, default=5)
    run_command.add_argument("--minutes", type=int, default=10)
    run_command.add_argument("--poll-seconds", type=float, default=2.0)
    run_command.add_argument("--fresh-quote-ms", type=int, default=5000)
    run_command.add_argument("--expected-parameters-sha256")
    run_command.add_argument(
        "--i-understand-this-uses-live-iex-market-data", action="store_true"
    )
    commands.choices["audit"].add_argument(
        "--recording", help="a normalized events.ndjson, for replay evidence"
    )
    continuity = commands.choices["verify-continuity"]
    continuity.add_argument("--account-id", required=True)
    continuity.add_argument("--instrument", default="AAPL")
    continuity.add_argument("--timeout-seconds", type=float, default=120.0)
    continuity.add_argument("--poll-seconds", type=float, default=1.0)
    continuity.add_argument("--fresh-quote-ms", type=int, default=5000)
    arguments = parser.parse_args(argv)
    try:
        url = os.environ.get("CHRONOS_VALIDATION_DATABASE_URL")
        database = database_environment(url) if url else None
        if arguments.command == "verify-stopped":
            result = verify_stopped(arguments)
        elif database is None:
            raise ValidationError(
                "set CHRONOS_VALIDATION_DATABASE_URL to the isolated database"
            )
        elif arguments.command == "preflight":
            result = preflight(database, arguments.psql)
        elif arguments.command == "run":
            result = run(arguments, database)
        elif arguments.command == "verify-continuity":
            result = verify_continuity(arguments, database)
        else:
            result = audit(arguments, database)
    except (ValidationError, subprocess.TimeoutExpired) as error:
        result = {"error": str(error), "passed": False}
    except KeyboardInterrupt:
        result = {
            "error": "interrupted; bounded Stop cleanup was attempted. If Stop was not confirmed, stop the isolated server.",
            "passed": False,
        }
    text = json.dumps(result, indent=2)
    print(text)
    if arguments.evidence:
        Path(arguments.evidence).write_text(text + "\n")
    passed = result.get("passed", result.get("safe", True))
    return 0 if passed else 1


if __name__ == "__main__":
    # SIGTERM should unwind the same cleanup as Ctrl-C, not silently leave
    # a deployment started by this process running on the server.
    def interrupted(signum: int, frame: Any) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    sys.exit(main(sys.argv[1:]))
