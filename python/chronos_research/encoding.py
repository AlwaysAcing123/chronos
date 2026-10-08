"""How the evaluation artifact spells numbers and documents.

Like every Chronos artifact, its documents hold no JSON numbers: integers
are canonical decimal strings, and binary64 floats are their shortest
round-trip decimal spelling (Python's repr), so a value read back is the
same float bit for bit. NaN and infinity are refused.
"""

import hashlib
import json
import math
import re

_CANONICAL_INTEGER = re.compile(r"0|-?[1-9][0-9]*")


def encode_float(value: float) -> str:
    """The shortest decimal string that reads back as exactly `value`."""
    if not math.isfinite(value):
        raise ValueError(f"refusing to persist a non-finite value: {value!r}")
    return repr(float(value))


def decode_float(text: str) -> float:
    """Reads a float written by encode_float, refusing any other spelling."""
    value = float(text)
    if not math.isfinite(value) or repr(value) != text:
        raise ValueError(f"not a canonical finite float: {text!r}")
    return value


def encode_integer(value: int) -> str:
    return str(value)


def decode_integer(text: str) -> int:
    if not isinstance(text, str) or not _CANONICAL_INTEGER.fullmatch(text):
        raise ValueError(f"not a canonical decimal integer: {text!r}")
    return int(text)


def _require_no_numbers(document: object, path: str = "") -> None:
    if isinstance(document, bool) or document is None or isinstance(document, str):
        return
    if isinstance(document, dict):
        for key, value in document.items():
            if not isinstance(key, str):
                raise TypeError(f"{path}: object keys must be strings")
            _require_no_numbers(value, f"{path}.{key}")
        return
    if isinstance(document, list):
        for index, value in enumerate(document):
            _require_no_numbers(value, f"{path}[{index}]")
        return
    raise TypeError(f"{path or 'document'}: {type(document).__name__} must be encoded as a string")


def canonical_json(document: object) -> bytes:
    """Keys sorted, no whitespace, one trailing newline, and no numbers."""
    _require_no_numbers(document)
    text = json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    return text.encode("utf-8") + b"\n"


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class StrictJsonError(ValueError):
    """A document is not strict JSON; why is its reason."""

    def __init__(self, reason: str, message: str, field: str | None = None) -> None:
        super().__init__(message)
        self.reason, self.field = reason, field


def _no_duplicates(pairs: list[tuple[str, object]]) -> dict[str, object]:
    document: dict[str, object] = {}
    for key, value in pairs:
        if key in document:
            raise StrictJsonError("duplicate_field", f"{key} appears more than once", key)
        document[key] = value
    return document


def _refuse_number(text: str) -> object:
    raise StrictJsonError("invalid_type", "numbers are written as strings")


def strict_json_object(data: bytes) -> dict[str, object]:
    """One UTF-8 JSON object with no duplicate keys and no numbers, as
    requests and uploaded manifests must be."""
    try:
        document = json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_no_duplicates,
            parse_float=_refuse_number,
            parse_int=_refuse_number,
            parse_constant=_refuse_number,
        )
    except UnicodeDecodeError as error:
        raise StrictJsonError("invalid_json", "the body is not UTF-8") from error
    except StrictJsonError:
        raise
    except ValueError as error:
        raise StrictJsonError("invalid_json", f"the body is not JSON: {error}") from error
    if not isinstance(document, dict):
        raise StrictJsonError("invalid_type", "the body must be one JSON object")
    return document
