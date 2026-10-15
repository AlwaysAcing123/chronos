"""The isolated, guided demo workspace's foreground supervisor (ADR 0025).

Standard library only, so it can run before the research environment it
creates exists. `tools/demo/chronos_demo.py` is the command; these modules
are its parts: owned files, prerequisites, the toolchain, the demo database,
the child services, and the workspace that orders them.
"""
