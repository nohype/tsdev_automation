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
  - `--json` - emit a single JSON result object on stdout. For batch subcommands it emits the JSON results array on stdout (the only mode that produces one - without `--json` batch stdout is empty) and selects the stderr progress rendering: one JSON event object per line instead of human-readable `BATCH` lines.
  - `--pbar` - render an aesthetic visual progress bar on stderr. Position-free and idempotent; `--pbar=<value>` is accepted and the value ignored. The bar redraws in place on the last stderr line and is quiet on success: successful items emit nothing else, and a successful single mutation op's `SUCCESS:` stdout line is suppressed (query ops still print their stdout answer); failures and library warnings scroll above the bar. Usage error (exit 1) when combined with `--json` or used with `serve`.
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

With `--json`, stdout is a JSON array (one element per item); without it stdout is empty. Exit codes:

- `0` - all succeeded
- `1` - usage error, manifest not found, or manifest empty (batch did not run)
- `2` - batch ran, but one or more items failed (per-item details in the JSON array under `--json`, else on stderr)
- `3` - batch cooperatively cancelled via `--batch-progress-control` (under `--json` the array stays full length; unattempted items are exit-12 `cancelled` markers)
- `14` - batch aborted by the WHServer VAS-headroom guard (same full-length array + exit-12 not-run markers, `error.message="Batch aborted (WHServer VAS headroom) before this item was attempted"`; the summary gains `aborted=true abort_reason=whserver_vas_free=<N>MB` or `whserver_vas_largest_free=<N>MB`, and a `batch_aborted` / `BATCH ABORTED` event precedes it). Under `serve`, `14` is instead a per-request refusal code (`error.code="whserver_vas_abort"`) on the refused item — the broker process keeps running and non-mutating requests still pass.

### WHServer VAS-headroom guard (batch-* and serve)

WHServer.exe is 32-bit and non-LARGEADDRESSAWARE; a field crash showed it dying at ~10 MB free user address space. After each completed non-final batch item — and in `serve` before dispatching each mutating request — the guard measures WHServer's free user address space and aborts loudly when headroom drops below the thresholds, so a runaway batch can never push WHServer over the cliff. A `serve` breach refuses that request with exit `14` / `error.code="whserver_vas_abort"` (the Python client stops the batch there and marks the unattempted remainder not-run); a direct `batch-*` breach aborts the whole run with process exit `14`:

- `--whserver-free-mb <N>` - abort when total free VAS falls below N MB (default `256`)
- `--whserver-largest-free-mb <N>` - abort when the largest contiguous free block falls below N MB (default `64`; fragmentation kills contiguous allocations even while total free looks ample)
- `--no-whserver-vas-guard` - disable the guard entirely

Env mirrors: `TSDEV_WHSERVER_FREE_MB`, `TSDEV_WHSERVER_LARGEST_FREE_MB`, `TSDEV_NO_WHSERVER_VAS_GUARD` (any value but `0` disables). Explicit CLI flags win over env. WHServer not running (lazy singleton not yet spawned) passes; a transient measurement failure warns once and does not abort. Breach semantics: strictly below the threshold aborts (exactly-at-threshold passes). The guard only reads WHServer's address-space layout — it never writes to or signals the process.

### Dispatch watchdog (all subcommands)

Every orlib20 warehouse dispatch raises an in-flight marker; a watchdog thread polls the marker once per second and aborts the process when a dispatch exceeds the bound (`--dispatch-timeout-secs`, 300 s by default). If WHServer.exe wedges or crashes, orlib20's synchronous COM wait never returns — the watchdog then aborts truthfully instead of hanging forever:

- in-flight dispatch older than `--dispatch-timeout-secs <N>` (default `300`, range `0..86400`, `0` disables; env mirror `TSDEV_DISPATCH_TIMEOUT_SECS`, flag wins) → `dispatch_timeout`
- a `TSCrashReport.exe` running as a child of a WHServer.exe pid while a dispatch is in-flight → `whserver_crashed` (immediate; a TSCrashReport parented by any other process never fires)
- WHServer.exe seen during the flight then vanished → `whserver_gone` (the absence must persist across 3 consecutive 1 s polls — a single empty poll is inconclusive because a Toolhelp32 snapshot failure looks identical)

The timeout is the sole *guaranteed* backstop: a wedged-but-alive WHServer that keeps running is only detected when the dispatch exceeds `--dispatch-timeout-secs` (300 s by default). The `whserver_crashed`/`whserver_gone` checks are best-effort fast paths that can abort earlier when the crash is externally visible.

On fire the process prints `tsdev_cli: WATCHDOG ABORT: warehouse dispatch '<site>' aux=<aux> in-flight <N>s (bound <T>s) [reason: ...]; exiting with code 15` on **stderr** and terminates itself with exit code `15` (`TerminateProcess` — never `ExitProcess`/DLL detach, never `closeWarehouse`). **No JSON frame is emitted for the in-flight item** — under `--json` the abort produces no result line for that item; it is the "uncertain state" item whose true outcome is unknown. What survives of the already-completed items depends on the path: under `serve` each completed request already returned its JSON frame to the client, so those results are safe; on the direct `batch-*` path the stdout JSON array is emitted only after the worker finishes the whole batch, so a mid-batch abort loses every item's result — the stderr abort line is the only output besides the streamed progress events (the per-item `BATCH SUCCESS`/`BATCH FAILED` lines — or `batch_item_completed` events under `--json` — stream to stderr as items complete and do survive; under `--pbar` successes emit no line). Idle waits (e.g. the `serve` broker blocked in `ConnectNamedPipe`) carry no in-flight marker and are exempt.

Under `TsDevClient(use_serve=True)` the broker is spawned hidden with no stderr capture, so the `WATCHDOG ABORT` line and its site/reason detail are **not observable** on that path — exit code `15`, surfaced as `TsDevWatchdogAbortError`, is the only machine-readable signal. On watchdog abort the correct operator action is to recycle/restart WHServer (e.g. restart the TradeStation desktop, or let WHServer SCM-respawn once no clients hold it) before retrying the batch — never blindly retry into the same server (the Python client already refuses the retry).

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

### Batch progress

Every batch that runs emits progress on **stderr** (always on, independent of `-v`), one line per event:

- `BATCH SUCCESS [i/n] op=<op> type="<type>" name="<name>" exit_code=0` / `BATCH FAILED [i/n] op=<op> type="<type>" name="<name>" exit_code=<n> message="<msg>"` - after each attempted item's result, in input order (including items rejected at manifest parse/pre-validation and TsDevLib-init failures; `message=` is omitted only when the error message is empty; quoted values are JSON-escaped so one event is always one line). A `BATCH FAILED` line is followed by the item's full detail block - compiler errors etc., the same text a single-op failure prints.
- `BATCH COMPLETE op=<op> completed=<n> total=<n> successes=<n> failures=<n> not_run=<n> cancelled=<bool> close_failed=<bool> exit_code=<n>` - once, always last, after the end-of-batch warehouse close and any retroactive result rewrite

With `--json`, each stderr line is one compact JSON object instead: `batch_item_started` before each attempted item, `batch_item_completed` (carrying the exact per-item result object), then `batch_completed`. Started events exist only in this NDJSON protocol and on tagged serve requests - human mode has no `BATCH START` lines. `--json` is also what produces the stdout JSON results array; without it batch stdout is empty.

Under `--json` the stdout array remains the authoritative result: a failed end-of-batch warehouse close rewrites still-successful items to failures, and `BATCH COMPLETE`/`batch_completed` is emitted after that rewrite (`close_failed=true`).

`--pbar` pins a visual progress bar to the last stderr line - redrawn in place via `\r` (no ANSI escapes), `█`/`░` blocks on a real console, UTF-8 bytes when stderr is piped. The bar fills 0→N as items complete and gains a `— <n> failed` suffix once items have failed; while items succeed the bar is the only output - a failed item's `BATCH FAILED` marker and full detail block scroll above it, as do library warnings/errors as event lines. `BATCH COMPLETE` prints only when the run was not fully clean (failures, not-run items, cancellation, or a close failure). For single-item mutation subcommands the bar fills 0→1 (`— 1 failed` on failure) and a success prints only the bar - the `SUCCESS: ... ProgId=N` stdout line is suppressed while failures still print their stderr detail. Query ops (`exists`/`get-source`/`dependents`) are unchanged: the stdout answer always prints. The exact rendering is a visual aid, not a contract; `--pbar` can coexist with `--batch-progress-control`.

### Cooperative batch control (`--batch-progress-control`)

`--batch-progress-control` is a batch-only, position-free flag (repeating it is a usage error) that turns **stdin** into a cooperative between-items control channel. After each completed item except the last, the batch synchronously reads exactly one LF-terminated decision line before attempting the next item:

- `continue\n` - proceed to the next item
- `cancel\n` - stop cooperatively: the batch closes the warehouse once, fills the remaining positions with not-run markers, and exits `3`

Any other input — EOF before the line terminator, a read error, or a malformed token — emits a `batch_control_error` (JSON mode) / `BATCH CONTROL ERROR: <message>; cancelling before next item` (human mode) diagnostic on stderr, then cancels the same way. Cancelling fills every unattempted position with a marker result (`success=false`, `exit_code=12`, `error.code="cancelled"`, `error.message="Batch cancelled before this item was attempted"`), so under `--json` the stdout array stays **full length**. The summary reports `completed` = attempted items, `not_run` = markers, `cancelled=true`, and exit `3` dominates even when items failed or the end-of-batch close failed.

Stdin layout depends on how the manifest is supplied:

- `batch-* --batch-stdin --batch-progress-control`: stdin begins with a framed manifest — the exact ASCII header `TSDEV_BATCH_CONTROL/1 <decimal byte length>\n` (digits only, 64 MiB cap) followed by exactly that many manifest bytes — then decision lines on the same stream. A malformed frame is a pre-run failure (exit 1, no array).
- `batch-* <manifest_file> --batch-progress-control` and `batch-*-dir ... --batch-progress-control`: the manifest/directory still comes from its normal source; stdin carries decision lines only.

### Batch progress in serve mode

`tsdev_cli serve` accepts an optional `"batch":{"id":"<nonempty string>","index":<1-based>,"total":<n>}` member on `insert`/`modify`/`delete`/`upsert` requests. The object is all-or-none and closed - exactly those three members, `1 <= index <= total`; anything else rejects the request. For tagged requests the broker emits the same started/completed progress lines on its own stderr (human, or JSON under `--json`) and appends `"progress":{"batch_id":"<id>","index":<i>,"total":<t>,"phase":"completed"}` to the ordinary response - still exactly one response frame per request. This also holds when a request with *valid* batch metadata fails later validation or dispatch (unknown type, missing source, item already exists): the started/completed pair is still emitted and the failure response still carries `progress`. Only a request rejected for invalid `batch` metadata itself is an untagged failure with no progress. Untagged requests are byte-identical to before.

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

- `batch_insert(items, *, on_progress=None)` / `batch_modify(items, *, on_progress=None)` / `batch_upsert(items, *, on_progress=None)` / `batch_delete(items, *, on_progress=None)`
  - `items`: `Sequence[BatchItem]` -> `list[TsDevResult]`
- `batch_insert_dir(type, dir_path, extensions=None, *, on_progress=None)`
- `batch_modify_dir(type, dir_path, extensions=None, *, on_progress=None)`
- `batch_delete_dir(type, dir_path, extensions=None, *, on_progress=None)`
- `batch_upsert_dir(type, dir_path, extensions=None, *, on_progress=None)`

Per-item failures are returned in the list. Manifest/directory-level failures raise `TsDevError`.

### `BatchProgress` / `on_progress`

All eight batch methods accept the keyword-only `on_progress: BatchProgressCallback | None` argument, with the same public semantics on the serve path and the direct elevated path:

```python
from tsdev_automation import BatchProgress, BatchProgressCallback
# BatchProgress: frozen dataclass with .index (1-based), .total, .result (TsDevResult)
# BatchProgressCallback = Callable[[BatchProgress], bool | None]
```

- The callback is invoked synchronously, in input order, once per **attempted** item after its actual `TsDevResult` is available — success or failure. For directory batches the position is the sorted entry-list position; a local source-read failure is an actual result (`.error.code == "source_file_error"`) that also invokes the callback.
- Only a return value of `False` cancels, and only before the final item (`True`/`None`/`0`/anything else continue; `False` on the last item is ignored). On cancellation the returned list stays **full length**: attempted items keep their results and each unattempted item is a synthetic `TsDevResult` with `success=False`, `exit_code=12`, `error.code="cancelled"`, `error.message="Batch cancelled before this item was attempted"`. Markers never invoke the callback and cause no broker request or file read.
- A callback exception aborts the batch: the original exception propagates unchanged, no marker list is returned, and the serve broker stays alive and usable.
- Serve path (default non-elevated `use_serve=True`): every batch call tags each broker request with `"batch":{"id":<uuid4 hex>,"index":<i>,"total":<n>}` (one id per call) and validates the response's `"progress":{"batch_id","index","total","phase":"completed"}` member before parsing — a missing, malformed, or mismatched `progress` raises `TsDevError` without invoking the callback. This happens with or without a callback.
- Direct path (elevated session): the batch runs as a single `tsdev_cli` child with `--json` + `--batch-progress-control` on binary pipes — manifest batches stream the manifest as a framed `TSDEV_BATCH_CONTROL/1` block over stdin (no temp manifest file), directory batches keep the native argv with stdin carrying decisions only. The client validates the stderr progress events strictly (started/completed pairing, contiguous indexes, stable total/op/type/name, nested-result consistency, one coherent `batch_completed`), writes `continue`/`cancel` after each non-final item, and returns the authoritative final stdout array — close-rewrite and exit-12 markers included. Malformed/missing events raise `TsDevError` (after a cooperative `cancel` when a decision is pending); exit `1` is a batch-level `TsDevError`, NTSTATUS exits map to `TsDevCrashError`, exit `15` (dispatch watchdog) maps to `TsDevWatchdogAbortError`, and a `timeout` terminates the child and raises `TimeoutError`. A non-elevated `use_serve=False` session still raises `ElevationRequiredError` before launch; no-callback direct batches are unchanged.

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

