"""Shared fixtures. Integration tests run the C++ tools, found through
CHRONOS_DATASET (the chronos_dataset executable) and
CHRONOS_SYNTHETIC_DATASET (the test-only chronos_synthetic_dataset). Without
them those tests are skipped, unless CHRONOS_REQUIRE_TOOLS=1, as in CI, which
makes a missing tool a failure.

ONNX tests build their models at test time with the optional onnx package,
and sandbox tests need Docker and the built sandbox image
(`python -m chronos_research onnx-sandbox build`). Each is skipped when
missing, unless CHRONOS_REQUIRE_ONNX_SANDBOX=1, as in CI."""

import os
import shutil
from collections.abc import Callable
from pathlib import Path

import pytest

from chronos_research import pipeline as research_pipeline
from chronos_research.demo import DemoSource, run_demo
from chronos_research.onnx_sandbox import Sandbox
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


def _missing(message: str) -> None:
    if os.environ.get("CHRONOS_REQUIRE_ONNX_SANDBOX") == "1":
        pytest.fail(message)
    pytest.skip(message)


@pytest.fixture(scope="session")
def onnx_available() -> None:
    try:
        import onnx  # noqa: F401
    except ImportError:
        _missing("install the onnx extra (pip install .[onnx]) to build ONNX test models")


@pytest.fixture(scope="session")
def sandbox(onnx_available: None) -> Sandbox:
    configured = Sandbox()
    capability = configured.capability()
    if capability["status"] != "available":
        _missing(f"the ONNX sandbox is unavailable: {capability['detail']}")
    return configured


@pytest.fixture(scope="module")
def demo_artifacts(
    tools: Tools, tmp_path_factory: pytest.TempPathFactory
) -> tuple[research_pipeline.Tools, Path]:
    """The saved demonstration over the synthetic market, for service tests."""
    base = tmp_path_factory.mktemp("demo")
    directory = research_pipeline.Tools(tools.chronos_dataset.parent)
    (base / "events.csv").write_text(synthetic_events(seed=7))
    run_demo(
        directory,
        research_pipeline.ArtifactRoots.under(base / "data"),
        DemoSource("synthetic-stream", synthetic_events=base / "events.csv"),
        150_000,
    )
    return directory, base / "data"


@pytest.fixture
def demo_data(demo_artifacts: tuple[research_pipeline.Tools, Path], tmp_path: Path) -> Path:
    """A private copy of the demonstration's artifact roots."""
    copy = tmp_path / "data"
    shutil.copytree(demo_artifacts[1], copy, symlinks=True)
    return copy
