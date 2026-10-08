# ONNX import sandbox

The container image in which Chronos checks and runs imported ONNX models
([ADR 0021](../../docs/decisions/0021-model-registry-imports-and-comparisons.md)).
The host never parses an imported model. The research service, the
`import-model` and `export-onnx` commands, comparisons, and the
paper-trading server's inference worker reach it only through
`python/chronos_research/onnx_sandbox.py`.

## Building it

```sh
cd python
.venv/bin/python -m chronos_research onnx-sandbox build    # needs the network once
.venv/bin/python -m chronos_research onnx-sandbox status   # available, or why not
```

- **The context.** The build sends Docker a context of exactly:
  - this `Dockerfile`;
  - `python/requirements-onnx.lock`;
  - `chronos_research/__init__.py`, `onnx_profile.py`, and
    `onnx_sandbox_worker.py`.
- **The tag.** The image is tagged `chronos-onnx-sandbox:<first 16 hex
  digits>` of that content's SHA-256, and labelled with the whole digest.
  `status` reports it available only when an image of exactly this source
  exists. After any change to these files, build again.
- **Pins.** The base image is `python:3.12-slim-trixie`, pinned by its
  multi-architecture digest. Every wheel is pinned by hash in
  `python/requirements-onnx.lock` (onnx 1.23.2, onnxruntime 1.30.0, and
  their dependencies), installed with `--no-deps --require-hashes
  --only-binary :all:`. To regenerate the lock, see its header.
- **Network.** Building downloads the base image and the wheels once.
  Running never pulls (`--pull never`) and has no network.
- **Docker.** `--docker` and `--docker-host` name the Docker CLI and an
  optional `DOCKER_HOST`; the CLI is run with only `PATH` (and
  `DOCKER_HOST`) in its environment. The paper-trading server needs the
  CLI's absolute path, `--onnx-docker`.

## What a container may do

Each model runs in its own container with:

```text
--network none --read-only --cap-drop ALL --security-opt no-new-privileges
--user 65534:65534 --pids-limit 32 --memory 256m --memory-swap 256m
--cpus 1 --ulimit nofile=64:64 --ipc none --log-driver none --pull never
```

Nothing of the host is mounted; the model arrives on stdin. The host
bounds:
- the time: 60 s to load and 2 s per prediction;
- each reply line, to 64 KiB, and the kept stderr, to a 4 KiB tail.

A session removes its container on close, failure, or timeout. A
container never outlives its owner, even when the owner is killed before
its own cleanup runs (ADR 0021):
- A watchdog process, started in its own session before the container,
  removes the container when its owner dies (`onnx_sandbox_watchdog.py`).
- A supervisor removes every container labelled with its token when the
  child it ran ends, and verifies that none is left. The supervisors are
  the research service, per import or comparison job, and the paper server,
  per worker of a registered model.
- `onnx-sandbox status` also reaps containers whose owning process on this
  host has exited.

`python/tests/test_onnx_sandbox.py` checks these limits against a real
container: no network, a read-only root, uid 65534, no capabilities, no
host paths or environment, the memory, process, and file limits, and
timeout cleanup. `python/tests/test_sandbox_supervision.py` hangs the
runtime and checks that no container survives:
- a job's deadline, output flood, or early exit;
- shutdown, including the forced kill;
- a SIGKILL of the owner's whole process group;
- a restart after a crash.
It also checks that cleanup leaves another session's container alone.
