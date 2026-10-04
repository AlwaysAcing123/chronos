"""Keyset pages of the research service's growing lists: following a cursor
chain never repeats or skips an entry, even while entries are added."""

from pathlib import Path

import pytest

from chronos_research.service import (
    ApiError,
    JobStore,
    encode_cursor,
    page_after_id,
    page_before_sequence,
)


def finish_all(store: JobStore) -> None:
    for job in store.newest_first():
        job.state = "succeeded"
        store.save(job)


def test_job_pages_continue_with_older_jobs_while_new_ones_arrive(tmp_path: Path) -> None:
    store = JobStore(tmp_path / "jobs", retain=500)
    for number in range(60):
        store.create({"n": str(number)}, f"key-{number}")
    finish_all(store)
    first = page_before_sequence(store.newest_first(), 50, None)
    assert [job.sequence for job in first.items] == list(range(60, 10, -1))
    assert first.next_cursor is not None
    # Five jobs arrive between the pages; with an offset cursor the second
    # page would repeat five of the first.
    for number in range(60, 65):
        store.create({"n": str(number)}, f"key-{number}")
    second = page_before_sequence(store.newest_first(), 50, first.next_cursor)
    assert [job.sequence for job in second.items] == list(range(10, 0, -1))
    assert second.next_cursor is None
    seen = [job.job_id for job in (*first.items, *second.items)]
    assert len(seen) == len(set(seen)) == 60
    # Reading from the start again shows the new jobs first.
    assert [job.sequence for job in page_before_sequence(store.newest_first(), 5, None).items] == [
        65,
        64,
        63,
        62,
        61,
    ]


def test_artifact_pages_continue_after_their_last_id_while_artifacts_arrive() -> None:
    ids = sorted(f"{number:064x}" for number in range(0, 240, 2))
    first = page_after_id(ids, 50, None, "results")
    assert first.items == ids[:50]
    # New artifacts land before and after the cursor's ID.
    grown = sorted([*ids, f"{1:064x}", f"{99:064x}", f"{301:064x}"])
    second = page_after_id(grown, 50, first.next_cursor, "results")
    third = page_after_id(grown, 50, second.next_cursor, "results")
    chain = [*first.items, *second.items, *third.items]
    assert len(chain) == len(set(chain))
    # Every artifact that existed throughout appears exactly once; ones added
    # behind the cursor appear only when the list is read again.
    assert set(ids) <= set(chain)
    assert f"{1:064x}" not in chain
    assert f"{99:064x}" in chain
    assert f"{301:064x}" in chain
    assert third.next_cursor is None


def test_keyset_cursors_are_bound_to_their_list_and_version() -> None:
    ids = [f"{number:064x}" for number in range(3)]
    cursor = page_after_id(ids, 1, None, "experiments").next_cursor
    assert cursor is not None
    with pytest.raises(ApiError, match="cursor"):
        page_after_id(ids, 1, cursor, "results")
    # An offset cursor of the earlier version is refused, not reinterpreted.
    with pytest.raises(ApiError, match="cursor"):
        page_after_id(ids, 1, encode_cursor("experiments", "1"), "experiments")
    with pytest.raises(ApiError, match="cursor"):
        page_before_sequence([], 1, encode_cursor("runs", "1"))
