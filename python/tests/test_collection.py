"""Research collections: independent recordings referenced by their verified
artifacts, in time order, with their provenance as their datasets state it,
and every way a set of recordings can fail to be independent refused."""

import dataclasses
import json
import os
import shutil
from pathlib import Path
from typing import Any

import pytest

from chronos_research import collection
from chronos_research import pipeline as research_pipeline
from chronos_research.collection import (
    CollectionError,
    build_collection,
    collections_root,
    create_collection,
    inspect_collection,
    verified_collection,
)
from chronos_research.encoding import canonical_json
from tests import pipeline as built
from tests.recordings import WEEK, Recording, Recordings, prepare, recording_events


@pytest.fixture(scope="module")
def week(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> Recordings:
    recordings = Recordings(
        artifacts=tmp_path_factory.mktemp("week"), tools=research_pipeline.Tools(tools.chronos_dataset.parent)
    )
    for recording in WEEK:
        prepare(recordings, recording)
    return recordings


@pytest.fixture
def copy(week: Recordings, tmp_path: Path) -> Recordings:
    """The prepared recordings in a root of this test's own."""
    root = tmp_path / "artifacts"
    shutil.copytree(week.artifacts, root, symlinks=True)
    return Recordings(artifacts=root, tools=week.tools, prepared=dict(week.prepared))


def raw_quotes(recording: Recording) -> dict[str, list[int]]:
    """Receive times of each instrument's quotes, from the CSV itself."""
    times: dict[str, list[int]] = {}
    for line in recording_events(recording).splitlines():
        kind, instrument, at, *_ = line.split(",")
        if kind == "quote":
            times.setdefault(instrument, []).append(int(at))
    return times


def raw_labels(artifacts: Path, label_id: str) -> list[dict[str, Any]]:
    return [
        json.loads(line)
        for line in (artifacts / "labels" / label_id / "labels.ndjson").read_text().splitlines()
    ]


def reason_of(error: pytest.ExceptionInfo[CollectionError]) -> str:
    return error.value.reason


def test_a_collection_records_each_verified_recording_in_time_order(copy: Recordings) -> None:
    labels = copy.label_ids(*(recording.stream for recording in reversed(WEEK)))
    declared = {copy.prepared[WEEK[0].stream].label_id: "Monday 2026-09-21, 13:30 UTC, partial"}
    result = create_collection(copy.tools, copy.artifacts, labels, declared)
    manifest = result["collection"]
    assert result["outcome"] == "published"
    assert manifest["evidence"] == "synthetic"
    assert manifest["contract"]["horizon_ns"] == "1000000"
    assert [member["dataset"]["stream"] for member in manifest["members"]] == [r.stream for r in WEEK]
    for recording, member in zip(WEEK, manifest["members"], strict=True):
        prepared = copy.prepared[recording.stream]
        assert member["dataset"]["dataset_id"] == prepared.dataset_id
        assert member["labels"]["artifact_id"] == prepared.label_id
        assert member["provenance"]["evidence"] == "synthetic"
        assert member["provenance"]["recording_sha256"] == {"unavailable": "synthetic_placeholder"}
        assert member["provenance"]["query_coverage"] == {"unavailable": "not_recorded"}
        assert member["coverage"]["completeness"] == "partial"
        quotes = raw_quotes(recording)
        rows = raw_labels(copy.artifacts, prepared.label_id)
        for instrument, times in quotes.items():
            entry = member["instruments"][instrument]
            assert entry["feature_rows"] == str(len(times))
            assert entry["first_available_at_ns"] == str(min(times))
            assert entry["last_available_at_ns"] == str(max(times))
            usable = [row for row in rows if row["instrument"] == instrument and "direction" in row["label"]]
            assert entry["usable_labels"] == str(len(usable))
        everything = [at for times in quotes.values() for at in times]
        assert member["dataset"]["receive_time_ns"] == {
            "earliest": str(min(everything)),
            "latest": str(max(everything)),
        }
    assert manifest["members"][0]["session"] == {"basis": "declared", "label": declared[labels[-1]]}
    assert manifest["members"][1]["session"] == {"unavailable": "not_declared"}
    # Every recording numbers its own events from 0: identities keep the stream.
    first_rows = [raw_labels(copy.artifacts, label_id)[0] for label_id in labels]
    assert {row["ingest_sequence"] for row in first_rows} == {"0"}
    assert len({row["stream"] for row in first_rows}) == len(WEEK)
    again = create_collection(copy.tools, copy.artifacts, labels, declared)
    assert (again["outcome"], again["collection"]) == ("already_present", manifest)
    assert inspect_collection(copy.tools, copy.artifacts, manifest["collection_id"]) == manifest


def test_inspection_reverifies_every_member_from_its_sources(copy: Recordings) -> None:
    labels = copy.label_ids(*(recording.stream for recording in WEEK))
    collection_id = create_collection(copy.tools, copy.artifacts, labels)["collection"]["collection_id"]
    # A member's label rows change after the collection was made.
    rows = copy.artifacts / "labels" / labels[1] / "labels.ndjson"
    rows.parent.chmod(0o755)
    rows.chmod(0o644)
    rows.write_text(rows.read_text().replace('"direction":"up"', '"direction":"down"', 1))
    with pytest.raises(CollectionError) as refused:
        inspect_collection(copy.tools, copy.artifacts, collection_id)
    assert reason_of(refused) == "source_invalid"
    # And the collection itself, edited.
    directory = collections_root(copy.artifacts) / collection_id
    directory.chmod(0o755)
    manifest = directory / "manifest.json"
    manifest.chmod(0o644)
    manifest.write_text(manifest.read_text().replace('"partial"', '"complete"', 1))
    with pytest.raises(CollectionError) as edited:
        verified_collection(collections_root(copy.artifacts), collection_id)
    assert reason_of(edited) == "artifact_invalid"


def test_a_collection_names_the_members_only_once(copy: Recordings) -> None:
    label = copy.prepared[WEEK[0].stream].label_id
    with pytest.raises(CollectionError) as twice:
        build_collection(copy.tools, copy.artifacts, [label, label])
    assert reason_of(twice) == "duplicate_member"
    # The same dataset labelled at another horizon is the same recording.
    other = research_pipeline.extract_labels(
        copy.tools,
        research_pipeline.ArtifactRoots.under(copy.artifacts),
        copy.prepared[WEEK[0].stream].feature_id,
        2_000_000,
    ).artifact_id
    with pytest.raises(CollectionError) as same_dataset:
        build_collection(copy.tools, copy.artifacts, [label, other])
    assert reason_of(same_dataset) == "duplicate_dataset"


def test_recordings_that_are_not_independent_or_comparable_are_refused(copy: Recordings) -> None:
    first = copy.prepared[WEEK[0].stream].label_id
    # Another recording claiming the same stream, on another day.
    renamed = prepare(copy, Recording(WEEK[0].stream, day=5, quotes=200, seed=31))
    with pytest.raises(CollectionError) as stream:
        build_collection(copy.tools, copy.artifacts, [first, renamed.label_id])
    assert reason_of(stream) == "duplicate_stream"
    # A different stream on the same day, overlapping in time.
    overlapping = prepare(copy, Recording("synthetic-overlap", day=0, quotes=200, seed=32))
    with pytest.raises(CollectionError) as overlap:
        build_collection(copy.tools, copy.artifacts, [first, overlapping.label_id])
    assert reason_of(overlap) == "overlapping_sources"
    # Another horizon.
    later = prepare(copy, Recording("synthetic-horizon", day=6, quotes=200, seed=33), horizon_ns=2_000_000)
    with pytest.raises(CollectionError) as horizon:
        build_collection(copy.tools, copy.artifacts, [first, later.label_id])
    assert reason_of(horizon) == "inconsistent_contract"


def tie_events(start_ns: int, stream_quotes: int = 40) -> str:
    lines = []
    for step in range(stream_quotes):
        mid = 10_000 + (step % 3) - 1
        lines.append(
            f"quote,AAPL,{start_ns + step * 300_000},{(mid - 1) / 100:.2f},5,{(mid + 1) / 100:.2f},7"
        )
    return "\n".join(lines) + "\n"


def prepare_events(recordings: Recordings, stream: str, events: str) -> str:
    roots = research_pipeline.ArtifactRoots.under(recordings.artifacts)
    path = recordings.artifacts / f"{stream}.csv"
    path.write_text(events)
    dataset = research_pipeline.synthesize_dataset(recordings.tools, path, stream, roots.catalog)
    features = research_pipeline.extract_features(recordings.tools, roots, dataset.artifact_id)
    return research_pipeline.extract_labels(
        recordings.tools, roots, features.artifact_id, 1_000_000
    ).artifact_id


def test_a_recording_beginning_exactly_where_another_ends_is_refused_and_one_nanosecond_later_is_not(
    copy: Recordings,
) -> None:
    first = prepare_events(copy, "synthetic-tie-a", tie_events(10**18))
    last = json.loads((copy.artifacts / "labels" / first / "labels.ndjson").read_text().splitlines()[-1])
    end = int(last["prediction_at_ns"])
    tied = prepare_events(copy, "synthetic-tie-b", tie_events(end))
    with pytest.raises(CollectionError) as refused:
        build_collection(copy.tools, copy.artifacts, [first, tied])
    assert reason_of(refused) == "overlapping_sources"
    after = prepare_events(copy, "synthetic-tie-c", tie_events(end + 1))
    members = build_collection(copy.tools, copy.artifacts, [after, first])["members"]
    assert [member["dataset"]["stream"] for member in members] == ["synthetic-tie-a", "synthetic-tie-c"]


def provenance_variant(recording: Any, **source: Any) -> Any:
    manifest = json.loads(json.dumps(recording.dataset_manifest))
    manifest["source"].update(source)
    return dataclasses.replace(recording, dataset_manifest=manifest)


def test_aliased_recordings_and_mixtures_are_refused_where_provenance_shows_them(copy: Recordings) -> None:
    """The rule on provenance as a provider's dataset states it: the
    synthetic generator cannot produce an observed recording, so its
    verified recordings are given an observed provider's source block."""
    recordings = collection.verified_members(
        copy.tools, copy.artifacts, copy.label_ids(WEEK[0].stream, WEEK[1].stream)
    )
    observed = {"provider": "databento", "provider_dataset": "EQUS.MINI", "provider_schema": "mbp-1"}
    same = [provenance_variant(recording, **observed, recording_sha256="ab" * 32) for recording in recordings]
    with pytest.raises(CollectionError) as aliased:
        collection._collection(same, {})
    assert reason_of(aliased) == "aliased_recording"
    distinct = [
        provenance_variant(recording, **observed, recording_sha256=digit * 64)
        for recording, digit in zip(recordings, "12", strict=True)
    ]
    assert collection._collection(distinct, {})[0]["evidence"] == "observed"
    mixed = [recordings[0], distinct[1]]
    with pytest.raises(CollectionError) as mixture:
        collection._collection(mixed, {})
    assert reason_of(mixture) == "unsupported_mixture"


@pytest.mark.parametrize(
    ("labels", "declared"),
    [
        ([], {}),
        (["a" * 64] * 2, {}),
        ([f"{index:064x}" for index in range(collection.MAX_SOURCES + 1)], {}),
        (["not-an-id"], {}),
        (["a" * 64], {"b" * 64: "not a member"}),
        (["a" * 64], {"a" * 64: "line\nbreak"}),
        (["a" * 64], {"a" * 64: "x" * (collection.MAX_SESSION_LABEL + 1)}),
    ],
)
def test_malformed_requests_are_refused_before_anything_is_read(
    copy: Recordings, labels: list[str], declared: dict[str, str]
) -> None:
    with pytest.raises(CollectionError) as refused:
        build_collection(copy.tools, copy.artifacts, labels, declared)
    assert reason_of(refused) in ("invalid_request", "duplicate_member")


def test_a_source_over_the_row_bound_is_refused(copy: Recordings, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(collection, "MAX_FEATURE_ROWS_PER_SOURCE", 10)
    with pytest.raises(CollectionError) as refused:
        build_collection(copy.tools, copy.artifacts, copy.label_ids(WEEK[0].stream))
    assert reason_of(refused) == "source_too_large"


def test_symlinks_paths_and_collisions(copy: Recordings, tmp_path: Path) -> None:
    labels = copy.label_ids(*(recording.stream for recording in WEEK))
    manifest = build_collection(copy.tools, copy.artifacts, labels)
    root = collections_root(copy.artifacts)
    root.mkdir()
    # Something else already has the collection's name: refused, untouched.
    squatter = root / manifest["collection_id"]
    squatter.mkdir()
    (squatter / "manifest.json").write_text("{}\n")
    with pytest.raises(CollectionError) as collision:
        create_collection(copy.tools, copy.artifacts, labels)
    assert reason_of(collision) == "publication_failed"
    assert (squatter / "manifest.json").read_text() == "{}\n"
    # A collection reached through a symlink is not read.
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    real = elsewhere / manifest["collection_id"]
    real.mkdir()
    (real / "manifest.json").write_bytes(canonical_json(manifest))
    shutil.rmtree(squatter)
    os.symlink(real, squatter)
    with pytest.raises(CollectionError) as linked:
        verified_collection(root, manifest["collection_id"])
    assert reason_of(linked) == "artifact_invalid"
    with pytest.raises(CollectionError) as escape:
        verified_collection(root, "../" + manifest["collection_id"][3:])
    assert reason_of(escape) == "artifact_invalid"
