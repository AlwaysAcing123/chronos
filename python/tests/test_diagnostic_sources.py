"""The digests a diagnostics read reuses (diagnostic_sources.ContentDigests):
a file's SHA-256 is reused only while the file is unchanged by every measure
the file system keeps, and never through a symlink."""

import hashlib
import os
from pathlib import Path

import pytest

from chronos_research.artifact_files import ArtifactUnavailableError
from chronos_research.diagnostic_sources import ContentDigests


def _listed(path: Path) -> dict[str, str]:
    data = path.read_bytes()
    return {"name": path.name, "sha256": hashlib.sha256(data).hexdigest(), "size_bytes": str(len(data))}


def test_a_digest_is_reused_only_while_the_file_is_unchanged(tmp_path: Path) -> None:
    directory = tmp_path / "root" / ("a" * 64)
    directory.mkdir(parents=True)
    rows = directory / "rows.ndjson"
    rows.write_bytes(b'{"raw":"1"}\n')
    listed = _listed(rows)
    digests = ContentDigests()
    assert digests.verify(directory, listed) is False
    assert digests.verify(directory, listed) is True
    # The same size, and the modification time put back: the change time
    # still differs, so the file is read again and refused.
    status = rows.stat()
    rows.write_bytes(b'{"raw":"2"}\n')
    os.utime(rows, ns=(status.st_atime_ns, status.st_mtime_ns))
    with pytest.raises(ArtifactUnavailableError, match="size or checksum"):
        digests.verify(directory, listed)
    rows.write_bytes(b'{"raw":"1"}\n')
    assert digests.verify(directory, listed) is False
    # A symlink is never followed, and a missing file is refused.
    rows.unlink()
    (tmp_path / "elsewhere.ndjson").write_bytes(b'{"raw":"1"}\n')
    rows.symlink_to(tmp_path / "elsewhere.ndjson")
    with pytest.raises(ArtifactUnavailableError, match="not a regular file"):
        digests.verify(directory, listed)
    rows.unlink()
    with pytest.raises(ArtifactUnavailableError, match="does not exist"):
        digests.verify(directory, listed)
    with pytest.raises(ArtifactUnavailableError, match="single filename"):
        digests.verify(directory, {**listed, "name": "../rows.ndjson"})
