"""The Chronos demo workspace: one command for an isolated, guided, local
paper-trading stack over invented synthetic prices (ADR 0025).

    python3.12 tools/demo/chronos_demo.py start        build, seed, and run it
    python3.12 tools/demo/chronos_demo.py status       what is there and running
    python3.12 tools/demo/chronos_demo.py new-session  a fresh trading database

start runs in the foreground; Ctrl-C stops the stack in order and keeps
every database, log, and artifact. A second start while one runs reports it
and starts nothing. The first start of a session plays its replay; every
later start of that session opens it in history mode, with no replay;
new-session creates the next session, and refuses while the stack runs.

Options (each at most once):
    --root DIR        the private state root (default: .chronos-demo/ here)
    --toolchain DIR   the private toolchain (default: <root>/toolchain)
    --port-base PORT  a new root's ports: web PORT, trading PORT+1, research
                      PORT+2, PostgreSQL PORT+3 (default 18510)

Exit codes: 0 done, 1 failed, 2 missing prerequisite, 3 busy (another start
is in progress, or the stack runs during new-session), 4 processes of an
earlier, hard-killed supervisor may still run, 5 a port is in use, 6 done,
but something the command started could not be stopped (status shows it
until it is resolved), 130 interrupted during startup.

Simulated trading only. Nothing contacts a market-data provider or reads a
credential; setup downloads only pinned build and package dependencies.
"""

import argparse
import io
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from demo_workspace.files import WorkspaceError
from demo_workspace.workspace import Workspace

REPOSITORY = Path(__file__).resolve().parents[2]


def _port(text: str) -> int:
    if not text.isdigit() or not 1024 <= int(text) <= 65_532:
        raise argparse.ArgumentTypeError("a port from 1024 to 65532")
    return int(text)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="chronos_demo.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )
    parser.add_argument("command", choices=("start", "status", "new-session"))
    parser.add_argument("--root", type=Path, default=REPOSITORY / ".chronos-demo")
    parser.add_argument("--toolchain", type=Path, default=None)
    parser.add_argument("--port-base", type=_port, default=None)
    return parser


def main(argv: list[str]) -> int:
    arguments = _parser().parse_args(argv)
    # Each stage is reported as it begins, even into a file or a pipe.
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(line_buffering=True)
    workspace = Workspace(
        REPOSITORY, arguments.root, arguments.toolchain, arguments.port_base
    )
    try:
        if arguments.command == "start":
            return workspace.start()
        if arguments.command == "status":
            return workspace.status()
        return workspace.new_session()
    except WorkspaceError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
