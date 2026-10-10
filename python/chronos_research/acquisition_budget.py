"""Money for acquisition decisions: exact, and never rounded upward.

Allowances and caps are whole US cents, written `D.CC` (`0.30`, `12.00`)
and held as integers. A provider's estimate is read from the tool's JSON
as a Decimal, so sums never pass through binary floating point; the
estimate itself is only as precise as the provider SDK's double, and
Chronos claims nothing beyond it.

chronos_databento_historical takes its cost limit as a double (`strtod`).
`cap_argument` writes the largest double that is at most the authorized
cents, as its shortest decimal text, which a correctly rounded `strtod`
reads back exactly: the limit the tool enforces can be one unit in the
last place below the authorization, never above it.
"""

import math
import re
from decimal import Decimal
from fractions import Fraction
from typing import Any

MAX_ALLOWANCE_CENTS = 100_00
# A cap leaves room for the fresh quote the download takes just before it.
CAP_HEADROOM = Fraction(5, 4)
_CENTS = re.compile(r"(0|[1-9][0-9]{0,5})\.([0-9]{2})")
_MAX_ESTIMATE = Decimal(1_000_000)


class BudgetError(ValueError):
    """An amount is not exact money, or a budget does not add up."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(message)
        self.reason = reason


def parse_cents(text: Any, field: str) -> int:
    """`D.CC` US dollars as whole cents."""
    match = _CENTS.fullmatch(text) if isinstance(text, str) else None
    if match is None:
        raise BudgetError("invalid_amount", f"{field} is US dollars with exactly two decimals, such as 0.30")
    return int(match.group(1)) * 100 + int(match.group(2))


def format_cents(cents: int) -> str:
    if cents < 0:
        raise ValueError("money here is never negative")
    return f"{cents // 100}.{cents % 100:02d}"


def parse_estimate(value: Any, field: str = "estimated_cost_usd") -> Decimal:
    """A provider estimate read with parse_float=Decimal: finite, at least zero."""
    if isinstance(value, bool) or not isinstance(value, Decimal | int):
        raise BudgetError("invalid_estimate", f"{field} is not a number")
    estimate = Decimal(value)
    if not estimate.is_finite() or estimate < 0 or estimate > _MAX_ESTIMATE:
        raise BudgetError("invalid_estimate", f"{field} is not a finite, nonnegative estimate")
    return estimate


def proposed_cap_cents(estimate: Decimal) -> int:
    """A cap to propose for one estimate: 1.25 times it, up to the next cent,
    and at least one cent."""
    cents = Fraction(estimate) * 100 * CAP_HEADROOM
    return max(1, math.ceil(cents))


def cap_argument(cents: int) -> str:
    """The --max-cost-usd text whose double is the largest at most `cents`."""
    if not 0 < cents <= MAX_ALLOWANCE_CENTS:
        raise BudgetError("invalid_amount", "a cap is a positive amount within the allowance ceiling")
    authorized = Fraction(cents, 100)
    nearest = float(authorized)
    if Fraction(nearest) > authorized:
        nearest = math.nextafter(nearest, 0.0)
    text = repr(nearest)
    if Fraction(float(text)) > authorized or float(text) != nearest:
        raise AssertionError("the cost-limit text does not read back below the authorization")
    return text
