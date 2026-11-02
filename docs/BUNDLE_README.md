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

`TsDevClient(use_serve=True)` (the default) starts an elevated `tsdev_cli serve` broker server with one UAC prompt for the whole session. With `use_serve=False` (no persistent broker), a non-elevated Python session raises `ElevationRequiredError` before launch. Gotcha: in an ALREADY-elevated shell, `use_serve=True` BYPASSES the broker by design (`_serve_active = use_serve AND not elevated`) and runs the direct path -- results look identical either way. To verify the broker contract, run the client UNELEVATED with `use_serve=True` (or force the branch in tests by stubbing `_is_elevated`/`_serve_active`) and confirm via `broker_process_handle()` (`client.py:2325`) -- non-None means the broker ran.

## Native CLI (`tsdev_cli.exe`)

Single-item operations:

```powershell
tsdev_cli insert   <type> <name> <source_file>
tsdev_cli modify   <type> <name> <source_file>
tsdev_cli upsert   <type> <name> <source_file>
tsdev_cli delete   <type> <name>
tsdev_cli exists   <type> <name> [--case-insensitive]
tsdev_cli list     <type>
tsdev_cli get-source <type> <name> [--output <file>]
tsdev_cli update [check | <version> | latest] [--yes] [--force]
tsdev_cli --version
```

Global: `--json` emits a single JSON result -- for `batch-*` it instead emits the results array on stdout (empty without it) and renders stderr progress as JSON event lines; `--pbar` renders a visual progress bar on stderr that is quiet on success -- a successful mutation op's `SUCCESS:` stdout line is suppressed while query ops still print their answer (position-free, idempotent; usage error with `--json` or `serve`); `--ts-path <dir>` overrides the TradeStation install. `--backup-cache` / `--restore-cache` copy `<TS>\Program\Cache` to/from `%LOCALAPPDATA%\tsdev_cli\cache-backup` before the subcommand (or standalone; `--cache-backup-policy <prompt|overwrite|skip|abort>` governs an existing backup; not valid with `serve`). After a successful backup the backup tree is NTFS-compressed in the background (`--no-cache-compress` suppresses). `-v`/`--verbose` (position-free, repeatable) raises the stderr log level: default = warnings + errors only; one occurrence (`-v`) adds info; two or more (`-vv`) add debug internals; additional occurrences clamp at debug. `exists` is case-sensitive by default; `--case-insensitive` relaxes matching. `get-source` decrypts encrypted items automatically. `list` is a read-only query with no `<name>`: it prints the stored names of all items of `<type>`, one per line, sorted by name (case-insensitive ASCII comparison; names differing only by case keep their warehouse order) with no dedup -- names are verbatim stored names, case-preserved. Zero items produces no output at all -- that is success, not an error. With `--json` its single result object is `{"op":"list","type":"strategy","success":true,"exit_code":0,"names":[...]}` -- `names` is always present on success (an empty array when zero items). Exit codes follow `exists`: `0` success (including zero items), `1` usage error (`list` takes exactly one argument and no flags), `2` unknown type, `4` init/lookup failure (the CLI fails loudly rather than reporting a false empty list), `10` thread failure, `11` not elevated.

`--log-file <path>` (env mirror `TSDEV_LOG_FILE`, flag wins) opens the debug log-file sink: every log message is appended to `<path>` as a one-line record (`[<secs>] tid=<tid> <ERR|WRN|INF|DBG> | <message>`) at **every** level, independent of `-v` and `--pbar` -- the console stays user-facing while the file keeps the complete diagnostic stream. Off by default (nothing is written unless a path is configured). The file is created if missing and appended to otherwise, so sequential invocations and a respawned serve broker accumulate into one stream; an open failure warns once on stderr and never aborts. `TsDevClient` forwards `TSDEV_LOG_FILE` to the elevated `serve` broker (whose stderr is discarded, so the file is the only diagnostic record there); direct subprocess calls inherit the env automatically.

Batch via a manifest file or `--batch-stdin`:

```powershell
tsdev_cli batch-insert <manifest_file>
tsdev_cli batch-insert --batch-stdin
tsdev_cli batch-modify <manifest_file>
tsdev_cli batch-upsert <manifest_file>
tsdev_cli batch-delete <manifest_file>
```

Manifest format: one item per line (`<type> <name> [<source_file>]`; `source_file` is the rest of the line for insert/modify/upsert); `#` comments and blank lines are ignored. `<name>` may be double-quoted (`"<name>"`) when it contains whitespace -- resume manifests and directory batches use that form for spaced file stems. **Warning:** an *unquoted* spaced name silently re-parses to a different component -- on `batch-delete` the extra tokens are ignored and the delete hits the truncated name, so always quote names containing whitespace. An unterminated quote or junk fused after the closing quote (`"a"b`) is a pre-validation failure.

```text
# insert two functions
function MyFuncA ./src/MyFuncA.el
function "My Func B" ./src/MyFuncB.el
```

Every batch that runs also emits progress on **stderr** (always on, independent of `-v`): a `BATCH SUCCESS`/`BATCH FAILED` line after every attempted item -- a `BATCH FAILED` line is followed by the item's full detail block (compiler errors etc., the same text a single-op failure prints) -- then one `BATCH COMPLETE` summary, always last, after the end-of-batch warehouse close. With `--json` each stderr line is one compact JSON event object instead (`batch_item_started` before each attempted item -- started events exist only in this NDJSON protocol and on tagged serve requests -- `batch_item_completed`, `batch_completed`), and `--json` is also what produces the stdout JSON results array; without it stdout is empty. That array remains the authoritative result -- a failed end-of-batch warehouse close rewrites still-successful items to failures, and `BATCH COMPLETE`/`batch_completed` is emitted after that rewrite (`close_failed=true`). `--pbar` pins a visual progress bar to the last stderr line (redrawn in place, no ANSI escapes), quiet while items succeed: it fills 0->N as items complete with a `-- <n> failed` suffix on failures -- a failed item's `BATCH FAILED` marker and detail, plus library warnings/errors, scroll above the bar -- and `BATCH COMPLETE` prints only when the run was not fully clean. For single-item mutation subcommands the bar fills 0->1 (`-- 1 failed` on failure) and a success prints only the bar -- the `SUCCESS:` stdout line is suppressed while failures still print stderr detail; query ops still print their stdout answer. The rendering is a visual aid, not a contract; it may coexist with `--batch-progress-control`.

`--batch-progress-control` (batch-only) turns stdin into a cooperative control channel: after each completed non-final item the batch reads exactly one `continue\n` or `cancel\n` line before attempting the next. With `--batch-stdin` the manifest first arrives framed as `TSDEV_BATCH_CONTROL/1 <decimal byte length>\n` + exactly that many bytes (decision lines follow on the same stream); with a manifest path or `batch-*-dir`, stdin carries decision lines only. `cancel`, EOF, or malformed input stops before the next item (an anomaly first emits a `batch_control_error`/`BATCH CONTROL ERROR` diagnostic): the warehouse closes once, unattempted items become exit-12 `cancelled` markers -- under `--json` the stdout array stays full length -- and the process exits `3`.

Direct-path batches and `serve` mutating requests are additionally protected by the WHServer VAS-headroom guard: after each completed non-final batch item -- and in `serve` before dispatching each mutating request -- the guard measures WHServer.exe's free 32-bit user address space against TWO threshold pairs. The `--whserver-free-mb` (default `256`) / `--whserver-largest-free-mb` (default `64`) pair is the recycle trigger: a breach asks for a server restart only under an explicit `--recycle-whserver` authorization; it is not a danger level. The `--whserver-abort-free-mb` (default `64`) / `--whserver-abort-largest-free-mb` (default `32`) pair is the real floor. Below the trigger but above the floor the run continues in-process unless an explicit Full authorization recycles and no pre-activation latch saw another TradeStation program already holding the server's lock (a `whserver_vas_continue`/`NOTE WHServer memory low` diagnostic is emitted once per run; no markers, manifest, or nonzero exit) -- and a refused recycle whose reason is `census_lock` (a foreign client demonstrably holds the server) also continues above the floor. Only a real floor breach aborts: on `batch-*` it emits a `batch_aborted`/`BATCH ABORTED` event, fills unattempted items with exit-12 not-run markers (`error.message="Batch aborted (WHServer VAS headroom) before this item was attempted"`), adds `aborted=true abort_reason=whserver_vas_free=<N>MB` (or `whserver_vas_largest_free=<N>MB`) to the summary, and exits `14` -- unless the recycle boundary resolves the breach instead (next paragraph): the DEFAULT (`RescueOnly`) rescues below the floor, an explicit `--no-recycle-whserver` (`Off`) hard-aborts. On `serve` an `Off` below-floor breach refuses that one request with a per-item exit `14` (`error.code="whserver_vas_abort"`); the broker stays alive and non-mutating requests still pass. `--no-whserver-vas-guard` disables it; env mirrors `TSDEV_WHSERVER_FREE_MB` / `TSDEV_WHSERVER_LARGEST_FREE_MB` / `TSDEV_WHSERVER_ABORT_FREE_MB` / `TSDEV_WHSERVER_ABORT_LARGEST_FREE_MB` / `TSDEV_NO_WHSERVER_VAS_GUARD` apply when the flags are absent. WHServer not running passes; a transient measurement failure warns once and does not abort. The guard is read-only.

The WHServer recycle is TRI-STATE (resolved once per process from flags/env; the zero-client census gate applies in every mode that reaches the boundary). **Default `RescueOnly`:** the recycle is reserved for the abort-floor rescue -- a breach below the floor attempts the recycle boundary instead of aborting, a trigger-band breach always continues in-process. One one-line NOTE is printed per process/broker at batch-*/serve start: `tsdev_cli: NOTE WHServer auto-restart near the abort floor is on by default -- disable with --no-recycle-whserver or TSDEV_NO_RECYCLE_WHSERVER=1`. **`--recycle-whserver` (env mirror `TSDEV_RECYCLE_WHSERVER`, any value but `0`) = Full:** authorizes restarting WHServer.exe on ANY guard breach (the trigger band included). It is a dangerous-operation opt-in (one-time 5-line stderr warning; valid only with `batch-*`/`serve`; cannot combine with `--no-whserver-vas-guard` or `--batch-progress-control` -- those refusals apply to this explicit mode only). **`--no-recycle-whserver` (env mirror `TSDEV_NO_RECYCLE_WHSERVER`, any value but `0`) = Off:** today's unauthorized behavior -- a band breach keeps working, a below-floor breach hard-aborts (exit 14). Precedence: both polarity FLAGS together are a usage error (exit 1); an explicit flag beats the opposite-polarity env var; two conflicting env vars resolve to the NEGATIVE arm; an env value of `0` only neutralizes its own env arm (it never opts out of the default and never vetoes an explicit flag). A `RescueOnly` batch run under `--batch-progress-control` is not refused: the floor rescue is SUPPRESSED for that run (a mid-batch recycle exit would strand the control channel -- the run behaves as `Off`) and a one-line NOTE names the suppression: `tsdev_cli: NOTE WHServer auto-restart near the abort floor is suppressed for this run by --batch-progress-control (the control channel belongs to the dying process)`. On a breach that reaches the boundary: **exit `18`** means WHServer was recycled -- or, at an attached site, that the recycle was *deferred* to the resumed process/broker (the event carries `reason=recycle_deferred_own_session`): the engine only runs in-process at a never-attached site, because an attached process holds +7 `m_nLock` for its whole lifetime and `closeWarehouse` cannot release it -- only process exit does. Either way the full-length array (attempted head + exit-12 markers) is emitted and a `tsdev_resume_<pid>_<ticks>.manifest` covering the tail is written beside the source manifest (cwd for `--batch-stdin`); resubmit with `tsdev_cli batch-<op> <resume-file>` -- `TsDevClient` does this automatically, bounded at 3 recycles per batch call, and deletes the consumed file once the resumed batch returns. **Exit `19`** means the gate refused the recycle -- the batch aborts, the tail gets `WHServer recycle refused before this item was attempted` markers, no respawn. If the tail contains an item the manifest schema cannot represent (a name with a line break / leading quote / quote+whitespace, or a pre-validated `preValid=false` item), the writer refuses the whole manifest before creating any file -- auto-resume is forfeited for the good tail items too (fail-closed; a re-tokenized line could retarget a delete), and the stderr diagnostic names the refusing item. In serve mode each exit-18 boundary (recycle or defer) respawns the elevated broker -- one effective recycle can cost two respawns -- a non-elevated client pays one UAC prompt per respawn. If the deferred boundary's own close stage hangs, its monitor kills the process without returning (exit `19` with a `tsdev_cli: RECYCLE ABORT: closeWarehouse exceeded ...` stderr line): the already-emitted results array and the already-written resume manifest file survive, so no completed result is lost -- but no recycle event is emitted, so automatic resume cannot fire; resubmit the manifest file (`tsdev_resume_<pid>_<ticks>.manifest` in the resume directory) manually.

All subcommands are additionally protected by the dispatch watchdog: every orlib20 warehouse dispatch is bracketed by an in-flight marker, and a watchdog thread polls the marker once per second and aborts the process when a dispatch exceeds the bound. If WHServer.exe wedges or crashes mid-call (orlib20's synchronous COM wait never returns), the watchdog aborts truthfully instead of hanging forever: it fires when an in-flight dispatch exceeds `--dispatch-timeout-secs <N>` (default `300`, range `0..86400`, `0` disables; env mirror `TSDEV_DISPATCH_TIMEOUT_SECS`), when a `TSCrashReport.exe` runs as a child of a WHServer.exe pid during a flight (`whserver_crashed`), or when a WHServer.exe observed during the flight vanishes (`whserver_gone` -- confirmed only after 3 consecutive empty 1 s polls, since a single empty/failed process snapshot is inconclusive). The timeout is the sole guaranteed backstop -- a wedged-but-alive WHServer is detected only when the dispatch exceeds the bound; the `whserver_crashed`/`whserver_gone` checks are best-effort fast paths. On fire it prints a `tsdev_cli: WATCHDOG ABORT: warehouse dispatch '<site>' ... [reason: ...]; exiting with code 15` line on stderr and terminates itself with exit code `15`. A watchdog abort emits **no JSON frame** for the in-flight item -- `--json` consumers get no result line for it (it is the "uncertain state" item). What survives of the completed items depends on the path: under `serve` each completed request already returned its JSON frame, so those results are safe; on the direct `batch-*` path the stdout JSON array is emitted only after the whole batch finishes, so a mid-batch abort loses every item's result -- the stderr abort line plus the already-streamed per-item progress events (`BATCH SUCCESS`/`BATCH FAILED` lines, or `batch_item_completed` events under `--json`) are the only record -- and under `--pbar` successes emit no completion line, so a `--pbar` abort leaves no per-item record at all. On the serve path the watchdog kills the whole broker process; the Python client surfaces that as `TsDevWatchdogAbortError` rather than retrying the request into the same wedged server. Under `TsDevClient(use_serve=True)` the broker is spawned hidden with no stderr capture, so the `WATCHDOG ABORT` line and its site/reason detail are not observable on that path -- exit code `15`, surfaced as `TsDevWatchdogAbortError`, is the only machine-readable signal. Idle waits (e.g. the `serve` broker blocked in `ConnectNamedPipe`) carry no in-flight marker and are exempt.

`tsdev_cli serve` accepts an optional `"batch":{"id","index","total"}` member on `insert`/`modify`/`delete`/`upsert` requests (exactly those members, `1 <= index <= total`; malformed tags reject the request). Tagged requests emit a per-item started/completed progress pair on the broker's stderr (human lines, or JSON events under `--json`) and append a `"progress":{"batch_id","index","total","phase":"completed"}` member to the ordinary response -- still exactly one response frame per request. A request whose batch metadata is valid still gets the progress pair and enriched response even when the operation itself fails validation or dispatch (unknown type, missing source, duplicate insert); only an invalid `batch` member produces an untagged failure. Untagged requests are unchanged.

### Self-update

`tsdev_cli.exe` can update itself in place -- no installer, no manual re-download:

```powershell
.\tsdev_cli.exe update check   # check only, change nothing
.\tsdev_cli.exe update         # install the latest release ("update latest" is the same)
.\tsdev_cli.exe update v0.1.5  # install a specific version (upgrade, reinstall, or downgrade)
.\tsdev_cli.exe --version      # print the running version; evaluated before the elevation check
```

Typical session (administrator terminal):

```text
> .\tsdev_cli.exe update check
Checking for updates ...
Update available: v0.1.5 -> v0.1.6.
Run: tsdev_cli update

> .\tsdev_cli.exe update
Checking for updates ...
Downloading tsdev_automation_v0.1.6.zip (2.4 MB) ...
Installing v0.1.6 ...
Updated: v0.1.5 -> v0.1.6.
```

Windows asks for administrator permission when the command runs -- that is normal; click Yes. This is the same UAC prompt every `tsdev_cli.exe` command already shows; update adds zero extra prompts.

- **Release feed:** the GitHub Releases of `nohype/tsdev_automation`. "Latest" means the highest published version number (numeric semver compare: `v0.1.10 > v0.1.9`); drafts and prereleases are ignored, and releases whose tag is not a plain `vX.Y.Z` version are invisible. This requires the GitHub repo to be PUBLIC -- while it is private, every check fails with exit `25`.
- **Integrity:** the downloaded zip's SHA-256 is compared to the checksum published with the release asset. A release with no published checksum, or a checksum mismatch, is refused -- nothing is changed.
- **Staged swap:** the zip is downloaded and unpacked into a staging folder next to the exe; then the existing `tsdev_cli.exe`, `tsdev_automation\`, and `README.md` are renamed to `*.old` and the new files are moved in. The `*.old` leftovers are deleted automatically after the command exits (and stale ones are cleaned up on later runs). A failure mid-install rolls the `*.old` files back.
- **Capability-loss confirmation:** releases older than `v0.1.6` cannot update themselves. Installing one asks a yes/no confirmation first; `--yes` (or `-y`) pre-answers it. On a non-interactive console (scripts, hidden windows) the install is refused with exit `28` unless `--yes` is passed.
- **First-time note:** copies at v0.1.5 or older have no `update` command at all -- they must be replaced by one last manual unzip. Self-update only exists from v0.1.6 onward.
- **`--force`:** overrides the development-build refusal (`0.0.0-dev`), the not-a-release-bundle refusal (folder without `tsdev_automation\` + `README.md` next to the exe), and forces a reinstall at the same version (repair). It does not pre-answer the confirmation prompt.
- **Another `tsdev_cli.exe` running** (for example a `serve` broker) does not block the update; the broker keeps serving the OLD version until it is restarted -- a NOTE is printed in that case.
- **Power loss mid-install:** if the tool folder is left broken -- the exe missing and `*.old` files present -- rename the `*.old` files back to their original names to recover, or unzip the release over the folder.

### Exit codes (complete table -- SSOT)

This is the single authoritative exit-code table for every `tsdev_cli.exe`
subcommand (item ops, batch aggregates, cache phase, WHServer guard/recycle,
watchdogs, and `update`). Success is always `0`; any nonzero exit means the
operation did not succeed. `error.code` is the machine-readable string in
`--json` failure frames (`op` disambiguates exit `3` -- `get-source` reports
`output_write_error`, other ops report `source_file_error` -- and exit `5` --
`insert` reports `already_exists`, other ops report `not_found`). Codes
`13`/`15`/`20-24` and SEH crashes terminate the process directly and emit no
JSON frame. Batch aggregates reuse `0`/`1`/`2`/`3`/`14` with the batch
meanings noted below. (Sources: `src/tsdev_cli.cpp` header comment +
`errorCodeString`, `src/dispatch_watchdog.cpp`,
`src/diag_watchdog.cpp`, `docs/UPDATE_DESIGN.md` section 5.)

| Exit | `error.code` | Meaning |
|---|---|---|
| 0 | - | Success: the operation succeeded (`list` with zero items and `update check` with no new version are successes). Batch aggregate: all items succeeded. |
| 1 | `usage_error` | Usage/argument error (bad subcommand, flags, or version token). Batch aggregate: batch did not run (manifest missing/empty/unusable). |
| 2 | `unknown_type` | Unknown component type. Batch aggregate: batch ran but one or more items failed (per-item detail on stderr, or the `--json` results array). |
| 3 | `source_file_error` (`output_write_error` on `get-source`) | Source file unreadable, or `get-source` output write failure. Batch aggregate: cooperatively cancelled via `--batch-progress-control` (stdout array stays full length; unattempted items are exit-12 `cancelled` markers). |
| 4 | `init_failure` | TradeStation 9.5 / orlib20 / warehouse init or lookup failure (install not found -- pass `--ts-path` -- or a name-index lookup failure, which fails loudly). |
| 5 | `already_exists` (`insert`) / `not_found` (other ops) | `insert`: name already taken (also: `restore-cache` with no backup found). `modify`/`delete`/etc.: name not in the warehouse. |
| 6 | `entry_op_failure` | Warehouse entry operation failed (create/delete entry). |
| 7 | `save_source_failure` | Saving the EasyLanguage source into the warehouse failed. |
| 8 | `compile_error` | EasyLanguage compile/verify failed (compiler errors printed; nothing was added or changed). |
| 9 | `artifact_missing` | Post-op `.ell`/artifact check failed (file missing or unreadable). |
| 10 | `thread_failure` | `CreateThread` failure (dispatch/worker thread could not start). |
| 11 | `not_elevated` | Process is not elevated -- re-run from an administrator terminal (`--version` is exempt: it is evaluated before the guard). |
| 12 | `cancelled` | User declined a confirmation prompt (update capability-loss prompt, cooperative batch cancel). Also the per-item marker for unattempted batch items and the Python client elevation-required marker. |
| 13 | (no JSON frame) | Watchdog abort in DIAG builds only (`tsdev_cli_diag`; see `src/diag_watchdog.cpp`). Production watchdog aborts are exit 15. |
| 14 | `whserver_vas_abort` | Below-floor WHServer free-VAS breach: batch aborted (remaining items become exit-12 not-run markers) or, on `serve`, the one mutating request refused without executing. |
| 15 | (no JSON frame) | Dispatch watchdog abort: an orlib20 warehouse dispatch exceeded `--dispatch-timeout-secs` (or WHServer crashed/vanished mid-call). Stderr prints `WATCHDOG ABORT: ... exiting with code 15`. |
| 16 | `cache_io_error` | `--backup-cache`/`--restore-cache` I/O failure (cache dir or backup root unreachable, locked-cache restore). |
| 17 | `cache_cancelled` | Cache backup cancelled (`--cache-backup-policy=abort` or `C` at the prompt). |
| 18 | `whserver_recycled` | WHServer.exe was recycled at the recycle boundary (explicit `--recycle-whserver`, or the default abort-floor rescue) -- resume in a fresh process / respawned broker. |
| 19 | `whserver_recycle_refused` | Recycle refused (gate or fail-closed stage) and the batch aborted. |
| 20 | (no JSON frame) | `std::terminate` containment (unhandled C++ exception / noexcept violation). |
| 21 | (no JSON frame) | `abort()` / SIGABRT containment. |
| 22 | (no JSON frame) | CRT invalid-parameter fatal containment. |
| 23 | (no JSON frame) | Pure virtual call containment. |
| 24 | (no JSON frame) | UEF invoked with no exception record (defensive) containment. |
| 25 | `update_check_failed` | Cannot check for updates: offline/DNS, HTTP error, rate limit, malformed feed. (While the GitHub repo is still private every check fails this way -- expected pre-flip.) |
| 26 | `update_not_found` | Feed worked but the requested version is not a published release, or its release has no `tsdev_automation_<tag>.zip` bundle asset. |
| 27 | `update_failed` | Fetch/verify/install failure: checksum mismatch, missing checksum, corrupt zip, disk/write error, swap failure (rolled back when possible), concurrent-update lock held. |
| 28 | `update_refused` | Policy refusal needing explicit opt-in: dev build or non-bundle dir without `--force`, or a required confirmation on a non-interactive channel without `--yes`. |
| crash | (no JSON frame) | Any other fatal crash exits with the SEH/NTSTATUS exception code itself (e.g. `0xC0000005`), never through WER/TSCrashReport. |

**Testing/internal overrides** (hidden from usage text; not supported end-user flags): `--update-url <base>` (env mirror `TSDEV_UPDATE_URL`, flag wins) replaces the release-feed API base URL entirely -- used by test fixtures. `--update-result-file <path>` (global flag, valid only with the `update` subcommand) atomically writes the JSON result frame to `<path>` at every point where `--json` would emit one -- used by the Python client's elevated launch and by test harnesses.

## Python wrapper (`TsDevClient`)

Add the directory containing `tsdev_automation/` to `PYTHONPATH`; `tsdev_cli.exe` is discovered as a sibling automatically.

```python
from tsdev_automation import TsDevClient, BatchItem

client = TsDevClient()  # use_serve=True by default; one UAC prompt
```

`TsDevClient(recycle_whserver=...)` is tri-state: `None` (default) forwards nothing -- the CLI's default (`RescueOnly`: the WHServer auto-restart is reserved for the abort-floor rescue) plus the `TSDEV_RECYCLE_WHSERVER`/`TSDEV_NO_RECYCLE_WHSERVER` env vars apply; `True` forwards `--recycle-whserver` (Full); `False` forwards `--no-recycle-whserver` (Off) on batch command lines AND on the serve broker launch (`runas` drops the caller's environment, so the negative env var could not reach the broker otherwise). The client-side `UserWarning` fires only on explicit authorization (`True` or the env var), never for the default.

Single-item methods:

```python
client.insert(type, name, source)                   # -> TsDevResult
client.modify(type, name, source)                   # -> TsDevResult
client.upsert(type, name, source)                   # -> TsDevResult
client.delete(type, name)                           # -> TsDevResult
client.exists(type, name, case_insensitive=False)   # -> TsDevResult
client.list_items(type)                             # -> TsDevResult
client.get_source(type, name, output_file=None)     # -> TsDevResult
```

All return a `TsDevResult` dataclass. `insert`/`modify`/`upsert`/`delete`/`get_source` raise a `TsDevError` subclass on failure; `exists` does not raise on not-found (not-found is a successful query); `list_items` raises a `TsDevError` subclass only on real errors (a type with no items is a successful query returning an empty list). Relevant fields: `.success: bool`, `.exit_code: int`; insert/modify/upsert/delete populate `.prog_id: int | None` and `.ell_path: str | None`; `exists` populates `.exists: bool`, `.stored_name: str`, `.prog_id: int` (`0` and `""` when not found); `list_items` populates `.names: list[str]` -- the stored names, verbatim, in the CLI's output order (empty list when the type has no items); `get_source` populates `.source: str`. On failure, `.error: TsDevErrorInfo | None` carries `.code`, `.message`, and `.compile_errors` (compile failure).

`source` may be a file path or an EasyLanguage source string. If `output_file` is given, the source is also written to disk.

Batch methods:

```python
client.batch_insert(items, *, on_progress=None)   # -> list[TsDevResult]
client.batch_modify(items, *, on_progress=None)   # -> list[TsDevResult]
client.batch_upsert(items, *, on_progress=None)   # -> list[TsDevResult]
client.batch_delete(items, *, on_progress=None)   # -> list[TsDevResult]
```

`items` is a `Sequence[BatchItem]`; each method returns one `TsDevResult` per item, in order. Directory batch helpers (`batch_*_dir`) are also available and also return `list[TsDevResult]`. Per-item failures are returned in the result list (not raised); manifest- or directory-level failures raise `TsDevError`.

`on_progress` (keyword-only, `BatchProgressCallback | None`, all eight batch methods) is invoked synchronously in input order with a frozen `BatchProgress(index, total, result)` after each **attempted** item's actual result -- success or failure -- with the same public semantics on the serve path and the direct elevated path. Only a `False` return cancels (before the next item; `False` on the final item is ignored). On cancellation the list stays full length: unattempted items become `exit_code=12`, `error.code="cancelled"` markers that never invoke the callback. A callback exception aborts the batch and propagates unchanged; the broker stays usable. Serve requests carry `"batch":{"id","index","total"}` metadata and each response's `"progress"` member is validated before parsing (a malformed/mismatched member raises `TsDevError`, no callback). On the direct path the batch runs as one `tsdev_cli` child with `--json` + `--batch-progress-control`: manifest batches stream a framed `TSDEV_BATCH_CONTROL/1` manifest over stdin (kept open for `continue`/`cancel` decisions; no temp manifest file), directory batches keep the native argv with decisions-only stdin; the client validates the stderr progress events strictly, writes a decision after each non-final item, and returns the authoritative final stdout array (exit `1` -> `TsDevError`, NTSTATUS -> `TsDevCrashError`, timeout -> `TimeoutError`). A non-elevated `use_serve=False` session still raises `ElevationRequiredError` before launch; no-callback direct batches are unchanged.

Self-update methods:

```python
client.check_update()                                       # -> TsDevResult
client.update(version=None, *, yes=False, force=False)      # -> TsDevResult
```

`version=None` installs the latest release; `"v0.1.5"`/`"0.1.5"`/`"latest"` are passed through to the CLI (which validates them). `yes=True` forwards `--yes` (pre-answers the capability-loss confirmation -- required on the non-interactive elevated launch); `force=True` forwards `--force`. `check_update()` takes no arguments.

These calls are **never routed through the serve broker** -- the broker is the binary being replaced. Both always launch `tsdev_cli --json update ...` directly, even when `use_serve=True` and a broker is running. On a non-elevated session each call costs **one UAC prompt** (elevated `runas` launch with no console, regardless of `use_serve`; a declined prompt raises `PermissionError`). On an elevated session they run as a plain subprocess.

`check_update()` never raises for the "up to date"/"ahead"/"dev build" verdicts -- those are successful results. `update()` raises `CancelledError` when a confirmation is declined, and `UpdateRefusedError` when a needed confirmation could not be asked (the exception's remedy note says to re-run with `yes=True`).

`TsDevResult` gains update fields (all `None`/`False`/empty on non-update calls): `.current_version`, `.latest_version`, `.update_available`, `.target_version`, `.dev_build`, `.asset` (dict or `None`), `.from_version`, `.to_version`, `.downgrade`, `.already_current`, `.files_replaced`, `.notes` (list of strings), `.rolled_back`, `.requires_force`, `.requires_confirmation`. `TsDevErrorInfo` gains `.retry_after_secs`.

New exceptions (`TsDevError` subclasses, mapped by `error.code`): `UpdateCheckError` (exit 25), `UpdateNotFoundError` (26), `UpdateFailedError` (27), `UpdateRefusedError` (28), `CancelledError` (12).

When `update()` succeeds while the client owns a live serve broker, `.notes` contains `broker_restart_required`: the old broker keeps running and keeps serving valid requests (the wire protocol is unchanged) until it is restarted -- `client.close()` plus a new `TsDevClient()` picks up the new version.
