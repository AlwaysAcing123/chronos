"""Quoting, authorizing, and acquiring a request plan's recordings.

    quote      metadata only: `chronos_databento_historical quote` per new
               recording. Databento documents metadata requests as free.
               Never downloads.
    authorize  the human decision: one exact plan (its ID typed by the
               operator), one canonical state root, an allowance, and a cap
               per recording, in whole cents. No default budget exists.
    acquire    the only path that can spend money. One attempt per
               destination, ever: its full cap is reserved in the ledger,
               durably, before the provider child starts.
    recover    an operator's explicit release of an attempt whose child was
               never seen to finish. It refunds nothing and retries nothing.

The state root holds:

    authorizations/<id>/{authorization.json, plan.json}  published read-only
    ledgers/<id>.ndjson                                  append-only, fsynced
    receipts/<id>/<stream>.json                           write-once provenance
    recordings/<destination>                              written by the C++ tool
    acquisition.lock                                      the root's flock

Invariant: for an authorization, the caps of all reserved attempts never
exceed its allowance. It is about estimated caps the operator approved, not
the provider's invoice; the actual cost is recorded as unknown.

An attempt that did not end in a verified publication (failed, timed out,
killed, its output malformed, or never observed to finish) may already
have been charged. It is never retried, refunded, or forgotten: its
destination stays blocked for every authorization in the root. Retrying
after reconciling with the provider's billing needs a new destination, so a
new plan and a new human authorization.

Concurrency and crashes: an invocation holds an exclusive, non-blocking
flock on the root's lock file and passes that descriptor to the provider
child, so a child that outlives its parent keeps the root locked. A
reserved attempt without its finish blocks the whole root until `recover`
is run, under the lock, with explicit confirmation; a dead parent's PID
never clears anything. Supported filesystems: local POSIX ones (APFS,
ext4), where flock and fsync hold; not network filesystems.
"""

import contextlib
import errno
import fcntl
import json
import os
import re
import stat
from collections.abc import Iterator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import UTC, datetime
from decimal import Decimal
from pathlib import Path
from typing import Any

from chronos_research.acquisition_budget import (
    MAX_ALLOWANCE_CENTS,
    BudgetError,
    cap_argument,
    format_cents,
    parse_cents,
    parse_estimate,
    proposed_cap_cents,
)
from chronos_research.artifacts import is_artifact_id
from chronos_research.bounded_process import EXITED, Finished, run_bounded
from chronos_research.collection import MAX_SOURCES
from chronos_research.encoding import canonical_json, sha256_hex
from chronos_research.pipeline import DATABENTO_HISTORICAL, PipelineError, Tools
from chronos_research.publication import PublicationError, publish
from chronos_research.recording_plan import (
    RECORDINGS_DIRECTORY,
    RecordingPlanError,
    instant_ns,
    plan_identity,
    query_of,
    validated_plan,
)
from chronos_research.recording_verification import (
    RecordingVerificationError,
    VerifiedRecordingFile,
    manifest_path,
    verify_recording_file,
)

AUTHORIZATION_SCHEMA = {"name": "chronos.acquisition_authorization", "version": "1"}
RECEIPT_SCHEMA = {"name": "chronos.recording_receipt", "version": "1"}
AUTHORIZATION = "authorization.json"
PLAN = "plan.json"
LOCK = "acquisition.lock"
API_KEY = "DATABENTO_API_KEY"
QUOTE_TIMEOUT_SECONDS = 120
DEFAULT_DOWNLOAD_TIMEOUT_SECONDS = 900
MAX_DOWNLOAD_TIMEOUT_SECONDS = 3_600
STDOUT_LIMIT = 1024 * 1024
STDERR_LIMIT = 64 * 1024
DETAIL_LIMIT = 1_000
MAX_LEDGER_LINES = 1 + 2 * MAX_SOURCES
MAX_LEDGER_LINE_BYTES = 4_096
ACTUAL_COST = "unknown: Chronos has no authoritative billing record; reconcile with the provider's account"
_CHILD_ENVIRONMENT = ("PATH", "HOME", "TMPDIR", "LANG", "LC_ALL", "SSL_CERT_FILE", "SSL_CERT_DIR")
_REDACTED = "[redacted]"
# Every code the C++ tool writes (HistoricalRecordingErrorCode); those it
# reports, by its code path, before any billable request come first.
_BEFORE_REQUEST_CODES = (
    "invalid_query",
    "missing_api_key",
    "provider_connection_failed",
    "provider_quote_failed",
    "invalid_destination",
    "destination_already_exists",
    "cost_limit_exceeded",
    "temporary_file_creation_failed",
)
_TOOL_ERROR_CODES = (
    *_BEFORE_REQUEST_CODES,
    "provider_download_failed",
    "publish_failed",
    "recording_validation_failed",
    "manifest_write_failed",
)


class AcquisitionError(Exception):
    """A quote, authorization, acquisition, or recovery is refused."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


# --- Secrets and bounded diagnostics ---------------------------------------------------------------------


def redact(text: str, secrets: Sequence[str]) -> str:
    for secret in secrets:
        if len(secret) >= 4:
            text = text.replace(secret, _REDACTED)
    return text


def bounded_detail(raw: bytes | str, secrets: Sequence[str]) -> str:
    """Text safe to persist or print: the secret removed, then truncated."""
    text = raw.decode("utf-8", "replace") if isinstance(raw, bytes) else raw
    text = redact(text, secrets).strip()
    if len(text) > DETAIL_LIMIT:
        text = text[:DETAIL_LIMIT] + " … (truncated)"
    # Truncation could cut a secret split across the limit; redact once more.
    return redact(text, secrets)


def api_key_from_environment() -> str:
    key = os.environ.get(API_KEY, "")
    if not key:
        raise AcquisitionError(
            "missing_api_key",
            f"{API_KEY} is not set; set it in this shell only, as docs/historical-data-acquisition.md shows",
        )
    return key


def child_environment(api_key: str) -> dict[str, str]:
    environment = {name: os.environ[name] for name in _CHILD_ENVIRONMENT if os.environ.get(name)}
    environment.setdefault("PATH", "/usr/bin:/bin")
    environment[API_KEY] = api_key
    return environment


def _strict_tool_json(data: bytes, what: str) -> dict[str, Any]:
    """The tool's JSON, with decimals read exactly and no duplicate keys."""

    def no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        document: dict[str, Any] = {}
        for key, value in pairs:
            if key in document:
                raise ValueError(f"{key} appears more than once")
            document[key] = value
        return document

    def no_constant(text: str) -> Any:
        raise ValueError(f"{text} is not a JSON value")

    try:
        document = json.loads(
            data.decode("utf-8"),
            parse_float=Decimal,
            parse_constant=no_constant,
            object_pairs_hook=no_duplicates,
        )
    except (UnicodeDecodeError, ValueError) as error:
        raise AcquisitionError("malformed_tool_output", f"{what} is not one strict JSON document") from error
    if not isinstance(document, dict):
        raise AcquisitionError("malformed_tool_output", f"{what} is not a JSON object")
    return document


def _require_completed(recording: Mapping[str, Any], now: datetime) -> None:
    if instant_ns(recording["end"]) >= int(now.timestamp()) * 10**9:
        raise AcquisitionError(
            "window_not_complete",
            f"recording {recording['stream_id']} ends at {recording['end']}, not yet past",
        )


def _count(value: Any, field: str) -> str:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise AcquisitionError("malformed_tool_output", f"{field} is not a count")
    return str(value)


# --- Quotes: metadata only -------------------------------------------------------------------------------


def _quote_argv(tools: Tools, plan: Mapping[str, Any], recording: Mapping[str, Any]) -> list[str]:
    symbols = [part for symbol in plan["symbols"] for part in ("--symbol", symbol)]
    return [
        str(tools.path(DATABENTO_HISTORICAL)),
        "quote",
        "--start", recording["start"],
        "--end", recording["end"],
        *symbols,
    ]  # fmt: skip


def quote_recording(
    tools: Tools, plan: Mapping[str, Any], recording: Mapping[str, Any], api_key: str
) -> dict[str, Any]:
    """One recording's metadata quote from the C++ tool, read exactly."""
    finished = run_bounded(
        _quote_argv(tools, plan, recording),
        child_environment(api_key),
        QUOTE_TIMEOUT_SECONDS,
        STDOUT_LIMIT,
        STDERR_LIMIT,
    )
    stream = recording["stream_id"]
    if finished.outcome != EXITED or finished.returncode != 0:
        detail = bounded_detail(finished.stderr, [api_key])
        raise AcquisitionError("quote_failed", f"quoting {stream} failed ({finished.outcome}): {detail}")
    document = _strict_tool_json(finished.stdout, f"the quote for {stream}")
    if document.get("query") != query_of(plan, recording):
        raise AcquisitionError("malformed_tool_output", f"the quote for {stream} is for another query")
    quote = document.get("quote")
    if not isinstance(quote, dict):
        raise AcquisitionError("malformed_tool_output", f"the quote for {stream} has no estimate")
    try:
        estimate = parse_estimate(quote.get("estimated_cost_usd"))
    except BudgetError as error:
        raise AcquisitionError("malformed_tool_output", f"the quote for {stream}: {error}") from error
    return {
        "estimated_billable_size_bytes": _count(quote.get("estimated_billable_size_bytes"), "billable bytes"),
        "estimated_cost_usd": str(estimate),
        "estimated_record_count": _count(quote.get("estimated_record_count"), "record count"),
    }


def quote_plan(tools: Tools, document: Any, now: datetime | None = None) -> dict[str, Any]:
    """Every new recording quoted, every existing one verified offline, and a
    proposed budget for a human to accept, change, or refuse."""
    plan = validated_plan(document)
    plan_id = plan_identity(plan)
    now = now or datetime.now(UTC)
    new = [recording for recording in plan["recordings"] if "destination" in recording]
    existing_inputs = []
    for recording in plan["recordings"]:
        if "existing" in recording:
            verified = verify_recording_file(tools, plan, recording, Path(recording["existing"]["path"]))
            existing_inputs.append({"stream_id": recording["stream_id"], **verified.as_json()})
    items: list[dict[str, Any]] = []
    if new:
        api_key = api_key_from_environment()
        for recording in new:
            _require_completed(recording, now)
            estimate = quote_recording(tools, plan, recording, api_key)
            cap = proposed_cap_cents(Decimal(estimate["estimated_cost_usd"]))
            items.append(
                {
                    "estimate": estimate,
                    "proposed_cap_usd": format_cents(cap),
                    "query": query_of(plan, recording),
                    "stream_id": recording["stream_id"],
                }
            )
    total = sum((Decimal(item["estimate"]["estimated_cost_usd"]) for item in items), Decimal(0))
    allowance = sum(parse_cents(item["proposed_cap_usd"], "cap") for item in items)
    return {
        "actual_cost": ACTUAL_COST,
        "estimated_total_usd": str(total),
        "existing_inputs": existing_inputs,
        "items": items,
        "notes": [
            "estimates are the provider SDK's metadata quotes, not charges; streaming is billed on bytes "
            "delivered, and a repeated request is billed again",
            "each proposed cap is 1.25 times its estimate, up to the next cent; the download quotes again "
            "and stops if that fresh estimate exceeds the cap",
            "nothing has been authorized or downloaded; see authorize-acquisition",
        ],
        "plan_id": plan_id,
        "proposed_allowance_usd": format_cents(allowance),
    }


# --- The state root and its lock -------------------------------------------------------------------------


def canonical_root(path: Path) -> Path:
    if not path.is_dir():
        raise AcquisitionError("invalid_state_root", f"the state root {path} is not an existing directory")
    return Path(os.path.realpath(path))


# The state root is trusted and private: owned by its operator and writable
# by no one else. Everything Chronos keeps in it is a real directory or a
# regular file, never a symbolic link (dangling or not), checked under the
# root's lock just before anything is reserved or written, and state files
# are opened without following links. This does not defend against another
# process that can write inside the root and swaps a directory between a
# check and the provider child's own open (the C++ tool takes a path, not a
# directory descriptor); a shared or hostile-writable state root is not
# supported.
NAMESPACES = ("authorizations", "ledgers", "receipts", RECORDINGS_DIRECTORY)


def _unsafe(message: str) -> AcquisitionError:
    return AcquisitionError(
        "unsafe_path",
        f"{message}; a state root holds only real directories and files, private to its owner",
    )


def _require_private_directory(path: Path) -> None:
    try:
        status = path.lstat()
    except FileNotFoundError as error:
        raise _unsafe(f"{path} does not exist") from error
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise _unsafe(f"{path} is a link or not a directory")
    if status.st_uid != os.getuid() or status.st_mode & 0o022:
        raise _unsafe(f"{path} is not owned by this user alone, or others can write to it")


def _private_directory(path: Path, create: bool) -> Path:
    """`path` as a private real directory, made one level, mode 0700, if
    missing and asked for."""
    if create and not os.path.lexists(path):
        with contextlib.suppress(FileExistsError):
            os.mkdir(path, 0o700)
    _require_private_directory(path)
    return path


def _require_regular_file(path: Path) -> None:
    status = path.lstat()
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISREG(status.st_mode):
        raise _unsafe(f"{path} is a link or not a regular file")


def require_safe_state(root: Path, create: bool = False) -> None:
    """The root and every namespace in it that exists (or, with `create`,
    every one) are private real directories; the lock is a regular file."""
    _require_private_directory(root)
    for name in NAMESPACES:
        if create or os.path.lexists(root / name):
            _private_directory(root / name, create)
    if os.path.lexists(root / LOCK):
        _require_regular_file(root / LOCK)


def read_state_file(path: Path) -> bytes:
    """A state file's bytes, never through a link."""
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise _unsafe(f"{path} is a link") from error
        raise
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise _unsafe(f"{path} is not a regular file")
        with os.fdopen(os.dup(descriptor), "rb") as stream:
            return stream.read()
    finally:
        os.close(descriptor)


def safe_destination(root: Path, recording: Mapping[str, Any], create: bool = False) -> Path:
    """The recording's path, with every directory on the way a private real
    directory inside the root (made one level at a time with `create`), and
    the recording and its manifest, where they exist, regular files."""
    parts = str(recording["destination"]).split("/")
    directory = _private_directory(root / RECORDINGS_DIRECTORY, create)
    for part in parts[:-1]:
        directory = directory / part
        if not create and not os.path.lexists(directory):
            # Nothing can exist below a directory that does not.
            return root / RECORDINGS_DIRECTORY / Path(*parts)
        _private_directory(directory, create)
    destination = directory / parts[-1]
    for path in (destination, manifest_path(destination)):
        if os.path.lexists(path):
            _require_regular_file(path)
    return destination


def _authorizations(root: Path) -> Path:
    return root / "authorizations"


def _ledger_path(root: Path, authorization_id: str) -> Path:
    return root / "ledgers" / f"{authorization_id}.ndjson"


def _receipts(root: Path, authorization_id: str) -> Path:
    return root / "receipts" / authorization_id


def _fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


@contextmanager
def root_lock(root: Path) -> Iterator[int]:
    """The root's exclusive lock, refused rather than waited for. Yields the
    descriptor, which a provider child inherits."""
    try:
        descriptor = os.open(root / LOCK, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise _unsafe(f"{root / LOCK} is a link") from error
        raise
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise _unsafe(f"{root / LOCK} is not a regular file")
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            if error.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
                raise AcquisitionError(
                    "busy",
                    "another process holds this state root's acquisition lock: an acquisition, a recovery, "
                    "or a provider child that outlived its parent; wait, or find and stop it",
                ) from error
            raise
        yield descriptor
    finally:
        os.close(descriptor)


# --- Authorization ---------------------------------------------------------------------------------------


def _authorization_document(
    plan_id: str, root: Path, allowance_cents: int, caps: Mapping[str, int]
) -> dict[str, Any]:
    return {
        "allowance_usd": format_cents(allowance_cents),
        "item_caps_usd": {stream: format_cents(cents) for stream, cents in sorted(caps.items())},
        "plan_id": plan_id,
        "rules": [
            "only recordings with a cap may be downloaded, each at most once in this state root",
            "each attempt reserves its full cap before it starts",
            "the reserved caps never exceed the allowance",
            "an attempt that is not a verified publication is never retried or refunded",
            "the allowance bounds approved estimates, not the provider's invoice",
        ],
        "schema": AUTHORIZATION_SCHEMA,
        "state_root": str(root),
    }


def _validated_caps(
    plan: Mapping[str, Any], allowance_text: Any, cap_texts: Sequence[tuple[str, str]]
) -> tuple[int, dict[str, int]]:
    try:
        allowance = parse_cents(allowance_text, "the allowance")
        caps: dict[str, int] = {}
        for stream, text in cap_texts:
            if stream in caps:
                raise AcquisitionError("invalid_authorization", f"{stream} has more than one cap")
            caps[stream] = parse_cents(text, f"the cap for {stream}")
    except BudgetError as error:
        raise AcquisitionError("invalid_authorization", str(error)) from error
    downloadable = {recording["stream_id"] for recording in plan["recordings"] if "destination" in recording}
    if not set(caps) <= downloadable:
        unknown = ", ".join(sorted(set(caps) - downloadable))
        raise AcquisitionError("invalid_authorization", f"caps name no recording to download: {unknown}")
    if any(cents <= 0 for cents in caps.values()):
        raise AcquisitionError(
            "invalid_authorization", "every cap is positive; leave a recording out instead"
        )
    if allowance > MAX_ALLOWANCE_CENTS:
        raise AcquisitionError(
            "invalid_authorization", f"the allowance is at most {format_cents(MAX_ALLOWANCE_CENTS)} here"
        )
    if sum(caps.values()) > allowance:
        raise AcquisitionError("invalid_authorization", "the caps add up to more than the allowance")
    return allowance, caps


def _plan_ids_in(root: Path) -> dict[str, str]:
    """Every published authorization's plan, by authorization ID."""
    directory = _authorizations(root)
    if not directory.is_dir():
        return {}
    found: dict[str, str] = {}
    for entry in sorted(directory.iterdir()):
        if is_artifact_id(entry.name):
            document, _ = verified_authorization(root, entry.name)
            found[entry.name] = document["plan_id"]
    return found


def authorize(
    document: Any,
    typed_plan_id: str,
    state_root: Path,
    allowance_text: str,
    cap_texts: Sequence[tuple[str, str]],
) -> dict[str, Any]:
    """Records one human authorization and initializes its empty ledger."""
    plan = validated_plan(document)
    plan_id = plan_identity(plan)
    if typed_plan_id != plan_id:
        raise AcquisitionError(
            "plan_mismatch", f"the plan file's ID is {plan_id}, not the {typed_plan_id} that was approved"
        )
    root = canonical_root(state_root)
    allowance, caps = _validated_caps(plan, allowance_text, cap_texts)
    authorization = _authorization_document(plan_id, root, allowance, caps)
    authorization_id = sha256_hex(canonical_json(authorization))
    _require_private_directory(root)
    with root_lock(root):
        require_safe_state(root, create=True)
        existing = _plan_ids_in(root)
        if plan_id in existing.values():
            raise AcquisitionError(
                "already_authorized",
                "this state root already holds an authorization for this plan; its ledger is kept as it is",
            )
        _create_ledger(root, authorization_id, plan_id)
        try:
            publish(
                _authorizations(root),
                authorization_id,
                {AUTHORIZATION: canonical_json(authorization), PLAN: canonical_json(plan)},
                AUTHORIZATION,
                lambda directory: (directory / AUTHORIZATION).read_bytes(),
            )
        except PublicationError as error:
            raise AcquisitionError("publication_failed", str(error)) from error
    return {"authorization": authorization, "authorization_id": authorization_id}


def verified_authorization(root: Path, authorization_id: str) -> tuple[dict[str, Any], dict[str, Any]]:
    """A published authorization and its plan, each checked against its
    identity, and bound to this very root."""
    directory = _authorizations(root) / authorization_id
    if not os.path.lexists(directory):
        raise AcquisitionError("unknown_authorization", f"no authorization {authorization_id} here")
    _require_private_directory(directory)
    try:
        names = sorted(os.listdir(directory))
        authorization_bytes = read_state_file(directory / AUTHORIZATION)
        plan_bytes = read_state_file(directory / PLAN)
        authorization = json.loads(authorization_bytes)
        plan = validated_plan(json.loads(plan_bytes))
    except FileNotFoundError as error:
        raise AcquisitionError(
            "unknown_authorization", f"no authorization {authorization_id} here"
        ) from error
    except (OSError, ValueError, RecordingPlanError) as error:
        raise AcquisitionError(
            "state_corrupt", f"authorization {authorization_id} is unreadable: {error}"
        ) from error
    if (
        not isinstance(authorization, dict)
        or names != [AUTHORIZATION, PLAN]
        or sha256_hex(authorization_bytes) != authorization_id
        or canonical_json(authorization) != authorization_bytes
        or canonical_json(plan) != plan_bytes
        or authorization.get("schema") != AUTHORIZATION_SCHEMA
        or authorization.get("plan_id") != plan_identity(plan)
    ):
        raise AcquisitionError(
            "state_corrupt", f"authorization {authorization_id} is not as it was published"
        )
    if authorization["state_root"] != str(root):
        raise AcquisitionError(
            "root_mismatch",
            f"authorization {authorization_id} was given for {authorization['state_root']}, not {root}",
        )
    return authorization, plan


# --- The ledger ------------------------------------------------------------------------------------------


@dataclass
class Attempt:
    number: int
    stream_id: str
    destination: str
    cap_cents: int
    closing: str | None = None
    outcome: str | None = None


_OUTCOMES = ("published", "failed", "ambiguous")
_SAFE_CODE = re.compile(r"[a-z0-9_-]{0,64}")
_MAX_DETAIL = DETAIL_LIMIT + len(" … (truncated)")


def _closing_is_valid(entry: Mapping[str, Any]) -> bool:
    """A finish names a known outcome and a code Chronos writes; a recovery
    is always ambiguous; every detail is bounded text."""
    detail = entry["detail"]
    if not isinstance(detail, str) or len(detail) > _MAX_DETAIL:
        return False
    if entry["entry"] == "recovered":
        return bool(entry["outcome"] == "ambiguous")
    code = entry["error_code"]
    return (
        entry["outcome"] in _OUTCOMES
        and isinstance(code, str)
        and bool(_SAFE_CODE.fullmatch(code))
        and (code == "") == (entry["outcome"] == "published")
    )


def _ledger_line(entry: Mapping[str, Any]) -> bytes:
    line = canonical_json(entry)
    if len(line) > MAX_LEDGER_LINE_BYTES:
        raise AcquisitionError("internal_error", "a ledger entry would exceed its bound")
    return line


def _create_ledger(root: Path, authorization_id: str, plan_id: str) -> None:
    path = _ledger_path(root, authorization_id)
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    except FileExistsError as error:
        raise AcquisitionError(
            "already_authorized", f"a ledger for authorization {authorization_id} already exists"
        ) from error
    try:
        line = _ledger_line(
            {"authorization_id": authorization_id, "entry": "initialized", "plan_id": plan_id}
        )
        os.write(descriptor, line)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    _fsync_directory(path.parent)


class Ledger:
    """An authorization's attempts, replayed from its append-only file and
    held to the authorization and plan it belongs to: every reservation is
    of a capped recording of that plan, at its own destination, for exactly
    its cap, at most once. Anything unexpected refuses rather than guesses."""

    def __init__(
        self, path: Path, authorization: Mapping[str, Any], authorization_id: str, plan: Mapping[str, Any]
    ) -> None:
        self.path = path
        self.authorization_id = authorization_id
        self.allowance_cents = parse_cents(authorization["allowance_usd"], "allowance")
        self.caps = {
            stream: parse_cents(text, stream) for stream, text in authorization["item_caps_usd"].items()
        }
        self.destinations = {
            recording["stream_id"]: recording["destination"]
            for recording in plan["recordings"]
            if "destination" in recording
        }
        self.attempts: list[Attempt] = []
        self._load(authorization["plan_id"])

    def _corrupt(self, message: str) -> AcquisitionError:
        return AcquisitionError(
            "state_corrupt",
            f"the ledger {self.path.name}: {message}; reconcile it by hand, never by deleting it",
        )

    def _load(self, plan_id: str) -> None:
        try:
            data = read_state_file(self.path)
        except FileNotFoundError as error:
            raise AcquisitionError(
                "state_missing",
                f"authorization {self.authorization_id} exists but its ledger does not, "
                "so its attempts are unknown",
            ) from error
        if not data.endswith(b"\n"):
            raise self._corrupt("its last entry is incomplete")
        lines = data.split(b"\n")[:-1]
        if len(lines) > MAX_LEDGER_LINES:
            raise self._corrupt("it has more entries than any plan allows")
        for position, line in enumerate(lines):
            try:
                entry = json.loads(line)
            except ValueError as error:
                raise self._corrupt(f"entry {position + 1} is not JSON") from error
            if not isinstance(entry, dict) or canonical_json(entry) != line + b"\n":
                raise self._corrupt(f"entry {position + 1} is not canonical")
            if position == 0:
                if entry != {
                    "authorization_id": self.authorization_id,
                    "entry": "initialized",
                    "plan_id": plan_id,
                }:
                    raise self._corrupt("it was not initialized for this authorization")
                continue
            self._apply(entry, position + 1)
        if not lines:
            raise self._corrupt("it is empty")

    def _apply(self, entry: Mapping[str, Any], position: int) -> None:
        kind = entry.get("entry")
        number_text = entry.get("attempt")
        if not isinstance(number_text, str) or not number_text.isdigit():
            raise self._corrupt(f"entry {position} names no attempt")
        number = int(number_text)
        if kind == "reserved":
            if set(entry) != {
                "attempt",
                "cap_usd",
                "destination",
                "entry",
                "reserved_total_usd",
                "stream_id",
            }:
                raise self._corrupt(f"entry {position} is not a reservation")
            if number != len(self.attempts) + 1:
                raise self._corrupt(f"entry {position} skips or repeats an attempt number")
            cap = parse_cents(entry["cap_usd"], "cap")
            if parse_cents(entry["reserved_total_usd"], "total") != self.reserved_cents + cap:
                raise self._corrupt(f"entry {position}'s running total does not add up")
            problem = self._reservation_problem(entry["stream_id"], entry["destination"], cap)
            if problem is not None:
                raise self._corrupt(f"entry {position}: {problem}")
            self.attempts.append(Attempt(number, entry["stream_id"], entry["destination"], cap))
            if self.reserved_cents > self.allowance_cents:
                raise self._corrupt("its reservations exceed the allowance")
            return
        if kind not in ("finished", "recovered") or not 1 <= number <= len(self.attempts):
            raise self._corrupt(f"entry {position} is not a known kind of entry for a known attempt")
        attempt = self.attempts[number - 1]
        if attempt.closing is not None:
            raise self._corrupt(f"entry {position} closes attempt {number} twice")
        expected = {"attempt", "detail", "entry", "outcome"} | (
            {"error_code"} if kind == "finished" else set()
        )
        if set(entry) != expected or not _closing_is_valid(entry):
            raise self._corrupt(f"entry {position} is not a valid ending of attempt {number}")
        attempt.closing, attempt.outcome = kind, entry["outcome"]

    def _reservation_problem(self, stream: Any, destination: Any, cap_cents: int) -> str | None:
        """Why a reservation cannot be this authorization's, if it cannot."""
        if stream not in self.caps or self.destinations.get(stream) != destination:
            return f"{stream!r} at {destination!r} is not a recording the authorization caps there"
        if cap_cents != self.caps[stream]:
            cap = format_cents(self.caps[stream])
            return f"{stream} reserves {format_cents(cap_cents)}, not its cap of {cap}"
        if any(
            attempt.stream_id == stream or attempt.destination == destination for attempt in self.attempts
        ):
            return f"{stream} is reserved a second time"
        return None

    @property
    def reserved_cents(self) -> int:
        return sum(attempt.cap_cents for attempt in self.attempts)

    def unfinished(self) -> list[Attempt]:
        return [attempt for attempt in self.attempts if attempt.closing is None]

    def _append(self, entry: Mapping[str, Any]) -> None:
        line = _ledger_line(entry)
        descriptor = os.open(self.path, os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW)
        try:
            os.write(descriptor, line)
            os.fsync(descriptor)
        finally:
            os.close(descriptor)

    def reserve(self, stream: str, destination: str, cap_cents: int) -> Attempt:
        if self.reserved_cents + cap_cents > self.allowance_cents:
            raise AcquisitionError(
                "allowance_exhausted",
                f"reserving {format_cents(cap_cents)} for {stream} would exceed the allowance of "
                f"{format_cents(self.allowance_cents)}; {format_cents(self.reserved_cents)} is reserved",
            )
        if len(self.attempts) >= MAX_SOURCES:
            raise AcquisitionError("allowance_exhausted", "this authorization has made every attempt it may")
        problem = self._reservation_problem(stream, destination, cap_cents)
        if problem is not None:
            raise AcquisitionError("invalid_request", f"refusing to reserve: {problem}")
        attempt = Attempt(len(self.attempts) + 1, stream, destination, cap_cents)
        self._append(
            {
                "attempt": str(attempt.number),
                "cap_usd": format_cents(cap_cents),
                "destination": destination,
                "entry": "reserved",
                "reserved_total_usd": format_cents(self.reserved_cents + cap_cents),
                "stream_id": stream,
            }
        )
        self.attempts.append(attempt)
        return attempt

    def finish(self, attempt: Attempt, outcome: str, error_code: str, detail: str) -> None:
        self._append(
            {
                "attempt": str(attempt.number),
                "detail": detail,
                "entry": "finished",
                "error_code": error_code,
                "outcome": outcome,
            }
        )
        attempt.closing, attempt.outcome = "finished", outcome

    def recover(self, attempt: Attempt, detail: str) -> None:
        self._append(
            {"attempt": str(attempt.number), "detail": detail, "entry": "recovered", "outcome": "ambiguous"}
        )
        attempt.closing, attempt.outcome = "recovered", "ambiguous"


def plan_authorization(root: Path, plan_id: str) -> tuple[str, Ledger] | None:
    """The state root's one authorization for a plan, with its ledger."""
    require_safe_state(root)
    for authorization_id, authorized_plan in _plan_ids_in(root).items():
        if authorized_plan == plan_id:
            authorization, plan = verified_authorization(root, authorization_id)
            return authorization_id, Ledger(
                _ledger_path(root, authorization_id), authorization, authorization_id, plan
            )
    return None


def _ledgers(root: Path) -> dict[str, tuple[dict[str, Any], dict[str, Any], Ledger]]:
    """Every authorization in the root with its plan and ledger. A ledger
    left by an authorization that never published holds no attempt, or the
    root refuses."""
    found: dict[str, tuple[dict[str, Any], dict[str, Any], Ledger]] = {}
    for authorization_id in _plan_ids_in(root):
        authorization, plan = verified_authorization(root, authorization_id)
        found[authorization_id] = (
            authorization,
            plan,
            Ledger(_ledger_path(root, authorization_id), authorization, authorization_id, plan),
        )
    ledgers = root / "ledgers"
    for path in sorted(ledgers.iterdir()) if ledgers.is_dir() else []:
        orphan = path.name.removesuffix(".ndjson")
        if orphan not in found and read_state_file(path).count(b"\n") != 1:
            raise AcquisitionError(
                "state_corrupt", f"the ledger {path.name} has attempts but no authorization"
            )
    return found


# --- Receipts --------------------------------------------------------------------------------------------


def _receipt_path(root: Path, authorization_id: str, stream: str) -> Path:
    return _receipts(root, authorization_id) / f"{stream}.json"


def read_receipt(root: Path, authorization_id: str, stream: str) -> dict[str, Any] | None:
    path = _receipt_path(root, authorization_id, stream)
    if not os.path.lexists(path.parent):
        return None
    _require_private_directory(path.parent)
    try:
        data = read_state_file(path)
    except FileNotFoundError:
        return None
    receipt = json.loads(data)
    if canonical_json(receipt) != data or receipt.get("schema") != RECEIPT_SCHEMA:
        raise AcquisitionError("state_corrupt", f"the receipt for {stream} is not as it was written")
    return dict(receipt)


def check_receipt(
    receipt: Mapping[str, Any],
    authorization_id: str,
    plan: Mapping[str, Any],
    recording: Mapping[str, Any],
    own_attempts: Sequence[Attempt],
    verified: VerifiedRecordingFile,
) -> None:
    """A receipt must agree with the plan, the ledger, and the recording as
    verified now; provenance that was edited is refused, not believed."""
    origin, attempt = receipt.get("origin"), receipt.get("attempt")
    published = [
        str(item.number) for item in own_attempts if item.destination == recording.get("destination")
    ]
    consistent = {
        "existing": "existing" in recording and attempt is None,
        "found_at_destination": "destination" in recording and attempt is None,
        "acquired": "destination" in recording and attempt in published,
    }
    if (
        not consistent.get(str(origin), False)
        or receipt.get("authorization_id") != authorization_id
        or receipt.get("plan_id") != plan_identity(plan)
        or receipt.get("query") != query_of(plan, recording)
        or receipt.get("recording") != verified.as_json()
    ):
        raise AcquisitionError(
            "state_corrupt",
            f"the receipt for {recording['stream_id']} disagrees with the plan, ledger, or file",
        )


def _write_receipt(root: Path, authorization_id: str, receipt: Mapping[str, Any]) -> None:
    directory = _private_directory(_receipts(root, authorization_id), create=True)
    path = _receipt_path(root, authorization_id, receipt["stream_id"])
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        os.write(descriptor, canonical_json(receipt))
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    os.chmod(path, stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)
    _fsync_directory(directory)


def _receipt(
    authorization_id: str,
    plan: Mapping[str, Any],
    recording: Mapping[str, Any],
    verified: VerifiedRecordingFile,
    origin: str,
    attempt: Attempt | None,
    quote: Any,
) -> dict[str, Any]:
    return {
        "actual_cost": ACTUAL_COST if origin == "acquired" else "none: no request was made for it here",
        "attempt": None if attempt is None else str(attempt.number),
        "authorization_id": authorization_id,
        "origin": origin,
        "plan_id": plan_identity(plan),
        "query": query_of(plan, recording),
        "quote_before_download": quote,
        "recording": verified.as_json(),
        "schema": RECEIPT_SCHEMA,
        "stream_id": recording["stream_id"],
        "verified": (
            "size and SHA-256 against the manifest, the manifest against the plan's provider, window, "
            "symbols, symbology, and stream, and the manifest recomputed by chronos_databento_replay"
        ),
    }


# --- Acquisition -----------------------------------------------------------------------------------------


def _download_argv(
    tools: Tools, plan: Mapping[str, Any], recording: Mapping[str, Any], destination: Path, cap_cents: int
) -> list[str]:
    symbols = [part for symbol in plan["symbols"] for part in ("--symbol", symbol)]
    return [
        str(tools.path(DATABENTO_HISTORICAL)),
        "download",
        "--start", recording["start"],
        "--end", recording["end"],
        *symbols,
        "--output", str(destination),
        "--stream-id", recording["stream_id"],
        "--max-cost-usd", cap_argument(cap_cents),
    ]  # fmt: skip


def _quote_text(quote: Any) -> dict[str, str] | None:
    """The fresh quote the download reported, as text; None if malformed."""
    try:
        return {
            "estimated_billable_size_bytes": _count(quote["estimated_billable_size_bytes"], "billable bytes"),
            "estimated_cost_usd": str(parse_estimate(quote["estimated_cost_usd"])),
            "estimated_record_count": _count(quote["estimated_record_count"], "record count"),
        }
    except (AcquisitionError, BudgetError, KeyError, TypeError):
        return None


@dataclass(frozen=True)
class _Classified:
    outcome: str
    error_code: str
    detail: str
    quote: Any = None


def _classify(
    finished: Finished,
    plan: Mapping[str, Any],
    recording: Mapping[str, Any],
    destination: Path,
    secrets: list[str],
) -> _Classified:
    """What a download child's ending shows, from its exit and output alone."""
    if finished.outcome != EXITED:
        return _Classified(
            "ambiguous", finished.outcome, f"the provider tool {finished.outcome.replace('_', ' ')}"
        )
    if finished.returncode == 0:
        try:
            document = _strict_tool_json(finished.stdout, "the download's report")
        except AcquisitionError:
            return _Classified(
                "ambiguous", "malformed_tool_output", "the tool reported success with malformed output"
            )
        expected = (str(destination), str(manifest_path(destination)), query_of(plan, recording))
        if (document.get("recording_path"), document.get("manifest_path"), document.get("query")) != expected:
            return _Classified(
                "ambiguous", "malformed_tool_output", "the tool reported another recording or query"
            )
        return _Classified(
            "published", "", "the tool reported a validated recording", _quote_text(document.get("quote"))
        )
    if finished.returncode == 1:
        try:
            error = _strict_tool_json(finished.stderr, "the download's error")["error"]
            code = error["code"] if isinstance(error, dict) and isinstance(error.get("code"), str) else None
        except (AcquisitionError, KeyError, TypeError):
            code = None
        if code in _TOOL_ERROR_CODES:
            message = bounded_detail(str(error.get("message", "")), secrets)
            when = "before" if code in _BEFORE_REQUEST_CODES else "at or after"
            detail = bounded_detail(
                f"{code} ({when} the billable request, by the tool's code): {message}", secrets
            )
            return _Classified("failed", code, detail)
        if code is not None:
            # Not the tool's own output: its text is never echoed.
            return _Classified(
                "ambiguous", "unrecognized_error_code", "the tool reported an error code it never writes"
            )
    return _Classified(
        "ambiguous",
        f"exit_{finished.returncode}",
        bounded_detail(
            f"the tool exited {finished.returncode}: " + finished.stderr.decode("utf-8", "replace"), secrets
        ),
    )


def _verified_or_none(
    tools: Tools, plan: Mapping[str, Any], recording: Mapping[str, Any], path: Path
) -> tuple[VerifiedRecordingFile | None, str]:
    try:
        return verify_recording_file(tools, plan, recording, path), ""
    except (RecordingVerificationError, PipelineError, OSError) as error:
        return None, str(error)


def _attempts_by_destination(
    ledgers: Mapping[str, tuple[Any, Any, Ledger]],
) -> dict[str, list[tuple[str, Attempt]]]:
    found: dict[str, list[tuple[str, Attempt]]] = {}
    for authorization_id, (_, _, ledger) in ledgers.items():
        for attempt in ledger.attempts:
            found.setdefault(attempt.destination, []).append((authorization_id, attempt))
    return found


def _debris(destination: Path) -> list[str]:
    """Files the C++ tool leaves only mid-download: its temporary recordings."""
    if not destination.parent.is_dir():
        return []
    prefix = destination.name + ".download."
    return sorted(entry.name for entry in destination.parent.iterdir() if entry.name.startswith(prefix))


@dataclass(frozen=True)
class _Run:
    """What one acquisition run works with, under the root's lock."""

    tools: Tools
    root: Path
    authorization_id: str
    plan: Mapping[str, Any]
    ledger: Ledger
    attempts: Mapping[str, list[tuple[str, Attempt]]]
    caps: Mapping[str, int]
    lock: int
    timeout_seconds: int
    now: datetime

    def write_receipt(
        self,
        recording: Mapping[str, Any],
        verified: VerifiedRecordingFile,
        origin: str,
        attempt: Attempt | None = None,
        quote: Any = None,
    ) -> None:
        receipt = _receipt(self.authorization_id, self.plan, recording, verified, origin, attempt, quote)
        _write_receipt(self.root, self.authorization_id, receipt)


def _result(stream: str, outcome: str, **fields: Any) -> dict[str, Any]:
    return {"outcome": outcome, "stream_id": stream, **fields}


def _require_no_unfinished(ledgers: Mapping[str, tuple[Any, Any, Ledger]]) -> None:
    unfinished = [
        (other, attempt) for other, (_, _, ledger) in ledgers.items() for attempt in ledger.unfinished()
    ]
    if unfinished:
        listed = ", ".join(
            f"{other[:12]}… attempt {attempt.number} ({attempt.stream_id})" for other, attempt in unfinished
        )
        raise AcquisitionError(
            "attempt_in_progress",
            f"an attempt was never seen to finish ({listed}); its provider child may still be running. "
            "Confirm it is not, then run recover-acquisition",
        )


def acquire(
    tools: Tools,
    state_root: Path,
    authorization_id: str,
    timeout_seconds: int = DEFAULT_DOWNLOAD_TIMEOUT_SECONDS,
    now: datetime | None = None,
) -> dict[str, Any]:
    """Acquires the authorized recordings that are not yet verified, one
    reserved attempt each, stopping at the first that does not publish."""
    if not 1 <= timeout_seconds <= MAX_DOWNLOAD_TIMEOUT_SECONDS:
        raise AcquisitionError(
            "invalid_request", f"the timeout is 1 to {MAX_DOWNLOAD_TIMEOUT_SECONDS} seconds"
        )
    root = canonical_root(state_root)
    with root_lock(root) as lock:
        require_safe_state(root)
        ledgers = _ledgers(root)
        if authorization_id not in ledgers:
            verified_authorization(root, authorization_id)  # names why: unknown, corrupt, or another root
        _require_no_unfinished(ledgers)
        authorization, plan, ledger = ledgers[authorization_id]
        for recording in plan["recordings"]:
            if "destination" in recording:
                safe_destination(root, recording)
        run = _Run(
            tools=tools,
            root=root,
            authorization_id=authorization_id,
            plan=plan,
            ledger=ledger,
            attempts=_attempts_by_destination(ledgers),
            caps={
                stream: parse_cents(text, stream) for stream, text in authorization["item_caps_usd"].items()
            },
            lock=lock,
            timeout_seconds=timeout_seconds,
            now=now or datetime.now(UTC),
        )
        results: list[dict[str, Any]] = []
        for recording in plan["recordings"]:
            stopped = next((r for r in results if r["outcome"] in ("failed", "ambiguous")), None)
            if stopped is not None:
                detail = f"not attempted: {stopped['stream_id']} did not publish, so this run stopped"
                results.append(_result(recording["stream_id"], "not_attempted", detail=detail))
            else:
                results.append(_acquire_one(run, recording))
    return {
        "actual_cost": ACTUAL_COST,
        "allowance_usd": authorization["allowance_usd"],
        "authorization_id": authorization_id,
        "plan_id": authorization["plan_id"],
        "recordings": results,
        "reserved_usd": format_cents(ledger.reserved_cents),
    }


def _acquire_one(run: _Run, recording: Mapping[str, Any]) -> dict[str, Any]:
    """One recording: verified as it is, left for reconciliation, or, only
    when nothing of it exists and it has a cap, downloaded once."""
    stream = recording["stream_id"]
    if "existing" in recording:
        return _existing_input(run, recording)
    destination = safe_destination(run.root, recording)
    receipt = read_receipt(run.root, run.authorization_id, stream)
    if receipt is not None:
        verified, problem = _verified_or_none(run.tools, run.plan, recording, destination)
        if verified is None:
            return _result(stream, "verification_failed", detail=problem)
        check_receipt(receipt, run.authorization_id, run.plan, recording, run.ledger.attempts, verified)
        return _result(stream, "already_acquired", recording=verified.as_json())
    earlier = run.attempts.get(recording["destination"], [])
    if earlier or destination.exists() or manifest_path(destination).exists():
        return _after_earlier_attempt(run, recording, destination, earlier)
    if debris := _debris(destination):
        return _result(stream, "needs_reconciliation", detail=f"interrupted download files: {debris}")
    if stream not in run.caps:
        return _result(stream, "not_authorized", detail="the authorization gives it no cap")
    return _download(run, recording, destination)


def _existing_input(run: _Run, recording: Mapping[str, Any]) -> dict[str, Any]:
    stream = recording["stream_id"]
    verified, problem = _verified_or_none(run.tools, run.plan, recording, Path(recording["existing"]["path"]))
    if verified is None:
        return _result(stream, "verification_failed", detail=problem)
    receipt = read_receipt(run.root, run.authorization_id, stream)
    if receipt is None:
        run.write_receipt(recording, verified, "existing")
    else:
        check_receipt(receipt, run.authorization_id, run.plan, recording, run.ledger.attempts, verified)
    return _result(stream, "existing_verified", recording=verified.as_json())


def _after_earlier_attempt(
    run: _Run, recording: Mapping[str, Any], destination: Path, earlier: Sequence[tuple[str, Attempt]]
) -> dict[str, Any]:
    """Files at the destination, or an earlier attempt for it: usable only if
    they verify, and never a reason to download again."""
    stream = recording["stream_id"]
    present = destination.exists() or manifest_path(destination).exists()
    verified, problem = (
        _verified_or_none(run.tools, run.plan, recording, destination) if present else (None, "")
    )
    if verified is None:
        listed = ", ".join(
            f"{other[:12]}… attempt {attempt.number}: {attempt.outcome}" for other, attempt in earlier
        )
        parts = [f"earlier attempts {listed}" if listed else "", problem]
        return _result(stream, "needs_reconciliation", detail="; ".join(part for part in parts if part))
    own = [attempt for other, attempt in earlier if other == run.authorization_id]
    run.write_receipt(
        recording, verified, "acquired" if own else "found_at_destination", own[-1] if own else None
    )
    return _result(stream, "verified_at_destination", recording=verified.as_json())


def _download(run: _Run, recording: Mapping[str, Any], destination: Path) -> dict[str, Any]:
    """The one paid attempt: reserved durably, then run, then recorded however
    it ends, even if this process fails while waiting."""
    stream = recording["stream_id"]
    api_key = api_key_from_environment()
    _require_completed(recording, run.now)
    # Its directories made inside the root, then the whole path checked again,
    # immediately before the reservation and the launch.
    safe_destination(run.root, recording, create=True)
    if safe_destination(run.root, recording) != destination or _debris(destination):
        raise _unsafe(f"the destination of {stream} changed while it was being prepared")
    argv = _download_argv(run.tools, run.plan, recording, destination, run.caps[stream])
    attempt = run.ledger.reserve(stream, recording["destination"], run.caps[stream])
    classified = _Classified("ambiguous", "interrupted", "the parent failed while the provider tool ran")
    verified: VerifiedRecordingFile | None = None
    try:
        finished = run_bounded(
            argv,
            child_environment(api_key),
            run.timeout_seconds,
            STDOUT_LIMIT,
            STDERR_LIMIT,
            pass_fds=(run.lock,),
        )
        classified = _classify(finished, run.plan, recording, destination, [api_key])
        if classified.outcome == "published":
            verified, problem = _verified_or_none(run.tools, run.plan, recording, destination)
            if verified is None:
                detail = bounded_detail(f"the output failed verification: {problem}", [api_key])
                classified = _Classified("ambiguous", "verification_failed", detail)
    finally:
        run.ledger.finish(attempt, classified.outcome, classified.error_code, classified.detail)
    if verified is None:
        return _result(stream, classified.outcome, attempt=str(attempt.number), detail=classified.detail)
    run.write_receipt(recording, verified, "acquired", attempt, classified.quote)
    return _result(stream, "acquired", attempt=str(attempt.number), recording=verified.as_json())


def recover(state_root: Path, authorization_id: str, attempt_number: int, confirmed: bool) -> dict[str, Any]:
    """Closes an attempt that was never seen to finish, as ambiguous. Needs
    the root's lock, so it is refused while a provider child still holds it."""
    if not confirmed:
        raise AcquisitionError(
            "confirmation_required",
            "confirm that no provider process for this state root is running (--confirm-no-provider-process)",
        )
    root = canonical_root(state_root)
    with root_lock(root):
        require_safe_state(root)
        ledgers = _ledgers(root)
        if authorization_id not in ledgers:
            raise AcquisitionError("unknown_authorization", f"no authorization {authorization_id} here")
        ledger = ledgers[authorization_id][2]
        if (
            not 1 <= attempt_number <= len(ledger.attempts)
            or ledger.attempts[attempt_number - 1].closing is not None
        ):
            raise AcquisitionError(
                "invalid_request", f"attempt {attempt_number} is not an unfinished attempt"
            )
        attempt = ledger.attempts[attempt_number - 1]
        ledger.recover(
            attempt,
            "closed by the operator, who confirmed no provider process was running; whether the provider "
            "charged for it is unknown, and its destination stays blocked",
        )
    return {
        "attempt": str(attempt.number),
        "authorization_id": authorization_id,
        "outcome": "ambiguous",
        "reserved_usd": format_cents(ledger.reserved_cents),
        "stream_id": attempt.stream_id,
    }
