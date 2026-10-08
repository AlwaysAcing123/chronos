"""The model library: immutable model versions registered from verified
experiments, their names, lifecycle, and verification on every read."""

import hashlib
import json
import os
import shutil
import stat
import threading
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from chronos_research.demo import DemoSource, run_demo
from chronos_research.encoding import canonical_json
from chronos_research.experiment import MANIFEST
from chronos_research.inspection import verified_experiment
from chronos_research.pipeline import ArtifactRoots, Tools, experiment_split, verified_sources
from chronos_research.publication import Outcome
from chronos_research.registry import (
    PARAMETERS,
    NameVersionTakenError,
    RegistryError,
    RegistryRoots,
    bind_name,
    experiment_model_version,
    lifecycle,
    model_version_ids,
    names_by_version,
    publish_model_version,
    register_experiment_model,
    set_lifecycle,
    source_experiment_status,
    verified_model_version,
    version_identity,
)
from tests import pipeline as built
from tests.synthetic import synthetic_events


@pytest.fixture(scope="module")
def demo(tools: built.Tools, tmp_path_factory: pytest.TempPathFactory) -> tuple[Tools, Path, str]:
    """The saved demonstration's artifacts, with its one experiment."""
    base = tmp_path_factory.mktemp("registry")
    directory = Tools(tools.chronos_dataset.parent)
    (base / "events.csv").write_text(synthetic_events(seed=7))
    run_demo(
        directory,
        ArtifactRoots.under(base / "data"),
        DemoSource("synthetic-stream", synthetic_events=base / "events.csv"),
        150_000,
    )
    (experiment,) = os.listdir(base / "data" / "experiments")
    return directory, base / "data", experiment


def tree_digest(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        digest.update(str(path.relative_to(root)).encode())
        if path.is_file():
            digest.update(path.read_bytes())
    return digest.hexdigest()


def register(demo: tuple[Tools, Path, str], data: Path, family: str, name: str, version: str = "1") -> str:
    tools, _, experiment = demo
    roots = ArtifactRoots.under(data)
    sources = verified_sources(tools, roots, experiment_split(roots.experiments, experiment), True)
    registration = register_experiment_model(
        RegistryRoots.under(data), roots.experiments, sources, experiment, "AAPL", family, name, version
    )
    return registration.model_version_id


@pytest.fixture
def data(demo: tuple[Tools, Path, str], tmp_path: Path) -> Path:
    """A private copy of the demonstration's artifact roots."""
    copy = tmp_path / "data"
    shutil.copytree(demo[1], copy, symlinks=True)
    return copy


def test_both_families_register_with_their_verified_provenance(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    experiments = data / "experiments"
    before = tree_digest(experiments)
    roots = RegistryRoots.under(data)
    experiment, models = verified_experiment(experiments / demo[2])
    for family in ("logistic", "majority"):
        version = verified_model_version(roots, register(demo, data, family, f"aapl-{family}"))
        manifest = version.manifest
        assert manifest["family"] == family
        assert manifest["instrument"] == "AAPL"
        assert version.payload == canonical_json(models["AAPL"][family].as_json())
        provenance = manifest["provenance"]
        assert provenance["origin"] == "experiment"
        assert provenance["experiment_id"] == demo[2]
        assert provenance["sources"] == experiment["sources"]
        assert provenance["code"] == {
            "unavailable": "an experiment records its library versions, not a source revision"
        }
        cutoff = manifest["training"]["information_cutoff"]
        assert cutoff["basis"] == "verified"
        assert cutoff["at_ns"] == experiment["sources"]["validation_start_at_ns"]
        majority = models["AAPL"]["majority"]
        assert manifest["training"]["rows"] == str(sum(majority.class_counts))  # type: ignore[union-attr]
        assert set(manifest["verification"]) == {"cutoff", "experiment", "payload", "sources"}
        assert source_experiment_status(experiments, version) == {
            "experiment_id": demo[2],
            "state": "matches",
        }
    # Registration reads experiments and never writes them.
    assert tree_digest(experiments) == before
    assert sorted(names_by_version(roots).values(), key=str) == sorted(
        [[{"name": "aapl-logistic", "version": "1"}], [{"name": "aapl-majority", "version": "1"}]], key=str
    )


def test_registration_is_deterministic_idempotent_and_names_the_schedules_parameters(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    first = register(demo, data, "logistic", "aapl-logistic")
    tools, _, experiment = demo
    artifacts = ArtifactRoots.under(data)
    sources = verified_sources(tools, artifacts, experiment_split(artifacts.experiments, experiment), True)
    again = register_experiment_model(
        RegistryRoots.under(data),
        artifacts.experiments,
        sources,
        experiment,
        "AAPL",
        "logistic",
        "aapl-logistic",
        "1",
    )
    assert again.model_version_id == first
    assert (again.outcome, again.binding_outcome) == ("already_present", "already_present")
    # Another name for the same bytes is the same version.
    assert register(demo, data, "logistic", "aapl-logistic-copy", "2.0.1") == first
    payload = verified_model_version(RegistryRoots.under(data), first).manifest["payload"]["sha256"]
    for schedule in (data / "schedules").iterdir():
        model = json.loads((schedule / "manifest.json").read_bytes())["model"]
        if model["instrument"] == "AAPL" and model["name"] == "logistic":
            assert model["parameters_sha256"] == payload


def test_a_name_and_version_never_points_to_different_bytes(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    logistic = register(demo, data, "logistic", "aapl-model")
    binding_files = sorted((data / "model-names").rglob("*"))
    before = [path.read_bytes() for path in binding_files if path.is_file()]
    with pytest.raises(NameVersionTakenError, match=logistic):
        register(demo, data, "majority", "aapl-model")
    after = [path.read_bytes() for path in sorted((data / "model-names").rglob("*")) if path.is_file()]
    assert after == before
    # The name is checked before anything is published: no unnamed version.
    assert model_version_ids(RegistryRoots.under(data)) == [logistic]


def prepared(demo: tuple[Tools, Path, str], family: str) -> tuple[dict[str, Any], bytes]:
    tools, data, experiment = demo
    artifacts = ArtifactRoots.under(data)
    sources = verified_sources(tools, artifacts, experiment_split(artifacts.experiments, experiment), True)
    manifest, models = verified_experiment(artifacts.experiments / experiment)
    majority = models["AAPL"]["majority"]
    return experiment_model_version(
        manifest,
        sources,
        "AAPL",
        family,
        models["AAPL"][family],  # type: ignore[arg-type]
        majority.class_counts,  # type: ignore[union-attr]
    )


def concurrently(count: int, work: Callable[[], object]) -> list[object]:
    barrier = threading.Barrier(count)
    results: list[object] = [None] * count

    def run(index: int) -> None:
        barrier.wait()
        try:
            results[index] = work()
        except Exception as error:
            results[index] = error

    threads = [threading.Thread(target=run, args=(index,)) for index in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return results


def test_concurrent_publication_and_binding_have_one_winner(
    demo: tuple[Tools, Path, str], tmp_path: Path
) -> None:
    roots = RegistryRoots.under(tmp_path)
    manifest, payload = prepared(demo, "logistic")
    outcomes = concurrently(8, lambda: publish_model_version(roots, manifest, payload)[0])
    assert sorted(outcome.value for outcome in outcomes) == ["already_present"] * 7 + ["published"]  # type: ignore[attr-defined]
    assert not [name for name in os.listdir(roots.models) if name.startswith(".chronos-staging-")]
    verified_model_version(roots, manifest["model_version_id"])

    other, other_payload = prepared(demo, "majority")
    publish_model_version(roots, other, other_payload)
    candidates = [manifest["model_version_id"], other["model_version_id"]] * 4
    index = iter(range(8))
    lock = threading.Lock()

    def bind() -> object:
        with lock:
            target = candidates[next(index)]
        return bind_name(roots, "contested", "1", target)[1]

    results = concurrently(8, bind)
    winners = [result for result in results if result is Outcome.PUBLISHED]
    assert len(winners) == 1
    bound = names_by_version(roots)
    (winner,) = [
        version for version, names in bound.items() if {"name": "contested", "version": "1"} in names
    ]
    for result in results:
        assert result in (Outcome.PUBLISHED, Outcome.ALREADY_PRESENT) or (
            isinstance(result, NameVersionTakenError) and result.existing == winner
        )


def writable(directory: Path) -> None:
    os.chmod(directory, stat.S_IRWXU)
    for path in directory.iterdir():
        os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)


def edited_version(
    roots: RegistryRoots,
    version_id: str,
    edit: Callable[[dict[str, Any], dict[str, bytes]], None],
    into: Path,
    resign: bool = True,
) -> tuple[RegistryRoots, str]:
    """A copy of a version, in a registry of its own under `into`, with its
    manifest or payload edited and re-signed so the ID matches unless
    resign is false."""
    source = roots.models / version_id
    manifest = json.loads((source / MANIFEST).read_bytes())
    files = {name: (source / name).read_bytes() for name in os.listdir(source) if name != MANIFEST}
    edit(manifest, files)
    if resign:
        manifest["model_version_id"] = version_identity(manifest)
    target_id = manifest["model_version_id"] if resign else "f" * 64
    target = into / "models" / target_id
    target.mkdir(parents=True)
    try:
        manifest_bytes = canonical_json(manifest)
    except TypeError:
        # A JSON number: spelled as plain JSON, for the registry to refuse.
        manifest_bytes = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode() + b"\n"
    (target / MANIFEST).write_bytes(manifest_bytes)
    for name, contents in files.items():
        (target / name).write_bytes(contents)
    return RegistryRoots.under(into), target_id


def resign_payload(manifest: dict[str, Any], files: dict[str, bytes]) -> None:
    payload = files[PARAMETERS]
    manifest["payload"] = {
        "name": PARAMETERS,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "size_bytes": str(len(payload)),
    }


def test_malformed_manifests_and_corrupt_payloads_are_refused(
    demo: tuple[Tools, Path, str], data: Path, tmp_path: Path
) -> None:
    roots = RegistryRoots.under(data)
    version_id = register(demo, data, "logistic", "aapl-logistic")

    def payload_document(
        edit: Callable[[dict[str, Any]], None],
    ) -> Callable[[dict[str, Any], dict[str, bytes]], None]:
        def apply(manifest: dict[str, Any], files: dict[str, bytes]) -> None:
            document = json.loads(files[PARAMETERS])
            edit(document)
            files[PARAMETERS] = canonical_json(document)
            resign_payload(manifest, files)

        return apply

    def replace_payload(old: bytes, new: bytes) -> Callable[[dict[str, Any], dict[str, bytes]], None]:
        def apply(manifest: dict[str, Any], files: dict[str, bytes]) -> None:
            assert old in files[PARAMETERS]
            files[PARAMETERS] = files[PARAMETERS].replace(old, new, 1)
            resign_payload(manifest, files)

        return apply

    first_mean = json.loads((roots.models / version_id / PARAMETERS).read_bytes())["scaler"]["mean"][0]
    cases: list[tuple[Callable[[dict[str, Any], dict[str, bytes]], None], str]] = [
        (lambda m, f: m.update(extra="1"), "must have exactly the keys"),
        (
            lambda m, f: m.update(schema={"name": "chronos.model_version", "version": "2"}),
            "unsupported model version schema",
        ),
        (lambda m, f: m.update(family="majority"), "format or runtime contract"),
        (lambda m, f: m["inputs"]["fields"].reverse(), "input or output contract"),
        (lambda m, f: m["output"].update(class_order=["up", "flat", "down"]), "input or output contract"),
        (lambda m, f: m.update(instrument="../AAPL"), "valid instrument identifier"),
        (
            lambda m, f: m["training"]["information_cutoff"].update(basis="declared"),
            "verified split's validation start",
        ),
        (
            lambda m, f: m["training"].update(
                labels_available_through_ns=m["training"]["information_cutoff"]["at_ns"]
            ),
            "at or after the information cutoff",
        ),
        (lambda m, f: m["provenance"].update(experiment_id="not-an-id"), "64-digit lowercase ID"),
        (lambda m, f: m["provenance"]["sources"].update(horizon_ns="1"), "target horizon differs"),
        (lambda m, f: m.update(verification={}), "must state what was verified"),
        (lambda m, f: m["payload"].update(name="../parameters.json"), "unexpected file or checksum"),
        (
            lambda m, f: f.update({PARAMETERS: f[PARAMETERS] + b" "}),
            "differs from the manifest's checksum or size",
        ),
        (replace_payload(b'","', b'", "'), "not in canonical form"),
        (replace_payload(f'"{first_mean}"'.encode(), b'"inf"'), "not a canonical finite float"),
        (payload_document(lambda d: d["scaler"]["scale"].__setitem__(0, "-1.0")), "parameters are malformed"),
        (payload_document(lambda d: d.update(status="unevaluable", reason="x", detail={})), "keys"),
    ]
    for number, (edit, message) in enumerate(cases):
        case_roots, edited = edited_version(roots, version_id, edit, tmp_path / f"case-{number}")
        with pytest.raises(RegistryError, match=message):
            verified_model_version(case_roots, edited)
    # An ID that does not match the content.
    case_roots, unsigned = edited_version(
        roots, version_id, lambda m, f: m.update(instrument="SPY"), tmp_path / "unsigned", resign=False
    )
    with pytest.raises(RegistryError, match="does not match the manifest's content"):
        verified_model_version(case_roots, unsigned)
    # A JSON number is not a manifest at all.
    case_roots, numbered = edited_version(
        roots, version_id, lambda m, f: m.update(instrument=1), tmp_path / "numbered", resign=False
    )
    with pytest.raises(RegistryError):
        verified_model_version(case_roots, numbered)
    # The original still verifies.
    verified_model_version(roots, version_id)


def test_symlinks_extra_files_and_escapes_are_refused(
    demo: tuple[Tools, Path, str], data: Path, tmp_path: Path
) -> None:
    roots = RegistryRoots.under(data)
    version_id = register(demo, data, "majority", "aapl-majority")
    directory = roots.models / version_id

    def corrupted(change: Callable[[Path], None]) -> RegistryRoots:
        base = tmp_path / f"case-{len(list(tmp_path.iterdir()))}"
        (base / "models").mkdir(parents=True)
        copy = base / "models" / version_id
        shutil.copytree(directory, copy)
        writable(copy)
        change(copy)
        return RegistryRoots.under(base)

    def link_file(name: str) -> Callable[[Path], None]:
        def change(copy: Path) -> None:
            outside = copy.parent.parent / f"outside-{name}"
            shutil.copy(copy / name, outside)
            (copy / name).unlink()
            (copy / name).symlink_to(outside)

        return change

    for change, message in [
        (link_file(MANIFEST), "cannot be read as a regular artifact file"),
        (link_file(PARAMETERS), "cannot be read as a regular artifact file"),
        (lambda copy: (copy / "extra.txt").write_text("x"), "holds exactly"),
        (lambda copy: (copy / PARAMETERS).unlink(), "holds exactly"),
    ]:
        with pytest.raises(RegistryError, match=message):
            verified_model_version(corrupted(change), version_id)
    # The version directory itself a symlink.
    base = tmp_path / "linked"
    (base / "models").mkdir(parents=True)
    (base / "models" / version_id).symlink_to(directory)
    with pytest.raises(RegistryError, match="not an artifact directory"):
        verified_model_version(RegistryRoots.under(base), version_id)
    # The models root a symlink.
    other = tmp_path / "rootlink"
    other.mkdir()
    (other / "models").symlink_to(roots.models)
    with pytest.raises(RegistryError, match="not a real directory"):
        verified_model_version(RegistryRoots.under(other), version_id)
    with pytest.raises(RegistryError, match="not an artifact ID"):
        verified_model_version(roots, "../" + version_id[3:])


def test_lifecycle_is_presentation_only_and_never_silently_active(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    roots = RegistryRoots.under(data)
    version_id = register(demo, data, "logistic", "aapl-logistic")
    payload_before = (roots.models / version_id / PARAMETERS).read_bytes()
    assert lifecycle(roots, version_id) == {"changed_at": None, "reason": None, "state": "active"}
    archived = set_lifecycle(roots, version_id, "archived", "superseded in a review")
    assert archived["state"] == "archived"
    assert lifecycle(roots, version_id)["state"] == "archived"
    assert set_lifecycle(roots, version_id, "active", "restored")["state"] == "active"
    # The version's bytes never change.
    assert (roots.models / version_id / PARAMETERS).read_bytes() == payload_before
    verified_model_version(roots, version_id)
    # A corrupt record is an error, never read as active.
    (roots.lifecycle / f"{version_id}.json").write_text('{"state":"active"}')
    with pytest.raises(RegistryError, match="lifecycle record"):
        lifecycle(roots, version_id)
    with pytest.raises(RegistryError, match="one of active, archived"):
        set_lifecycle(roots, version_id, "champion", "no")
    with pytest.raises(RegistryError):
        set_lifecycle(roots, "e" * 64, "archived", "a model that does not exist")
    with pytest.raises(RegistryError, match="printable"):
        set_lifecycle(roots, version_id, "archived", "line\nbreak")


def test_a_version_does_not_need_its_experiment_and_says_where_it_came_from(
    demo: tuple[Tools, Path, str], data: Path, tmp_path: Path
) -> None:
    roots = RegistryRoots.under(data)
    version = verified_model_version(roots, register(demo, data, "logistic", "aapl-logistic"))
    empty = tmp_path / "no-experiments"
    empty.mkdir()
    assert source_experiment_status(empty, version) == {"experiment_id": demo[2], "state": "missing"}
    # A copy of the registry alone still verifies.
    alone = tmp_path / "alone"
    shutil.copytree(roots.models, alone / "models", symlinks=True)
    assert (
        verified_model_version(RegistryRoots.under(alone), version.model_version_id).payload
        == version.payload
    )


def test_unevaluable_and_unknown_models_cannot_be_registered(
    demo: tuple[Tools, Path, str], data: Path
) -> None:
    tools, _, experiment = demo
    artifacts = ArtifactRoots.under(data)
    sources = verified_sources(tools, artifacts, experiment_split(artifacts.experiments, experiment), True)
    registry = RegistryRoots.under(data)
    for instrument, family, name, version, message in [
        ("MSFT", "logistic", "x", "1", "has no models"),
        ("AAPL", "onnx", "x", "1", "the model is one of"),
        ("AAPL", "logistic", "Upper", "1", "a model name"),
        ("AAPL", "logistic", "x", "01", "a model version"),
    ]:
        with pytest.raises(RegistryError, match=message):
            register_experiment_model(
                registry, artifacts.experiments, sources, experiment, instrument, family, name, version
            )
    assert model_version_ids(registry) == []
