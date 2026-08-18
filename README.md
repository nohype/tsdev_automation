# TSDev Automation

Automated management of the TradeStation Development Environment (TSDev) Library without using the TSDev UI (TSDev.exe). Insert, delete, and modify EasyLanguage strategies, indicators, functions, and trading apps via a standalone CLI.

## Supported component types

| Type | WHType | Status |
|------|--------|--------|
| Strategy | 0 | Supported |
| Indicator | 2 | Supported |
| Function | 4 | Supported |
| TradingApp | 5122 (0x1402) | Supported |
| ActivityBar | — | Unconfirmed |
| PaintBar | — | Unconfirmed |
| ProbabilityMap | — | Unconfirmed |
| ShowMe | — | Unconfirmed |

## CLI usage

```
tsdev_cli insert <type> <name> <source-file>
tsdev_cli delete <type> <name>
tsdev_cli modify <type> <name> <source-file>
tsdev_cli [--ts-path <dir>] <subcommand> ...
```

- `insert` — Compile and insert a new component. Fails loudly on compile errors (exit 8).
- `delete` — Delete an existing component. Notifies if it doesn't exist (exit 5).
- `modify` — Replace source code of an existing component and recompile. Preserves ProgId.

## Build

See [docs/BUILD.md](docs/BUILD.md) for build prerequisites, CMake commands, and test instructions.

## Project rules

See [AGENTS.md](AGENTS.md) for project rules, acceptance criteria, and conventions.

## Architecture

Signature data lives in `signatures/*.json` (authoritative source of truth) → `tools/sigcodegen.py` generates `signatures.gen.h` at build time → `src/orlib_resolver.cpp` consumes it. No runtime JSON dependency in the production CLI.

`src/sigtool.cpp` is a standalone developer tool for verifying orlib20.dll signatures against a DLL file on disk.

## Python integration

A stdlib-only Python wrapper (`TsDevClient`) drives the 32-bit `tsdev_cli.exe`
as a subprocess using its `--json` structured output and batch subcommands.
This lets you automate the TSDev Library from **64-bit Python** (the CLI loads
`orlib20.dll`, which is 32-bit and cannot be loaded in-process).

### Install

```powershell
# From a source checkout (creates a .venv and installs the package + deps):
uv sync

# Or install from source into an existing environment:
pip install .
```

The package is `tsdev-automation` (import name `tsdev_automation`). It requires
**64-bit Python 3.14+** and depends on `capstone` (used by the C++ build tooling
bundled in the repo). The CLI binary (`tsdev_cli.exe`) is **not** bundled —
build it first with CMake, or point the client at it via `cli_path` /
`TSDEV_CLI_PATH`.

### `TsDevClient` API

```python
from tsdev_automation import TsDevClient, BatchItem

client = TsDevClient(
    cli_path=None,   # auto-discover build/Release/tsdev_cli.exe (or TSDEV_CLI_PATH)
    ts_path=None,    # optional TradeStation install dir (--ts-path)
    timeout=None,    # optional subprocess timeout in seconds
)
```

| Method | Returns | Raises |
|--------|---------|--------|
| `insert(type, name, source)` | `TsDevResult` | `AlreadyExistsError`, `CompileError`, … |
| `modify(type, name, source)` | `TsDevResult` | `NotFoundError`, `CompileError`, … |
| `delete(type, name)` | `TsDevResult` | `NotFoundError`, … |
| `batch_insert(items)` | `list[TsDevResult]` | `TsDevError` (manifest-level only) |
| `batch_modify(items)` | `list[TsDevResult]` | `TsDevError` (manifest-level only) |
| `batch_delete(items)` | `list[TsDevResult]` | `TsDevError` (manifest-level only) |

`type` is one of `strategy`, `function`, `indicator`, `tradingapp`. `source`
may be a path to an existing `.el` file (used directly) or an inline
EasyLanguage source string (written to a temp file and cleaned up
automatically). Batch methods take a list of `BatchItem(type, name, source)`
and return one `TsDevResult` per item in order — per-item failures are reported
in the list, not raised (only a manifest-level failure raises).

### Exception hierarchy

All CLI-reported failures raise a subclass of `TsDevError` (the base
exception). Each carries the originating `TsDevResult` on `.result` for
inspection:

```
TsDevError                      # base
├── UsageError                  # exit 1  — CLI usage error
├── UnknownTypeError            # exit 2  — unknown component type
├── SourceFileError             # exit 3  — source file unreadable/empty
├── InitFailure                 # exit 4  — orlib20 init failure
├── NotFoundError               # exit 5  — component not found (modify/delete)
├── AlreadyExistsError          # exit 5  — component exists (insert)
├── EntryOpError                # exit 6  — createEntry/deleteEntry failure
├── SaveSourceError             # exit 7  — saveSource failure
├── CompileError                # exit 8  — EasyLanguage compile failure
├── EllMissingError             # exit 9  — .ell not produced after compile
└── ThreadFailure               # exit 10 — worker-thread creation failure
```

### Code example

```python
from tsdev_automation import (
    TsDevClient, BatchItem, CompileError, AlreadyExistsError,
)

client = TsDevClient()

# Insert a strategy (inline source string).
result = client.insert("strategy", "MyStrat",
                       "Inputs: Length(10); "
                       "If Close > Average(Close, Length) Then Buy next bar at Market;")
print(f"ProgId={result.prog_id}  ell={result.ell_path}")

# Handle a compile error.
try:
    client.insert("strategy", "BadStrat", "this is not valid easylanguage !!!")
except CompileError as e:
    print(f"compile failed: {e.result.error.compile_errors}")

# Batch-insert 20 strategies (init is amortized across the batch).
items = [BatchItem("strategy", f"Batch{i:02d}",
                   "Inputs: Length(10); If Close > Average(Close, Length) "
                   "Then Buy next bar at Market;")
         for i in range(20)]
results = client.batch_insert(items)
assert all(r.success for r in results)

# Cleanup.
client.batch_delete([BatchItem("strategy", f"Batch{i:02d}") for i in range(20)])
client.delete("strategy", "MyStrat")
```

### `tsdev-automation` CLI entry point

The package also installs a convenience CLI (`tsdev-automation` console script
and `python -m tsdev_automation`) that wraps the Python API:

```
tsdev-automation [--cli-path PATH] [--ts-path DIR] [--timeout SEC]
                 {insert,modify,delete,batch-insert,batch-modify,batch-delete} ...
```

For `insert`/`modify`, the source argument is interpreted as a file path if it
exists, otherwise as inline EasyLanguage source text (matching the Python API).
Run `tsdev-automation --help` for full usage.

### Notes

- **64-bit Python required.** The wrapper is stdlib-only and runs under 64-bit
  Python; it drives the 32-bit CLI as a subprocess (no daemon, no in-process
  DLL loading).
- The CLI binary must be built separately via CMake (`cmake --build build
  --config Release`). Auto-discovery looks for `build/Release/tsdev_cli.exe`
  relative to the repo root.

## Next workstream

Workstream D (Python Integration) is **complete**. See
[docs/HANDOFF_D_PYTHON_INTEGRATION.md](docs/HANDOFF_D_PYTHON_INTEGRATION.md)
for the original handoff.

## Regression suite

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
```

Runs 60 tests: sigtool verification (12) + C++ unit tests (14) + E2E CLI tests (34).
