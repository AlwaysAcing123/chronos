"""The strategy runner's eight options, as research specs freeze them.

A comparison and a walk-forward plan both name every option, as canonical
decimal text. `configuration_problem` checks their structure.
`canonical_configuration` checks every value against what the C++ runner
can hold exactly (money and quantities at 8 decimal places in a signed
64-bit integer, durations as positive 64-bit integers, the threshold in
parts per billion) and gives its canonical text. The research service's
evaluation and walk-forward requests and the walk-forward CLI share it.
"""

import re
from collections.abc import Mapping
from typing import Any

from chronos_research.pipeline import StrategyOptions

INT64_MAX = 2**63 - 1
# Money and quantities: 10^-8 (core/include/chronos/fixed_decimal.hpp).
DECIMAL_PLACES = 8
_DECIMAL = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?")
_UNSIGNED = re.compile(r"0|[1-9][0-9]*")


class ConfigurationError(ValueError):
    """A value the engine cannot hold. `domain` is true for a well-formed
    value outside its domain, false for one that is malformed or out of
    the engine's range."""

    def __init__(self, field: str, reason: str, message: str, domain: bool) -> None:
        super().__init__(message)
        self.field, self.reason, self.message, self.domain = field, reason, message, domain


def canonical_decimal(text: str, field: str, scale: int, positive: bool, nonnegative: bool) -> str:
    """A decimal the engine holds exactly at `scale` places, canonically."""
    if not _DECIMAL.fullmatch(text):
        raise ConfigurationError(field, "invalid_decimal", f"{field} is not a decimal", False)
    whole, _, fraction = text.lstrip("-").partition(".")
    fraction = fraction.rstrip("0")
    if len(fraction) > scale:
        raise ConfigurationError(
            field, "too_many_fractional_digits", f"{field} has more than {scale} decimals", False
        )
    raw = int(whole) * 10**scale + int(fraction.ljust(scale, "0") or "0")
    if text.startswith("-") and raw:
        raw = -raw
    if raw > INT64_MAX or raw < -INT64_MAX - 1:
        raise ConfigurationError(field, "decimal_out_of_range", f"{field} is out of range", False)
    if (positive and raw <= 0) or (nonnegative and raw < 0):
        raise ConfigurationError(
            field, "out_of_domain", f"{field} must be {'positive' if positive else 'nonnegative'}", True
        )
    canonical = whole.lstrip("0") or "0"
    return f"{'-' if raw < 0 else ''}{canonical}{'.' + fraction if fraction else ''}"


def decimal_units(text: str, scale: int = DECIMAL_PLACES) -> int:
    """A canonical decimal as an exact count of 10^-scale units."""
    negative = text.startswith("-")
    whole, _, fraction = text.lstrip("-").partition(".")
    units = int(whole + fraction.ljust(scale, "0"))
    return -units if negative else units


def canonical_integer(text: str, field: str, minimum: int, maximum: int) -> str:
    """A whole number from `minimum` to `maximum`, canonically."""
    if not _UNSIGNED.fullmatch(text):
        raise ConfigurationError(
            field, "invalid_type", f"{field} must be a whole number written as a string", False
        )
    if not minimum <= int(text) <= maximum:
        raise ConfigurationError(field, "out_of_domain", f"{field} must be from {minimum} to {maximum}", True)
    return text


def canonical_configuration(values: Mapping[str, str], prefix: str = "") -> dict[str, str]:
    """Every option checked against the engine's domain, canonically."""
    return {
        "decision_interval_ns": canonical_integer(
            values["decision_interval_ns"], prefix + "decision_interval_ns", 1, INT64_MAX
        ),
        "fee_per_fill": canonical_decimal(
            values["fee_per_fill"], prefix + "fee_per_fill", DECIMAL_PLACES, False, True
        ),
        "latency_ns": canonical_integer(values["latency_ns"], prefix + "latency_ns", 1, INT64_MAX),
        "maximum_order_notional": canonical_decimal(
            values["maximum_order_notional"], prefix + "maximum_order_notional", DECIMAL_PLACES, True, False
        ),
        "maximum_quote_age_ns": canonical_integer(
            values["maximum_quote_age_ns"], prefix + "maximum_quote_age_ns", 1, INT64_MAX
        ),
        "starting_cash": canonical_decimal(
            values["starting_cash"], prefix + "starting_cash", DECIMAL_PLACES, True, False
        ),
        "target_quantity": canonical_decimal(
            values["target_quantity"], prefix + "target_quantity", DECIMAL_PLACES, True, False
        ),
        "threshold_ppb": canonical_integer(
            values["threshold_ppb"], prefix + "threshold_ppb", 0, 1_000_000_000
        ),
    }


CONFIGURATION_KEYS = (
    "decision_interval_ns", "fee_per_fill", "latency_ns", "maximum_order_notional", "maximum_quote_age_ns",
    "starting_cash", "target_quantity", "threshold_ppb",
)  # fmt: skip
WHOLE_NUMBER_KEYS = ("decision_interval_ns", "latency_ns", "maximum_quote_age_ns", "threshold_ppb")


def configuration_problem(configuration: Any) -> str | None:
    """Why a configuration is not the eight options as text, if it is not."""
    if (
        not isinstance(configuration, dict)
        or set(configuration) != set(CONFIGURATION_KEYS)
        or not all(isinstance(value, str) and value for value in configuration.values())
    ):
        return f"configuration has exactly {', '.join(CONFIGURATION_KEYS)}, as strings"
    if not all(configuration[key].isascii() and configuration[key].isdigit() for key in WHOLE_NUMBER_KEYS):
        return f"{', '.join(WHOLE_NUMBER_KEYS)} are whole numbers"
    return None


def strategy_options(configuration: Mapping[str, str]) -> StrategyOptions:
    return StrategyOptions(
        threshold_ppb=int(configuration["threshold_ppb"]),
        target_quantity=configuration["target_quantity"],
        latency_ns=int(configuration["latency_ns"]),
        decision_interval_ns=int(configuration["decision_interval_ns"]),
        starting_cash=configuration["starting_cash"],
        fee_per_fill=configuration["fee_per_fill"],
        maximum_quote_age_ns=int(configuration["maximum_quote_age_ns"]),
        maximum_order_notional=configuration["maximum_order_notional"],
    )
