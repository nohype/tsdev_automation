# TSDev Automation

Automates TradeStation **9.5** Development Environment (TSDev) Library management - insert, modify, upsert, delete, query existence, and retrieve source - for EasyLanguage items, without the TSDev UI.

Two front-ends: a native CLI executable `tsdev_cli` and a companion Python wrapper `TsDevClient`.

## Supported types

`strategy`, `function`, `indicator`, `tradingapp`, `showme` (case-insensitive). ActivityBar, PaintBar, and ProbabilityMap are not supported.

## Prerequisites

- TradeStation 9.5 installed. Default root: `C:\Program Files (x86)\TradeStation 9.5`; override with `--ts-path <dir>` (CLI) or `ts_path=<dir>` (Python).
   - Note: TS 10.0 is *not supported*.
- `tsdev_cli` is 32-bit (loads `orlib20.dll`).
- TS Desktop does not need to be running.

## Elevation

- Direct `tsdev_cli` calls (including `serve`) must run elevated. Non-elevated direct CLI exits with code `11`.
- Two options:
  1. Run `tsdev_cli` from an elevated terminal (or run Python elevated).
  2. `TsDevClient(use_serve=True)` (default): starts an elevated `tsdev_cli serve` broker with a single UAC prompt for the whole session.
- `TsDevClient(use_serve=False)` from a non-elevated Python session raises `ElevationRequiredError` before launching.

## Native CLI (`tsdev_cli`)

### Subcommands

Positional arguments: `<type>`, `<name>`, `<source_file>`

- `insert <type> <name> <source_file>`
- `modify <type> <name> <source_file>`
- `upsert <type> <name> <source_file>`
- `delete <type> <name>`
- `exists <type> <name> [--case-insensitive]`
- `get-source <type> <name> [--output <file>]`

### Flags

- Global:
  - `--json` - emit a single JSON result object on stdout.
  - `--ts-path <dir>` - override TradeStation install directory.
  - `-v` / `--verbose` - raise the stderr log level. Position-free and repeatable; each occurrence raises the level one step: default = warnings + errors only; one occurrence (`-v`) adds info; two or more (`-vv`) add debug internals. Additional occurrences clamp at debug. Accepted forms: `-v`, `-vv`, `-vvv` (bundled), `--verbose`, `--verbose=<value>` (value ignored, counts as one occurrence). Log output goes to stderr only; stdout (results and JSON) is unaffected.
- `exists`:
  - `--case-insensitive` (default: case-sensitive exact match).
- `get-source`:
  - `--output <file>` - write source to file; internally encrypted items are decrypted automatically.

### Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | Invalid command-line usage or arguments |
| `2` | Unknown component type |
| `3` | Could not read the source file, or could not write the output file (`get-source`) |
| `4` | Failed to initialize the TSDev Library (check TradeStation install / `--ts-path`) |
| `5` | Item not found (modify/delete/get-source), or item already exists (insert) |
| `6` | The item operation could not be completed |
| `7` | Failed to save the source text to the library |
| `8` | The EasyLanguage source failed to compile |
| `9` | The item's compiled library file is missing |
| `10` | Internal error: failed to start a required worker thread |
| `11` | Not running as administrator (elevation required) |

### Batch operations

File batch subcommands take a `<manifest_file>` or `--batch-stdin`:

- `batch-insert <manifest_file>` / `batch-insert --batch-stdin`
- `batch-modify <manifest_file>` / `batch-modify --batch-stdin`
- `batch-upsert <manifest_file>` / `batch-upsert --batch-stdin`
- `batch-delete <manifest_file>` / `batch-delete --batch-stdin`

Directory batch subcommands operate on a directory of `.el`/`.txt` files:

- `batch-insert-dir <dir>`
- `batch-modify-dir <dir>`
- `batch-upsert-dir <dir>`
- `batch-delete-dir <dir>`

Use `-t <ext>` (repeatable) to filter extensions. File stem (without the last extension) = item name.

Output is always a JSON array (one element per item). Exit codes:

- `0` - all succeeded
- `1` - usage error, manifest not found, or manifest empty (batch did not run)
- `2` - batch ran, but one or more items failed (per-item details in the JSON)

### Manifest format

Line-based text (not JSON). `#` comments and blank lines are skipped. One item per line.

- `insert`/`modify`/`upsert`: `<type> <name> <source_file>` (`source_file` is the rest of the line after the second token, so paths with spaces work).
- `delete`: `<type> <name>`

Example `manifest.txt`:

```text
# insert two functions
function MyFuncA ./src/MyFuncA.el
function MyFuncB ./src/MyFuncB.el
```

### Examples

```powershell
# insert a function
tsdev_cli insert function MyFn ./src/MyFn.el

# case-insensitive exists check, JSON output
tsdev_cli --json exists function MyFn --case-insensitive

# retrieve source to a file
tsdev_cli get-source function MyFn --output MyFn.el

# batch from a file
tsdev_cli batch-insert manifest.txt
```

## Python wrapper (`TsDevClient`)

### Import

```python
from tsdev_automation import TsDevClient, BatchItem
```

### Constructor

```python
client = TsDevClient(
    cli_path=None,   # path to tsdev_cli.exe; auto-discovered if None
    ts_path=None,    # forwarded as --ts-path
    timeout=None,    # subprocess timeout in seconds; None = no timeout
    use_serve=True,  # auto-elevate via serve broker (one UAC prompt)
)
```

### Single-item methods

All return a `TsDevResult` dataclass. `insert`/`modify`/`upsert`/`delete`/`get_source` raise a `TsDevError` subclass on failure; `exists` does not raise on not-found (not-found is a successful query).

`TsDevResult` fields:

- `.success: bool`, `.exit_code: int`, `.op: str`, `.type: str | None`, `.name: str | None`
- `.prog_id: int | None`, `.ell_path: str | None` — populated by insert/modify/upsert/delete
- `.exists: bool`, `.stored_name: str`, `.prog_id: int` — populated by `exists` (`.prog_id == 0` and `.stored_name == ""` when not found)
- `.source: str` — populated by `get_source` (empty string on failure)
- `.error: TsDevErrorInfo | None` — set when `success` is False; carries `.code: str`, `.message: str`, plus `.compile_errors: str | None` (exit 8) and `.existing_prog_id: int | None` (insert, exit 5)

Methods:
- `insert(type, name, source)`
- `modify(type, name, source)`
- `upsert(type, name, source)`

`source` may be a file path or an EasyLanguage source string (auto-written to a temp `.el` file).

- `delete(type, name)`
- `exists(type, name, *, case_insensitive=False)`
- `get_source(type, name, output_file=None)` — if `output_file` is given, the source is also written to disk.

### Batch methods

- `batch_insert(items)` / `batch_modify(items)` / `batch_upsert(items)` / `batch_delete(items)`
  - `items`: `Sequence[BatchItem]` -> `list[TsDevResult]`
- `batch_insert_dir(type, dir_path, extensions=None)`
- `batch_modify_dir(type, dir_path, extensions=None)`
- `batch_delete_dir(type, dir_path, extensions=None)`
- `batch_upsert_dir(type, dir_path, extensions=None)`

Per-item failures are returned in the list. Manifest/directory-level failures raise `TsDevError`.

### `BatchItem`

```python
from dataclasses import dataclass
from os import PathLike

@dataclass
class BatchItem:
    type: str
    name: str
    source: str | PathLike | None  # required for insert/modify/upsert; None for delete
```

`source` may be a file path or an EasyLanguage source string.

### Install

```shell
pip install .
```

Requires Python `>=3.14`. `TsDevClient` uses only the standard library.

### Example

```python
from tsdev_automation import TsDevClient, BatchItem

client = TsDevClient()  # use_serve=True by default; one UAC prompt if not elevated

src = '{ name = MyFn }  { user function }  Value1 = 1;'

ins = client.insert('function', 'MyFn', src)
# ins: TsDevResult -> .success=True, .prog_id, .ell_path
r = client.exists('function', 'MyFn', case_insensitive=True)
# r: TsDevResult -> .exists, .stored_name, .prog_id
print(r.exists, r.stored_name, r.prog_id)
g = client.get_source('function', 'MyFn')
# g: TsDevResult -> .source (str)
print(g.source)
client.delete('function', 'MyFn')

# batch
items = [
    BatchItem('function', 'FnA', 'Value1 = 1;'),
    BatchItem('indicator', 'IndA', 'Plot(Close, "C");'),
]
results = client.batch_insert(items)
# results: list[TsDevResult], one per item in order; per-item failures stay in the list
```

## Notes

- TradeStation install path: use `--ts-path <dir>` or `ts_path=<dir>` when the default registry auto-discovery is wrong.
- A TradeStation popup or crash dialog during an operation means the operation failed. Read the message, dismiss it, and make sure you have the right TS version and followed the usage guidelines. When in doubt, try again.

