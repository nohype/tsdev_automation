# TSDev Automation

Automated management of the TradeStation Development Environment (TSDev) Library without using the TSDev UI (TSDev.exe). Insert, delete, modify, query, and retrieve EasyLanguage strategies, indicators, functions, and trading apps via a standalone CLI and a Python wrapper.

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
tsdev_cli [--json] [--ts-path <dir>] insert       <type> <name> <source-file>
tsdev_cli [--json] [--ts-path <dir>] modify       <type> <name> <source-file>
tsdev_cli [--json] [--ts-path <dir>] delete       <type> <name>
tsdev_cli [--json] [--ts-path <dir>] exists       <type> <name> [--case-insensitive]
tsdev_cli [--json] [--ts-path <dir>] get-source   <type> <name> [--output <file>]
tsdev_cli [--json] [--ts-path <dir>] upsert       <type> <name> <source-file>
tsdev_cli [--ts-path <dir>] batch-insert  <manifest-file> | --batch-stdin
tsdev_cli [--ts-path <dir>] batch-modify  <manifest-file> | --batch-stdin
tsdev_cli [--ts-path <dir>] batch-delete  <manifest-file> | --batch-stdin
```

- `insert` — Compile and insert a new component. Fails loudly on compile errors (exit 8).
- `modify` — Replace source code of an existing component and recompile. Preserves ProgId.
- `delete` — Delete an existing component. Notifies if it doesn't exist (exit 5).
- `exists` — Query whether a component exists by name. Returns `exists=true/false`, the verbatim stored name, and ProgId. Use `--case-insensitive` for case-insensitive name matching. Exit 0 for both found and not-found.
- `get-source` — Retrieve the EasyLanguage source of a component. Prints to stdout by default; `--output <file>` writes to a file. Standard (non-password-protected) items only.
- `upsert` — Insert if the component doesn't exist, otherwise modify. Preserves ProgId on the modify path.
- `batch-insert`/`batch-modify`/`batch-delete` — Batch operations from a line-based manifest file or stdin (`--batch-stdin`). Always emit a JSON array (one result per item).
- `--json` — Emit structured JSON output (single object for single-item ops; batch ops always emit a JSON array).
- `--ts-path <dir>` — Override the TradeStation install directory (default: auto-discovered from registry).

`<type>` is one of `strategy`, `function`, `indicator`, `tradingapp` (case-insensitive).

## Build

See [docs/BUILD.md](docs/BUILD.md) for build prerequisites, CMake commands, and test instructions.

## Distribution / Bundle

Running `cmake --build build --config Release` automatically produces a
`dist/` directory at the repo root containing the assembled bundle:

```
dist/
├── tsdev_cli.exe        # the C++ CLI (static, self-contained)
├── tsdev_automation/    # the Python wrapper package (6 .py files)
└── README.md            # bundle quick-start guide
```

Both `dist/` and `artifacts/` are gitignored (root `.gitignore`).

### Using `dist/`

- **CLI directly:** `dist\tsdev_cli.exe insert strategy MyStrat my_strategy.el`
- **Python wrapper:** add `dist/` to `sys.path` or `PYTHONPATH`, then
  `from tsdev_automation import TsDevClient`. The wrapper auto-discovers the
  sibling `tsdev_cli.exe` — no `cli_path` needed.

### Using `artifacts/`

`artifacts/` contains an automatically generated timestamped zip ready to
distribute — extract and use directly, no build step required:

```
artifacts/tsdev_automation_<yymmdd><HHMM>.zip
```

The zip contains exactly the three bundle components (`tsdev_cli.exe`,
`tsdev_automation/`, `README.md`) — no wheels, sdist, or `__pycache__/`. Each
build creates a new timestamped zip; the latest build wins within the same
minute (`-Force` overwrite).

### Manual zip (supplementary)

For ad-hoc use, a selective zip can be created manually from the repo root:

```powershell
Compress-Archive -Path dist\tsdev_cli.exe,dist\tsdev_automation,dist\README.md -DestinationPath tsdev_automation-bundle.zip
```

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
**64-bit Python 3.14+** and is **stdlib-only** (no third-party dependencies).
The CLI binary (`tsdev_cli.exe`) is **not** bundled — build it first with CMake,
or point the client at it via `cli_path` / `TSDEV_CLI_PATH`.

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
| `exists(type, name, *, case_insensitive=False)` | `TsDevResult` | `UnknownTypeError`, … (not-found is NOT raised) |
| `get_source(type, name, output_file=None)` | `TsDevResult` | `NotFoundError`, `OutputWriteError`, … |
| `upsert(type, name, source)` | `TsDevResult` | `CompileError`, `SourceFileError`, … |
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
├── OutputWriteError            # exit 3  — get-source output file write failure
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
                 {insert,modify,delete,exists,get-source,upsert,batch-insert,batch-modify,batch-delete} ...
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

## Regression suite

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1
```

Runs 93 test cases across 8 sub-suites: sigtool (12) + C++ unit (18) + E2E CLI (34) + exists (5) + batch-stdin (8) + upsert (5) + get-source (6) + exists-ci (5).
