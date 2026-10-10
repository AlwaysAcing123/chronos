"""The real-recording commands, in the order an operator uses them.

plan-recordings        offline: validates a request plan and prints its ID
quote-recordings       metadata quotes only (Databento documents them as
                       free); never downloads; needs DATABENTO_API_KEY
authorize-acquisition  records a human decision to spend at most an
                       allowance on one exact plan; downloads nothing
acquire-recordings     THE ONLY COMMAND THAT CAN SPEND MONEY: downloads
                       each authorized recording at most once
recover-acquisition    closes an attempt whose provider process was never
                       seen to finish, as ambiguous; refunds nothing
prepare-recordings     offline: verifies the plan's recordings and runs its
                       frozen walk-forward studies and their audits
"""

import argparse
from pathlib import Path
from typing import Any

from chronos_research.acquisition import (
    DEFAULT_DOWNLOAD_TIMEOUT_SECONDS,
    acquire,
    authorize,
    quote_plan,
    recover,
)
from chronos_research.encoding import StrictJsonError, canonical_json, strict_json_object
from chronos_research.pipeline import Tools
from chronos_research.recording_pilot import prepare_plan, render_markdown
from chronos_research.recording_plan import RecordingPlanError, plan_identity, validated_plan

MAX_PLAN_BYTES = 256 * 1024
SPENDS_NOTHING = "nothing: this command contacts no provider and reads no credential"


def read_plan_file(path: Path) -> dict[str, Any]:
    """A plan file: bounded, strict JSON (no numbers, no duplicate keys)."""
    try:
        with path.open("rb") as stream:
            data = stream.read(MAX_PLAN_BYTES + 1)
    except OSError as error:
        raise RecordingPlanError("unreadable_plan", f"cannot read {path}: {error.strerror}") from error
    if len(data) > MAX_PLAN_BYTES:
        raise RecordingPlanError("invalid_plan", f"a plan is at most {MAX_PLAN_BYTES} bytes")
    try:
        return strict_json_object(data)
    except StrictJsonError as error:
        raise RecordingPlanError("invalid_plan", f"the plan file: {error}") from error


def _cap(text: str) -> tuple[str, str]:
    stream, separator, amount = text.partition("=")
    if not separator or not stream or not amount:
        raise argparse.ArgumentTypeError("must be <stream-id>=<US dollars, two decimals>")
    return stream, amount


def _timeout(text: str) -> int:
    if not text.isascii() or not text.isdigit() or text.startswith("0"):
        raise argparse.ArgumentTypeError("must be a positive whole number of seconds")
    return int(text)


def add_commands(commands: Any, once: type[argparse.Action], path: Any, artifact_id: Any) -> None:
    def plan_option(parser: argparse.ArgumentParser) -> None:
        parser.add_argument(
            "--plan", required=True, type=path, action=once, help="the chronos.recording_request_plan/1 file"
        )

    def root_option(parser: argparse.ArgumentParser) -> None:
        parser.add_argument(
            "--state-root",
            required=True,
            type=path,
            action=once,
            help="the acquisition state root (a directory)",
        )

    def tools_option(parser: argparse.ArgumentParser, what: str) -> None:
        parser.add_argument("--tools", required=True, type=path, action=once, help=what)

    plan = commands.add_parser(
        "plan-recordings",
        allow_abbrev=False,
        help="offline: validate a recording request plan and print its ID; spends nothing",
    )
    plan_option(plan)
    quote = commands.add_parser(
        "quote-recordings",
        allow_abbrev=False,
        help="metadata quotes only, never a download; needs DATABENTO_API_KEY",
        description=(
            "Asks the provider for each new recording's estimated records, bytes, and cost "
            "(chronos_databento_historical quote), verifies existing recordings offline, and proposes caps. "
            "It never downloads and creates no authorization."
        ),
    )
    plan_option(quote)
    tools_option(quote, "the directory holding chronos_databento_historical and chronos_databento_replay")
    authorization = commands.add_parser(
        "authorize-acquisition",
        allow_abbrev=False,
        help="record a human decision to spend at most an allowance on one exact plan; downloads nothing",
        description=(
            "Records the operator's authorization: the plan ID typed from the quote, one state root, an "
            "allowance, and a cap per recording, in whole cents. There is no default budget; a recording "
            "without a cap is never downloaded."
        ),
    )
    plan_option(authorization)
    authorization.add_argument(
        "--plan-id",
        required=True,
        type=artifact_id,
        action=once,
        help="the plan ID, as quote-recordings printed it",
    )
    root_option(authorization)
    authorization.add_argument(
        "--allowance-usd", required=True, action=once, help="the most to spend in total, e.g. 1.20"
    )
    authorization.add_argument(
        "--cap",
        type=_cap,
        action="append",
        default=[],
        help="<stream-id>=<D.CC>: the most one recording's fresh estimate may be; once per recording",
    )
    acquisition = commands.add_parser(
        "acquire-recordings",
        allow_abbrev=False,
        help="THE ONLY COMMAND THAT CAN SPEND MONEY: download each authorized recording at most once",
        description=(
            "Downloads, under one authorization, each capped recording that is not yet verified, reserving "
            "its full cap before the provider child starts. An attempt that does not end in a verified "
            "recording is never retried; see docs/real-market-validation.md."
        ),
    )
    root_option(acquisition)
    acquisition.add_argument(
        "--authorization", required=True, type=artifact_id, action=once, help="the authorization ID"
    )
    tools_option(
        acquisition, "the directory holding chronos_databento_historical and chronos_databento_replay"
    )
    acquisition.add_argument(
        "--timeout-seconds",
        type=_timeout,
        action=once,
        help=f"how long one download may run (default {DEFAULT_DOWNLOAD_TIMEOUT_SECONDS})",
    )
    recovery = commands.add_parser(
        "recover-acquisition",
        allow_abbrev=False,
        help="close an attempt never seen to finish, as ambiguous; refunds and retries nothing",
    )
    root_option(recovery)
    recovery.add_argument(
        "--authorization", required=True, type=artifact_id, action=once, help="the authorization ID"
    )
    recovery.add_argument("--attempt", required=True, type=_timeout, action=once, help="the attempt number")
    recovery.add_argument(
        "--confirm-no-provider-process",
        action="store_true",
        help="I checked that no chronos_databento_historical process for this state root is running",
    )
    preparation = commands.add_parser(
        "prepare-recordings",
        allow_abbrev=False,
        help="offline: verify the plan's recordings, then run its frozen studies and audits; spends nothing",
        description=(
            "Verifies every recording the plan can use (an existing input, or one acquired under the "
            "plan's authorization in --state-root), prepares its dataset, features, and labels, builds the "
            "collection, and for each instrument publishes the frozen plan, runs its study, and runs the "
            "independent auditor. Recordings not yet acquired are reported as pending."
        ),
    )
    plan_option(preparation)
    tools_option(preparation, "the directory of built Chronos tools, including chronos_databento_normalize")
    preparation.add_argument(
        "--artifacts", required=True, type=path, action=once, help="the directory of artifact roots"
    )
    preparation.add_argument(
        "--state-root", type=path, action=once, help="the acquisition state root holding acquired recordings"
    )
    preparation.add_argument(
        "--markdown", type=path, action=once, help="also write the report for a reader to this new file"
    )


def plan_recordings(arguments: argparse.Namespace) -> dict[str, Any]:
    plan = validated_plan(read_plan_file(arguments.plan))
    return {
        "existing_recordings": sum("existing" in recording for recording in plan["recordings"]),
        "new_recordings": sum("destination" in recording for recording in plan["recordings"]),
        "plan": plan,
        "plan_canonical_bytes": str(len(canonical_json(plan))),
        "plan_id": plan_identity(plan),
        "spends": SPENDS_NOTHING,
    }


def quote_recordings(arguments: argparse.Namespace) -> dict[str, Any]:
    return quote_plan(Tools(arguments.tools), read_plan_file(arguments.plan))


def authorize_acquisition(arguments: argparse.Namespace) -> dict[str, Any]:
    return authorize(
        read_plan_file(arguments.plan),
        arguments.plan_id,
        arguments.state_root,
        arguments.allowance_usd,
        arguments.cap,
    )


def acquire_recordings(arguments: argparse.Namespace) -> dict[str, Any]:
    timeout = arguments.timeout_seconds or DEFAULT_DOWNLOAD_TIMEOUT_SECONDS
    return acquire(Tools(arguments.tools), arguments.state_root, arguments.authorization, timeout)


def recover_acquisition(arguments: argparse.Namespace) -> dict[str, Any]:
    return recover(
        arguments.state_root,
        arguments.authorization,
        arguments.attempt,
        arguments.confirm_no_provider_process,
    )


def prepare_recordings(arguments: argparse.Namespace) -> dict[str, Any]:
    markdown: Path | None = arguments.markdown
    if markdown is not None and markdown.exists():
        raise RecordingPlanError("invalid_request", f"{markdown} exists; the report never replaces a file")
    report = prepare_plan(
        Tools(arguments.tools), read_plan_file(arguments.plan), arguments.artifacts, arguments.state_root
    )
    if markdown is not None:
        with markdown.open("x", encoding="utf-8") as stream:
            stream.write(render_markdown(report))
    return report


COMMANDS = {
    "plan-recordings": plan_recordings,
    "quote-recordings": quote_recordings,
    "authorize-acquisition": authorize_acquisition,
    "acquire-recordings": acquire_recordings,
    "recover-acquisition": recover_acquisition,
    "prepare-recordings": prepare_recordings,
}
