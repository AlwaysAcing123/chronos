# The ONNX import sandbox (ADR 0021). Built by
#   python -m chronos_research onnx-sandbox build
# from a context of exactly this file, python/requirements-onnx.lock, and
# the worker's modules; the image's label records that content's SHA-256.
# The base is pinned by its multi-architecture digest, every wheel by hash.
FROM python:3.12-slim-trixie@sha256:05cda9777409a9c3ffddd94a4c476b79f0769a0b4857f0c7ed9226b6800b0d6f
COPY requirements-onnx.lock /tmp/requirements-onnx.lock
RUN pip install --no-cache-dir --no-deps --require-hashes --only-binary :all: -r /tmp/requirements-onnx.lock \
    && rm /tmp/requirements-onnx.lock
COPY chronos_research/__init__.py chronos_research/onnx_profile.py chronos_research/onnx_sandbox_worker.py \
    /usr/local/lib/python3.12/site-packages/chronos_research/
RUN python -m compileall -q /usr/local/lib/python3.12/site-packages/chronos_research
ENV OMP_NUM_THREADS=1
USER 65534:65534
ENTRYPOINT ["python", "-I", "-m", "chronos_research.onnx_sandbox_worker"]
