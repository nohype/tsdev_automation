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

`TsDevClient(use_serve=True)` (the default) starts an elevated `tsdev_cli serve` broker server with one UAC prompt for the whole session. With `use_serve=False` (no persistent broker), a non-elevated Python session raises `ElevationRequiredError` before launch.

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

Global: `--json` emits a single JSON result — for `batch-*` it instead emits the results array on stdout (empty without it) and renders stderr progress as JSON event lines; `--pbar` renders a visual progress bar on stderr that is quiet on success — a successful mutation op's `SUCCESS:` stdout line is suppressed while query ops still print their answer (position-free, idempotent; usage error with `--json` or `serve`); `--ts-path <dir>` overrides the TradeStation install. `--backup-cache` / `--restore-cache` copy `<TS>\Program\Cache` to/from `%LOCALAPPDATA%\tsdev_cli\cache-backup` before the subcommand (or standalone; `--cache-backup-policy <prompt|overwrite|skip|abort>` governs an existing backup; not valid with `serve`). After a successful backup the backup tree is NTFS-compressed in the background (`--no-cache-compress` suppresses). `-v`/`--verbose` (position-free, repeatable) raises the stderr log level: default = warnings + errors only; one occurrence (`-v`) adds info; two or more (`-vv`) add debug internals; additional occurrences clamp at debug. `exists` is case-sensitive by default; `--case-insensitive` relaxes matching. `get-source` decrypts encrypted items automatically.

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

Every batch that runs also emits progress on **stderr** (always on, independent of `-v`): a `BATCH SUCCESS`/`BATCH FAILED` line after every attempted item — a `BATCH FAILED` line is followed by the item's full detail block (compiler errors etc., the same text a single-op failure prints) — then one `BATCH COMPLETE` summary, always last, after the end-of-batch warehouse close. With `--json` each stderr line is one compact JSON event object instead (`batch_item_started` before each attempted item — started events exist only in this NDJSON protocol and on tagged serve requests — `batch_item_completed`, `batch_completed`), and `--json` is also what produces the stdout JSON results array; without it stdout is empty. That array remains the authoritative result — a failed end-of-batch warehouse close rewrites still-successful items to failures, and `BATCH COMPLETE`/`batch_completed` is emitted after that rewrite (`close_failed=true`). `--pbar` pins a visual progress bar to the last stderr line (redrawn in place, no ANSI escapes), quiet while items succeed: it fills 0→N as items complete with a `— <n> failed` suffix on failures — a failed item's `BATCH FAILED` marker and detail, plus library warnings/errors, scroll above the bar — and `BATCH COMPLETE` prints only when the run was not fully clean. For single-item mutation subcommands the bar fills 0→1 (`— 1 failed` on failure) and a success prints only the bar — the `SUCCESS:` stdout line is suppressed while failures still print stderr detail; query ops still print their stdout answer. The rendering is a visual aid, not a contract; it may coexist with `--batch-progress-control`.

`--batch-progress-control` (batch-only) turns stdin into a cooperative control channel: after each completed non-final item the batch reads exactly one `continue\n` or `cancel\n` line before attempting the next. With `--batch-stdin` the manifest first arrives framed as `TSDEV_BATCH_CONTROL/1 <decimal byte length>\n` + exactly that many bytes (decision lines follow on the same stream); with a manifest path or `batch-*-dir`, stdin carries decision lines only. `cancel`, EOF, or malformed input stops before the next item (an anomaly first emits a `batch_control_error`/`BATCH CONTROL ERROR` diagnostic): the warehouse closes once, unattempted items become exit-12 `cancelled` markers — under `--json` the stdout array stays full length — and the process exits `3`.

Direct-path batches and `serve` mutating requests are additionally protected by the WHServer VAS-headroom guard: after each completed non-final batch item — and in `serve` before dispatching each mutating request — the guard measures WHServer.exe's free 32-bit user address space and refuses to proceed when total free MB drops below `--whserver-free-mb` (default `256`) or the largest contiguous free block drops below `--whserver-largest-free-mb` (default `64`). On `batch-*` an abort emits a `batch_aborted`/`BATCH ABORTED` event, fills unattempted items with exit-12 not-run markers (`error.message="Batch aborted (WHServer VAS headroom) before this item was attempted"`), adds `aborted=true abort_reason=whserver_vas_free=<N>MB` (or `whserver_vas_largest_free=<N>MB`) to the summary, and exits `14`. On `serve` the same breach instead refuses that one request with a per-item exit `14` (`error.code="whserver_vas_abort"`); the broker stays alive and non-mutating requests still pass. `--no-whserver-vas-guard` disables it; env mirrors `TSDEV_WHSERVER_FREE_MB` / `TSDEV_WHSERVER_LARGEST_FREE_MB` / `TSDEV_NO_WHSERVER_VAS_GUARD` apply when the flags are absent. WHServer not running passes; a transient measurement failure warns once and does not abort. The guard is read-only.

All subcommands are additionally protected by the dispatch watchdog: every orlib20 warehouse dispatch is bracketed by an in-flight marker, and a watchdog thread polls the marker once per second and aborts the process when a dispatch exceeds the bound. If WHServer.exe wedges or crashes mid-call (orlib20's synchronous COM wait never returns), the watchdog aborts truthfully instead of hanging forever: it fires when an in-flight dispatch exceeds `--dispatch-timeout-secs <N>` (default `300`, range `0..86400`, `0` disables; env mirror `TSDEV_DISPATCH_TIMEOUT_SECS`), when a `TSCrashReport.exe` runs as a child of a WHServer.exe pid during a flight (`whserver_crashed`), or when a WHServer.exe observed during the flight vanishes (`whserver_gone` — confirmed only after 3 consecutive empty 1 s polls, since a single empty/failed process snapshot is inconclusive). The timeout is the sole guaranteed backstop — a wedged-but-alive WHServer is detected only when the dispatch exceeds the bound; the `whserver_crashed`/`whserver_gone` checks are best-effort fast paths. On fire it prints a `tsdev_cli: WATCHDOG ABORT: warehouse dispatch '<site>' ... [reason: ...]; exiting with code 15` line on stderr and terminates itself with exit code `15`. A watchdog abort emits **no JSON frame** for the in-flight item — `--json` consumers get no result line for it (it is the "uncertain state" item). What survives of the completed items depends on the path: under `serve` each completed request already returned its JSON frame, so those results are safe; on the direct `batch-*` path the stdout JSON array is emitted only after the whole batch finishes, so a mid-batch abort loses every item's result — the stderr abort line plus the already-streamed per-item progress events (`BATCH SUCCESS`/`BATCH FAILED` lines, or `batch_item_completed` events under `--json`) are the only record — and under `--pbar` successes emit no completion line, so a `--pbar` abort leaves no per-item record at all. On the serve path the watchdog kills the whole broker process; the Python client surfaces that as `TsDevWatchdogAbortError` rather than retrying the request into the same wedged server. Under `TsDevClient(use_serve=True)` the broker is spawned hidden with no stderr capture, so the `WATCHDOG ABORT` line and its site/reason detail are not observable on that path — exit code `15`, surfaced as `TsDevWatchdogAbortError`, is the only machine-readable signal. Idle waits (e.g. the `serve` broker blocked in `ConnectNamedPipe`) carry no in-flight marker and are exempt.

`tsdev_cli serve` accepts an optional `"batch":{"id","index","total"}` member on `insert`/`modify`/`delete`/`upsert` requests (exactly those members, `1 <= index <= total`; malformed tags reject the request). Tagged requests emit a per-item started/completed progress pair on the broker's stderr (human lines, or JSON events under `--json`) and append a `"progress":{"batch_id","index","total","phase":"completed"}` member to the ordinary response — still exactly one response frame per request. A request whose batch metadata is valid still gets the progress pair and enriched response even when the operation itself fails validation or dispatch (unknown type, missing source, duplicate insert); only an invalid `batch` member produces an untagged failure. Untagged requests are unchanged.

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
client.batch_insert(items, *, on_progress=None)   # -> list[TsDevResult]
client.batch_modify(items, *, on_progress=None)   # -> list[TsDevResult]
client.batch_upsert(items, *, on_progress=None)   # -> list[TsDevResult]
client.batch_delete(items, *, on_progress=None)   # -> list[TsDevResult]
```

`items` is a `Sequence[BatchItem]`; each method returns one `TsDevResult` per item, in order. Directory batch helpers (`batch_*_dir`) are also available and also return `list[TsDevResult]`. Per-item failures are returned in the result list (not raised); manifest- or directory-level failures raise `TsDevError`.

`on_progress` (keyword-only, `BatchProgressCallback | None`, all eight batch methods) is invoked synchronously in input order with a frozen `BatchProgress(index, total, result)` after each **attempted** item's actual result — success or failure — with the same public semantics on the serve path and the direct elevated path. Only a `False` return cancels (before the next item; `False` on the final item is ignored). On cancellation the list stays full length: unattempted items become `exit_code=12`, `error.code="cancelled"` markers that never invoke the callback. A callback exception aborts the batch and propagates unchanged; the broker stays usable. Serve requests carry `"batch":{"id","index","total"}` metadata and each response's `"progress"` member is validated before parsing (a malformed/mismatched member raises `TsDevError`, no callback). On the direct path the batch runs as one `tsdev_cli` child with `--json` + `--batch-progress-control`: manifest batches stream a framed `TSDEV_BATCH_CONTROL/1` manifest over stdin (kept open for `continue`/`cancel` decisions; no temp manifest file), directory batches keep the native argv with decisions-only stdin; the client validates the stderr progress events strictly, writes a decision after each non-final item, and returns the authoritative final stdout array (exit `1` -> `TsDevError`, NTSTATUS -> `TsDevCrashError`, timeout -> `TimeoutError`). A non-elevated `use_serve=False` session still raises `ElevationRequiredError` before launch; no-callback direct batches are unchanged.
