"""The workspace's own toolchain, built from this repository into a private
directory: the C++ server and tools, a hash-pinned Python environment, and
the web client with the guide enabled.

    <toolchain>/build        CMake build: PostgreSQL and API, no provider
                             adapter, no tests
    <toolchain>/python       a venv with python/requirements.lock, by hash
    <toolchain>/web/app      a copy of apps/web with its own node_modules
                             (npm ci from package-lock.json)
    <toolchain>/web/dist     its build, with VITE_CHRONOS_DEMO=1

Nothing here touches the repository's own build directories, its
apps/web/node_modules, or any global installation. Each destination is
checked to be a real, private directory before a tool writes into it, so a
link there (to a directory a build would fill or empty) is refused. Setup
downloads are the pinned dependencies only: CMake's hash-pinned cpp-httplib
and nlohmann/json, the hash-pinned Python wheels, and npm packages from the
lockfile.
"""

import hashlib
import os
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from demo_workspace.files import (
    WorkspaceError,
    appending,
    private_tree,
    read_file,
    refuse_link,
    replacing,
)
from demo_workspace.preflight import Toolset
from demo_workspace.processes import OwnedProcess

TARGETS = ("chronos_paper_trading_server", "chronos_dataset", "chronos_strategy_cli")
# apps/web entries never copied: installed, built, or test output.
_WEB_EXCLUDED = {"node_modules", "dist", "test-results", "playwright-report", ".vite"}


@dataclass(frozen=True)
class Toolchain:
    root: Path
    repository: Path

    @property
    def build(self) -> Path:
        return self.root / "build"

    @property
    def python(self) -> Path:
        return self.root / "python" / "bin" / "python"

    @property
    def web_app(self) -> Path:
        return self.root / "web" / "app"

    @property
    def web_dist(self) -> Path:
        return self.root / "web" / "dist"

    @property
    def server(self) -> Path:
        return self.build / "chronos_paper_trading_server"


def base_environment(extra: Mapping[str, str] | None = None) -> dict[str, str]:
    """What a child may see: never a provider key, token, or credential."""
    allowed = ("PATH", "HOME", "LANG", "LC_ALL", "TMPDIR", "USER", "LOGNAME")
    environment = {name: os.environ[name] for name in allowed if name in os.environ}
    return {**environment, **(extra or {})}


def run_step(process: OwnedProcess, what: str, timeout_s: float, log: Path) -> int:
    """Waits for a one-off step, then stops its whole group (a step's
    processes never outlive it) and returns its exit code. At the deadline,
    or if this process is interrupted, the group is killed."""
    try:
        finished = process.wait(timeout_s)
    except BaseException:
        process.stop(0)
        raise
    code = process.stop(0 if not finished else 5)
    if not finished:
        raise WorkspaceError(
            f"{what} did not finish within {timeout_s:.0f} s; see {log}"
        )
    if code is None:
        raise WorkspaceError(f"{what} could not be stopped; see {log}")
    return code


def run_logged(
    command: Sequence[str | Path],
    log: Path,
    what: str,
    timeout_s: int,
    cwd: Path | None = None,
    environment: Mapping[str, str] | None = None,
) -> None:
    """Runs one step with its output appended to its log, in its own process
    group, killed as a group at the deadline."""
    with appending(log) as output:
        output.write(f"$ {' '.join(map(str, command))}\n".encode())
        output.flush()
        process = OwnedProcess(
            [str(part) for part in command],
            cwd,
            environment if environment is not None else base_environment(),
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        code = run_step(process, what, timeout_s, log)
    if code != 0:
        raise WorkspaceError(f"{what} failed (exit {code}); see {log}")


def _digest(paths: Sequence[Path], base: Path, salt: str = "") -> str:
    hasher = hashlib.sha256(salt.encode())
    for path in sorted(paths):
        hasher.update(str(path.relative_to(base)).encode() + b"\0")
        hasher.update(path.read_bytes() + b"\0")
    return hasher.hexdigest()


def _stamp_matches(stamp: Path, value: str) -> bool:
    return read_file(stamp) == value.encode()


def _write_stamp(stamp: Path, value: str) -> None:
    with replacing(stamp) as file:
        file.write(value.encode())


def build_cpp(toolchain: Toolchain, tools: Toolset, log: Path) -> None:
    private_tree(toolchain.root, "build")
    if not (toolchain.build / "CMakeCache.txt").is_file():
        configure: list[str | Path] = [
            tools.cmake, "-S", toolchain.repository, "-B", toolchain.build,
            "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=OFF",
            "-DCHRONOS_ENABLE_POSTGRESQL_ADAPTER=ON",
            "-DCHRONOS_ENABLE_PAPER_TRADING_API=ON",
        ]  # fmt: skip
        if tools.libpq_root is not None:
            configure.append(f"-DPostgreSQL_ROOT={tools.libpq_root}")
        # The compiler a person chose (CC, CXX) applies to the first configure.
        compilers = {
            name: os.environ[name] for name in ("CC", "CXX") if name in os.environ
        }
        run_logged(
            configure,
            log,
            "configuring the C++ build",
            900,
            environment=base_environment(compilers),
        )
    jobs = str(max(1, (os.cpu_count() or 2) - 1))
    run_logged(
        [
            tools.cmake,
            "--build",
            toolchain.build,
            "--parallel",
            jobs,
            "--target",
            *TARGETS,
        ],
        log,
        "building the C++ server and tools",
        3_600,
    )


def build_python(toolchain: Toolchain, log: Path) -> None:
    lockfile = toolchain.repository / "python" / "requirements.lock"
    stamp = toolchain.root / "python" / ".requirements.sha256"
    private_tree(toolchain.root, "python")
    wanted = hashlib.sha256(lockfile.read_bytes()).hexdigest()
    if toolchain.python.is_file() and _stamp_matches(stamp, wanted):
        return
    if not toolchain.python.is_file():
        run_logged(
            [sys.executable, "-m", "venv", toolchain.root / "python"],
            log,
            "creating the Python environment",
            300,
        )
    run_logged(
        [
            toolchain.python,
            "-m",
            "pip",
            "install",
            "--quiet",
            "--require-hashes",
            "--no-deps",
            "-r",
            lockfile,
        ],
        log,
        "installing the hash-pinned Python dependencies",
        1_800,
    )
    _write_stamp(stamp, wanted)


def _mirror_web(source: Path, destination: Path) -> None:
    """destination becomes a copy of apps/web, keeping its node_modules."""
    for entry in destination.iterdir():
        if entry.name == "node_modules":
            continue
        if entry.is_dir() and not entry.is_symlink():
            shutil.rmtree(entry)
        else:
            entry.unlink()
    for entry in source.iterdir():
        if entry.name in _WEB_EXCLUDED:
            continue
        if entry.is_symlink():
            raise WorkspaceError(f"apps/web/{entry.name} is a link; it was not copied")
        if entry.is_dir():
            shutil.copytree(entry, destination / entry.name, symlinks=True)
        else:
            shutil.copy2(entry, destination / entry.name)


def build_web(toolchain: Toolchain, tools: Toolset, log: Path) -> None:
    private_tree(toolchain.root, "web", "app")
    # The build empties its output directory; npm ci replaces node_modules.
    private_tree(toolchain.root, "web", "dist")
    refuse_link(toolchain.web_app / "node_modules")
    _mirror_web(toolchain.repository / "apps" / "web", toolchain.web_app)
    lock = toolchain.web_app / "package-lock.json"
    installed = toolchain.root / "web" / ".package-lock.sha256"
    wanted = hashlib.sha256(lock.read_bytes()).hexdigest()
    if not (toolchain.web_app / "node_modules").is_dir() or not _stamp_matches(
        installed, wanted
    ):
        run_logged(
            [tools.npm, "ci", "--prefer-offline", "--no-audit", "--no-fund"],
            log,
            "installing the web client's locked dependencies (npm ci)",
            1_800,
            cwd=toolchain.web_app,
        )
        _write_stamp(installed, wanted)
    sources = [
        path
        for path in toolchain.web_app.rglob("*")
        if path.is_file()
        and "node_modules" not in path.relative_to(toolchain.web_app).parts
    ]
    built = toolchain.root / "web" / ".build.sha256"
    wanted_build = _digest(sources, toolchain.web_app, salt="VITE_CHRONOS_DEMO=1")
    if (toolchain.web_dist / "index.html").is_file() and _stamp_matches(
        built, wanted_build
    ):
        return
    run_logged(
        [
            tools.npm,
            "run",
            "build",
            "--",
            "--outDir",
            toolchain.web_dist,
            "--emptyOutDir",
        ],
        log,
        "building the web client with the guide",
        900,
        cwd=toolchain.web_app,
        environment=base_environment({"VITE_CHRONOS_DEMO": "1", "NO_COLOR": "1"}),
    )
    _write_stamp(built, wanted_build)
