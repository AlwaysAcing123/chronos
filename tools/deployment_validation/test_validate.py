"""Credential-free regression checks for the deployment validation tool."""

import argparse
import os
import subprocess
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from unittest.mock import patch

import validate


def run_arguments() -> argparse.Namespace:
    return argparse.Namespace(
        i_understand_this_uses_live_iex_market_data=True,
        minutes=1,
        psql="psql",
        server="http://127.0.0.1:8083",
        instrument="AAPL",
        fresh_quote_ms=5000,
        deployment_id="review-model",
        account_id="review-account",
        starting_cash="10000",
        experiment="a" * 64,
        model="logistic",
        threshold="0.5",
        latency_ms="1",
        max_signal_age_ms="5000",
        max_orders=5,
        expected_parameters_sha256=None,
        poll_seconds=0.1,
        quiet_seconds=1,
    )


class RunServer:
    def __init__(self, failure: str) -> None:
        self.failure = failure
        self.requests: list[tuple[str, str]] = []

    def get(self, path: str) -> dict:
        if path == "/api/v1/deployments":
            return {"enabled": True}
        raise validate.ValidationError("status response lost after Start")

    def request(self, method: str, path: str, body: object = None) -> tuple[int, dict]:
        self.requests.append((method, path))
        if path == "/api/v1/deployments":
            return 201, {
                "created": True,
                "deployment": {"state": "stopped", "model": {}},
            }
        if path.endswith("/start"):
            if self.failure == "start":
                raise validate.ValidationError("Start response lost after commit")
            return 200, {"deployment": {"state": "running", "run_generation": "1"}}
        return 200, {"deployment": {"state": "stopped", "active": False}}


class LifecycleTests(unittest.TestCase):
    def assert_cleanup(self, failure: str) -> None:
        server = RunServer(failure)
        with (
            patch.object(validate, "Server", return_value=server),
            patch.object(validate, "preflight", return_value={"safe": True}),
            patch.object(validate, "live_and_fresh", return_value=(True, "fresh")),
            self.assertRaises(validate.ValidationError),
        ):
            validate.run(run_arguments(), {})
        self.assertIn(
            ("POST", "/api/v1/deployments/review-model/stop"), server.requests
        )

    def test_poll_failure_after_start_still_attempts_stop(self) -> None:
        self.assert_cleanup("poll")

    def test_unknown_start_outcome_still_attempts_stop(self) -> None:
        self.assert_cleanup("start")

    def test_interrupt_after_start_still_attempts_stop(self) -> None:
        server = RunServer("poll")
        with (
            patch.object(validate, "Server", return_value=server),
            patch.object(validate, "preflight", return_value={"safe": True}),
            patch.object(validate, "live_and_fresh", return_value=(True, "fresh")),
            patch.object(
                server, "get", side_effect=[{"enabled": True}, KeyboardInterrupt]
            ),
            self.assertRaises(KeyboardInterrupt),
        ):
            validate.run(run_arguments(), {})
        self.assertIn(
            ("POST", "/api/v1/deployments/review-model/stop"), server.requests
        )

    def test_unknown_stop_has_bounded_retries_and_an_explicit_warning(self) -> None:
        with (
            patch.object(
                validate.Server, "request", side_effect=validate.ValidationError("lost")
            ) as request,
            self.assertRaisesRegex(validate.ValidationError, "may still be trading"),
        ):
            validate.stop_validation_deployment(
                validate.Server("http://127.0.0.1:8083"), "review-model"
            )
        self.assertEqual(request.call_count, 3)

    def test_missing_active_flag_does_not_prove_stop(self) -> None:
        with (
            patch.object(
                validate.Server,
                "request",
                return_value=(200, {"deployment": {"state": "stopped"}}),
            ),
            self.assertRaisesRegex(validate.ValidationError, "Stop outcome unknown"),
        ):
            validate.stop_validation_deployment(
                validate.Server("http://127.0.0.1:8083"), "review-model"
            )

    def test_no_fresh_quote_is_inconclusive_and_not_a_pass(self) -> None:
        with (
            patch.object(validate, "Server", return_value=RunServer("poll")),
            patch.object(validate, "preflight", return_value={"safe": True}),
            patch.object(
                validate, "live_and_fresh", return_value=(False, "market closed")
            ),
        ):
            result = validate.run(run_arguments(), {})
        self.assertEqual(result["verdict"], "inconclusive")
        self.assertIs(result.get("passed"), False)

    def test_verdict_reads_older_decisions_instead_of_only_the_newest_page(
        self,
    ) -> None:
        class Server:
            def get(self, path: str) -> dict:
                if "cursor=" not in path:
                    return {
                        "decisions": [{"state": "held", "order": None, "fill": None}],
                        "next_cursor": "x2.review-model.2",
                    }
                return {
                    "decisions": [
                        {
                            "state": "recorded",
                            "order": {"side": "buy"},
                            "fill": {"quantity": "1"},
                        }
                    ],
                    "next_cursor": None,
                }

        rows = validate.all_decisions(Server(), "review-model")
        self.assertEqual(
            validate.verdict({"decisions": rows, "after_stop": {"unchanged": True}}),
            "fill observed",
        )

    def test_quiet_check_detects_resolution_with_unchanged_sequence(self) -> None:
        class Server:
            def __init__(self) -> None:
                self.reads = 0

            def get(self, path: str) -> dict:
                if path.endswith("decisions?limit=1"):
                    self.reads += 1
                    return {
                        "decisions": [
                            {
                                "decision_sequence": "1",
                                "state": "pending" if self.reads == 1 else "recorded",
                            }
                        ]
                    }
                if path.endswith("review-model"):
                    return {
                        "deployment": {
                            "state": "stopped",
                            "active": False,
                            "account_id": "review-account",
                            "run_generation": "1",
                        }
                    }
                return {"account": {"version": "1"}}

        with patch.object(validate.time, "sleep"):
            result = validate.quiet_after_stop(Server(), "review-model", 1)
        self.assertFalse(result["unchanged"])


class PreflightTests(unittest.TestCase):
    def test_replica_only_atomic_guard_is_not_safe_for_normal_writes(self) -> None:
        def query(database: dict, psql: str, sql: str) -> list[list[str]]:
            if "to_regclass('chronos_schema_migrations')" in sql:
                return [["t"]]
            if "SELECT version" in sql:
                return [[str(version)] for version in range(1, 6)]
            if "FROM pg_trigger" in sql:
                return [
                    [name, function, "t", "t", "R"]
                    for name, function in validate.REQUIRED_TRIGGERS.items()
                ]
            if "SELECT proname" in sql:
                return [[function] for function in validate.REQUIRED_TRIGGERS.values()]
            return [["0"]]

        with patch.object(validate, "query", side_effect=query):
            self.assertFalse(validate.preflight({}, "psql")["safe"])


class EvidenceScanTests(unittest.TestCase):
    def scan(
        self,
        credentials_available: bool,
        evidence_exists: bool,
        contains_secret: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            security = root / "security"
            security.write_text(
                "#!/bin/sh\nprintf '%s\\n' 'FAKE-CREDENTIAL-FOR-TESTS-ONLY'\n"
                if credentials_available
                else "#!/bin/sh\nexit 1\n"
            )
            security.chmod(0o700)
            evidence = root / "evidence"
            if evidence_exists:
                evidence.mkdir()
                if contains_secret:
                    (evidence / "log.txt").write_text("FAKE-CREDENTIAL-FOR-TESTS-ONLY")
            return subprocess.run(
                [
                    "bash",
                    str(Path(__file__).with_name("scan_evidence.sh")),
                    str(evidence),
                ],
                env={**os.environ, "PATH": str(root) + ":/usr/bin:/bin"},
                capture_output=True,
                text=True,
                check=False,
            )

    def test_missing_credentials_cannot_produce_a_clean_scan(self) -> None:
        self.assertNotEqual(self.scan(False, True).returncode, 0)

    def test_missing_evidence_directory_cannot_produce_a_clean_scan(self) -> None:
        self.assertNotEqual(self.scan(True, False).returncode, 0)

    def test_completed_clean_scan_returns_zero(self) -> None:
        result = self.scan(True, True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "0")

    def test_match_is_counted_without_printing_a_credential(self) -> None:
        result = self.scan(True, True, True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout.strip(), "1")
        self.assertNotIn(
            "FAKE-CREDENTIAL-FOR-TESTS-ONLY", result.stdout + result.stderr
        )


class AuditTests(unittest.TestCase):
    def audit_fills(
        self,
        fills: list[validate.Fill],
        cash: str,
        realized: str,
        position: str,
        portfolio: dict | None = None,
    ) -> dict:
        deployment = {
            "account_id": "review-account",
            "state": "stopped",
            "timing": {"latency_ms": "1"},
            "model": {"training_cutoff": "2026-09-25T13:30:00.000000000Z"},
            "policy": {"target_quantity": "1"},
        }
        cutoff = validate.nanoseconds(deployment["model"]["training_cutoff"])
        open_cost = 0
        open_quantity = 0
        for fill in fills:
            if fill.side == "buy":
                open_cost += fill.notional
                open_quantity += fill.quantity
            else:
                open_cost -= validate.half_away(
                    open_cost * fill.quantity, open_quantity
                )
                open_quantity -= fill.quantity

        class Server:
            def get(self, path: str) -> dict:
                if "/deployments/" in path:
                    return {"deployment": deployment}
                return {
                    "account": {
                        "cash": cash,
                        "fees_paid": "0",
                        "gross_realized_pnl": realized,
                        "positions": [
                            {
                                "instrument": "AAPL",
                                "quantity": position,
                                "open_cost_basis": str(
                                    Decimal(open_cost) / validate.QUANTITY_SCALE
                                ),
                            }
                        ],
                    }
                }

            def request(self, method: str, path: str) -> tuple[int, dict]:
                if portfolio is not None:
                    return 200, {"portfolio": portfolio}
                return 503, {"error": {"code": "market_data_unavailable"}}

        def query(database: dict, psql: str, sql: str) -> list[list[str]]:
            if "FROM model_deployment_decisions" in sql:
                return [
                    [
                        str(i),
                        "recorded",
                        fill.client_order_id,
                        fill.side,
                        str(fill.quantity),
                        str(i * 2),
                        str(cutoff + i * 10_000_000),
                        str(cutoff + i * 10_000_000 + 1_000_000),
                        str(cutoff + i * 10_000_000 + 5_000_000_000),
                        str(i * 2 + 1),
                        str(cutoff + i * 10_000_000 + 2_000_000),
                        str(cutoff + i * 10_000_000 + 2_000_000),
                        "{1,2,3,4,5}",
                    ]
                    for i, fill in enumerate(fills, 1)
                ]
            if "FROM paper_order_records" in sql:
                return [
                    [
                        fill.client_order_id,
                        fill.side,
                        str(fill.quantity),
                        str(cutoff + i * 10_000_000 + 2_000_000),
                        str(i * 2 + 1),
                        str(fill.price),
                        str(fill.price),
                        "model:review-model",
                    ]
                    for i, fill in enumerate(fills, 1)
                ]
            if "FROM paper_fills" in sql:
                return [[str(value) for value in fill] for fill in fills]
            return [[str(1000 * validate.QUANTITY_SCALE)]]

        arguments = argparse.Namespace(
            server="http://127.0.0.1:8083",
            deployment_id="review-model",
            psql="psql",
            recording=None,
        )
        with (
            patch.object(validate, "Server", return_value=Server()),
            patch.object(validate, "query", side_effect=query),
        ):
            return validate.audit(arguments, {})

    def test_weighted_average_cost_matches_the_actual_account_contract(self) -> None:
        scale = validate.QUANTITY_SCALE
        fills = [
            validate.Fill(
                "buy-1", "buy", 100 * validate.PRICE_SCALE, scale // 4, 0, 25 * scale
            ),
            validate.Fill(
                "buy-2",
                "buy",
                200 * validate.PRICE_SCALE,
                3 * scale // 4,
                0,
                150 * scale,
            ),
            validate.Fill(
                "sell-1", "sell", 300 * validate.PRICE_SCALE, scale // 2, 0, 150 * scale
            ),
        ]
        self.assertTrue(self.audit_fills(fills, "975", "62.5", "0.5")["passed"])

    def test_valid_half_away_rounded_notional_is_not_flagged_as_inexact(self) -> None:
        fills = [
            validate.Fill(
                "buy-1",
                "buy",
                100_000_000_005,
                validate.QUANTITY_SCALE,
                0,
                10_000_000_001,
            )
        ]
        self.assertTrue(self.audit_fills(fills, "899.99999999", "0", "1")["passed"])

    def test_requested_recording_with_missing_observations_fails_the_audit(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.ndjson"
            path.write_text("")
            problems: list[str] = []
            validate.rederive_features(
                path, [{"observation_sequence": "1", "sequence": "1"}], problems
            )
            self.assertTrue(
                problems, "missing observations cannot count as verified features"
            )

    def test_incorrect_equity_is_not_merely_copied_into_a_passing_report(self) -> None:
        portfolio = {
            "equity": "999999",
            "market_value": "0",
            "unrealized_pnl": "0",
            "total_pnl": "0",
            "open_cost_basis": "0",
            "positions": [],
        }
        self.assertFalse(self.audit_fills([], "1000", "0", "0", portfolio)["passed"])

    def test_equity_and_pnl_are_independently_verified_at_the_best_bid(self) -> None:
        fills = [
            validate.Fill(
                "buy-1",
                "buy",
                100 * validate.PRICE_SCALE,
                validate.QUANTITY_SCALE,
                0,
                100 * validate.QUANTITY_SCALE,
            )
        ]
        portfolio = {
            "equity": "1001",
            "market_value": "101",
            "unrealized_pnl": "1",
            "total_pnl": "1",
            "open_cost_basis": "100",
            "positions": [
                {
                    "instrument": "AAPL",
                    "quantity": "1",
                    "mark_price": "101",
                    "market": {"bid": {"price": "101"}},
                }
            ],
        }
        result = self.audit_fills(fills, "900", "0", "1", portfolio)
        self.assertTrue(result["passed"], result["problems"])
        self.assertTrue(result["valuation_verified"])
        self.assertEqual(result["independent"]["equity"], "1001")
        portfolio["unrealized_pnl"] = "2"
        self.assertFalse(self.audit_fills(fills, "900", "0", "1", portfolio)["passed"])

    def test_a_reconciled_ledger_does_not_claim_a_valuation(self) -> None:
        fills = [
            validate.Fill(
                "buy-1",
                "buy",
                100 * validate.PRICE_SCALE,
                validate.QUANTITY_SCALE,
                0,
                100 * validate.QUANTITY_SCALE,
            )
        ]
        result = self.audit_fills(fills, "900", "0", "1")
        self.assertTrue(result["passed"], result["problems"])
        self.assertFalse(result["valuation_verified"])
        self.assertEqual(result["equity"], "unavailable (market_data_unavailable)")


def preflight_query(
    versions: range, streams: list[list[str]], maxima: list[list[str]]
) -> object:
    def query(database: dict, psql: str, sql: str) -> list[list[str]]:
        if "to_regclass('chronos_schema_migrations')" in sql:
            return [["t"]]
        if "SELECT version" in sql:
            return [[str(version)] for version in versions]
        if "FROM pg_trigger" in sql:
            return [
                [name, function, "t", "t", "O"]
                for name, function in validate.REQUIRED_TRIGGERS.items()
            ]
        if "SELECT proname" in sql:
            return [[function] for function in validate.REQUIRED_TRIGGERS.values()]
        if "FROM live_stream_sequences" in sql:
            return streams
        if "AS durable" in sql:
            return maxima
        return [["0"]]

    return query


class LiveStreamPreflightTests(unittest.TestCase):
    def preflight(
        self, versions: range, streams: list[list[str]], maxima: list[list[str]]
    ) -> dict:
        with patch.object(
            validate, "query", side_effect=preflight_query(versions, streams, maxima)
        ):
            return validate.preflight({}, "psql")

    def test_a_database_without_migration_006_is_not_safe(self) -> None:
        report = self.preflight(range(1, 6), [], [])
        self.assertFalse(report["safe"])
        self.assertTrue(any("006" in problem for problem in report["problems"]))

    def test_a_database_without_migration_007_is_not_safe(self) -> None:
        report = self.preflight(range(1, 7), [], [])
        self.assertFalse(report["safe"])
        self.assertTrue(
            any("007_live_stream_writer_fence.sql" in p for p in report["problems"])
        )

    def test_streams_are_reported_and_history_beyond_them_is_unsafe(self) -> None:
        stream = [
            "alpaca-iex-validation",
            "1049663",
            "adopted",
            "1087",
            "",
            "",
            "false",
            "",
        ]
        maxima = [["alpaca-iex-validation", "1100"], ["databento-replay", "121297"]]
        report = self.preflight(range(1, 8), [stream], maxima)
        self.assertTrue(report["safe"], report["problems"])
        self.assertEqual(
            report["live_streams"]["alpaca-iex-validation"]["durable_maximum"], "1100"
        )
        self.assertIsNone(
            report["live_streams"]["alpaca-iex-validation"]["writer_first_sequence"]
        )
        # A replay, or a live stream that needs one explicit adoption.
        self.assertEqual(report["unregistered_streams"], {"databento-replay": "121297"})
        maxima[0][1] = "1049663"
        self.assertFalse(self.preflight(range(1, 8), [stream], maxima)["safe"])


class ContinuityServer:
    """A restarted server: the account holds one share bought on sequence
    1087; the first quote after the restart is `sequence`."""

    def __init__(self, sequence: str, portfolio_status: int) -> None:
        self.sequence = sequence
        self.portfolio_status = portfolio_status

    def get(self, path: str) -> dict:
        if path.startswith("/api/v1/accounts/"):
            return {
                "account": {
                    "market_stream_id": "alpaca-iex-validation",
                    "positions": [{"instrument": "AAPL", "quantity": "1"}],
                }
            }
        return {
            "market_data": {
                "instruments": [
                    {
                        "instrument": "AAPL",
                        "quote": {
                            "stream_id": "alpaca-iex-validation",
                            "ingest_sequence": self.sequence,
                            "receive_timestamp": "2026-10-05T16:33:29.259857708Z",
                            "bid": {"price": "334.24", "quantity": "80"},
                        },
                    }
                ]
            }
        }

    def request(self, method: str, path: str) -> tuple[int, dict]:
        if self.portfolio_status != 200:
            return self.portfolio_status, {
                "error": {
                    "code": "market_data_unavailable",
                    "reason": "position_mark_unavailable",
                    "instruments": [
                        {"instrument": "AAPL", "reason": "incompatible_market_timeline"}
                    ],
                }
            }
        return 200, {
            "portfolio": {
                "equity": "9999.89",
                "positions": [
                    {
                        "instrument": "AAPL",
                        "mark_price": "334.24",
                        "market": {
                            "ingest_sequence": self.sequence,
                            "bid": {"price": "334.24"},
                        },
                    }
                ],
            }
        }


class ContinuityTests(unittest.TestCase):
    def verify(self, sequence: str, portfolio_status: int, fresh: bool = True) -> dict:
        def query(database: dict, psql: str, sql: str) -> list[list[str]]:
            if "FROM paper_execution_states" in sql and "instrument =" in sql:
                return [["alpaca-iex-validation", "1087"]]
            if "FROM paper_fills" in sql and "max(filled_at_ns)" in sql:
                # 2026-10-05T16:21:19.925765166Z, before the restart.
                return [["1791217279925765166"]]
            if "AS durable" in sql:
                return [["alpaca-iex-validation", "1090"]]
            raise AssertionError(sql)

        arguments = argparse.Namespace(
            server="http://127.0.0.1:8083",
            account_id="live-paper-20261005",
            instrument="AAPL",
            psql="psql",
            timeout_seconds=0.05,
            poll_seconds=0.0,
            fresh_quote_ms=5000,
        )
        server = ContinuityServer(sequence, portfolio_status)
        snapshot = server.get("/api/v1/market-data")["market_data"]
        with (
            patch.object(validate, "Server", return_value=server),
            patch.object(validate, "query", side_effect=query),
            patch.object(
                validate,
                "fresh_market",
                return_value=(snapshot, "fresh") if fresh else (None, "market closed"),
            ),
        ):
            return validate.verify_continuity(arguments, {})

    def test_the_first_fresh_quote_must_value_the_open_position(self) -> None:
        result = self.verify("1049663", 200)
        self.assertTrue(result["passed"], result.get("problems"))
        self.assertEqual(result["portfolio"]["mark_sequence"], "1049663")

    def test_the_acceptance_failure_is_reported_with_its_reason(self) -> None:
        # The 2026-10-05 restart: sequence 70 after 1087, and a 503.
        result = self.verify("70", 503)
        self.assertFalse(result["passed"])
        self.assertIn("does not follow", result["verdict"])
        self.assertTrue(
            any("incompatible_market_timeline" in p for p in result["problems"])
        )

    def test_a_quote_inside_old_history_fails_even_if_valued(self) -> None:
        result = self.verify("1089", 200)
        self.assertFalse(result["passed"])
        self.assertIn("durable history", result["problems"][0])

    def test_no_fresh_quote_is_inconclusive_and_not_a_pass(self) -> None:
        result = self.verify("1049663", 200, fresh=False)
        self.assertFalse(result["passed"])
        self.assertTrue(result["verdict"].startswith("inconclusive"))


def market_snapshot(sequence: str | None, first_ingest: str | None = None) -> dict:
    """/api/v1/market-data with one AAPL entry, executable when `sequence` is
    given, and the server's first sequence since start when it reports one."""
    stream: dict = {"market_time": "2026-10-05T16:33:29.259857708Z"}
    if first_ingest is not None:
        stream["first_ingest_sequence"] = first_ingest
    entry: dict = {
        "instrument": "AAPL",
        "execution_quote_available": sequence is not None,
        "unavailable_reason": None if sequence is not None else "source_unavailable",
        "quote": None,
    }
    if sequence is not None:
        entry["quote"] = {
            "stream_id": "alpaca-iex-validation",
            "ingest_sequence": sequence,
            "receive_timestamp": "2026-10-05T16:33:29.259857708Z",
            "bid": {"price": "334.24", "quantity": "80"},
        }
    return {"market_data": {"stream": stream, "instruments": [entry]}}


class MovingServer(ContinuityServer):
    """A live server whose market reads return scripted snapshots in turn,
    as a reset counter catching up or a reconnect would."""

    def __init__(self, snapshots: list[dict], portfolio_status: int = 200) -> None:
        super().__init__("1049663", portfolio_status)
        self.snapshots = snapshots
        self.market_reads = 0

    def get(self, path: str) -> dict:
        if path == "/health":
            return {"market_data_source": {"mode": "live", "state": "live"}}
        if path == "/api/v1/market-data":
            snapshot = self.snapshots[min(self.market_reads, len(self.snapshots) - 1)]
            self.market_reads += 1
            return snapshot
        return super().get(path)


class SnapshotContinuityTests(unittest.TestCase):
    """Codex's review, finding 4: the snapshot that is checked is the one
    that is judged."""

    def verify(self, server: MovingServer) -> dict:
        def query(database: dict, psql: str, sql: str) -> list[list[str]]:
            if "FROM paper_execution_states" in sql and "instrument =" in sql:
                return [["alpaca-iex-validation", "1087"]]
            if "FROM paper_fills" in sql and "max(filled_at_ns)" in sql:
                return [["1791217279925765166"]]
            if "AS durable" in sql:
                return [["alpaca-iex-validation", "1090"]]
            raise AssertionError(sql)

        arguments = argparse.Namespace(
            server="http://127.0.0.1:8083",
            account_id="live-paper-20261005",
            instrument="AAPL",
            psql="psql",
            timeout_seconds=1,
            poll_seconds=0,
            fresh_quote_ms=5000,
        )
        with (
            patch.object(validate, "Server", return_value=server),
            patch.object(validate, "query", side_effect=query),
        ):
            return validate.verify_continuity(arguments, {})

    def test_a_violating_first_fresh_quote_fails_at_once(self) -> None:
        # Sequence 70 after 1087, then the counter "catches up".
        server = MovingServer([market_snapshot("70"), market_snapshot("1049663")])
        result = self.verify(server)
        self.assertFalse(result["passed"])
        self.assertEqual(result["first_fresh_quote"]["ingest_sequence"], "70")
        self.assertIn("does not follow", result["verdict"])
        self.assertEqual(server.market_reads, 1)

    def test_a_quote_gone_before_valuation_is_not_a_pass(self) -> None:
        server = MovingServer(
            [market_snapshot("1049663"), market_snapshot(None)], portfolio_status=503
        )
        result = self.verify(server)
        self.assertFalse(result["passed"])

    def test_the_servers_first_sequence_proves_every_quote_since_start(self) -> None:
        result = self.verify(MovingServer([market_snapshot("1049663", "1049600")]))
        self.assertTrue(result["passed"], result.get("problems"))
        self.assertEqual(result["proof"], "server_first_sequence")

    def test_a_reset_counter_is_caught_even_after_it_caught_up(self) -> None:
        # The process started at 0; by the first poll it has passed 1090.
        result = self.verify(MovingServer([market_snapshot("1049663", "0")]))
        self.assertFalse(result["passed"])
        self.assertIn("first sequence since start", result["verdict"])

    def test_without_server_evidence_the_proof_says_it_was_sampled(self) -> None:
        result = self.verify(MovingServer([market_snapshot("1049663")]))
        self.assertTrue(result["passed"], result.get("problems"))
        self.assertEqual(result["proof"], "first_observed_quote")
        self.assertIn("sampled", result["proof_limitation"])


if __name__ == "__main__":
    unittest.main()
