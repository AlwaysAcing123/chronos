import math
import struct

import numpy as np
import pytest

from chronos_research.encoding import (
    canonical_json,
    decode_float,
    decode_integer,
    encode_float,
)


def test_floats_round_trip_bit_for_bit() -> None:
    generator = np.random.default_rng(3)
    values = [0.0, -0.0, 1.0, -1.5, 1e-300, 5e-324, 1.7976931348623157e308, 0.1, 1 / 3]
    values += [struct.unpack("<d", generator.bytes(8))[0] for _ in range(2000)]
    for value in values:
        if not math.isfinite(value):
            continue
        text = encode_float(value)
        assert struct.pack("<d", decode_float(text)) == struct.pack("<d", value)
        assert encode_float(decode_float(text)) == text


@pytest.mark.parametrize("value", [math.nan, math.inf, -math.inf])
def test_non_finite_values_are_never_persisted(value: float) -> None:
    with pytest.raises(ValueError, match="non-finite"):
        encode_float(value)


@pytest.mark.parametrize("text", ["nan", "inf", "1.00", "1e0", "+1.0", " 1.0", "0.10"])
def test_only_the_canonical_spelling_is_read(text: str) -> None:
    with pytest.raises(ValueError, match="canonical"):
        decode_float(text)


@pytest.mark.parametrize("text", ["01", "-0", "+1", "1.0", "", " 1"])
def test_integers_have_one_spelling(text: str) -> None:
    with pytest.raises(ValueError, match="canonical"):
        decode_integer(text)
    assert decode_integer("-12") == -12


def test_documents_are_canonical_and_hold_no_numbers() -> None:
    assert (
        canonical_json({"b": ["1", None, True], "a": {"z": "x"}}) == b'{"a":{"z":"x"},"b":["1",null,true]}\n'
    )
    for number in (1, 1.5):
        with pytest.raises(TypeError, match="must be encoded as a string"):
            canonical_json({"value": number})
