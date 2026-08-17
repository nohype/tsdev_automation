# Handoff D — Python Integration

**Priority (d):** Provide a simple, robust way to drive TradeStation component management (INSERT / DELETE / MODIFY) from Python.

**Why this matters:** The user wants to drive the three functions from Python. The core CLI (`tsdev_cli`) already implements all three subcommands reliably (insert / delete / modify), including compilation, error reporting with source line/column, and `.ell` persistence.

---

## Decision record (APPROACH CHANGE)

The originally planned approach for Workstream D was a **Python DLL bridge via msl-loadlib** — a 32-bit/64-bit inter-process bridge with a frozen 32-bit server executable, a custom C ABI DLL layer, and pickle-over-HTTP IPC. **This approach has been REJECTED.**

Rationale for rejection: high complexity (32-bit server subprocess, frozen-executable dependency, custom C ABI DLL layer, cross-process IPC), fragile dependencies, and unnecessary given that the CLI already solves the hard problems (orlib20.dll integration, compilation, error reporting, persistence).

**New chosen direction:** a **simple, robust Python integration** — most likely a **Python wrapper class around the existing CLI** (`tsdev_cli`). The wrapper would invoke the CLI as a subprocess and map exit codes / parse output. No 32-bit bridge, no custom DLL, no msl-loadlib.

---

## Current State

> **WORKSTREAM E PREREQUISITE (2026-08-17):** Workstream E (`docs/ARCHITECTURE_REDESIGN.md`) should complete before Workstream D begins. After E, the CLI (`tsdev_cli`) builds from a fresh clone without any `tmp/` dependencies, which simplifies Python integration testing and CI. The Python wrapper can assume a standard build process (cmake + build) without needing the `tmp/re_artifacts/` directory to exist.

- The CLI (`tsdev_cli`) is COMPLETE and APPROVED. The `insert`, `delete`, and `modify` subcommands all work reliably (see AGENTS.md TODO for evidence).
- Exit-code conventions: `0` = success; `5` = not-found (delete / modify); `8` = compile error (insert / modify).
- Compiler errors are reported with `Line X, Col Y:` prefixes pointing to the exact source location.
- No Workstream D work has been started.

---

## Next steps (DEFERRED)

The detailed design (wrapper class API, subprocess invocation, exit-code / output mapping, packaging / distribution) and the TVS decomposition will be determined in the corresponding TVS. This document intentionally does NOT pre-specify a TVS breakdown or implementation details.

Determining the optimal approach (wrapper class vs. other simple options) is itself part of that future TVS.

---

## Constraints

- Must remain simple and robust. Must not reintroduce the complexity that motivated rejecting the msl-loadlib bridge.
- Must build on the existing CLI — do not duplicate the `orlib20.dll` integration logic in Python.
