# TSDev Automation Bundle

This bundle automates the TradeStation 9.5 Development Environment (TSDev) Library without using the `TSDev.exe` UI. It contains `tsdev_cli.exe` (32-bit CLI executable), the `tsdev_automation/` Python package, and this `README.md`.

## Supported component types

Function, Indicator, ShowMe, Strategy, and TradingApp are supported. ActivityBar, PaintBar, and ProbabilityMap are not.

## Requirements

- TradeStation 9.5 installed; TradeStation 10.0 is **not supported**.
- Default TradeStation root: `C:\Program Files (x86)\TradeStation 9.5`.
- Default compiled TSDev Library: `C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin`.
- Override the install with `--ts-path <dir>` (CLI) or `ts_path=<dir>` (Python).
- `tsdev_cli.exe` is 32-bit and loads `orlib20.dll`. TS Desktop does not need to be running.
- Python `>=3.14` for the Python wrapper package; the core `TsDevClient` uses only the standard library.
   - Not needed to use the native CLI (`tsdev_cli.exe`)

## Elevation

Direct `tsdev_cli.exe` calls must run in an elevated administrator process; otherwise the CLI exits with code `11`.

`TsDevClient(use_serve=True)` (the default) starts an elevated `tsdev_cli serve` broker server ith one UAC prompt for the whole session. With `use_serve=False` (no persistent broker), a non-elevated Python session raises `ElevationRequiredError` before launch.

## Native CLI (`tsdev_cli.exe`)

Single-item operations:

```powershell
tsdev_cli insert   <type> <name> <source_file>
tsdev_cli modify   <type> <name> <source_file>
tsdev_cli upsert   <type> <name> <source_file>
tsdev_cli delete   <type> <name>
tsdev_cli exists   <type> <name> [--case-insensitive]
tsdev_cli get-source <type> <name> [--output <file>]
```

Global: `--json` emits a single JSON result; `--ts-path <dir>` overrides the TradeStation install. `-v`/`--verbose` (position-free, repeatable) raises the stderr log level: default = warnings + errors only; one occurrence (`-v`) adds info; two or more (`-vv`) add debug internals; additional occurrences clamp at debug. `exists` is case-sensitive by default; `--case-insensitive` relaxes matching. `get-source` decrypts encrypted items automatically.

Batch via a manifest file or `--batch-stdin`:

```powershell
tsdev_cli batch-insert <manifest_file>
tsdev_cli batch-insert --batch-stdin
tsdev_cli batch-modify <manifest_file>
tsdev_cli batch-upsert <manifest_file>
tsdev_cli batch-delete <manifest_file>
```

Manifest format: one item per line; `#` comments and blank lines are ignored.

```text
# insert two functions
function MyFuncA ./src/MyFuncA.el
function MyFuncB ./src/MyFuncB.el
```

## Python wrapper (`TsDevClient`)

Add the directory containing `tsdev_automation/` to `PYTHONPATH`; `tsdev_cli.exe` is discovered as a sibling automatically.

```python
from tsdev_automation import TsDevClient, BatchItem

client = TsDevClient()  # use_serve=True by default; one UAC prompt
```

Single-item methods:

```python
client.insert(type, name, source)                   # -> TsDevResult
client.modify(type, name, source)                   # -> TsDevResult
client.upsert(type, name, source)                   # -> TsDevResult
client.delete(type, name)                           # -> TsDevResult
client.exists(type, name, case_insensitive=False)   # -> TsDevResult
client.get_source(type, name, output_file=None)     # -> TsDevResult
```

All return a `TsDevResult` dataclass. `insert`/`modify`/`upsert`/`delete`/`get_source` raise a `TsDevError` subclass on failure; `exists` does not raise on not-found (not-found is a successful query). Relevant fields: `.success: bool`, `.exit_code: int`; insert/modify/upsert/delete populate `.prog_id: int | None` and `.ell_path: str | None`; `exists` populates `.exists: bool`, `.stored_name: str`, `.prog_id: int` (`0` and `""` when not found); `get_source` populates `.source: str`. On failure, `.error: TsDevErrorInfo | None` carries `.code`, `.message`, and `.compile_errors` (compile failure).

`source` may be a file path or an EasyLanguage source string. If `output_file` is given, the source is also written to disk.

Batch methods:

```python
client.batch_insert(items)   # -> list[TsDevResult]
client.batch_modify(items)   # -> list[TsDevResult]
client.batch_upsert(items)   # -> list[TsDevResult]
client.batch_delete(items)   # -> list[TsDevResult]
```

`items` is a `Sequence[BatchItem]`; each method returns one `TsDevResult` per item, in order. Directory batch helpers (`batch_*_dir`) are also available and also return `list[TsDevResult]`. Per-item failures are returned in the result list (not raised); manifest- or directory-level failures raise `TsDevError`.
