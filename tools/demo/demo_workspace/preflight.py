"""Prerequisites, checked before anything is built or started.

Each check names what is missing and how to install it; nothing here
installs, upgrades, or configures anything outside the workspace.
"""

import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

NODE_VERSION = (22, 22, 2)
CMAKE_VERSION = (3, 24)
_LIBPQ_PREFIXES = (
    Path("/opt/homebrew/opt/libpq"),
    Path("/usr/local/opt/libpq"),
)
_LIBPQ_HEADERS = (
    Path("/usr/include/postgresql/libpq-fe.h"),
    Path("/usr/include/libpq-fe.h"),
)


@dataclass(frozen=True)
class Toolset:
    """What the checks found: the executables the workspace runs."""

    cmake: str
    node: str
    npm: str
    docker: str
    # PostgreSQL_ROOT for CMake when libpq is keg-only, as with Homebrew.
    libpq_root: Path | None


@dataclass(frozen=True)
class Problem:
    what: str
    fix: str


def _output(command: list[str]) -> str | None:
    try:
        completed = subprocess.run(
            command, capture_output=True, text=True, timeout=30, check=False
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return completed.stdout if completed.returncode == 0 else None


def _version(text: str | None) -> tuple[int, ...] | None:
    if text is None:
        return None
    found = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    if found is None:
        return None
    return tuple(int(part) for part in found.groups() if part is not None)


def _libpq() -> tuple[bool, Path | None]:
    for prefix in _LIBPQ_PREFIXES:
        if (prefix / "include" / "libpq-fe.h").is_file():
            return True, prefix
    if any(header.is_file() for header in _LIBPQ_HEADERS):
        return True, None
    return _output(["pkg-config", "--exists", "libpq"]) is not None, None


def check() -> tuple[Toolset | None, list[Problem]]:
    problems: list[Problem] = []
    if sys.version_info[:2] != (3, 12):
        problems.append(
            Problem(
                f"Python 3.12 is required; this is {sys.version.split()[0]}",
                "run the command with python3.12 (https://www.python.org/downloads/)",
            )
        )
    cmake = shutil.which("cmake")
    cmake_version = _version(_output([cmake, "--version"])) if cmake else None
    if cmake is None or cmake_version is None or cmake_version[:2] < CMAKE_VERSION:
        problems.append(
            Problem(
                "CMake 3.24 or newer is required",
                "install it (brew install cmake, or apt-get install cmake)",
            )
        )
    if shutil.which("c++") is None and shutil.which("clang++") is None:
        problems.append(
            Problem(
                "a C++20 compiler is required",
                "install Xcode Command Line Tools (xcode-select --install), "
                "or clang/g++",
            )
        )
    found, libpq_root = _libpq()
    if not found:
        problems.append(
            Problem(
                "PostgreSQL's C client library (libpq) is required",
                "install it (brew install libpq, or apt-get install libpq-dev)",
            )
        )
    node = shutil.which("node")
    node_version = _version(_output([node, "--version"])) if node else None
    if (
        node is None
        or node_version is None
        or not (node_version[0] == NODE_VERSION[0] and node_version >= NODE_VERSION)
    ):
        problems.append(
            Problem(
                "Node.js 22.22.2 or a later 22.x is required (apps/web/.nvmrc)",
                "install it, for example with nvm install 22.22.2",
            )
        )
    npm = shutil.which("npm")
    if npm is None:
        problems.append(Problem("npm is required", "it is installed with Node.js"))
    docker = shutil.which("docker")
    if docker is None:
        problems.append(
            Problem(
                "Docker with Compose is required for the demo database",
                "install Docker Desktop or Docker Engine with the Compose plugin",
            )
        )
    elif _output([docker, "compose", "version"]) is None:
        problems.append(
            Problem(
                "Docker Compose v2 is required",
                "install the Docker Compose plugin (docker compose version)",
            )
        )
    elif _output([docker, "version", "--format", "{{.Server.Version}}"]) is None:
        problems.append(
            Problem(
                "the Docker daemon is not reachable",
                "start Docker Desktop (or the Docker service) and run start again",
            )
        )
    if problems or cmake is None or node is None or npm is None or docker is None:
        return None, problems
    return Toolset(
        cmake=cmake, node=node, npm=npm, docker=docker, libpq_root=libpq_root
    ), []
