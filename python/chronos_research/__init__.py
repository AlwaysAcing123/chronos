"""Baseline predictive evaluation over verified Chronos artifacts.

The package consumes feature, label, and split artifacts that the C++
`chronos_dataset` tool has verified, joins them by row identity, trains a
majority-class and a logistic-regression baseline per instrument on the
training split only, and publishes an immutable evaluation artifact. It
never writes or redefines a C++ artifact. See docs/baseline-evaluation.md.
"""
