"""The demo supervisor's guards, without Docker or a database: owned files,
the root lock, ports, migration digests, orphaned children, busy refusals,
child readiness and stopping, database rollback, and the environment
children get."""

import contextlib
import http.server
import io
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import unittest
from collections.abc import Callable
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

from demo_workspace import (
    database,
    files,
    preflight,
    processes,
    services,
    toolchain,
    workspace,
)
from demo_workspace.workspace import Workspace

REPOSITORY = Path(__file__).resolve().parents[2]
_HOLD_LOCK = textwrap.dedent(
    """
    import sys, time
    sys.path.insert(0, sys.argv[1])
    from pathlib import Path
    from demo_workspace.files import Lock
    lock = Lock(Path(sys.argv[2]))
    assert lock.acquire()
    print("held", flush=True)
    time.sleep(60)
    """
)


# A first start held where it writes the new root's record, so another start
# can be run against it at that moment. Its prerequisites "fail", so it
# builds and starts nothing.
_PAUSED_FIRST_START = textwrap.dedent(
    """
    import sys
    from pathlib import Path
    from unittest.mock import patch
    sys.path.insert(0, sys.argv[1])
    from demo_workspace import preflight, workspace

    original = workspace.write_json

    def paused(path, record):
        if path.name == "workspace.json":
            print("paused", flush=True)
            sys.stdin.readline()
        original(path, record)

    missing = preflight.Problem("a stand-in prerequisite", "none")
    demo = workspace.Workspace(
        Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), int(sys.argv[5]),
        lambda _: None,
    )
    with (
        patch.object(workspace, "write_json", paused),
        patch.object(preflight, "check", return_value=(None, [missing])),
    ):
        sys.exit(demo.start())
    """
)
# A start that begins on "go", for starts that race one another. It holds
# the lock for a second, in a stand-in prerequisite check, then stops.
_RACING_START = textwrap.dedent(
    """
    import sys, time
    from pathlib import Path
    from unittest.mock import patch
    sys.path.insert(0, sys.argv[1])
    from demo_workspace import preflight, workspace

    def check():
        time.sleep(1)
        return None, [preflight.Problem("a stand-in prerequisite", "none")]

    demo = workspace.Workspace(
        Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4]), int(sys.argv[5])
    )
    sys.stdin.readline()
    with patch.object(preflight, "check", check):
        try:
            code = demo.start()
        except Exception as error:
            print(f"error: {error}", flush=True)
            code = 1
    sys.exit(code)
    """
)


def touching(marker: Path) -> list[str]:
    """A command that leaves `marker` behind if it runs at all."""
    return [sys.executable, "-c", f"open({str(marker)!r}, 'w').close()"]


def running(pid: int) -> bool:
    """Whether the process runs: a killed one awaiting its reaper (a zombie,
    where nothing reaps orphans) does not."""
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


class Holder:
    """Another process holding a root's lock, as a running supervisor does."""

    def __init__(self, root: Path) -> None:
        self.process = subprocess.Popen(
            [
                sys.executable,
                "-c",
                _HOLD_LOCK,
                str(Path(__file__).parent),
                str(root / "supervisor.lock"),
            ],
            stdout=subprocess.PIPE,
            text=True,
        )
        assert self.process.stdout is not None
        assert self.process.stdout.readline().strip() == "held"

    def kill(self) -> None:
        self.process.kill()
        self.process.wait()
        assert self.process.stdout is not None
        self.process.stdout.close()


class Scratch(unittest.TestCase):
    def setUp(self) -> None:
        self.base = Path(tempfile.mkdtemp(prefix="chronos-demo-test-")).resolve()
        self.root = self.base / "root"

    def tearDown(self) -> None:
        shutil.rmtree(self.base, ignore_errors=True)

    def workspace(self, port_base: int | None = 18610) -> workspace.Workspace:
        lines: list[str] = []
        self.output = lines
        return workspace.Workspace(
            REPOSITORY, self.root, self.base / "toolchain", port_base, lines.append
        )

    def write_run(self, **fields: object) -> None:
        files.write_json(
            self.root / "run.json", {"schema": workspace.RUN_SCHEMA, **fields}
        )

    def initialize(self, demo: Workspace) -> dict[str, object]:
        """The root's record, made as a start makes it: under the root lock."""
        files.private_directory(self.root)
        with files.holding(demo.lock) as held:
            assert held
            return demo._workspace()


class OwnedFiles(Scratch):
    def test_a_private_directory_is_real_owned_and_closed(self) -> None:
        made = files.private_directory(self.root)
        self.assertEqual(os.stat(made).st_mode & 0o777, 0o700)
        os.chmod(made, 0o755)
        files.private_directory(made)
        self.assertEqual(os.stat(made).st_mode & 0o777, 0o700)
        link = self.base / "link"
        link.symlink_to(made)
        with self.assertRaisesRegex(files.WorkspaceError, "not a link"):
            files.private_directory(link)

    def test_records_are_replaced_whole_and_links_are_not_followed(self) -> None:
        files.private_directory(self.root)
        record = self.root / "record.json"
        files.write_json(record, {"a": 1})
        files.write_json(record, {"b": 2})
        self.assertEqual(files.read_json(record), {"b": 2})
        self.assertEqual(
            sorted(entry.name for entry in self.root.iterdir()), ["record.json"]
        )
        (self.root / "linked.json").symlink_to(record)
        with self.assertRaisesRegex(files.WorkspaceError, "link"):
            files.read_json(self.root / "linked.json")

    def test_a_secret_is_made_once_and_owner_only(self) -> None:
        files.private_directory(self.root)
        secret = files.write_secret(self.root / "password")
        self.assertEqual(files.write_secret(self.root / "password"), secret)
        self.assertEqual(os.stat(self.root / "password").st_mode & 0o777, 0o600)


class Locking(Scratch):
    def test_the_lock_is_exclusive_and_dies_with_its_holder(self) -> None:
        files.private_directory(self.root)
        holder = Holder(self.root)
        lock = files.Lock(self.root / "supervisor.lock")
        self.assertFalse(lock.acquire())
        self.assertTrue(lock.held_elsewhere())
        # A SIGKILL cannot leave the root locked: the kernel releases it.
        holder.kill()
        self.assertFalse(lock.held_elsewhere())
        self.assertTrue(lock.acquire())
        lock.release()

    def test_a_root_whose_filesystem_ignores_locks_is_refused(self) -> None:
        files.private_directory(self.root)
        lock = files.Lock(self.root / "supervisor.lock")
        # As on a Docker Desktop bind mount: every flock succeeds.
        with (
            patch("fcntl.flock", lambda *_: None),
            self.assertRaisesRegex(files.WorkspaceError, "does not enforce file locks"),
        ):
            lock.acquire()
        self.assertTrue(lock.acquire())
        lock.release()

    def test_a_second_start_reports_the_running_stack_and_starts_nothing(self) -> None:
        demo = self.workspace()
        self.initialize(demo)
        holder = Holder(self.root)
        try:
            self.write_run(
                state="ready", urls={"guide": "http://127.0.0.1:18610/#/demo"}
            )
            self.assertEqual(demo.start(), 0)
            self.assertIn(
                "already running: http://127.0.0.1:18610/#/demo", self.output[-1]
            )
            self.write_run(state="starting", stage="Building the web client")
            self.assertEqual(self.workspace().start(), workspace.EXIT_BUSY)
            self.assertIn("Another start is in progress", self.output[-1])
            self.assertEqual(self.workspace().new_session(), workspace.EXIT_BUSY)
            self.assertIn("Stop it first", self.output[-1])
        finally:
            holder.kill()
        self.assertFalse((self.root / "sessions").exists())

    def test_port_base_applies_only_to_a_new_root(self) -> None:
        record = self.initialize(self.workspace(18610))
        self.assertEqual(
            record["ports"],
            {"postgres": 18613, "research": 18612, "trading": 18611, "web": 18610},
        )
        self.assertTrue(str(record["compose_project"]).startswith("chronos-demo-"))
        self.assertNotEqual(record["compose_project"], "chronos")
        with self.assertRaisesRegex(files.WorkspaceError, "only to a new root"):
            self.initialize(self.workspace(18620))
        self.assertEqual(
            self.initialize(self.workspace(None))["ports"], record["ports"]
        )
        # Only the lock's holder may create or change the record.
        with self.assertRaisesRegex(files.WorkspaceError, "only under the root lock"):
            self.workspace(None)._workspace()


class Orphans(Scratch):
    def test_children_of_a_hard_killed_supervisor_are_refused_never_killed(
        self,
    ) -> None:
        demo = self.workspace()
        self.initialize(demo)
        orphan = subprocess.Popen(["/bin/sleep", "60"])
        try:
            self.write_run(
                state="ready",
                pid=999_999,
                children=[
                    {"name": "trading", "pid": orphan.pid, "command": "/bin/sleep"}
                ],
            )
            self.assertEqual(demo.start(), workspace.EXIT_ORPHANS)
            self.assertIn("never kills a process", " ".join(self.output))
            self.assertIsNone(orphan.poll())
            self.assertEqual(files.read_json(self.root / "run.json")["state"], "ready")  # type: ignore[index]
        finally:
            orphan.kill()
            orphan.wait()
        # Once they are gone, a start may go on; a reused PID is not an orphan.
        self.assertEqual(demo._orphans(), [])
        unrelated = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(60)"]
        )
        try:
            self.write_run(
                state="ready",
                children=[
                    {
                        "name": "trading",
                        "pid": unrelated.pid,
                        "command": "/x/chronos_paper_trading_server",
                    }
                ],
            )
            self.assertEqual(demo._orphans(), [])
        finally:
            unrelated.kill()
            unrelated.wait()

    def test_an_orphan_with_a_long_program_name_is_found(self) -> None:
        # Linux cuts a process's name to 15 bytes ("chronos_paper_t"), so the
        # trading server is matched by its whole command line instead.
        demo = self.workspace()
        self.initialize(demo)
        program = self.base / "chronos_paper_trading_server"
        program.symlink_to("/bin/sleep")
        orphan = subprocess.Popen([str(program), "60"])
        try:
            self.assertIsNone(orphan.poll())
            self.write_run(
                state="ready",
                children=[
                    {"name": "trading", "pid": orphan.pid, "command": str(program)}
                ],
            )
            self.assertEqual([found["pid"] for found in demo._orphans()], [orphan.pid])
        finally:
            orphan.kill()
            orphan.wait()


class Ports(unittest.TestCase):
    def test_a_listening_or_bound_port_is_in_use(self) -> None:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
            self.assertTrue(workspace.port_in_use(port))
            listener.listen()
            self.assertTrue(workspace.port_in_use(port))
        self.assertFalse(workspace.port_in_use(port))

    def test_a_port_freed_by_a_stopped_server_is_free_at_once(self) -> None:
        # The server closes first, so its side of the connection is left in
        # TIME_WAIT; a restarted server can bind, so the port is not in use.
        with socket.socket() as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", 0))
            port = server.getsockname()[1]
            server.listen()
            with socket.create_connection(("127.0.0.1", port)) as client:
                connection, _ = server.accept()
                connection.close()
                client.recv(1)
        self.assertFalse(workspace.port_in_use(port))


class Probes(unittest.TestCase):
    def test_an_identity_header_is_found_whatever_its_case(self) -> None:
        # Vite's proxy forwards header names in lower case.
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                self.send_response(200)
                self.send_header("x-chronos-trading-mode", "paper")
                self.end_headers()

            def log_message(self, format: str, *args: object) -> None:
                pass

        with http.server.HTTPServer(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.handle_request)
            thread.start()
            answer = services.http(f"http://127.0.0.1:{server.server_port}/health")
            thread.join()
        assert answer is not None
        self.assertEqual(answer.header("X-Chronos-Trading-Mode"), "paper")
        self.assertIsNone(answer.header("X-Chronos-Service"))


class Migrations(Scratch):
    def test_every_released_migration_is_listed_with_its_digest(self) -> None:
        released = sorted(
            path.name for path in (REPOSITORY / "migrations").glob("*.sql")
        )
        self.assertEqual([name for name, _ in database.MIGRATIONS], released)
        self.assertEqual(len(database.checked_migrations(REPOSITORY)), len(released))

    def test_an_edited_migration_is_refused_before_anything_is_applied(self) -> None:
        copy = self.base / "repository"
        shutil.copytree(REPOSITORY / "migrations", copy / "migrations")
        edited = copy / "migrations" / "005_model_deployments.sql"
        edited.write_bytes(edited.read_bytes() + b"\n-- edited\n")
        with self.assertRaisesRegex(
            files.WorkspaceError, "005_model_deployments.sql differs"
        ):
            database.checked_migrations(copy)
        fake = database.Database(
            docker="/nonexistent/docker",
            project="chronos-demo-test",
            port=1,
            password="x",
            log=self.base / "log",
        )
        with (
            patch.object(
                database.Database, "psql", side_effect=AssertionError("ran SQL")
            ),
            self.assertRaisesRegex(
                files.WorkspaceError, "differs from the released migration"
            ),
        ):
            database.create_session_database(fake, "chronos_demo_session_0001", copy)

    def test_session_databases_have_demo_names_only(self) -> None:
        self.assertEqual(database.session_database_name(7), "chronos_demo_session_0007")
        fake = database.Database("/nonexistent", "p", 1, "x", self.base / "log")
        for name in (
            "chronos",
            "chronos_demo_session_1",
            'x"; DROP DATABASE chronos; --',
        ):
            with self.assertRaisesRegex(
                files.WorkspaceError, "not a demo session database name"
            ):
                database.create_session_database(fake, name, REPOSITORY)

    def test_a_failed_migration_names_itself_and_stops(self) -> None:
        applied: list[str] = []

        def psql(
            _: database.Database, name: str, sql: bytes, timeout_s: int = 120
        ) -> str:
            del timeout_s
            if name == "postgres":
                return ""
            applied.append(sql.decode().split("\n", 1)[0][:40])
            if len(applied) == 4:
                raise files.WorkspaceError("PostgreSQL refused: ERROR: relation exists")
            return ""

        fake = database.Database("/nonexistent", "p", 1, "x", self.base / "log")
        with (
            patch.object(database.Database, "psql", psql),
            self.assertRaisesRegex(
                files.WorkspaceError, "migration 004_working_orders.sql failed"
            ),
        ):
            database.create_session_database(
                fake, "chronos_demo_session_0001", REPOSITORY
            )
        self.assertEqual(len(applied), 4)


class Children(Scratch):
    def spec(self, script: str) -> services.ChildSpec:
        files.private_directory(self.root)
        return services.ChildSpec(
            name="probe",
            command=[sys.executable, "-c", script],
            cwd=self.root,
            environment=toolchain.base_environment(),
            log=self.root / "probe.log",
            announcement=re.compile("listening on 127.0.0.1:"),
        )

    def announced_pid(self, child: services.Child) -> int:
        """The process ID the child printed after its announcement."""
        deadline = time.monotonic() + 10
        while not child.announced() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(child.announced())
        return int((self.root / "probe.log").read_text().split(": ")[-1])

    def test_a_child_is_ready_only_when_it_announces_and_answers(self) -> None:
        server = textwrap.dedent(
            """
            import http.server
            server = http.server.HTTPServer(("127.0.0.1", 0), http.server.SimpleHTTPRequestHandler)
            print(f"listening on 127.0.0.1:{server.server_port}", flush=True)
            server.serve_forever()
            """
        )
        child = services.Child(self.spec(server))
        child.start()
        try:
            port: list[int] = []

            def probe() -> str | None:
                if not port:
                    found = re.search(r":(\d+)", (self.root / "probe.log").read_text())
                    if found is None:
                        return "no port yet"
                    port.append(int(found.group(1)))
                answer = services.http(f"http://127.0.0.1:{port[0]}/")
                return (
                    None if answer is not None and answer.status == 200 else "no answer"
                )

            services.wait_ready(child, probe, 20)
        finally:
            self.assertIsNotNone(child.stop(5))
        self.assertFalse(child.alive())

    def test_a_child_that_exits_or_never_announces_fails_the_startup(self) -> None:
        exiting = services.Child(self.spec("import sys; sys.exit(3)"))
        exiting.start()
        with self.assertRaisesRegex(
            files.WorkspaceError, r"exited during startup \(exit 3\)"
        ):
            services.wait_ready(exiting, lambda: None, 10)
        self.assertEqual(exiting.stop(5), 3)
        silent = services.Child(self.spec("import time; time.sleep(30)"))
        silent.start()
        try:
            with self.assertRaisesRegex(
                files.WorkspaceError, "did not announce its address"
            ):
                services.wait_ready(silent, lambda: None, 1)
        finally:
            silent.stop(5)

    def test_stop_waits_for_the_leaders_exit_notification(self) -> None:
        # A process-list snapshot can already exclude the exited leader
        # before waitid/kqueue reports its exit. Defer that notification
        # once, deterministically, rather than depend on the kernel race.
        for code in (0, 3):
            with self.subTest(code=code):
                child = services.Child(self.spec(f"import sys; sys.exit({code})"))
                child.start()
                assert child.process is not None
                try:
                    self.assertTrue(child.process.wait(10))
                    self.assertIsNone(child.exit_code(), "the leader was reaped early")
                    with patch.object(
                        child.process._exit, "exited", side_effect=[False, True]
                    ):
                        self.assertEqual(child.stop(1), code)
                    self.assertEqual(child.survivors(), [])
                finally:
                    child.stop(1)

    def test_stop_does_not_reap_a_leader_without_an_exit_notification(self) -> None:
        child = services.Child(self.spec("import sys; sys.exit(3)"))
        child.start()
        assert child.process is not None
        try:
            self.assertTrue(child.process.wait(10))
            with (
                patch.object(child.process._exit, "exited", return_value=False),
                patch.object(processes, "_KILL_GRACE_S", 0),
                patch.object(time, "sleep", side_effect=AssertionError("slept")),
            ):
                self.assertIsNone(child.stop(1))
                self.assertIsNone(child.exit_code())
            # A later stop can observe and reap the same owned leader.
            self.assertEqual(child.stop(1), 3)
        finally:
            child.stop(1)

    def test_a_child_that_ignores_sigterm_is_killed_with_its_group(self) -> None:
        stubborn = textwrap.dedent(
            """
            import signal, subprocess, sys, time
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            print(grandchild.pid, flush=True)
            time.sleep(60)
            """
        )
        child = services.Child(self.spec(stubborn))
        child.start()
        deadline = time.monotonic() + 10
        while (
            not (self.root / "probe.log").read_text().strip()
            and time.monotonic() < deadline
        ):
            time.sleep(0.05)
        grandchild = int((self.root / "probe.log").read_text().split()[0])
        started = time.monotonic()
        self.assertEqual(child.stop(1), -signal.SIGKILL)
        self.assertLess(time.monotonic() - started, 5)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if not running(grandchild):
                break
            time.sleep(0.05)
        else:
            self.fail("the child's group outlived the stop")

    def test_stop_ends_a_descendant_that_outlives_a_responsive_leader(self) -> None:
        # The leader leaves SIGTERM alone and exits on it; what it started
        # ignores SIGTERM. The stop must not end with the leader.
        leader = textwrap.dedent(
            """
            import subprocess, sys, time
            stubborn = "import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(60)"
            grandchild = subprocess.Popen([sys.executable, "-c", stubborn])
            time.sleep(0.5)
            print(f"listening on 127.0.0.1: {grandchild.pid}", flush=True)
            time.sleep(60)
            """
        )
        child = services.Child(self.spec(leader))
        child.start()
        grandchild = self.announced_pid(child)
        try:
            self.assertEqual(child.stop(0.5), -signal.SIGTERM)
            self.assertFalse(running(grandchild), "a descendant outlived the stop")
            self.assertEqual(child.survivors(), [])
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.kill(grandchild, signal.SIGKILL)

    def test_a_leader_that_exited_is_stopped_with_its_group(self) -> None:
        # The leader exits on its own, leaving a process in its group. It is
        # not reaped until that group is gone, so the group is still its own.
        leader = textwrap.dedent(
            """
            import subprocess, sys
            grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            print(f"listening on 127.0.0.1: {grandchild.pid}", flush=True)
            sys.exit(3)
            """
        )
        child = services.Child(self.spec(leader))
        child.start()
        grandchild = self.announced_pid(child)
        try:
            deadline = time.monotonic() + 10
            while child.alive() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(child.alive())
            self.assertIsNone(child.exit_code(), "the exited leader was reaped early")
            self.assertTrue(running(grandchild))
            with self.assertRaisesRegex(
                files.WorkspaceError, r"exited during startup \(exit 3\)"
            ):
                services.wait_ready(child, lambda: None, 10)
            self.assertFalse(running(grandchild), "the exited leader's group survived")
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.kill(grandchild, signal.SIGKILL)

    def test_a_step_leaves_no_process_behind(self) -> None:
        files.private_directory(self.root)
        step = textwrap.dedent(
            f"""
            import subprocess, sys
            grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            open({str(self.root / "grandchild")!r}, "w").write(str(grandchild.pid))
            """
        )
        toolchain.run_logged(
            [sys.executable, "-c", step], self.root / "step.log", "a step", 30
        )
        grandchild = int((self.root / "grandchild").read_text())
        try:
            self.assertFalse(running(grandchild), "a step's process outlived it")
        finally:
            with contextlib.suppress(ProcessLookupError):
                os.kill(grandchild, signal.SIGKILL)

    def test_what_outlives_sigkill_is_reported(self) -> None:
        demo = self.workspace()
        files.private_directory(self.root)
        child = services.Child(self.spec("import time; time.sleep(60)"))
        child.start()
        demo.children = [child]
        files.write_json(
            self.root / "run.json",
            {"schema": workspace.RUN_SCHEMA, "pid": os.getpid(), "state": "ready"},
        )
        # As if a process of its group could not be killed (one stuck in the
        # kernel, say): it is reported, not passed over.
        with (
            patch.object(processes, "group_members", return_value=[999_999]),
            patch.object(processes, "_KILL_GRACE_S", 0.2),
            patch.dict(services.STOP_TIMEOUTS_S, {"web": 0.2}),
        ):
            child.spec = services.ChildSpec(**{**child.spec.__dict__, "name": "web"})
            demo._shutdown()
        run = files.read_json(self.root / "run.json")
        assert run is not None
        self.assertEqual(run["state"], "stopped")
        self.assertEqual(
            run["cleanup_problems"],
            ["web's processes [999999] still run after SIGKILL"],
        )
        self.assertIn(
            "warning: web's processes [999999] still run after SIGKILL", self.output
        )


class Environment(unittest.TestCase):
    def test_children_get_no_credentials(self) -> None:
        secrets = {
            "DATABENTO_API_KEY": "db-secret",
            "APCA_API_KEY_ID": "alpaca-id",
            "APCA_API_SECRET_KEY": "alpaca-secret",
            "AWS_SECRET_ACCESS_KEY": "aws",
            "PGPASSWORD": "pg",
        }
        with patch.dict(os.environ, secrets):
            environment = toolchain.base_environment({"EXTRA": "1"})
        self.assertFalse(set(secrets) & set(environment))
        self.assertEqual(environment["EXTRA"], "1")
        self.assertIn("PATH", environment)


class Status(Scratch):
    def test_status_reports_without_secrets(self) -> None:
        demo = self.workspace()
        self.initialize(demo)
        password = files.write_secret(
            files.private_tree(self.root, "secrets") / "postgres-password"
        )
        self.write_run(state="ready", pid=999_999, stage="ready")
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(demo.status(), 0)
        text = "\n".join(self.output)
        self.assertIn("supervisor: not running", text)
        self.assertIn("ready (ended without cleanup)", text)
        self.assertNotIn(password, text)


class OwnedOutput(Scratch):
    """Nothing is written through a link, a hard link, or a special file."""

    def setUp(self) -> None:
        super().setUp()
        files.private_directory(self.root)
        self.outside = self.base / "unrelated.txt"
        self.outside.write_text("DO NOT CHANGE\n")

    def unsafe_entries(self, name: str) -> list[tuple[str, Path]]:
        """Each kind of unsafe entry at root/name, made in turn."""
        made = []
        for kind in ("link", "hard link", "fifo"):
            entry = self.root / f"{name}-{kind.replace(' ', '-')}"
            if kind == "link":
                entry.symlink_to(self.outside)
            elif kind == "hard link":
                os.link(self.outside, entry)
            else:
                os.mkfifo(entry, 0o600)
            made.append((kind, entry))
        return made

    def test_a_log_is_appended_only_to_its_own_plain_file(self) -> None:
        for kind, entry in self.unsafe_entries("log"):
            with (
                self.subTest(kind),
                self.assertRaisesRegex(
                    files.WorkspaceError, "not this user's own plain"
                ),
            ):
                files.appending(entry).close()
        self.assertEqual(self.outside.read_text(), "DO NOT CHANGE\n")
        # The control: an ordinary log is appended to, a missing one made.
        log = self.root / "ordinary.log"
        log.write_bytes(b"one\n")
        with files.appending(log) as output:
            output.write(b"two\n")
        self.assertEqual(log.read_bytes(), b"one\ntwo\n")
        with files.appending(self.root / "new.log") as output:
            output.write(b"x")
        self.assertEqual(os.stat(self.root / "new.log").st_mode & 0o777, 0o600)

    def test_a_replaced_file_replaces_a_link_never_its_target(self) -> None:
        for kind, entry in self.unsafe_entries("record"):
            with self.subTest(kind), files.replacing(entry) as output:
                output.write(b"{}\n")
            self.assertEqual(entry.read_bytes(), b"{}\n")
            self.assertFalse(entry.is_symlink())
        self.assertEqual(self.outside.read_text(), "DO NOT CHANGE\n")
        # A block that fails leaves the old content, and no partial file.
        record = self.root / "record-link"
        with self.assertRaises(RuntimeError), files.replacing(record) as output:
            output.write(b"partial")
            raise RuntimeError("failed while writing")
        self.assertEqual(record.read_bytes(), b"{}\n")
        self.assertFalse(list(self.root.glob(".*.partial")))

    def test_a_read_never_follows_a_link_or_waits_on_a_fifo(self) -> None:
        entries = dict(self.unsafe_entries("read"))
        with self.assertRaisesRegex(files.WorkspaceError, "is a link"):
            files.read_file(entries["link"])
        with self.assertRaisesRegex(files.WorkspaceError, "not a plain file"):
            files.read_file(entries["fifo"])
        with self.assertRaisesRegex(files.WorkspaceError, "is a link"):
            files.write_secret(entries["link"])

    def test_captured_output_and_its_log_are_never_written_through_links(
        self,
    ) -> None:
        demo = self.workspace()
        logs = files.private_tree(self.root, "logs")
        # A link where the output goes is replaced, never truncated through.
        (logs / "seed.json").symlink_to(self.outside)
        demo._capture(
            [sys.executable, "-c", "print('{}')"],
            self.root,
            logs / "seed.log",
            logs / "seed.json",
            "printing",
            30,
        )
        self.assertEqual(self.outside.read_text(), "DO NOT CHANGE\n")
        self.assertEqual(files.read_json(logs / "seed.json"), {})
        # A link where its log goes is refused before the command runs.
        (logs / "check.log").symlink_to(self.outside)
        marker = self.base / "ran"
        with self.assertRaisesRegex(files.WorkspaceError, "not this user's own plain"):
            demo._capture(
                touching(marker),
                self.root,
                logs / "check.log",
                logs / "c.json",
                "x",
                30,
            )
        self.assertFalse(marker.exists())
        self.assertEqual(self.outside.read_text(), "DO NOT CHANGE\n")

    def test_every_writer_refuses_a_linked_log_before_running(self) -> None:
        log = self.root / "linked.log"
        log.symlink_to(self.outside)
        marker = self.base / "ran"
        with self.assertRaisesRegex(files.WorkspaceError, "not this user's own plain"):
            toolchain.run_logged(touching(marker), log, "a step", 30)
        child = services.Child(
            services.ChildSpec(
                name="probe",
                command=touching(marker),
                cwd=self.root,
                environment=toolchain.base_environment(),
                log=log,
                announcement=re.compile("never"),
            )
        )
        with self.assertRaisesRegex(files.WorkspaceError, "not this user's own plain"):
            child.start()
        self.assertIsNone(child.process)
        docker = self.base / "docker"
        docker.write_text(f"#!/bin/sh\ntouch {marker}\n")
        docker.chmod(0o700)
        compose = database.Database(str(docker), "chronos-demo-test", 1, "x", log)
        with self.assertRaisesRegex(files.WorkspaceError, "not this user's own plain"):
            compose.running()
        self.assertFalse(marker.exists())
        self.assertEqual(self.outside.read_text(), "DO NOT CHANGE\n")
        # The control: with an ordinary log, each one runs its command.
        compose = database.Database(
            str(docker), "chronos-demo-test", 1, "x", self.root / "db.log"
        )
        compose.running()
        self.assertTrue(marker.exists())

    def test_build_destinations_must_be_real_directories(self) -> None:
        elsewhere = self.base / "elsewhere"
        elsewhere.mkdir()
        (elsewhere / "keep").write_text("kept")
        marker = self.base / "ran"
        tool = self.base / "tool"
        tool.write_text(f"#!/bin/sh\ntouch {marker}\n")
        tool.chmod(0o700)
        tools = preflight.Toolset(str(tool), str(tool), str(tool), str(tool), None)
        log = self.root / "build.log"
        builds: list[tuple[str, Callable[[toolchain.Toolchain], None]]] = [
            ("build", lambda chain: toolchain.build_cpp(chain, tools, log)),
            ("python", lambda chain: toolchain.build_python(chain, log)),
            ("web/dist", lambda chain: toolchain.build_web(chain, tools, log)),
            (
                "web/app/node_modules",
                lambda chain: toolchain.build_web(chain, tools, log),
            ),
        ]
        for number, (destination, build) in enumerate(builds):
            with self.subTest(destination):
                chain = toolchain.Toolchain(
                    root=files.private_tree(self.base, f"chain-{number}"),
                    repository=REPOSITORY,
                )
                parent = (chain.root / destination).parent
                files.private_tree(chain.root, *parent.relative_to(chain.root).parts)
                (chain.root / destination).symlink_to(elsewhere)
                with self.assertRaisesRegex(files.WorkspaceError, "link"):
                    build(chain)
        self.assertFalse(marker.exists())
        self.assertEqual(sorted(path.name for path in elsewhere.iterdir()), ["keep"])
        # A stamp is read without following a link, and replaced, not written through.
        stamp = self.root / "stamp"
        stamp.symlink_to(self.outside)
        with self.assertRaisesRegex(files.WorkspaceError, "is a link"):
            toolchain._stamp_matches(stamp, "x")
        toolchain._write_stamp(stamp, "x")
        self.assertTrue(toolchain._stamp_matches(stamp, "x"))
        self.assertEqual(self.outside.read_text(), "DO NOT CHANGE\n")


class FirstStarts(Scratch):
    """Only the start that holds the root lock creates or changes its state."""

    def contender(self, script: str, port_base: int) -> subprocess.Popen[str]:
        return subprocess.Popen(
            [
                sys.executable,
                "-c",
                script,
                str(Path(__file__).parent),
                str(REPOSITORY),
                str(self.root),
                str(self.base / "toolchain"),
                str(port_base),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def test_a_start_that_loses_the_lock_to_a_first_start_writes_nothing(
        self,
    ) -> None:
        first = self.contender(_PAUSED_FIRST_START, 19620)
        try:
            assert first.stdout is not None and first.stdin is not None
            self.assertEqual(first.stdout.readline().strip(), "paused")
            # The first start holds the lock and is about to write the record.
            missing = preflight.Problem("a stand-in prerequisite", "none")
            with patch.object(preflight, "check", return_value=(None, [missing])):
                self.assertEqual(self.workspace(19610).start(), workspace.EXIT_BUSY)
            self.assertIn("Another start is in progress", self.output[-1])
            self.assertEqual(
                sorted(entry.name for entry in self.root.iterdir()), ["supervisor.lock"]
            )
            first.stdin.write("\n")
            first.stdin.flush()
            self.assertEqual(first.wait(30), workspace.EXIT_PREREQUISITE)
        finally:
            first.kill()
            first.communicate()
        record = files.read_json(self.root / "workspace.json")
        assert record is not None
        self.assertEqual(record["ports"]["web"], 19620)

    def test_simultaneous_first_starts_make_one_record_the_winners(self) -> None:
        for round in range(3):
            self.root = self.base / f"root-{round}"
            bases = [19610, 19620, 19630]
            racers = [self.contender(_RACING_START, base) for base in bases]
            for racer in racers:
                assert racer.stdin is not None
                racer.stdin.write("go\n")
                racer.stdin.flush()
            results = []
            for racer in racers:
                output, _ = racer.communicate(timeout=60)
                results.append((racer.returncode, output))
            with self.subTest(round=round):
                winners = [
                    base
                    for base, (code, _) in zip(bases, results, strict=True)
                    if code == workspace.EXIT_PREREQUISITE
                ]
                self.assertEqual(len(winners), 1, results)
                record = files.read_json(self.root / "workspace.json")
                assert record is not None
                self.assertEqual(record["ports"]["web"], winners[0])
                for code, output in results:
                    self.assertTrue(
                        code == workspace.EXIT_PREREQUISITE
                        or (code == workspace.EXIT_BUSY and "in progress" in output)
                        or (code == 1 and "only to a new root" in output),
                        (code, output),
                    )
                self.assertFalse(list(self.root.glob(".*.partial")))

    def test_a_root_made_by_another_start_meanwhile_is_used(self) -> None:
        made = os.mkdir

        def made_meanwhile(path: Path, mode: int = 0o777) -> None:
            made(path, 0o700)  # another start, between the look and the make
            made(path, mode)

        with patch.object(os, "mkdir", made_meanwhile):
            files.private_directory(self.root)
        self.assertTrue(self.root.is_dir())


class FakeDatabase:
    """The demo PostgreSQL as the lifecycle sees it, with no Docker."""

    def __init__(
        self, up_fails: bool, stop_fails: bool = False, running: bool = False
    ) -> None:
        self.up_fails = up_fails
        self.stop_fails = stop_fails
        self.is_running = running
        self.calls: list[str] = []

    def running(self) -> bool:
        self.calls.append("running")
        return self.is_running

    def up(self) -> None:
        self.calls.append("up")
        if self.up_fails:
            # Compose created and started the container, then the wait failed.
            raise files.WorkspaceError("the demo PostgreSQL did not become healthy")

    def stop(self) -> None:
        self.calls.append("stop")
        if self.stop_fails:
            raise files.WorkspaceError("the demo PostgreSQL did not stop")


class DatabaseRollback(Scratch):
    def start(self, fake: FakeDatabase) -> int:
        demo = self.workspace()
        tools = preflight.Toolset("cmake", "node", "npm", "docker", None)
        with (
            patch.object(preflight, "check", return_value=(tools, [])),
            patch.object(demo, "_database_of", return_value=fake),
            patch.object(demo, "_refuse_ports", return_value=None),
            patch.object(workspace, "build_cpp"),
            patch.object(workspace, "build_python"),
            patch.object(workspace, "build_web"),
            patch.object(demo, "_seed", return_value="a" * 64),
        ):
            return demo.start()

    def test_a_database_whose_start_fails_is_stopped(self) -> None:
        fake = FakeDatabase(up_fails=True)
        self.assertEqual(self.start(fake), 1)
        self.assertEqual(fake.calls, ["running", "up", "stop"])
        run = files.read_json(self.root / "run.json")
        assert run is not None
        self.assertEqual(run["state"], "failed")
        self.assertEqual(run["cleanup_problems"], [])

    def test_a_database_that_does_not_stop_is_reported(self) -> None:
        fake = FakeDatabase(up_fails=True, stop_fails=True)
        self.assertEqual(self.start(fake), 1)
        self.assertIn("warning: the demo PostgreSQL did not stop", self.output)
        run = files.read_json(self.root / "run.json")
        assert run is not None
        self.assertEqual(run["cleanup_problems"], ["the demo PostgreSQL did not stop"])
        self.assertEqual(self.unresolved(), ["start: the demo PostgreSQL did not stop"])

    def unresolved(self) -> list[str]:
        """What status says is not cleaned up, by command."""
        status: list[str] = []
        workspace.Workspace(REPOSITORY, self.root, None, None, status.append).status()
        found = []
        for line in status:
            match = re.fullmatch(r"  not cleaned up \((\S+), [^)]+\): (.*)", line)
            if match:
                found.append(f"{match[1]}: {match[2]}")
        return found

    def new_session(self, fake: FakeDatabase) -> int:
        demo = self.workspace()
        tools = preflight.Toolset("cmake", "node", "npm", "docker", None)
        with (
            patch.object(preflight, "check", return_value=(tools, [])),
            patch.object(demo, "_database_of", return_value=fake),
            patch.object(demo, "_refuse_ports", return_value=None),
            patch.object(workspace, "create_session_database"),
        ):
            return demo.new_session()

    def test_a_new_session_whose_database_does_not_stop_is_not_a_clean_success(
        self,
    ) -> None:
        # The session is made and kept, but the command does not claim a
        # clean result, and status keeps the problem.
        fake = FakeDatabase(up_fails=False, stop_fails=True)
        self.assertEqual(self.new_session(fake), workspace.EXIT_CLEANUP)
        self.assertEqual(fake.calls, ["running", "up", "stop"])
        self.assertIn("warning: the demo PostgreSQL did not stop", self.output)
        self.assertTrue(any("Session 1 is ready" in line for line in self.output))
        self.assertEqual(
            files.read_json(self.root / "sessions" / "current.json"), {"number": 1}
        )
        self.assertEqual(
            self.unresolved(), ["new-session: the demo PostgreSQL did not stop"]
        )
        # A later stop of the database that succeeds resolves it.
        self.assertEqual(self.new_session(FakeDatabase(up_fails=False)), 0)
        self.assertEqual(self.unresolved(), [])
        self.assertFalse((self.root / "cleanup.json").exists())

    def test_a_failed_stop_never_masks_the_error_before_it(self) -> None:
        fake = FakeDatabase(up_fails=True, stop_fails=True)
        with self.assertRaisesRegex(files.WorkspaceError, "did not become healthy"):
            self.new_session(fake)
        self.assertEqual(
            self.unresolved(), ["new-session: the demo PostgreSQL did not stop"]
        )
        self.assertFalse((self.root / "sessions" / "current.json").exists())

    def test_a_start_that_cannot_stop_everything_is_not_a_clean_exit(self) -> None:
        # As after Ctrl-C on a ready stack: the stack ran, then its database
        # did not stop.
        fake = FakeDatabase(up_fails=False, stop_fails=True)
        demo = self.workspace()

        def ran(record: dict[str, object]) -> int:
            del record
            demo._database = fake  # type: ignore[assignment]
            demo._started_database = True
            return 0

        with patch.object(demo, "_start", ran):
            self.assertEqual(demo.start(), workspace.EXIT_CLEANUP)
        self.assertIn("warning: the demo PostgreSQL did not stop", self.output)
        self.assertEqual(self.unresolved(), ["start: the demo PostgreSQL did not stop"])

    def test_left_processes_are_shown_while_they_run(self) -> None:
        demo = self.workspace()
        self.initialize(demo)
        left = subprocess.Popen(["/bin/sleep", "60"])
        try:
            problem = demo._problem(
                "processes", "trading's processes still run", [left.pid]
            )
            demo._record_cleanup("start", [problem], database_stopped=True)
            self.assertEqual(
                self.unresolved(), ["start: trading's processes still run"]
            )
        finally:
            left.kill()
            left.wait()
        self.assertEqual(self.unresolved(), [])

    def test_new_session_stops_a_database_it_failed_to_start_only(self) -> None:
        tools = preflight.Toolset("cmake", "node", "npm", "docker", None)
        for was_running, calls in (
            (False, ["running", "up", "stop"]),
            # Already running: not this command's to stop.
            (True, ["running", "up"]),
        ):
            with self.subTest(was_running=was_running):
                fake = FakeDatabase(up_fails=True, running=was_running)
                demo = self.workspace()
                with (
                    patch.object(preflight, "check", return_value=(tools, [])),
                    patch.object(demo, "_database_of", return_value=fake),
                    patch.object(demo, "_refuse_ports", return_value=None),
                    self.assertRaisesRegex(
                        files.WorkspaceError, "did not become healthy"
                    ),
                ):
                    demo.new_session()
                self.assertEqual(fake.calls, calls)
                self.assertFalse(demo.lock.held())


if __name__ == "__main__":
    unittest.main()
