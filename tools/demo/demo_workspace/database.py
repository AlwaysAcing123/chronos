"""The demo-only PostgreSQL: its own Compose project, volume, port, and
password, and one new database per session with every migration applied and
verified explicitly before anything trades.

The project is `chronos-demo-<root hash>`, so its volume is never the
development `chronos-postgres-data` and a second root never shares one. The
migrations are applied in order through the container's psql, with
ON_ERROR_STOP, never through /docker-entrypoint-initdb.d (which does not run
on an existing volume and has been unreliable through bind mounts). Each
file must have its recorded SHA-256 first: an edited migration is refused
before anything is applied, as the released files are never edited.
"""

import hashlib
import json
import os
import re
import subprocess
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from demo_workspace.files import WorkspaceError, appending
from demo_workspace.toolchain import base_environment

# Every released migration, in order, by its SHA-256.
MIGRATIONS = (
    (
        "001_paper_trading.sql",
        "31f5a4f16f28e92ba23ee5270b10c52a640e6fb6556e9ef5c73752f397f1a0a9",
    ),
    (
        "002_paper_order_read_indexes.sql",
        "98653a9a046f85e89bdbc3356841b51d037488887250d2bc0261d9b9a8581678",
    ),
    (
        "003_multi_instrument_accounts.sql",
        "4afc05d05968dbbb8f621d78e3073dc9c10227ac9806036b4e4a6202ecb5a538",
    ),
    (
        "004_working_orders.sql",
        "e10b15a919938531e01936095194a9ab3a221b7f933c9421fc0913634226e95d",
    ),
    (
        "005_model_deployments.sql",
        "a1afa7b0a3af9ba176c41699b68ce1d0302c4241de7663a7645be81ce8ee1090",
    ),
    (
        "006_live_stream_sequences.sql",
        "83dc2f81e6db0928b1122e612e52d8dffed131018065a24a91095e0017d17cdc",
    ),
    (
        "007_live_stream_writer_fence.sql",
        "3a8102817ae4f38927a3b6647893ce14543b13687acb2c48b235eeeca3ca69b9",
    ),
    (
        "008_registered_model_deployments.sql",
        "646e52516e995b5fbe955789c9ed1d181f2d08a2ea368f40418e7e368671c6f3",
    ),
)
# The deployment guards migration 005 installs, with their functions; a
# version row alone does not prove them (tools/deployment_validation).
REQUIRED_TRIGGERS = (
    (
        "paper_order_records_deployment_ownership",
        "paper_order_respects_deployment_ownership",
    ),
    ("paper_order_records_deployment_resolution", "paper_deployment_order_is_resolved"),
)
USER = "chronos_demo"
SESSION_DATABASE = re.compile(r"chronos_demo_session_[0-9]{4}")
_COMPOSE_FILE = Path(__file__).resolve().parents[1] / "compose.yaml"


@dataclass(frozen=True)
class Database:
    """The demo PostgreSQL of one workspace root."""

    docker: str
    project: str
    port: int
    password: str
    log: Path

    def _environment(self) -> dict[str, str]:
        extra = {
            name: os.environ[name]
            for name in ("DOCKER_HOST", "DOCKER_CONTEXT", "DOCKER_CONFIG")
            if name in os.environ
        }
        return base_environment(
            {
                **extra,
                "CHRONOS_DEMO_POSTGRES_PASSWORD": self.password,
                "CHRONOS_DEMO_POSTGRES_PORT": str(self.port),
            }
        )

    def _compose(
        self, arguments: Sequence[str], timeout_s: int, stdin: bytes | None = None
    ) -> subprocess.CompletedProcess[bytes]:
        command = [
            self.docker,
            "compose",
            "-p",
            self.project,
            "-f",
            str(_COMPOSE_FILE),
            *arguments,
        ]
        # Opened first, so an unsafe log is refused before Compose runs.
        with appending(self.log) as output:
            output.write(
                f"$ docker compose -p {self.project} {' '.join(arguments[:4])}\n".encode()
            )
            output.flush()
            try:
                completed = subprocess.run(
                    command,
                    input=stdin,
                    capture_output=True,
                    env=self._environment(),
                    timeout=timeout_s,
                    check=False,
                )
            except subprocess.TimeoutExpired as error:
                raise WorkspaceError(
                    f"docker compose {arguments[0]} did not finish within {timeout_s} s"
                ) from error
            output.write(completed.stdout[-8_000:] + completed.stderr[-8_000:])
        return completed

    def running(self) -> bool:
        listed = self._compose(
            ["ps", "--status", "running", "--format", "json", "postgres"], 60
        )
        return listed.returncode == 0 and bool(listed.stdout.strip())

    def up(self) -> None:
        started = self._compose(
            ["up", "--detach", "--wait", "--wait-timeout", "120", "postgres"], 600
        )
        if started.returncode != 0:
            raise WorkspaceError(
                "the demo PostgreSQL did not become healthy; see "
                f"{self.log} (is port {self.port} free?)"
            )

    def stop(self) -> None:
        stopped = self._compose(["stop", "--timeout", "30", "postgres"], 120)
        if stopped.returncode != 0:
            raise WorkspaceError(f"the demo PostgreSQL did not stop; see {self.log}")

    def psql(self, database: str, sql: bytes, timeout_s: int = 120) -> str:
        """Runs SQL from stdin in the container, stopping at the first error."""
        completed = self._compose(
            [
                "exec",
                "-T",
                "postgres",
                "psql",
                "-X",
                "-q",
                "-At",
                "-v",
                "ON_ERROR_STOP=1",
                "-U",
                USER,
                "-d",
                database,
            ],
            timeout_s,
            stdin=sql,
        )
        if completed.returncode != 0:
            detail = completed.stderr.decode(errors="replace").strip().splitlines()
            raise WorkspaceError(
                f"PostgreSQL refused: {detail[-1] if detail else 'no output'}"
            )
        return completed.stdout.decode()

    def url(self, database: str) -> str:
        """The trading server's connection string: given to it alone, never printed."""
        return f"postgresql://{USER}:{self.password}@127.0.0.1:{self.port}/{database}"


def checked_migrations(repository: Path) -> list[tuple[str, bytes]]:
    """Every migration's bytes, each verified against its recorded digest."""
    checked = []
    for name, digest in MIGRATIONS:
        data = (repository / "migrations" / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise WorkspaceError(
                f"migrations/{name} differs from the released migration; released migrations are "
                "never edited, so nothing was applied"
            )
        checked.append((name, data))
    return checked


def create_session_database(database: Database, name: str, repository: Path) -> None:
    """A new, empty database for a session, migrated and verified. An existing
    database of that name is refused, never reused."""
    if not SESSION_DATABASE.fullmatch(name):
        raise WorkspaceError(f"{name} is not a demo session database name")
    migrations = checked_migrations(repository)
    exists = database.psql(
        "postgres", f"SELECT 1 FROM pg_database WHERE datname = '{name}'".encode()
    )
    if exists.strip():
        raise WorkspaceError(
            f"the database {name} already exists; a new session never reuses one"
        )
    database.psql("postgres", f'CREATE DATABASE "{name}"'.encode())
    for migration, data in migrations:
        try:
            database.psql(name, data, timeout_s=300)
        except WorkspaceError as error:
            raise WorkspaceError(
                f"migration {migration} failed on {name}: {error}"
            ) from error
    verify_session_database(database, name)


def verify_session_database(database: Database, name: str) -> Mapping[str, object]:
    """Exactly migrations 1 to 8 recorded, and both deployment guards and
    their functions installed."""
    versions = database.psql(
        name,
        b"SELECT string_agg(version::text, ',' ORDER BY version) FROM chronos_schema_migrations",
    ).strip()
    if versions != ",".join(str(index) for index in range(1, len(MIGRATIONS) + 1)):
        raise WorkspaceError(
            f"{name} records migrations {versions or 'none'}, not 1 to {len(MIGRATIONS)}"
        )
    query = (
        "SELECT json_agg(json_build_array(t.tgname, p.proname)) FROM pg_trigger t "
        "JOIN pg_proc p ON p.oid = t.tgfoid WHERE NOT t.tgisinternal"
    )
    installed = {
        tuple(pair)
        for pair in json.loads(database.psql(name, query.encode()).strip() or "[]")
    }
    missing = [
        trigger
        for trigger, function in REQUIRED_TRIGGERS
        if (trigger, function) not in installed
    ]
    if missing:
        raise WorkspaceError(f"{name} lacks the deployment guards {', '.join(missing)}")
    return {
        "database": name,
        "migrations": versions,
        "triggers": [pair[0] for pair in REQUIRED_TRIGGERS],
    }


def session_database_name(number: int) -> str:
    return f"chronos_demo_session_{number:04d}"
