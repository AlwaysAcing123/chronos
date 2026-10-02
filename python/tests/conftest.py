"""Shared fixtures. Integration tests run the C++ tools, found through
CHRONOS_DATASET (the chronos_dataset executable) and
CHRONOS_SYNTHETIC_DATASET (the test-only chronos_synthetic_dataset). Without
them those tests are skipped, unless CHRONOS_REQUIRE_TOOLS=1, as in CI, which
makes a missing tool a failure."""

import os
from collections.abc import Callable
from pathlib import Path

import pytest

from tests.pipeline import Pipeline, Tools, build_pipeline
from tests.synthetic import synthetic_events


@pytest.fixture(scope="session")
def tools() -> Tools:
    dataset = os.environ.get("CHRONOS_DATASET")
    synthetic = os.environ.get("CHRONOS_SYNTHETIC_DATASET")
    if not dataset or not synthetic:
        message = "set CHRONOS_DATASET and CHRONOS_SYNTHETIC_DATASET to the built C++ tools"
        if os.environ.get("CHRONOS_REQUIRE_TOOLS") == "1":
            pytest.fail(message)
        pytest.skip(message)
    return Tools(chronos_dataset=Path(dataset), synthetic_dataset=Path(synthetic))


@pytest.fixture(scope="session")
def pipeline(tools: Tools, tmp_path_factory: pytest.TempPathFactory) -> Pipeline:
    """One synthetic market taken from events to a split artifact, shared
    by the tests that only read it."""
    return build_pipeline(tmp_path_factory.mktemp("pipeline"), tools, synthetic_events(seed=7))


@pytest.fixture
def make_pipeline(tools: Tools, tmp_path_factory: pytest.TempPathFactory) -> Callable[..., Pipeline]:
    def make(events: str, **options: object) -> Pipeline:
        return build_pipeline(tmp_path_factory.mktemp("pipeline"), tools, events, **options)  # type: ignore[arg-type]

    return make
