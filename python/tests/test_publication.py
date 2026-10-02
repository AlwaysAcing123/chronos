import os
import stat
from pathlib import Path

import pytest

from chronos_research.publication import STAGING_PREFIX, Outcome, PublicationError, publish

ID = "a" * 64
FILES = {"manifest.json": b"{}\n", "rows.ndjson": b"1\n2\n"}


def manifest_of(directory: Path) -> bytes:
    if sorted(os.listdir(directory)) != sorted(FILES):
        raise ValueError("not an artifact")
    return (directory / "manifest.json").read_bytes()


def make_writable(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_dir():
            path.chmod(stat.S_IRWXU)


def test_an_artifact_is_published_read_only_and_never_replaced(tmp_path: Path) -> None:
    assert publish(tmp_path, ID, FILES, "manifest.json", manifest_of) == (Outcome.PUBLISHED, None)
    directory = tmp_path / ID
    assert {name: (directory / name).read_bytes() for name in os.listdir(directory)} == FILES
    assert stat.S_IMODE(directory.stat().st_mode) == 0o555
    assert all(stat.S_IMODE((directory / name).stat().st_mode) == 0o444 for name in FILES)
    # The same artifact again is recognized, not rewritten.
    assert publish(tmp_path, ID, FILES, "manifest.json", manifest_of) == (Outcome.ALREADY_PRESENT, None)
    # A different artifact under the same name is refused, and nothing is
    # touched.
    with pytest.raises(PublicationError, match="a different artifact is already published"):
        publish(tmp_path, ID, {**FILES, "manifest.json": b'{"other":"1"}\n'}, "manifest.json", manifest_of)
    assert (directory / "manifest.json").read_bytes() == b"{}\n"
    assert os.listdir(tmp_path) == [ID]
    make_writable(tmp_path)


@pytest.mark.parametrize("kind", ["empty directory", "file", "symlink"])
def test_an_entry_that_is_not_the_artifact_is_left_alone(tmp_path: Path, kind: str) -> None:
    destination = tmp_path / ID
    if kind == "empty directory":
        destination.mkdir()
    elif kind == "file":
        destination.write_bytes(b"x")
    else:
        destination.symlink_to(tmp_path)
    with pytest.raises(PublicationError, match="already exists and is not a valid artifact"):
        publish(tmp_path, ID, FILES, "manifest.json", manifest_of)
    # No staging directory is left behind.
    assert os.listdir(tmp_path) == [ID]
    assert destination.is_symlink() == (kind == "symlink")


def test_a_missing_root_is_refused(tmp_path: Path) -> None:
    with pytest.raises(PublicationError, match="must be an existing directory"):
        publish(tmp_path / "missing", ID, FILES, "manifest.json", manifest_of)
    assert not any(name.startswith(STAGING_PREFIX) for name in os.listdir(tmp_path))
