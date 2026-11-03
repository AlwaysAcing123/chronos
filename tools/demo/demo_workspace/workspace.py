"""The workspace: one owned root, its sessions, and the foreground stack.

    <root>/workspace.json        ports, Compose project, the verified seed's ID
    <root>/run.json              the supervisor's state, stage, URLs, children
    <root>/cleanup.json          what a stop could not end, until it is resolved
    <root>/supervisor.lock       held for the supervisor's lifetime
    <root>/secrets/              the demo database password, owner-only
    <root>/artifacts/            the seed and every research artifact
    <root>/research/             the research service's job records
    <root>/sessions/NNNN/        session.json and each launch's logs

Lifecycle (ADR 0025): a first `start` creates session 1, fresh and ready to
play. Before a trading process with the replay is started, the session
durably records `playback_launched_at`; from then on every start of that
session is in history mode, with no replay source, so nothing ever replays
from the beginning into accounts that already traded. `new-session`
creates the next session's own database and makes it current; the old one
is kept. Stopping keeps every database, log, and artifact.
"""

import contextlib
import hashlib
import os
import re
import signal
import socket
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from demo_workspace import preflight
from demo_workspace.database import (
    Database,
    create_session_database,
    session_database_name,
    verify_session_database,
)
from demo_workspace.files import (
    Lock,
    WorkspaceError,
    appending,
    private_directory,
    private_tree,
    read_json,
    replacing,
    write_json,
    write_secret,
)
from demo_workspace.processes import OwnedProcess, running
from demo_workspace.services import (
    STOP_TIMEOUTS_S,
    Child,
    ChildSpec,
    http,
    wait_ready,
)
from demo_workspace.toolchain import (
    Toolchain,
    base_environment,
    build_cpp,
    build_python,
    build_web,
    run_step,
)

SCHEMA = {"name": "chronos.demo_workspace", "version": "1"}
RUN_SCHEMA = {"name": "chronos.demo_run", "version": "1"}
SESSION_SCHEMA = {"name": "chronos.demo_session", "version": "1"}
CLEANUP_SCHEMA = {"name": "chronos.demo_cleanup", "version": "1"}
DEFAULT_PORT_BASE = 18510
INSTRUMENTS = ("AAPL", "SPY")
FEE_PER_FILL = "0.01"
# Exit codes beyond 0 (done) and 1 (failed).
EXIT_PREREQUISITE = 2
EXIT_BUSY = 3
EXIT_ORPHANS = 4
EXIT_PORT = 5
# The command did its work, but something it started could not be stopped.
EXIT_CLEANUP = 6
EXIT_INTERRUPTED = 130
_ACTIVE = ("starting", "ready", "stopping")


def _now() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


class _Interrupted(Exception):
    """Ctrl-C or SIGTERM: stop what this supervisor owns, in order."""


def _interrupt(signum: int, frame: Any) -> None:
    raise _Interrupted()


@dataclass(frozen=True)
class Ports:
    web: int
    trading: int
    research: int
    postgres: int

    @classmethod
    def from_base(cls, base: int) -> "Ports":
        return cls(web=base, trading=base + 1, research=base + 2, postgres=base + 3)

    def named(self) -> dict[str, int]:
        return {
            "postgres": self.postgres,
            "research": self.research,
            "trading": self.trading,
            "web": self.web,
        }


def port_in_use(port: int) -> bool:
    """Whether anything accepts or holds 127.0.0.1:port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(1)
        if probe.connect_ex(("127.0.0.1", port)) == 0:
            return True
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        # As the servers bind: a closed server's TIME_WAIT connections do not
        # hold the port, but another socket bound to it does.
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(("127.0.0.1", port))
        except OSError:
            return True
    return False


def _process_command_line(pid: int) -> str | None:
    """The running process's whole command line, or None if there is none
    or it is another user's. (Not its name: Linux cuts that to 15 bytes.)"""
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return None
    completed = subprocess.run(
        ["ps", "-ww", "-o", "args=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    return completed.stdout.strip() or None


class Workspace:
    def __init__(
        self,
        repository: Path,
        root: Path,
        toolchain_root: Path | None,
        port_base: int | None,
        out: Callable[[str], None] = print,
    ) -> None:
        self.repository = repository
        self.root = root.absolute()
        self.out = out
        self._port_base = port_base
        self.toolchain = Toolchain(
            root=(toolchain_root or self.root / "toolchain").absolute(),
            repository=repository,
        )
        self.lock = Lock(self.root / "supervisor.lock")
        self.children: list[Child] = []
        # Set by a start: the database it may have started, and the toolset.
        self._database: Database | None = None
        self._started_database = False
        self._tools: preflight.Toolset | None = None
        self._ready = False

    # --- Records -------------------------------------------------------------

    def _existing_workspace(self) -> dict[str, Any] | None:
        """The root's record, read only, or None if there is none yet."""
        record = read_json(self.root / "workspace.json")
        if record is None:
            return None
        if record.get("schema") != SCHEMA:
            raise WorkspaceError(f"{self.root} is not a {SCHEMA['name']} root")
        ports = Ports(**record["ports"])
        if self._port_base is not None and ports != Ports.from_base(self._port_base):
            raise WorkspaceError(
                f"this root uses ports from {ports.web}; --port-base applies only to a new root"
            )
        return record

    def _workspace(self) -> dict[str, Any]:
        """The root's record, created for a new root. Only the holder of the
        root lock creates or changes it, so a start that loses the lock
        changes nothing."""
        if not self.lock.held():
            raise WorkspaceError(
                "the workspace record is written only under the root lock"
            )
        record = self._existing_workspace()
        if record is None:
            base = self._port_base or DEFAULT_PORT_BASE
            digest = hashlib.sha256(str(self.root.resolve()).encode()).hexdigest()[:12]
            record = {
                "compose_project": f"chronos-demo-{digest}",
                "created_at": _now(),
                "ports": Ports.from_base(base).named(),
                "schema": SCHEMA,
                "seed_id": None,
            }
            write_json(self.root / "workspace.json", record)
        return record

    def _ports(self, record: dict[str, Any]) -> Ports:
        return Ports(**record["ports"])

    def _run(self) -> dict[str, Any] | None:
        return read_json(self.root / "run.json")

    def _set_run(self, **fields: Any) -> None:
        current = self._run() or {}
        if current.get("pid") != os.getpid():
            current = {}
        write_json(
            self.root / "run.json",
            {
                **current,
                **fields,
                "pid": os.getpid(),
                "schema": RUN_SCHEMA,
                "updated_at": _now(),
            },
        )

    def _session_path(self, number: int) -> Path:
        return self.root / "sessions" / f"{number:04d}" / "session.json"

    def _session(self, number: int) -> dict[str, Any]:
        record = read_json(self._session_path(number))
        if record is None or record.get("schema") != SESSION_SCHEMA:
            raise WorkspaceError(f"session {number} has no valid session record")
        return record

    def _current_session(self) -> int | None:
        record = read_json(self.root / "sessions" / "current.json")
        return None if record is None else int(record["number"])

    def _database_of(self, record: dict[str, Any], docker: str, log: Path) -> Database:
        return Database(
            docker=docker,
            project=record["compose_project"],
            port=self._ports(record).postgres,
            password=write_secret(
                private_tree(self.root, "secrets") / "postgres-password"
            ),
            log=log,
        )

    # --- Guards --------------------------------------------------------------

    def _acquire(self) -> bool:
        for _ in range(5):
            if self.lock.acquire():
                return True
            time.sleep(0.1)
        return False

    def _orphans(self) -> list[dict[str, Any]]:
        """Children a supervisor that never finished may have left running."""
        run = self._run()
        if run is None or run.get("state") not in _ACTIVE:
            return []
        alive = []
        for child in run.get("children", []):
            pid = child.get("pid")
            if not isinstance(pid, int):
                continue
            # Children run by absolute path, so a process still running the
            # recorded program shows that path in its command line.
            line = _process_command_line(pid)
            if line is not None and child["command"] in line:
                alive.append({**child, "running": Path(child["command"]).name})
        return alive

    def _refuse_orphans(self) -> int | None:
        orphans = self._orphans()
        run = self._run()
        if not orphans:
            if run is not None and run.get("state") in _ACTIVE:
                self.out(
                    "note: the previous supervisor ended without cleaning up; "
                    "none of its processes is still running"
                )
            return None
        self.out(
            "A previous demo supervisor was terminated without stopping its "
            "processes. These may still be running:"
        )
        for orphan in orphans:
            self.out(
                f"  {orphan['name']}: process {orphan['pid']} ({orphan['running']})"
            )
        self.out(
            "Chronos never kills a process it no longer supervises. Check each "
            "(ps -p <pid> -o command), stop it yourself, then run start again."
        )
        return EXIT_ORPHANS

    def _refuse_ports(self, ports: Ports, database: Database) -> int | None:
        busy = [
            (name, port)
            for name, port in ports.named().items()
            if name != "postgres" and port_in_use(port)
        ]
        if port_in_use(ports.postgres) and not database.running():
            busy.append(("postgres", ports.postgres))
        if not busy:
            return None
        for name, port in busy:
            self.out(f"Port {port} ({name}) is already in use by another program.")
        self.out(
            "Chronos never takes over a port. Free it, or use another root "
            "with --root <dir> --port-base <port>."
        )
        return EXIT_PORT

    # --- Status --------------------------------------------------------------

    def status(self) -> int:
        record = read_json(self.root / "workspace.json")
        if record is None:
            self.out(f"No demo workspace at {self.root} yet; run start.")
            return 0
        ports = self._ports(record)
        running = self.lock.held_elsewhere()
        run = self._run() or {}
        current = self._current_session()
        self.out(f"Workspace {self.root}")
        self.out(f"  supervisor: {'running' if running else 'not running'}")
        state = run.get("state", "never started")
        if not running and state in _ACTIVE:
            state = f"{state} (ended without cleanup)"
        self.out(
            f"  last run: {state}"
            + (f", stage {run['stage']}" if run.get("stage") else "")
        )
        for problem in self._cleanup_problems():
            self.out(
                f"  not cleaned up ({problem['command']}, {problem['at']}): "
                f"{problem['problem']}"
            )
        if current is not None:
            session = self._session(current)
            if (
                running
                and run.get("session") == current
                and run.get("mode") == "playback"
            ):
                mode = "playing its replay; a later start opens it as history"
            elif session["playback"]["launched_at"]:
                mode = "history: its replay was launched once"
            else:
                mode = "fresh playback"
            self.out(
                f"  current session: {current} ({mode}), database {session['database']}"
            )
        if running and run.get("state") == "ready":
            web = http(f"http://127.0.0.1:{ports.web}/")
            trading = http(f"http://127.0.0.1:{ports.trading}/health")
            research = http(f"http://127.0.0.1:{ports.research}/api/v1/research/status")
            self.out(
                f"  web: http://127.0.0.1:{ports.web}/ ({web.status if web else 'no answer'})"
            )
            self.out(f"  trading API: {trading.status if trading else 'no answer'}")
            self.out(
                f"  research service: {research.status if research else 'no answer'}"
            )
        return 0

    # --- Sessions --------------------------------------------------------------

    def _create_session(self, database: Database, seed_id: str | None) -> int:
        sessions = private_tree(self.root, "sessions")
        numbers = [
            int(entry.name)
            for entry in sessions.iterdir()
            if re.fullmatch(r"[0-9]{4}", entry.name)
        ]
        number = max(numbers, default=0) + 1
        name = session_database_name(number)
        private_directory(sessions / f"{number:04d}")
        create_session_database(database, name, self.repository)
        write_json(
            self._session_path(number),
            {
                "created_at": _now(),
                "database": name,
                "migrations": "1-8, applied and verified at creation",
                "number": number,
                "playback": {"dataset_id": None, "launched_at": None, "stream": None},
                "schema": SESSION_SCHEMA,
                "seed_id": seed_id,
            },
        )
        write_json(sessions / "current.json", {"number": number})
        return number

    def new_session(self) -> int:
        private_directory(self.root)
        if not self._acquire():
            self._existing_workspace()
            self.out(
                "The demo stack is running. Stop it first (Ctrl-C in its terminal), "
                "then run new-session."
            )
            return EXIT_BUSY
        try:
            record = self._workspace()
            refused = self._refuse_orphans()
            if refused is not None:
                return refused
            tools, problems = preflight.check()
            if tools is None:
                return self._prerequisites(problems)
            log = private_tree(self.root, "logs") / "database.log"
            database = self._database_of(record, tools.docker, log)
            was_running = database.running()
            refused = (
                None
                if was_running
                else self._refuse_ports(self._ports(record), database)
            )
            if refused is not None:
                return refused
            # Inside the try: Compose may create or start the container and
            # still fail, and a database this command started is stopped.
            # A stop that fails is recorded, and never masks an earlier error.
            unstopped: str | None = None
            try:
                database.up()
                number = self._create_session(database, record.get("seed_id"))
            finally:
                if not was_running:
                    unstopped = self._stop_database(database)
                    self._record_cleanup(
                        "new-session",
                        []
                        if unstopped is None
                        else [self._problem("database", unstopped)],
                        database_stopped=unstopped is None,
                    )
            session = self._session(number)
            self.out(
                f"Session {number} is ready: its own database {session['database']}, "
                "migrations 1 to 8 applied and verified. Earlier sessions are kept."
            )
            if unstopped is not None:
                self.out(
                    "error: the demo PostgreSQL this command started did not stop. "
                    "Check it (docker ps); status shows it until a later stop succeeds."
                )
                return EXIT_CLEANUP
            self.out("Run start to play it.")
            return 0
        finally:
            self.lock.release()

    # --- Start -----------------------------------------------------------------

    def _prerequisites(self, problems: list[preflight.Problem]) -> int:
        self.out("Missing prerequisites (nothing was installed or changed):")
        for problem in problems:
            self.out(f"  - {problem.what}: {problem.fix}")
        return EXIT_PREREQUISITE

    def _report_running(self) -> int:
        run = self._run() or {}
        if run.get("state") == "ready":
            self.out(f"The demo stack is already running: {run['urls']['guide']}")
            return 0
        self.out(
            f"Another start is in progress (stage: {run.get('stage', 'unknown')}); "
            "nothing new was started."
        )
        return EXIT_BUSY

    def start(self) -> int:
        private_directory(self.root)
        if not self._acquire():
            # Read only: the root's records belong to the start that holds it.
            self._existing_workspace()
            return self._report_running()
        try:
            return self._supervise(self._workspace())
        finally:
            self.lock.release()

    def _supervise(self, record: dict[str, Any]) -> int:
        previous = (
            signal.signal(signal.SIGINT, _interrupt),
            signal.signal(signal.SIGTERM, _interrupt),
        )
        code = 1
        try:
            code = self._run_start(record)
        finally:
            # Cleanup finishes even if Ctrl-C is pressed again.
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            unstopped = self._shutdown()
            signal.signal(signal.SIGINT, previous[0])
            signal.signal(signal.SIGTERM, previous[1])
        # A failure keeps its own code; a clean result must be clean.
        if unstopped and code == 0:
            self.out(
                "error: the stack stopped, but not all of it; see the warnings "
                "above. status shows what is left until it is resolved."
            )
            return EXIT_CLEANUP
        return code

    def _run_start(self, record: dict[str, Any]) -> int:
        try:
            refused = self._refuse_orphans()
            if refused is not None:
                return refused
            self._set_run(
                state="starting",
                stage="preflight",
                started_at=_now(),
                children=[],
                error=None,
            )
            return self._start(record)
        except _Interrupted:
            if self._ready:
                return 0
            self.out("Interrupted; stopping what this start began.")
            self._set_run(state="failed", error="interrupted during startup")
            return EXIT_INTERRUPTED
        except WorkspaceError as error:
            self.out(f"error: {error}")
            self._set_run(state="failed", error=str(error))
            return 1
        except Exception as error:  # noqa: BLE001 - still stopped in order, never "ready"
            self.out(f"error: unexpected {type(error).__name__}: {error}")
            self._set_run(state="failed", error=f"unexpected {type(error).__name__}")
            return 1

    def _stage(self, number: int, total: int, text: str) -> None:
        self.out(f"[{number}/{total}] {text}")
        self._set_run(stage=text)

    def _start(self, record: dict[str, Any]) -> int:
        total = 9
        ports = self._ports(record)
        self._stage(1, total, "Checking prerequisites")
        tools, problems = preflight.check()
        if tools is None:
            self._set_run(state="failed", error="missing prerequisites")
            return self._prerequisites(problems)
        self._tools = tools
        logs = private_tree(self.root, "logs")
        database = self._database_of(record, tools.docker, logs / "database.log")
        self._database = database
        refused = self._refuse_ports(ports, database)
        if refused is not None:
            self._set_run(state="failed", error="port in use")
            return refused
        private_directory(self.toolchain.root)
        build_log = private_tree(self.toolchain.root, "logs") / "build.log"
        self._stage(
            2, total, "Building the C++ server and tools (the first time takes minutes)"
        )
        build_cpp(self.toolchain, tools, build_log)
        self._stage(3, total, "Preparing the hash-pinned Python environment")
        build_python(self.toolchain, build_log)
        self._stage(4, total, "Building the web client with the guide")
        build_web(self.toolchain, tools, build_log)
        self._stage(5, total, "Seeding and verifying the synthetic research artifacts")
        seed_id = self._seed(record, logs / "seed.log")
        self._stage(
            6, total, "Starting the demo PostgreSQL and checking the session's database"
        )
        # The workspace's own database: started now, or left running by a
        # supervisor that never finished, it is stopped with this stack. That
        # holds from before `up`, which can create or start the container and
        # still fail (an unhealthy wait, a deadline, an interrupt).
        self._started_database = True
        if not database.running():
            database.up()
        number = self._current_session() or self._create_session(database, seed_id)
        session = self._session(number)
        verify_session_database(database, session["database"])
        mode = "history" if session["playback"]["launched_at"] else "playback"
        launched = datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ")
        launch = private_tree(self.root, "sessions", f"{number:04d}", "logs", launched)
        seed = self._seed_record(seed_id)
        self._stage(7, total, "Starting the research service")
        self._start_research(ports, seed_id, number, mode, launch)
        self._stage(8, total, "Starting the paper-trading server")
        if mode == "playback":
            # Durable before the trading process exists: whatever happens next,
            # this session is never played from the beginning again.
            session["playback"] = {
                "dataset_id": seed["playback"]["dataset_id"],
                "launched_at": _now(),
                "stream": seed["playback"]["stream"],
            }
            write_json(self._session_path(number), session)
        self._start_trading(ports, database, session, seed, mode, launch)
        self._stage(9, total, "Starting the web client")
        self._start_web(ports, launch)
        urls = {
            "guide": f"http://127.0.0.1:{ports.web}/#/demo",
            "trading": f"http://127.0.0.1:{ports.web}/",
        }
        self._set_run(
            state="ready",
            stage="ready",
            session=number,
            mode=mode,
            urls=urls,
            children=[child.record() for child in self.children],
        )
        self.out("")
        self.out("PAPER TRADING ONLY, with invented synthetic prices.")
        self.out(
            f"Session {number}, {'fresh playback' if mode == 'playback' else 'history (no replay)'}."
        )
        self.out(
            f"Open {urls['guide']} and follow the guide. Press Ctrl-C here to stop."
        )
        self._ready = True
        return self._monitor()

    def _seed(self, record: dict[str, Any], log: Path) -> str:
        python = [str(self.toolchain.python), "-m", "chronos_research"]
        roots = [
            "--tools",
            str(self.toolchain.build),
            "--artifacts",
            str(self.root / "artifacts"),
        ]
        output = log.with_suffix(".json")
        cwd = self.repository / "python"
        private_directory(self.root / "artifacts")
        if record.get("seed_id"):
            command = [*python, "check-demo-seed", *roots, "--seed", record["seed_id"]]
            # Its own record, so the first seed's output is kept.
            checked = log.with_name("seed-check.json")
            self._capture(command, cwd, log, checked, "verifying the seed", 600)
            return str(record["seed_id"])
        self._capture(
            [*python, "seed-demo", *roots],
            cwd,
            log,
            output,
            "seeding the research artifacts",
            3_600,
        )
        seeded = read_json(output)
        if seeded is None:
            raise WorkspaceError(f"the seed printed no result; see {log}")
        seed_id = str(seeded["seed"]["seed_id"])
        record["seed_id"] = seed_id
        write_json(self.root / "workspace.json", record)
        return seed_id

    def _capture(
        self,
        command: list[str],
        cwd: Path,
        log: Path,
        output: Path,
        what: str,
        timeout_s: int,
    ) -> None:
        """Runs a research command in its own process group, stopped whole when
        it ends, at the deadline, or on an interrupt. Its stdout (JSON)
        replaces `output` once it has finished; its stderr is appended to
        `log`. Both are opened before the command runs, so an unsafe entry is
        refused first, and neither is written through a link."""
        with appending(log) as errors, replacing(output) as document:
            process = OwnedProcess(
                command, cwd, base_environment(), stdout=document, stderr=errors
            )
            code = run_step(process, what, timeout_s, log)
        if code != 0:
            raise WorkspaceError(f"{what} failed; see {log}")

    def _seed_record(self, seed_id: str) -> dict[str, Any]:
        manifest = read_json(
            self.root / "artifacts" / "demo_seeds" / seed_id / "manifest.json"
        )
        if manifest is None:
            raise WorkspaceError(f"the seed {seed_id} is missing")
        return manifest

    def _spawn(
        self, spec: ChildSpec, probe: Callable[[], str | None], deadline_s: float
    ) -> Child:
        child = Child(spec)
        self.children.append(child)
        child.start()
        self._set_run(children=[entry.record() for entry in self.children])
        wait_ready(child, probe, deadline_s)
        return child

    def _start_research(
        self, ports: Ports, seed_id: str, number: int, mode: str, logs: Path
    ) -> None:
        origin = f"http://127.0.0.1:{ports.research}"
        state = private_directory(self.root / "research")

        def probe() -> str | None:
            status = http(f"{origin}/api/v1/research/status")
            if (
                status is None
                or status.status != 200
                or status.header("X-Chronos-Service") != "research"
            ):
                return "the research service does not answer as itself"
            demo = http(f"{origin}/api/v1/research/demo")
            if (
                demo is None
                or demo.status != 200
                or demo.json()["seed"]["seed_id"] != seed_id
            ):
                return "the research service does not serve this workspace's seed"
            return None

        self._spawn(
            ChildSpec(
                name="research",
                command=[
                    str(self.toolchain.python),
                    "-m",
                    "chronos_research",
                    "serve",
                    "--tools",
                    str(self.toolchain.build),
                    "--artifacts",
                    str(self.root / "artifacts"),
                    "--state",
                    str(state),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(ports.research),
                    "--demo-seed",
                    seed_id,
                    "--demo-session",
                    str(number),
                    "--demo-mode",
                    mode,
                ],
                cwd=self.repository / "python",
                environment=base_environment(),
                log=logs / "research.log",
                announcement=re.compile(
                    re.escape(f'"listening": "{origin}/api/v1/research"')
                ),
            ),
            probe,
            60,
        )

    def _start_trading(
        self,
        ports: Ports,
        database: Database,
        session: dict[str, Any],
        seed: dict[str, Any],
        mode: str,
        logs: Path,
    ) -> None:
        origin = f"http://127.0.0.1:{ports.trading}"
        stream = seed["playback"]["stream"]
        command = [
            str(self.toolchain.server), "--stream-id", stream,
            *[part for instrument in INSTRUMENTS for part in ("--instrument", instrument)],
            "--host", "127.0.0.1", "--port", str(ports.trading), "--fee-per-fill", FEE_PER_FILL,
            "--model-deployments", "--research-python", str(self.toolchain.python),
            "--research-python-root", str(self.repository / "python"),
            "--research-artifacts", str(self.root / "artifacts"),
        ]  # fmt: skip
        if mode == "playback":
            dataset = (
                self.root / "artifacts" / "normalized" / seed["playback"]["dataset_id"]
            )
            command += ["--replay-dataset", str(dataset), "--replay-speed", "1x"]

        def probe() -> str | None:
            health = http(f"{origin}/health")
            if health is None or health.header("X-Chronos-Trading-Mode") != "paper":
                return "the trading API does not answer as a paper server"
            market = http(f"{origin}/api/v1/market-data")
            if market is None or market.status != 200:
                return "the market-data resource does not answer"
            data = market.json()["market_data"]
            source = data["source"]
            if data["stream"]["stream_id"] != stream:
                return f"the server's stream is not {stream}"
            if mode == "playback" and not (
                source["mode"] == "historical_replay"
                and source["synthetic"]
                and source["state"] == "ready"
            ):
                return "the server is not a synthetic replay waiting in ready"
            if mode == "history" and source["mode"] != "none":
                return "a history session must have no replay source"
            return None

        self._spawn(
            ChildSpec(
                name="trading",
                command=command,
                cwd=self.root,
                environment=base_environment(
                    {"CHRONOS_POSTGRES_URL": database.url(session["database"])}
                ),
                log=logs / "trading.log",
                announcement=re.compile(
                    re.escape(f"PAPER TRADING ONLY: listening on {origin} ")
                ),
            ),
            probe,
            180,
        )

    def _start_web(self, ports: Ports, logs: Path) -> None:
        origin = f"http://127.0.0.1:{ports.web}"
        index = (self.toolchain.web_dist / "index.html").read_bytes()

        def probe() -> str | None:
            page = http(f"{origin}/")
            if page is None or page.body != index:
                return "the web server does not serve this toolchain's build"
            api = http(f"{origin}/health")
            research = http(f"{origin}/api/v1/research/status")
            if api is None or api.header("X-Chronos-Trading-Mode") != "paper":
                return "the web proxy does not reach the trading API"
            if research is None or research.header("X-Chronos-Service") != "research":
                return "the web proxy does not reach the research service"
            return None

        vite = self.toolchain.web_app / "node_modules" / "vite" / "bin" / "vite.js"
        self._spawn(
            ChildSpec(
                name="web",
                command=[
                    self._node(),
                    str(vite),
                    "preview",
                    "--outDir",
                    str(self.toolchain.web_dist),
                    "--host",
                    "127.0.0.1",
                    "--port",
                    str(ports.web),
                    "--strictPort",
                ],
                cwd=self.toolchain.web_app,
                environment=base_environment(
                    {
                        "CHRONOS_API_ORIGIN": f"http://127.0.0.1:{ports.trading}",
                        "CHRONOS_RESEARCH_ORIGIN": f"http://127.0.0.1:{ports.research}",
                        "NO_COLOR": "1",
                    }
                ),
                log=logs / "web.log",
                announcement=re.compile(re.escape(f"{origin}/")),
            ),
            probe,
            60,
        )

    def _node(self) -> str:
        assert self._tools is not None
        return self._tools.node

    def _monitor(self) -> int:
        while True:
            for child in self.children:
                if not child.alive():
                    # Whatever it started goes with it, then the rest in order.
                    code = child.stop(STOP_TIMEOUTS_S[child.spec.name])
                    self.out(
                        f"error: {child.spec.name} exited (code {code}); "
                        f"stopping the rest. See {child.spec.log}"
                    )
                    self._set_run(state="failed", error=f"{child.spec.name} exited")
                    return 1
            time.sleep(0.5)

    def _stop_database(self, database: Database) -> str | None:
        """Stops the demo PostgreSQL: None, or why it did not stop."""
        self.out("Stopping the demo PostgreSQL (its data is kept)…")
        try:
            database.stop()
        except WorkspaceError as error:
            self.out(f"warning: {error}")
            return str(error)
        return None

    # --- Cleanup the person must finish ------------------------------------------

    def _problem(
        self, kind: str, problem: str, pids: list[int] | None = None
    ) -> dict[str, Any]:
        return {
            "at": _now(),
            "command": "",
            "kind": kind,
            "pids": pids or [],
            "problem": problem,
        }

    def _cleanup_problems(self) -> list[dict[str, Any]]:
        """What earlier stops could not end and is not yet resolved: a
        database problem until a later stop of it succeeds, processes while
        any of them still runs."""
        record = read_json(self.root / "cleanup.json")
        if record is None or record.get("schema") != CLEANUP_SCHEMA:
            return []
        problems = []
        for problem in record.get("problems", []):
            if problem.get("kind") == "processes":
                left = [pid for pid in problem.get("pids", []) if running(pid)]
                if not left:
                    continue
                problem = {**problem, "pids": left}
            problems.append(problem)
        return problems

    def _record_cleanup(
        self, command: str, new: list[dict[str, Any]], database_stopped: bool
    ) -> None:
        """Adds this command's problems to the unresolved ones, dropping a
        database problem once this command stopped the database."""
        kept = [
            problem
            for problem in self._cleanup_problems()
            if not (database_stopped and problem["kind"] == "database")
        ]
        problems = kept + [{**problem, "command": command} for problem in new]
        path = self.root / "cleanup.json"
        if problems:
            write_json(path, {"problems": problems, "schema": CLEANUP_SCHEMA})
        else:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(path)

    def _shutdown(self) -> list[str]:
        """Stops what this supervisor started, in order: the web client, the
        trading server (which stops its deployments with server_stopped),
        the research service, then the demo database. Data is kept. What
        could not be stopped is reported, recorded, and returned, never
        passed over."""
        run = self._run() or {}
        if run.get("pid") == os.getpid() and run.get("state") not in ("failed",):
            self._set_run(state="stopping")
        problems: list[dict[str, Any]] = []
        for name in ("web", "trading", "research"):
            for child in self.children:
                if child.spec.name == name and child.process is not None:
                    self.out(f"Stopping {name}…")
                    if child.stop(STOP_TIMEOUTS_S[name]) is None or child.survivors():
                        problem = (
                            f"{name}'s processes {child.survivors()} still run after "
                            "SIGKILL"
                        )
                        problems.append(
                            self._problem("processes", problem, child.survivors())
                        )
                        self.out(f"warning: {problem}")
        database_stopped = False
        if self._database is not None and self._started_database:
            unstopped = self._stop_database(self._database)
            if unstopped is not None:
                problems.append(self._problem("database", unstopped))
            database_stopped = unstopped is None
        if problems or database_stopped:
            self._record_cleanup("start", problems, database_stopped)
        run = self._run() or {}
        if run.get("pid") == os.getpid():
            final = "failed" if run.get("state") == "failed" else "stopped"
            self._set_run(
                state=final,
                stopped_at=_now(),
                children=[entry.record() for entry in self.children],
                cleanup_problems=[problem["problem"] for problem in problems],
            )
        self.children = []
        return [problem["problem"] for problem in problems]
