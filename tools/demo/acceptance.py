"""Real-stack acceptance of the demo workspace (ADR 0025).

    python3.12 tools/demo/acceptance.py --evidence <dir> [--root <dir>]
        [--toolchain <dir>] [--port-base <port>] [--repeat <n>]

It runs the actual supervisor (chronos_demo.py) on a private root and ports,
so PostgreSQL, the trading server, the research service, the inference
worker, and the built web client are all real, and drives the three guided
flows in a browser (apps/web/e2e/demo-acceptance.spec.ts) with no route
mock. Around them it checks, through the services' own APIs:

- exact accounting of every manual and model account from its fills, in
  decimal arithmetic, against the server's cash, positions, and fees;
- a real evaluation job and its published, verified result;
- model ownership, model-source orders, and that Stop keeps the position
  and ends submissions;
- the independent walk-forward and diagnostics auditors;
- the lifecycle: a repeat and a concurrent start, simultaneous first
  starts on a fresh root, a graceful stop (server_stopped), a hard-killed
  trading server reopened as history (server_restarted, the pending
  decision abandoned, no automatic trade, valuation unavailable), a new
  session, an occupied port, a failed child rolled back, a database that
  cannot start or is interrupted while starting, rolled back, and a corrupt
  playback dataset refused before anything starts;
- that listeners, containers, and data outside the workspace are unchanged.

Waits are semantic polls with deadlines, never fixed sleeps standing in for
an outcome. It prints a summary and writes acceptance.json; it exits 1 if
any check failed. Synthetic data only: nothing contacts a provider.
"""

import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from collections.abc import Callable, Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from decimal import ROUND_HALF_UP, Decimal
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]
SUPERVISOR = HERE / "chronos_demo.py"
_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
MONEY = Decimal("0.00000001")


@dataclass
class Report:
    evidence: Path
    checks: list[dict[str, str]] = field(default_factory=list)

    def check(self, phase: str, name: str, passed: bool, detail: object = "") -> bool:
        self.checks.append(
            {
                "check": name,
                "detail": detail if isinstance(detail, str) else json.dumps(detail),
                "phase": phase,
                "result": "pass" if passed else "FAIL",
            }
        )
        print(f"[{'pass' if passed else 'FAIL'}] {phase}: {name}", flush=True)
        self.save()
        return passed

    def save(self) -> None:
        (self.evidence / "acceptance.json").write_text(
            json.dumps({"checks": self.checks}, indent=2) + "\n"
        )

    @property
    def failed(self) -> list[dict[str, str]]:
        return [entry for entry in self.checks if entry["result"] != "pass"]


def until(condition: Callable[[], Any], deadline_s: float, step_s: float = 0.5) -> Any:
    """The first truthy value of `condition` before the deadline, or None."""
    deadline = time.monotonic() + deadline_s
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(step_s)
    return None


def request(
    port: int, method: str, path: str, body: object | None = None
) -> tuple[int, Any]:
    data = None if body is None else json.dumps(body).encode()
    sent = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}", data=data, method=method
    )
    if data is not None:
        sent.add_header("Content-Type", "application/json")
    try:
        with _OPENER.open(sent, timeout=30) as answer:
            return answer.status, json.loads(answer.read() or b"null")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"null")
    except OSError:
        return 0, None


class Stack:
    """One root and toolchain, driven only through the supervisor."""

    def __init__(self, root: Path, toolchain: Path, port_base: int, logs: Path) -> None:
        self.root = root
        self.toolchain = toolchain
        self.port_base = port_base
        self.logs = logs
        self.process: subprocess.Popen[bytes] | None = None
        self.launches = 0

    @property
    def ports(self) -> dict[str, int]:
        base = self.port_base
        return {
            "web": base,
            "trading": base + 1,
            "research": base + 2,
            "postgres": base + 3,
        }

    def command(self, name: str) -> list[str]:
        return [
            sys.executable,
            str(SUPERVISOR),
            name,
            "--root",
            str(self.root),
            "--toolchain",
            str(self.toolchain),
            "--port-base",
            str(self.port_base),
        ]

    def run(self, name: str, timeout_s: float = 600) -> tuple[int, str]:
        """A command run to completion: its exit code and output."""
        completed = subprocess.run(
            self.command(name),
            capture_output=True,
            text=True,
            timeout=timeout_s,
            check=False,
        )
        output = completed.stdout + completed.stderr
        self.launches += 1
        (self.logs / f"{self.launches:02d}-{name}.log").write_text(output)
        return completed.returncode, output

    def start(self) -> None:
        self.launches += 1
        log = open(self.logs / f"{self.launches:02d}-start.log", "wb")  # noqa: SIM115
        self.process = subprocess.Popen(
            self.command("start"),
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )

    def record(self) -> dict[str, Any]:
        try:
            document: dict[str, Any] = json.loads((self.root / "run.json").read_text())
        except (OSError, ValueError):
            return {}
        return document

    def ready(self, deadline_s: float) -> bool:
        assert self.process is not None
        pid = self.process.pid

        def done() -> str | None:
            run = self.record()
            if self.process is not None and self.process.poll() is not None:
                return "exited"
            if run.get("pid") == pid and run.get("state") == "ready":
                return "ready"
            return None

        return bool(until(done, deadline_s) == "ready")

    def stage(self) -> str:
        return str(self.record().get("stage", ""))

    def interrupt(self, deadline_s: float = 180) -> int | None:
        """Ctrl-C, as a person stops the stack; the supervisor's exit code."""
        assert self.process is not None
        self.process.send_signal(signal.SIGINT)
        try:
            return self.process.wait(timeout=deadline_s)
        except subprocess.TimeoutExpired:
            return None

    def wait_exit(self, deadline_s: float) -> int | None:
        assert self.process is not None
        try:
            return self.process.wait(timeout=deadline_s)
        except subprocess.TimeoutExpired:
            return None

    def child(self, name: str) -> dict[str, Any] | None:
        for child in self.record().get("children", []):
            if child.get("name") == name:
                return dict(child)
        return None

    def trading(
        self, method: str, path: str, body: object | None = None
    ) -> tuple[int, Any]:
        return request(self.ports["trading"], method, path, body)

    def research(
        self, method: str, path: str, body: object | None = None
    ) -> tuple[int, Any]:
        return request(self.ports["research"], method, path, body)


def alive(pid: int) -> bool:
    """Whether the process runs: an exited one awaiting its reaper (a zombie)
    does not."""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    state = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.strip()
    return bool(state) and not state.startswith("Z")


def compose_containers(project: str, running_only: bool = False) -> list[str]:
    """The containers of one Compose project, by ID."""
    command = [
        "docker",
        "ps",
        "-q",
        "--filter",
        f"label=com.docker.compose.project={project}",
    ]
    command += ["--filter", "status=running"] if running_only else ["-a"]
    listed = subprocess.run(command, capture_output=True, text=True, check=False)
    return listed.stdout.split()


def workers(artifacts: Path) -> list[int]:
    """The inference workers serving this root's artifacts. Each runs in its
    own process group, not the trading server's."""
    listed = subprocess.run(
        ["ps", "-A", "-ww", "-o", "pid=,command="],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    found = []
    for line in listed.splitlines():
        pid, _, command = line.strip().partition(" ")
        if "inference-worker" in command and str(artifacts) in command:
            found.append(int(pid))
    return found


def group_alive(pgid: int) -> bool:
    """Whether any process of the group remains: signal 0 sends nothing."""
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def command_of(pid: int) -> str:
    completed = subprocess.run(
        ["ps", "-o", "command=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    return completed.stdout.strip()


def notional(price: str, quantity: str) -> Decimal:
    exact = Decimal(price) * Decimal(quantity)
    rounded = exact.copy_abs().quantize(MONEY, rounding=ROUND_HALF_UP)
    return rounded if exact >= 0 else -rounded


def pages(stack: Stack, path: str, key: str) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    cursor: str | None = None
    while True:
        query = "?limit=200" + (f"&cursor={cursor}" if cursor else "")
        status, document = stack.trading("GET", path + query)
        if status != 200:
            raise AssertionError(f"{path}: {status} {document}")
        items += document[key]
        cursor = document.get("next_cursor")
        if not cursor:
            return items


def reconcile(stack: Stack, account_id: str) -> tuple[bool, dict[str, Any]]:
    """The account rebuilt from its own fills, exactly, against the server."""
    status, document = stack.trading("GET", f"/api/v1/accounts/{account_id}")
    if status != 200:
        return False, {"status": status}
    account = document["account"]
    fills = pages(stack, f"/api/v1/accounts/{account_id}/fills", "fills")
    cash = Decimal(account["starting_cash"])
    fees = Decimal(0)
    held: dict[str, Decimal] = {}
    for fill in fills:
        amount = notional(fill["price"], fill["quantity"])
        sign = 1 if fill["side"] == "buy" else -1
        cash -= sign * amount + Decimal(fill["fee"])
        fees += Decimal(fill["fee"])
        held[fill["instrument"]] = held.get(
            fill["instrument"], Decimal(0)
        ) + sign * Decimal(fill["quantity"])
    server = {
        entry["instrument"]: Decimal(entry["quantity"])
        for entry in account["positions"]
    }
    rebuilt = {
        instrument: quantity for instrument, quantity in held.items() if quantity
    }
    detail = {
        "cash": [str(cash), account["cash"]],
        "fees": [str(fees), account["fees_paid"]],
        "fills": len(fills),
        "positions": [
            {k: str(v) for k, v in rebuilt.items()},
            {k: str(v) for k, v in server.items()},
        ],
    }
    passed = (
        cash == Decimal(account["cash"])
        and fees == Decimal(account["fees_paid"])
        and rebuilt == server
    )
    return passed, detail


def held_quantity(stack: Stack, account_id: str, instrument: str = "AAPL") -> Decimal:
    status, document = stack.trading("GET", f"/api/v1/accounts/{account_id}")
    if status != 200:
        return Decimal(0)
    for entry in document["account"]["positions"]:
        if entry["instrument"] == instrument:
            return Decimal(entry["quantity"])
    return Decimal(0)


def deployment(stack: Stack, deployment_id: str) -> dict[str, Any]:
    status, document = stack.trading("GET", f"/api/v1/deployments/{deployment_id}")
    if status != 200:
        raise AssertionError(f"deployment {deployment_id}: {status} {document}")
    return dict(document["deployment"])


def decisions(stack: Stack, deployment_id: str) -> list[dict[str, Any]]:
    return pages(stack, f"/api/v1/deployments/{deployment_id}/decisions", "decisions")


def orders(stack: Stack, account_id: str) -> list[dict[str, Any]]:
    return pages(stack, f"/api/v1/accounts/{account_id}/orders", "orders")


def create_deployment(
    stack: Stack, deployment_id: str, model_version_id: str, latency_ms: str
) -> bool:
    status, _ = stack.trading(
        "POST",
        "/api/v1/deployments",
        {
            "deployment_id": deployment_id,
            "account_id": f"{deployment_id}-acct",
            "starting_cash": "10000",
            "model_version_id": model_version_id,
            "instrument": "AAPL",
            "threshold": "0.5",
            "target_quantity": "1",
            "decision_interval_ms": "1000",
            "latency_ms": latency_ms,
            "max_signal_age_ms": "8000",
            "max_orders_per_run": "200",
            "max_consecutive_rejections": "3",
        },
    )
    if status != 201:
        return False
    started, _ = stack.trading("POST", f"/api/v1/deployments/{deployment_id}/start", {})
    return started == 200


def snapshot() -> dict[str, Any]:
    """Listeners, containers, and data outside the workspace."""
    listeners = subprocess.run(
        ["lsof", "-nP", "-iTCP", "-sTCP:LISTEN", "-Fcn"],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    found: set[str] = set()
    command = ""
    for line in listeners.splitlines():
        if line.startswith("c"):
            command = line[1:]
        elif line.startswith("n"):
            found.add(f"{command} {line[1:]}")
    containers = subprocess.run(
        [
            "docker",
            "ps",
            "-a",
            "--format",
            # State, not Status: a running container's uptime changes.
            '{{.ID}} {{.Names}} {{.Image}} {{.State}} {{.Label "com.docker.compose.project"}}',
        ],
        capture_output=True,
        text=True,
        check=False,
    ).stdout.splitlines()
    pilot = REPOSITORY / "data" / "real-pilot"
    digest = hashlib.sha256()
    for path in sorted(pilot.rglob("*")) if pilot.is_dir() else []:
        if path.is_file():
            digest.update(str(path.relative_to(pilot)).encode() + path.read_bytes())
    return {
        "containers": sorted(
            line for line in containers if "chronos-demo-" not in line
        ),
        "listeners": sorted(found),
        "real_pilot_sha256": digest.hexdigest(),
    }


@contextmanager
def listening(port: int) -> Iterator[socket.socket]:
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", port))
        listener.listen()
        yield listener


def playwright(stack: Stack, evidence: Path, phase: str, stamp: str) -> tuple[int, str]:
    app = stack.toolchain / "web" / "app"
    environment = {
        **{
            name: os.environ[name]
            for name in ("PATH", "HOME", "TMPDIR")
            if name in os.environ
        },
        "CHRONOS_DEMO_URL": f"http://127.0.0.1:{stack.ports['web']}",
        "CHRONOS_DEMO_EVIDENCE": str(evidence / f"browser-{phase}"),
        "CHRONOS_DEMO_EXPECT": phase,
        "CHRONOS_DEMO_STAMP": stamp,
    }
    completed = subprocess.run(
        ["npx", "playwright", "test", "--config", "playwright.demo.config.ts"],
        cwd=app,
        env=environment,
        capture_output=True,
        text=True,
        timeout=1_800,
        check=False,
    )
    (evidence / f"browser-{phase}.log").write_text(completed.stdout + completed.stderr)
    return completed.returncode, completed.stdout[-3000:]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=None)
    parser.add_argument("--toolchain", type=Path, default=None)
    parser.add_argument("--port-base", type=int, default=18710)
    parser.add_argument("--repeat", type=int, default=3)
    arguments = parser.parse_args(argv)
    evidence = arguments.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    root = (arguments.root or evidence / "root").resolve()
    toolchain = (arguments.toolchain or root / "toolchain").resolve()
    logs = evidence / "supervisor"
    logs.mkdir(exist_ok=True)
    report = Report(evidence)
    stack = Stack(root, toolchain, arguments.port_base, logs)
    before = snapshot()
    (evidence / "outside-before.json").write_text(json.dumps(before, indent=2))
    try:
        run_phases(stack, report, evidence, arguments.repeat)
    finally:
        if stack.process is not None and stack.process.poll() is None:
            stack.interrupt()
        after = snapshot()
        (evidence / "outside-after.json").write_text(json.dumps(after, indent=2))
        ours = {str(port) for port in stack.ports.values()}
        foreign = [
            entry
            for entry in after["listeners"]
            if not any(entry.endswith(f":{port}") for port in ours)
        ]
        foreign_before = [
            entry
            for entry in before["listeners"]
            if not any(entry.endswith(f":{port}") for port in ours)
        ]
        report.check(
            "outside",
            "listeners, containers, and data/real-pilot outside the workspace are unchanged",
            foreign == foreign_before
            and after["containers"] == before["containers"]
            and after["real_pilot_sha256"] == before["real_pilot_sha256"],
            {
                "listeners_added": sorted(set(foreign) - set(foreign_before)),
                "listeners_gone": sorted(set(foreign_before) - set(foreign)),
            },
        )
    print(
        f"\n{len(report.checks) - len(report.failed)} passed, {len(report.failed)} failed"
    )
    return 1 if report.failed else 0


def run_phases(stack: Stack, report: Report, evidence: Path, repeat: int) -> None:
    stamp = hashlib.sha256(str(time.time_ns()).encode()).hexdigest()[:8]
    seed: dict[str, Any] = {}
    code: int | None

    # --- A fresh first start ------------------------------------------------
    phase = "first start"
    stack.start()
    ready = stack.ready(3_600)
    report.check(phase, "the stack becomes ready", ready, stack.stage())
    if not ready:
        return
    run = stack.record()
    report.check(
        phase,
        "it opens session 1 in fresh playback mode",
        [run.get("session"), run.get("mode")] == [1, "playback"],
    )
    status, guide = stack.research("GET", "/api/v1/research/demo")
    seed = guide["seed"] if status == 200 else {}
    report.check(
        phase,
        "the guide's data is served and verified",
        status == 200 and guide["session"]["mode"] == "playback",
    )
    status, market = stack.trading("GET", "/api/v1/market-data")
    source = market["market_data"]["source"] if status == 200 else {}
    report.check(
        phase,
        "the replay is synthetic, labelled so, and waits in ready",
        source.get("synthetic") is True
        and source.get("state") == "ready"
        and source.get("events_applied") == "0",
        {
            key: source.get(key)
            for key in ("label", "state", "synthetic", "coverage_label")
        },
    )
    children = run.get("children", [])
    report.check(
        phase,
        "three owned children run, each in its own process group",
        len(children) == 3
        and all(os.getpgid(child["pid"]) == child["pid"] for child in children),
        [child["name"] for child in children],
    )

    # --- A repeat start, while it runs --------------------------------------
    phase = "repeat start"
    for attempt in range(repeat):
        code, output = stack.run("start", 60)
        report.check(
            phase,
            f"a repeat start reports the running stack and starts nothing ({attempt + 1})",
            code == 0
            and "already running" in output
            and stack.record().get("children") == children,
        )
    code, output = stack.run("new-session", 60)
    report.check(
        phase,
        "new-session is refused while the stack runs",
        code == 3 and "Stop it first" in output,
    )

    # --- The three flows in the browser -------------------------------------
    phase = "guided flows"
    code, tail = playwright(stack, evidence, "playback", stamp)
    if not report.check(
        phase,
        "the three flows and the phone layout pass in the browser",
        code == 0,
        tail[-600:],
    ):
        # Every later phase uses the accounts and deployment the flows made.
        return
    manual, model = f"demo-manual-{stamp}", f"demo-model-{stamp}"
    passed, detail = reconcile(stack, manual)
    report.check(
        phase, "the manual account reconciles exactly from its fills", passed, detail
    )
    status, account = stack.trading("GET", f"/api/v1/accounts/{manual}")
    report.check(
        phase,
        "the cancelled GTC order released its reservation",
        status == 200
        and account["account"]["reserved_cash"] == "0"
        and account["account"]["working_order_count"] == "0",
        account,
    )
    model_orders = orders(stack, f"{model}-acct") if status == 200 else []
    report.check(
        phase,
        "the model's orders carry its source, in its own account",
        bool(model_orders)
        and all(
            order["source"] == {"kind": "model", "id": model} for order in model_orders
        ),
        len(model_orders),
    )
    passed, detail = reconcile(stack, f"{model}-acct")
    report.check(
        phase, "the model account reconciles exactly from its fills", passed, detail
    )
    stopped = deployment(stack, model)
    held = detail["positions"][1] if isinstance(detail, dict) else {}
    count = len(model_orders)

    def changed() -> bool:
        _, after = reconcile(stack, f"{model}-acct")
        return (
            len(orders(stack, f"{model}-acct")) != count
            or after["positions"][1] != held
        )

    # The replay keeps playing; a stopped model must neither order nor sell.
    quiet = until(changed, 15, 1) is None
    report.check(
        phase,
        "Stop kept the position and ended submissions while the replay played",
        stopped["state"] == "stopped"
        and stopped["state_reason"] == "stop_requested"
        and quiet,
        {"state": stopped["state_reason"], "held_at_stop": held, "orders": count},
    )
    status, refused = stack.trading(
        "POST",
        f"/api/v1/accounts/{model}-acct/orders",
        {
            "client_order_id": f"manual-into-{stamp}",
            "source": {"kind": "manual", "id": "harness"},
            "instrument": "AAPL",
            "side": "sell",
            "type": "market",
            "quantity": "1",
            "time_in_force": "ioc",
        },
    )
    report.check(
        phase,
        "a manual order into the model's account is refused",
        status == 409 and refused["error"]["code"] == "account_owned_by_deployment",
        refused,
    )

    # Stop never sells. The flow above may stop a model holding nothing, so
    # a deployment is stopped the moment it holds a share; the model may sell
    # in between, so up to three are tried rather than assuming the timing.
    versions = seed["artifacts"]["model_versions"]
    holding: tuple[str, Decimal] | None = None
    for attempt in range(3):
        holder = f"demo-hold-{stamp}-{attempt}"
        if not create_deployment(stack, holder, versions["logistic"], "1"):
            break

        def holds(holder: str = holder) -> bool:
            return held_quantity(stack, f"{holder}-acct") > 0

        def ended(holder: str = holder) -> bool:
            return bool(deployment(stack, holder)["state"] == "stopped")

        until(holds, 300, 0.05)
        stack.trading("POST", f"/api/v1/deployments/{holder}/stop", {})
        until(ended, 60, 0.1)
        at_stop = held_quantity(stack, f"{holder}-acct")
        if at_stop > 0:
            holding = (holder, at_stop)
            break
    if report.check(
        phase,
        "a deployment stopped while it holds a share",
        holding is not None,
        holding and {"deployment": holding[0], "held": str(holding[1])},
    ):
        assert holding is not None
        holder, at_stop = holding
        orders_at_stop = len(orders(stack, f"{holder}-acct"))

        def moved() -> bool:
            return (
                held_quantity(stack, f"{holder}-acct") != at_stop
                or len(orders(stack, f"{holder}-acct")) != orders_at_stop
            )

        report.check(
            phase,
            "Stop sold nothing: the share stays, with no new order, while the replay plays",
            until(moved, 15, 1) is None
            and deployment(stack, holder)["state_reason"] == "stop_requested",
            {"held": str(at_stop), "orders": orders_at_stop},
        )

    # --- A real historical evaluation job -----------------------------------
    phase = "historical evaluation"
    status, started = stack.research(
        "POST",
        "/api/v1/research/runs",
        {
            "experiment_id": seed["artifacts"]["experiment_id"],
            "instrument": "AAPL",
            "model": "logistic",
            "window": "validation",
            "configuration": seed["fixed_in_advance"]["configuration"],
        },
    )
    job_id = started["job"]["job_id"] if status in (200, 202) else None

    def finished() -> dict[str, Any] | None:
        _, job = stack.research("GET", f"/api/v1/research/runs/{job_id}")
        state = job["run"]["state"] if job else None
        return job["run"] if state in ("succeeded", "failed", "interrupted") else None

    job = until(finished, 600, 1) if job_id else None
    report.check(
        phase,
        "an evaluation runs as a real job and succeeds",
        job is not None and job["state"] == "succeeded",
        job,
    )
    result_id = ((job or {}).get("result") or {}).get("run_id")
    if result_id:
        status, _ = stack.research("GET", f"/api/v1/research/results/{result_id}")
        report.check(
            phase, "its published result verifies when read", status == 200, status
        )

    # --- Independent audits -------------------------------------------------
    phase = "audits"
    artifacts = stack.root / "artifacts"
    for name, identifier in [
        *(
            ("walk_forward_audit", study)
            for study in seed["artifacts"]["walk_forward_studies"].values()
        ),
        (
            "strategy_diagnostics_audit",
            seed["artifacts"]["strategy_diagnostics"]["diagnostic_id"],
        ),
    ]:
        completed = subprocess.run(
            [
                sys.executable,
                "-I",
                "-S",
                str(REPOSITORY / "tools" / name / "audit.py"),
                str(artifacts),
                identifier,
            ],
            capture_output=True,
            text=True,
            timeout=1_800,
            check=False,
        )
        verdict = (
            json.loads(completed.stdout).get("verdict")
            if completed.stdout.strip()
            else None
        )
        report.check(
            phase,
            f"{name} passes {identifier[:12]}",
            completed.returncode == 0 and verdict == "pass",
            verdict,
        )

    # --- A hard-killed trading server --------------------------------------
    phase = "hard kill"
    killed = f"demo-kill-{stamp}"
    report.check(
        phase,
        "a second deployment starts (3 s latency)",
        create_deployment(stack, killed, versions["logistic"], "3000"),
    )
    pending = until(lambda: deployment(stack, killed)["pending_decision"], 300, 0.1)
    report.check(phase, "it has a pending decision", bool(pending), pending)
    trading = stack.child("trading")
    assert trading is not None
    serving = workers(stack.root / "artifacts")
    os.kill(trading["pid"], signal.SIGKILL)
    code = stack.wait_exit(120)
    report.check(
        phase,
        "the supervisor sees the dead child, stops the rest, and fails",
        code == 1,
        code,
    )
    run = stack.record()
    report.check(
        phase,
        "no stack is left claiming to be ready",
        run.get("state") == "failed"
        and not any(alive(child["pid"]) for child in run.get("children", [])),
        run.get("error"),
    )
    report.check(
        phase,
        "the killed server's process group is gone, and its inference worker, in "
        "a group of its own, exits at the end of its input",
        bool(serving)
        and until(
            lambda: (
                not group_alive(trading["pid"])
                and not any(alive(pid) for pid in serving)
            ),
            30,
        )
        is not None,
        {"server": trading["pid"], "workers": serving},
    )

    phase = "retained session"
    stack.start()
    if not report.check(
        phase,
        "the same session reopens",
        stack.ready(600) and stack.record().get("mode") == "history",
        stack.record().get("error"),
    ):
        return
    status, market = stack.trading("GET", "/api/v1/market-data")
    report.check(
        phase,
        "it has no replay source",
        status == 200 and market["market_data"]["source"]["mode"] == "none",
    )
    after = deployment(stack, killed)
    report.check(
        phase,
        "the killed deployment recovered stopped (server_restarted)",
        after["state"] == "stopped" and after["state_reason"] == "server_restarted",
        after["state_reason"],
    )
    abandoned = (
        [
            entry
            for entry in decisions(stack, killed)
            # pending_decision is that decision's sequence.
            if entry.get("decision_sequence") == pending
        ]
        if pending
        else []
    )
    report.check(
        phase,
        "its pending decision was abandoned, never resubmitted",
        bool(abandoned)
        and abandoned[0]["state"] == "abandoned"
        and abandoned[0]["resolution"] == "server_restarted",
        abandoned[:1],
    )
    counts = {
        account: len(orders(stack, account))
        for account in (manual, f"{model}-acct", f"{killed}-acct")
    }
    report.check(
        phase,
        "nothing trades on its own",
        until(
            lambda: (
                {account: len(orders(stack, account)) for account in counts} != counts
            ),
            15,
            1,
        )
        is None,
        counts,
    )
    status, valuation = stack.trading("GET", f"/api/v1/accounts/{manual}/portfolio")
    report.check(
        phase,
        "valuation is unavailable without quotes, never zero",
        status != 200 and "error" in (valuation or {}),
        valuation,
    )
    code, tail = playwright(stack, evidence, "history", stamp)
    report.check(
        phase,
        "the guide and trading say history in the browser",
        code == 0,
        tail[-400:],
    )

    # --- A graceful stop ----------------------------------------------------
    phase = "graceful stop"
    code = stack.interrupt()
    run = stack.record()
    report.check(
        phase,
        "Ctrl-C stops the stack in order and exits 0",
        code == 0 and run.get("state") == "stopped",
        code,
    )
    code, output = stack.run("status", 60)
    report.check(
        phase,
        "status reports it stopped, without secrets",
        code == 0
        and "supervisor: not running" in output
        and "postgresql://" not in output,
        output,
    )

    # --- A new session, stopped gracefully mid-deployment ------------------
    phase = "new session"
    code, output = stack.run("new-session", 600)
    report.check(
        phase,
        "new-session creates session 2 and keeps session 1",
        code == 0 and "Session 2 is ready" in output,
        output,
    )
    stack.start()
    report.check(
        phase,
        "session 2 starts fresh",
        stack.ready(600) and stack.record().get("mode") == "playback",
    )
    stack.trading("POST", "/api/v1/market-data/replay/start", {})
    graceful = f"demo-grace-{stamp}"
    report.check(
        phase,
        "a deployment starts in session 2",
        create_deployment(stack, graceful, versions["logistic"], "1"),
    )
    status, _ = stack.trading("GET", f"/api/v1/accounts/{manual}")
    report.check(
        phase,
        "session 1's accounts are not in session 2's database",
        status == 404,
        status,
    )
    until(lambda: decisions(stack, graceful), 120, 0.5)
    code = stack.interrupt()
    report.check(
        phase, "a graceful stop exits 0 with the deployment running", code == 0, code
    )
    stack.start()
    stack.ready(600)
    stopped = deployment(stack, graceful)
    report.check(
        phase,
        "the deployment kept the server's stop reason (server_stopped)",
        stopped["state"] == "stopped" and stopped["state_reason"] == "server_stopped",
        stopped["state_reason"],
    )

    # --- Concurrent starts --------------------------------------------------
    phase = "concurrent start"
    stack.interrupt()
    for attempt in range(repeat):
        stack.start()
        assert stack.process is not None
        first = stack.process.pid

        def starting(first: int = first) -> bool:
            run = stack.record()
            return bool(run.get("state") == "starting" and run.get("pid") == first)

        busy = until(starting, 60, 0.05)
        code, output = stack.run("start", 120)
        report.check(
            phase,
            f"a start during another's startup is refused, starting nothing ({attempt + 1})",
            bool(busy)
            and code in (0, 3)
            and ("in progress" in output or "already running" in output),
            output[-200:],
        )
        report.check(
            phase,
            f"the first start becomes ready alone ({attempt + 1})",
            stack.ready(600) and len(stack.record().get("children", [])) == 3,
        )
        stack.interrupt()

    # --- Simultaneous first starts on a fresh root ----------------------------
    # Two starts with different ports race for a root that does not exist
    # yet. Only the one holding the root lock creates its record; the other
    # reports it and changes nothing. The winner is stopped before it reaches
    # the database, so these roots touch nothing outside themselves.
    phase = "concurrent first start"
    for attempt in range(repeat):
        race_root = stack.root.parent / f"race-root-{attempt + 1}"
        racers = []
        for base in (stack.port_base + 20, stack.port_base + 30):
            logs = stack.logs / f"race-{attempt + 1}-{base}"
            logs.mkdir(exist_ok=True)
            racers.append(Stack(race_root, stack.toolchain, base, logs))
        for racer in racers:
            racer.start()

        def settled(racers: list[Stack] = racers) -> list[Stack] | None:
            done = [
                r
                for r in racers
                if r.process is not None and r.process.poll() is not None
            ]
            return done or None

        done = until(settled, 120, 0.05) or []
        winners = [racer for racer in racers if racer not in done]
        loser = done[0] if len(done) == 1 else None
        output = (
            (loser.logs / "01-start.log").read_text(errors="replace") if loser else ""
        )
        report.check(
            phase,
            f"one start takes the fresh root, the other reports it and starts nothing ({attempt + 1})",
            len(winners) == 1
            and loser is not None
            and loser.process is not None
            and (
                (loser.process.returncode == 3 and "in progress" in output)
                or (loser.process.returncode == 1 and "only to a new root" in output)
            ),
            {"exited": [r.port_base for r in done], "loser_output": output[-300:]},
        )
        if len(winners) != 1:
            for racer in winners:
                racer.interrupt()
            continue
        winner = winners[0]
        assert winner.process is not None
        pid = winner.process.pid
        record = json.loads((race_root / "workspace.json").read_text())
        report.check(
            phase,
            f"the root's record has the winner's ports, never the loser's ({attempt + 1})",
            record["ports"]["web"] == winner.port_base,
            {"winner": winner.port_base, "recorded": record["ports"]},
        )

        def recording(winner: Stack = winner, pid: int = pid) -> bool:
            return winner.record().get("pid") == pid

        # Its own run record means its Ctrl-C handler is in place.
        until(recording, 60, 0.05)
        code = winner.interrupt()
        report.check(
            phase,
            f"the winner stops on Ctrl-C, with no database started ({attempt + 1})",
            code == 130 and not compose_containers(record["compose_project"]),
            {"exit": code, "stage": winner.stage()},
        )

    # --- A hard-killed supervisor ---------------------------------------------
    # SIGKILL runs no cleanup: the children survive, and the next start must
    # refuse them rather than kill them. The person then stops them.
    phase = "supervisor killed"
    stack.start()
    report.check(phase, "the stack is ready again", stack.ready(600))
    survivors = [dict(child) for child in stack.record().get("children", [])]
    assert stack.process is not None
    os.kill(stack.process.pid, signal.SIGKILL)
    stack.process.wait()
    report.check(
        phase,
        "its children outlive it",
        bool(survivors) and all(alive(child["pid"]) for child in survivors),
        [child["name"] for child in survivors],
    )
    code, output = stack.run("start", 120)
    report.check(
        phase,
        "the next start refuses, naming the processes, and kills none",
        code == 4
        and all(str(child["pid"]) in output for child in survivors)
        and all(alive(child["pid"]) for child in survivors),
        output[-400:],
    )
    for child in survivors:
        if alive(child["pid"]) and Path(child["command"]).name in command_of(
            child["pid"]
        ):
            os.killpg(child["pid"], signal.SIGTERM)
    report.check(
        phase,
        "once the person stops them, a start goes on",
        until(lambda: not any(alive(child["pid"]) for child in survivors), 60)
        is not None,
    )
    stack.start()
    report.check(phase, "the stack starts after them", stack.ready(600))
    stack.interrupt()

    # --- An occupied port ---------------------------------------------------
    phase = "occupied port"
    with listening(stack.ports["web"]):
        code, output = stack.run("start", 300)
        report.check(
            phase,
            "a start refuses a port another program holds, and takes nothing over",
            code == 5
            and f"Port {stack.ports['web']} (web) is already in use" in output,
            output[-300:],
        )
    report.check(phase, "it left no children", not stack.record().get("children"))

    # --- A child that fails during startup ----------------------------------
    phase = "failed child"
    code, _ = stack.run("new-session", 600)
    stack.start()
    reached = until(lambda: "paper-trading server" in stack.stage(), 600, 0.05)
    with listening(stack.ports["web"]):
        code = stack.wait_exit(300)
    run = stack.record()
    report.check(
        phase,
        "a child that cannot start fails the startup",
        bool(reached) and code == 1,
        run.get("error"),
    )
    report.check(
        phase,
        "everything it had started is stopped, and nothing claims to be ready",
        run.get("state") == "failed"
        and not any(alive(child["pid"]) for child in run.get("children", [])),
        [child["name"] for child in run.get("children", [])],
    )

    # --- A database that cannot start, or is interrupted while starting --------
    # Compose can start (or create) the container and still fail, so the
    # start is responsible for it from before `up`. First its port is taken
    # after the start's port check, so Compose cannot start it. Then the start
    # is interrupted as soon as the container runs, during or just after its
    # health wait. Either way the start must fail and leave none of this
    # root's containers running, with its volume kept.
    phase = "database start failure"
    database_log = stack.root / "logs" / "database.log"
    project = json.loads((stack.root / "workspace.json").read_text())["compose_project"]

    def compose_since(logged: int) -> list[str]:
        """The Compose commands logged since line `logged`, without the project."""
        return [
            line.split(f"-p {project} ", 1)[-1]
            for line in database_log.read_text().splitlines()[logged:]
            if line.startswith("$ docker compose")
        ]

    def stopped_after_up(commands: list[str]) -> bool:
        ups = [index for index, line in enumerate(commands) if line.startswith("up ")]
        return bool(ups) and any(
            line.startswith("stop ") for line in commands[ups[-1] + 1 :]
        )

    logged = len(database_log.read_text().splitlines())
    stack.start()
    past_check = until(lambda: "Building" in stack.stage(), 600, 0.05)
    with listening(stack.ports["postgres"]):
        code = stack.wait_exit(900)
    run = stack.record()
    commands = compose_since(logged)
    report.check(
        phase,
        "with its port taken, the start fails on the database and claims nothing",
        bool(past_check)
        and code == 1
        and run.get("state") == "failed"
        and "did not become healthy" in str(run.get("error"))
        and not run.get("children"),
        run.get("error"),
    )
    report.check(
        phase,
        "it stops what it asked Compose to start, and none of this root's runs",
        stopped_after_up(commands)
        and bool(compose_containers(project))
        and not compose_containers(project, running_only=True)
        and run.get("cleanup_problems") == [],
        {"compose": commands, "cleanup_problems": run.get("cleanup_problems")},
    )
    logged = len(database_log.read_text().splitlines())
    stack.start()
    reached = until(lambda: "demo PostgreSQL" in stack.stage(), 900, 0.05)
    running = until(lambda: compose_containers(project, running_only=True), 120, 0.05)
    code = stack.interrupt()
    run = stack.record()
    commands = compose_since(logged)
    healthy_before = "Healthy" in database_log.read_text().split("up --detach")[-1]
    report.check(
        phase,
        "interrupted once its container runs, the start stops it",
        bool(reached)
        and bool(running)
        and code == 130
        and stopped_after_up(commands)
        and not compose_containers(project, running_only=True)
        and run.get("cleanup_problems") == [],
        {
            "compose": commands,
            "health_wait_had_finished": healthy_before,
            "cleanup_problems": run.get("cleanup_problems"),
        },
    )

    # --- A corrupt playback dataset -----------------------------------------
    phase = "corruption"
    events = (
        stack.root
        / "artifacts"
        / "normalized"
        / seed["playback"]["dataset_id"]
        / "events.ndjson"
    )
    original = events.read_bytes()
    events.chmod(0o644)
    events.write_bytes(original.replace(b'"quantity_raw":"1', b'"quantity_raw":"2', 1))
    session_before = json.loads((stack.root / "sessions" / "current.json").read_text())
    try:
        code, output = stack.run("start", 900)
        report.check(
            phase,
            "a corrupt playback dataset stops the start before any service",
            code == 1 and "verifying the seed failed" in output,
            output[-300:],
        )
        report.check(
            phase,
            "no child started and the session is untouched",
            not stack.record().get("children")
            and json.loads((stack.root / "sessions" / "current.json").read_text())
            == session_before,
        )
    finally:
        events.write_bytes(original)
        events.chmod(0o444)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
