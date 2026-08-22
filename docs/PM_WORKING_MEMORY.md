# PM Working Memory — TSDev Automation

> **READ THIS FILE FIRST at the start of every turn / after any context compaction.**
> This is the PM's outsourced memory. Keep it updated; re-read it regularly.

## CRITICAL CORRECTION (2026-08-22) — "PASSWORD PROTECTION" HYPOTHESIS IS WRONG

### What was wrong
The previous "password-protection fix" (section below) was based on a WRONG interpretation. The user has clarified:
- **There is NO password protection for items in the library.** Password protection only exists for exported .ELD files.
- TSDev may internally encrypt data on disk, but this is entirely hidden from the user.
- The user ALWAYS sees plain text EL code in the TSDev UI, as if raw text was stored.

### What the verified data actually shows
- `_LoadTextFileEX@16` with NULL password returns encrypted binary for certain items (BTA_PROTEGE-SmartCode-JW3, Average, Volume).
- The data has a 10-byte header `21 E0 71 9B 25 EE 1C 07 01 EB 00` followed by high-entropy bytes.
- Bit 10 (0x400) of USHORT at 0x7B correlates with encrypted storage — but this is NOT "password protection," it's "internally encrypted storage."
- TSDev decrypts this automatically. Our CLI does NOT. That's the bug.

### Ghidra found THREE paths in FUN_101a0f90
1. **Auto-decrypt path**: `0xD8 & 2` AND `0xCC == 0xCABECABE` → decrypts with internal hardcoded key
2. **Standard + NULL password**: raw byte copy (NO decryption) ← this is what we do, WRONG
3. **Standard + password**: decrypts with provided password

We took path 2 (raw copy) and got encrypted bytes. TSDev must take path 1 or 3, or use a different function entirely.

### Fix implemented and verified (2026-08-22)
- **Root cause:** `_LoadTextFileEX@16` with NULL password returns raw encrypted bytes for items with 0x400 flag set. TSDev passes a non-NULL zeroed 64-byte buffer (FUN_10108630: `memset(pw, 0, 0x40)`) which triggers Branch C decryption — the password parameter is an OUTPUT buffer that receives extracted password metadata from the data header. Any non-NULL pointer works.
- **Fix:** `getSource()` now checks 0x400 flag. If set, passes a zeroed 64-byte buffer → triggers decryption → returns plain text. If clear, passes NULL → raw copy (unchanged). If `_GetMasterRecByType` fails, falls back to NULL (graceful degradation).
- **Removed:** `PasswordProtectedError` (Python), exit code 6 / `password_protected` for get-source (C++), `isPasswordProtected` out-parameter from `getSource()`. Exit 6 is now only `entry_op_failure` (insert/modify/delete).
- **Kept:** dump() replace handler (safety net), ElevationRequiredError (correct, unrelated).
- **E2E verified:** BTA_PROTEGE-SmartCode-JW3 → success, full decrypted source. Average (built-in function) → success, full decrypted source. Nonexistent → exit 5 / not_found.
- **Python tests:** 120 passed, 15 deselected. Adversarial review: APPROVE.
- **Distribution updated:** D:\Nesnidalian (both layouts), dist/, artifacts/tsdev_automation_2608221310.zip

### Frida lesson: TSDev attach crash (2026-08-22)
A Frida subagent attached to TSDev.exe and hooked `_LoadTextFileEX@16` / `_GetMasterRecByType@16`. TSDev crashed after Frida detached. The likely cause was hook cleanup corrupting function bytes. Frida CAN and SHOULD be used to trace calls in TSDev — but the subagent should use care with attach/detach and prefer `Interceptor.attach` with proper cleanup, or use `Stalker` for non-invasive tracing where possible.

### Encoding facts (verified)
- Source is stored as raw bytes in OLE stream `M_%d_%d_Source` inside .ell files. No encoding conversion in the read path.
- Save path uses `lstrlenA()` — source is effectively ANSI or UTF-8 (no embedded nulls). UTF-16 cannot be saved correctly.
- No encoding field in master record. Encoding is implicit in the raw bytes.
- For non-encrypted items: plain ANSI/ASCII with CRLF, null-terminated. No BOM, no UTF-16.
- For encrypted items: encrypted binary with 10-byte header `21 E0 71 9B 25 EE 1C 07 01 EB 00`. After decryption: plain ANSI text.
- The dump() replace handler handles any edge cases with non-UTF-8 bytes in JSON serialization.

---

## GET-SOURCE CRASH + PASSWORD-PROTECTION FIX (2026-08-22) — SUPERSEDED (see correction above)

### Bug 1: get-source --json crash (0xE06D7363)
- **Root cause:** nlohmann::json `dump()` uses default `error_handler_t::strict`, which THROWS `type_error` (C++ exception 0xE06D7363) on invalid UTF-8 bytes in source text. Exception on main thread in `emitOutput()`, no try/catch, VEH doesn't catch 0xE06D7363 → TSCrashReport.exe.
- **Fix:** All 7 `dump()` calls replaced with `j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)` (replaces invalid UTF-8 with U+FFFD). `emitOutput()` and serve-mode `runServe()` inner loop wrapped in try/catch as belt-and-suspenders.

### Bug 2: get-source returns binary data for password-protected items
- **Root cause:** `getSource()` passes `password=NULL` to `_LoadTextFileEX@16`. For password-protected items, this does a plain `memcpy` of encrypted data (not decrypted source).
- **RE findings (Ghidra + Frida cross-verified):** Password-protection flag = **bit 10 (mask 0x400) of USHORT at byte offset 0x7B in the 266-byte master record**. NOT `rec[0x74] & 1` (that's a "source loaded" cache flag, not password). `_GetMasterRecByType@16` (already resolved, patched, SEH wrapper existed but was never called) retrieves the master record.
- **Fix:** `getSource()` gained `bool* isPasswordProtected` out-parameter. Before calling `_LoadTextFileEX`, calls `_GetMasterRecByType@16` and checks `*(ushort*)(rec+0x7B) & 0x400`. If set, returns exit 6 / `password_protected`. If `_GetMasterRecByType` fails, falls through to `_LoadTextFileEX` (graceful degradation — dump() fix handles binary data).
- **New exit code 6** for get-source (overloaded with `entry_op_failure` for insert/modify/delete; disambiguated by op in `errorCodeString()`).
- **New Python exception:** `PasswordProtectedError` (code="password_protected", exit 6).

### Bug 3: No elevation check for use_serve=False
- **Root cause:** When `use_serve=False` and Python not elevated, CLI (requireAdministrator manifest) fails with ERROR_ELEVATION_REQUIRED (740) — confusing error.
- **Fix (per user decision: "Hard error"):** `ElevationRequiredError` raised in `_run_cli()` and `_run_batch()` when `not self._use_serve and not self._is_elevated`. Message tells user to run as admin OR enable `use_serve=True` (default).
- **New Python exception:** `ElevationRequiredError` (code="elevation_required", exit 12).

### Adversarial review: APPROVE (1 MAJOR + 4 MINOR, all fixed)
- MAJOR: serve-mode catch block used exit 6 for unexpected exceptions → misclassified as password_protected for get-source. Fixed: use exit 1 / internal_error.
- MINOR: redundant exit-code check in get_source → removed, use exception_for. EntryOpError docstring fixed. ElevationRequiredError added to subclass test. Serve-mode password-protected test added.
- Additional fix: `ElevationRequiredError` and `PasswordProtectedError` exported from `__init__.py`.

### Verification
- C++ build: SUCCESS (VS 2022 Community, Win32/Release)
- E2E: get-source on password-protected item → exit 6, no crash. get-source on non-protected item → readable EL source. exists/insert/delete/modify all work.
- Python: 122 passed, 15 deselected.
- User's distribution updated (D:\Nesnidalian + D:\Nesnidalian\tsdev-automation).
- Distribution zip: D:\tsdev_automation\artifacts\tsdev_automation_2608221120.zip

### Key RE lesson
- The RE_F4 doc's claim that `param_6[0x74] & 1` is the password-protection flag was WRONG. It's a "source loaded" cache flag. The actual password flag is at `0x7B` bit 10. This was discovered by Frida dynamic verification (comparing protected vs non-protected items) and confirmed by Ghidra (FUN_101788d0 checks `*(ushort*)(param_1+0x7b) & 0x400` before decryption).

## K.6a COMPLETE (2026-08-21)

All 7 phases passed. Adversarial review APPROVED (all findings fixed: 1 CRITICAL, 1 MAJOR, 4 MINOR).

### Bugs found and fixed during K.6a:
1. `client.py`: `--serve` → `serve` subcommand
2. `client.py`: `f.read().strip()` → `f.readline().strip()` (handshake file has 2 lines)
3. `_pipe.py`: `ctypes.get_last_error()` → `_kernel32.GetLastError()` (Python 3.14 ctypes bug)
4. `tsdev_cli.cpp createServePipe()`: SDDL `OW` → dynamic user SID (OW resolves to Admins when elevated)
5. `tsdev_cli.cpp dispatchServeRequest()`: Removed per-op `closeWarehouse()`, use `flushPerItem=false`
6. `tsdev_cli.cpp doDelete()`: Added `flushPerItem` param
7. `tsdev_lib.cpp deleteEntry()`: Added `flush` param, skip `closeWarehouse()` when false
8. `tsdev_lib.cpp refreshCache()`: NULL check on `[warehouse+0x5E]` after cmd 0x0F
9. `tsdev_lib.cpp verifyModule()`: Auto-cleanup `deleteEntry` uses `flush=false` (CRITICAL fix from review)
10. `test_serve_e2e.py`: Valid EL source + compile-failure recovery test + public `broker_process_handle()`

### Frida trace findings:
- Crash at orlib20.dll RVA 0x1CF71B (cmd 0x4E handler, NULL deref on [warehouse+0x5E])
- Root cause: deleteEntry's closeWarehouse() frees IWHouse interface; refreshCache can't re-init; cmd 0x4E crashes

### Test counts:
- C++ regression: 12 suites, 135 cases (Suite 11: serve E2E, 2 pytest cases; Suite 12: ShowMe E2E, SM1-SM9)
- Python: pytest collects 131 tests (116 non-e2e + 15 e2e)
- Serve E2E: 2 tests (full lifecycle + compile-failure recovery)

### K.6b COMPLETE (2026-08-21)
- `use_serve` default flipped to `True` in `TsDevClient.__init__`
- Critical bug fixed: `_ServeBroker.start()` now validates CLI path exists before `ShellExecuteExW` (raises `FileNotFoundError` instead of Windows popup on non-existent CLI path)
- Serve-mode docs added to BUILD.md, E2E_TEST_SPEC.md (Part F), README.md
- `test_serve_e2e.py` updated to use default (no explicit `use_serve=True`)
- E2E: 2/2 serve E2E passed (default-flip path), 12/12 C++ regression suites / 135 cases passed
- Adversarial review: APPROVED (17 INFO findings)

## HARD RULES (never violate)
0. **NEVER rely on mutable item internals (e.g. source comments) for correctness.** Item source text, comments, naming conventions inside source code, etc. are ARBITRARY user-controlled content. We can never assume specific comments are present (e.g. `{ name }` blocks), that they match the stored name, or that they exist at all. Any implementation that parses source text to extract metadata (names, types, etc.) is INVALID. Metadata must come from orlib20 warehouse records or APIs, never from item source content.
1. **Emulate TSDev.exe.** We are replacing TSDev.exe for automated TS library management. Our code should get as close as possible to imitating what the TSDev app does. If TSDev refreshes its cache after insert/modify/delete, we should do the same. If TSDev uses a specific API or sequence to enumerate items, we should use the same. Do not invent workarounds (e.g. parsing source comments) when TSDev has a proper mechanism — reverse engineer and replicate TSDev's behavior.
2. **Stay in the PM role.** Do not do work that can be delegated to a subagent. Do not read files/URLs that can be passed to a subagent. Only perform management tasks.
3. **computer-use MCP tools — STRICT CONSTRAINTS:**
   - **ONLY** allowed to operate TSDev.exe and our own project executables (tsdev_cli.exe, test_*.exe). NEVER operate any other application.
   - **ALWAYS** use computer-use MCP to check whether a process is genuinely still running, frozen, or has crashed. If crashed (unresponsive but alive with a crash dialog), use computer-use MCP tools to close the crash notification dialog (mouse or keyboard), which terminates the process. Only then continue work.
   - May also be used to autonomously operate TSDev.exe for RE tasks (create/open/verify/save library items, etc.) — making the team fully autonomous with no need to ask the user for UI operations.
   - computer-use MCP provides screenshots and window enumeration to detect crash/error popups and notifications.
   - **PREFER computer-use MCP tools over custom PowerShell scripts** for screen inspection, window management, and UI automation. Use computer-use MCP as the first choice; only fall back to PowerShell if computer-use MCP cannot do the task.
   - **For image inspection:** use the `image-inspector` subagent profile. It has access to all tools (including computer-use MCP). Instruct it to take a screenshot via computer-use MCP and then answer questions about the image content. This is how the PM (who cannot view images directly) gets information from screenshots. **VERIFIED WORKING** (2026-08-20): image-inspector subagent successfully took a screenshot via computer-use MCP `openai_computer` tool and accurately answered questions about visible windows, crash dialogs, and active window title.
4. **Crash safety:** orlib20 calls with wrong calling conventions or missing thread-check patches cause crashes. ALWAYS verify calling conventions via Ghidra before calling any orlib20 function. ALWAYS ensure all required INT3 patches are applied before calling. Use SEH protection. If a test crashes, use computer-use MCP to close the crash dialog, then diagnose the root cause before retrying.
   - **CRASH DIALOG HOST PROCESS:** TradeStation crash dialogs are hosted by `TSCrashReport.exe` (at `C:\Program Files (x86)\TradeStation 9.5\Program\TSCrashReport.exe`), NOT by Windows WerFault. The dialog title shows the crashed process name (e.g. "test_enumerate.exe") but the actual window is owned by TSCrashReport.
   - **RELIABLE CLOSE PROCEDURE (VERIFIED 2026-08-20):** Use computer-use MCP `run_script` (PowerShell) to:
     1. `Get-Process | Where-Object { $_.ProcessName -match 'TSCrashReport|test_enumerate|WerFault' }` to find crash-related processes
     2. `Stop-Process -Id <PID> -Force` for each (kill TSCrashReport.exe AND the crashed process if still alive)
     3. Verify with another `Get-Process` query
     - Do NOT rely on clicking "Don't Send" button — pixel coordinates are unreliable. Do NOT rely on Alt+F4 — it may trigger "Close all tabs" dialogs instead. The `run_script` + `Stop-Process` approach is the most reliable.
   - **SUBAGENT HANG WARNING:** image-inspector subagents can hang on MCP tool calls. If a subagent takes too long, cancel it and use `run_script` directly instead.
   - **AUTONOMOUS CRASH DETECTION PROCESS (MANDATORY):** Before AND after any operation that could crash (running tsdev_cli, test_*.exe, or any orlib20 call), run this check via computer-use MCP `run_script`:
     ```powershell
     Get-Process | Where-Object { $_.ProcessName -match 'TSCrashReport|WerFault|tsdev_cli|test_' } | Select-Object Id, ProcessName, MainWindowTitle | Format-Table -AutoSize
     ```
     If ANY TSCrashReport process is found, kill it AND the crashed process immediately:
     ```powershell
     Get-CimInstance Win32_Process | Where-Object { $_.Name -match 'tsdev_cli|test_|TSCrashReport' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
     ```
     Then verify clean with another Get-Process query. Only continue work after all crash processes are gone.
   - **CRASH CASCADE WARNING:** Regression test scripts (run_regression.ps1, test_*.ps1) will keep spawning tsdev_cli.exe in a loop even after a crash. If a crash happens during a regression run, you MUST kill the parent PowerShell process (run_regression.ps1) AND all child processes to stop the cascade. Use `Get-CimInstance Win32_Process` to find parent PIDs.
   - **MCP HANG RECOVERY:** When a computer-use MCP tool call hangs indefinitely (e.g. screenshot, openai_computer), kill the corresponding Node.js processes (usually multiple ones). When you then try to call the MCP tools again, they will automatically be restarted and reconnected. Use: `Get-Process | Where-Object { $_.ProcessName -match 'node' } | Stop-Process -Force` then retry the MCP call.
   - **FRIDA FOR LIVE TRACING:** Use the `frida` CLI directly (NOT via MCP) for live instrumentation and tracing of orlib20 functions. Frida can attach to running processes, hook function entry/exit, log arguments, and catch exceptions/crashes to identify exact crash addresses. This is often more effective than static Ghidra analysis for diagnosing runtime crashes. **VERIFIED**: Used Frida to trace FUN_101a42d0 call chain and discovered that FUN_101c2e90 returns the ProgId (not the verbatim name as the RE doc claimed). Key technique: use FindExistPos export as a trigger to install hooks after TsDevLib constructor completes (avoiding signature-matching interference).
5. **No hacks: never suppress compiler errors/warnings or use workarounds that mask real issues.** If code produces warnings or errors, fix the root cause or find a clean, production-worthy alternative. If no clean approach exists, use the most explicit/raw form (e.g. a numeric constant with a comment naming the intended symbol) rather than suppressing the warning. Example: 
tstatus.h redefines some STATUS_SXS_* macros from winnt.h — instead of #pragma warning(disable: 4005) around the include, use the raw hex constant  xC0000374 with a comment // STATUS_HEAP_CORRUPTION.

## TVS-F.5 CRITICAL FINDINGS (Frida trace 2026-08-20) — RESOLVED

### Root cause: FUN_101a42d0 non-server mode returns ProgId, NOT verbatim name
- **DAT_1023aa8f = 0** in our CLI (non-server mode, confirmed by Frida)
- In non-server mode, FUN_101a42d0 calls FUN_101c2e90 which returns the **ProgId**, NOT a name pointer
- **FUN_101c2e90 is a ProgId getter**, NOT a verbatim name getter (RE doc was wrong)
- FUN_101a42d0 returns the ProgId in non-server mode — **CANNOT be used to get the verbatim name**

### Solution: Call FUN_101d05e0 + FUN_101c36d0 directly (IMPLEMENTED & VERIFIED)
- FUN_101d05e0 (BST search, __thiscall): ECX=warehouse, stack=(typeFilter, name, *outNode), RET 0xC. Returns bool, stores **node pointer** in *outNode
- FUN_101c36d0 (name getter, __fastcall): ECX=node pointer, no stack args, RET. Returns char* to verbatim name at node+0x1D
- New INT3 patch needed: FUN_101c36d0 at offset 0x6A (RVA 0x1C373A) — NOP'd to 0x90
- Phase 2 flow: GetWarehouse → BuildTypeArray → BstSearchWrapper → VerbatimNameGetter → FindExistPos
- **All 8/8 C++ regression suites + 62/62 Python tests PASS** (historical — now 10/10 suites / 113 cases after TVS-I.1 + TVS-I.2)
- Adversarial review: APPROVED (3 MINOR findings, all non-blocking)

### Signatures added (25 total, 12 patch sites)
- `FUN_101d05e0_bst_search_wrapper` (call_target, thiscall, RVA 0x1D05E0)
- `FUN_101c36d0_verbatim_name_getter` (call_target, fastcall, RVA 0x1C36D0)
- `FUN_101c36d0_thread_check_INT3` (patch_site, offset 4, CC→90)
- Note: `FUN_101d05e0` signature requires non-wildcarded `E8 A1 12 00 00` for uniqueness (distinguishes from 3 sibling functions)

### Dead code (retained for reference, marked with comment)
- `callFindItemGetName`, `FindItemGetNameSehResult`, `m_pFindItemGetName`, `resolveFindItemGetName()`, `FUN_101a42d0_find_item_get_name` signature — all superseded by the new direct-call path

**Hard rules, subagent profiles, work orchestration, and acceptance criteria are in `AGENTS.md` — do not duplicate them here. This file contains ONLY PM-specific operational memory: crash-safety procedures, Frida tips, current task/state, and reminders.**

## Project state (as of 2026-08-22)
- **Doc-sync pass COMPLETE + VERIFIED (2026-08-22):** Full documentation audit found ~20 discrepancies (2 CRITICAL: README ShowMe "Unconfirmed", RE_F5 wrong name-getter claims without erratum; plus MAJOR stale test counts everywhere, false BUILD.md CI/E2E claims, PROJECT_HISTORY/serve_plan marking K.6a PENDING, missing J/CI history entries, re-introduced username leaks). ALL FIXED across 15 files; adversarial review APPROVED. Ground truth now: **12 suites / 135 cases** (Suite 11 serve E2E wired earlier; Suite 12 ShowMe E2E SM1-SM9 wired by this pass w/ `-SkipShowMeE2e`), pytest collects 130 (115 non-e2e). VERIFIED: full elevated regression rc=0 ("ALL SUITES PASSED", run_regression.ps1 exits 0 ONLY on line 808 after that banner); `-NoTs` 2/2 PASS with new structure; pytest 115 passed / 15 deselected.
- **Elevated-run lessons:** (1) `cmake` NOT on PATH — use full path `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` (documented in BUILD.md); build does NOT need elevation. (2) Child powershell Write-Host output does NOT flow through stdout pipes in PS 5.1 — capture elevated runs via exit code or Start-Transcript inside the child. (3) Mixing elevated/non-elevated runs POISONS pytest dirs (`.pytest_cache`, `$env:TEMP\pytest-of-user`) with admin ACLs → non-elevated pytest gets 88 setup PermissionErrors; fix = delete both dirs from an elevated process (non-elevated deletion is denied). Driver script kept at tmp\elevated_verify.ps1 (untracked).
- **Doc-sync hygiene reminder:** NEVER write personal usernames or absolute C:\Users\<name> paths into tracked files — Workstream H scrubbed history once already; leaks were re-introduced into PM_WORKING_MEMORY.md and had to be re-scrubbed. Use neutral placeholders (`<username>`, `$env:USERPROFILE`).
- **Workstreams A, B, C, D, E, F, G, I, J COMPLETE.** Workstream K (Serve mode) K.1–K.6b COMPLETE. Crash handling (VEH + TsDevCrashError) COMPLETE. ShowMe component type (TVS-C.8) COMPLETE. Primary acceptance criteria SATISFIED at library + CLI + Python wrapper level.
- **ShowMe support (TVS-C.8):** whType=1282 (0x0502), category 2, sub-type 0x05. Added to C++ CLI, Python wrapper, and tests. Fixed `enumerateRecords` category-2 stack overflow (typeFilterBuf[3] → [16]). Files: src/tsdev_lib.cpp, src/tsdev_lib.h, src/tsdev_cli.cpp, src/tsdev_automation/_cli.py, src/tsdev_automation/client.py, tests/test_showme_type.cpp, tests/test_showme_e2e.ps1.
- **CLI supports 5 component types:** Strategy (0), Indicator (2), Function (4), TradingApp (5122, category 2, sub-type 0x14), ShowMe (1282, category 2, sub-type 0x05). 3 unconfirmed: ActivityBar, PaintBar, ProbabilityMap.
- **WORKSTREAM I — Directory Batch Operations — COMPLETE (I.1–I.3).** TVS-I.1 (`batch-upsert` C++ CLI manifest+stdin), TVS-I.2 (directory batch all 4 ops, C++ CLI), and TVS-I.3 (Python wrapper `batch_upsert` + directory batch + `_cli.py` subcommands) are ALL COMPLETE. Directory batch operations (insert/modify/delete/upsert) now work via BOTH the C++ CLI AND the Python wrapper (`TsDevClient`).
- **WORKSTREAM (F) New Operations & Inline Batch — COMPLETE (F.1–F.6, including F.5).** TVS-F.1 (`exists`, C++ CLI), TVS-F.2 (inline batch stdin, C++ CLI), TVS-F.3 (`upsert`, C++ CLI), TVS-F.4 (`get-source`, C++ CLI, RE-based), TVS-F.5 (case-insensitive `exists` with stored name + ProgId return, C++ CLI + Python), and TVS-F.6 (Python wrapper for all F ops) are ALL COMPLETE. All 6 functions now work via BOTH the C++ CLI AND the Python wrapper (`TsDevClient`). Workstreams A, B, C, D, E, F ALL COMPLETE.
- **Tech debt cleanup pass (2026-08-18):** Documentation consolidated (AGENTS.md/PM_WORKING_MEMORY.md condensed, historical detail archived to `docs/PROJECT_HISTORY.md`, 4 completed handoffs archived to `docs/archive/`). Code fixes: TD-001 (auto-cleanup partial entry on compile failure), TD-002 (per-patch restoration gap). Stale comments cleaned. Stray files removed.
- **Regression suite:** `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (12 C++ regression suites / 135 cases: sigtool (12) + unit (26 exes) + E2E (34) + exists (5) + batch-stdin (8) + upsert (9) + get-source (7) + exists-ci (9) + batch-upsert (5) + directory-batch (9) + serve E2E (2) + ShowMe E2E (9); skip flags include `-SkipServeE2e`, `-SkipShowMeE2e`; under `-NoTs` only sigtool + 9 non-TS unit tests run). C++ unit tests: 26 (including test_elevation_guard, test_json_serialize, test_serve_dispatch, test_serve_framing, test_crash_handler, test_showme_type). Python: pytest collects 131 tests (116 non-e2e; 15 e2e deselected without live TS). Non-TS C++ tests: 9 (all pass without TradeStation). All passing.
- **Architecture:** `signatures/*.json` (authoritative) → `tools/sigcodegen.py` → `signatures.gen.h` (build-time codegen) → `src/orlib_resolver.cpp`. No runtime JSON dep in production CLI. `src/sigtool.cpp` = standalone signature verification tool.
- **Detailed TVS history:** See `docs/PROJECT_HISTORY.md`.
- **Workstream F (New Operations & Inline Batch):** COMPLETE (F.1–F.6, including F.5). TVS-F.1 (`exists` exact-match, C++ CLI), TVS-F.2 (inline batch via stdin, C++ CLI), TVS-F.3 (`upsert` subcommand, C++ CLI), TVS-F.4 (`get-source` subcommand, C++ CLI, RE-based), TVS-F.5 (case-insensitive `exists` with stored name + ProgId return, C++ CLI + Python), and TVS-F.6 (Python wrapper for all F ops) ALL COMPLETE. Approved plan at `docs/archive/WORKSTREAM_F_PLAN.md` (6 TVSs: F.1 [DONE], F.2 [DONE], F.3 [DONE], F.4 [DONE], F.5 [DONE], F.6 [DONE]). See AGENTS.md `### NEXT WORKSTREAMS` item 2.

## NEXT WORKSTREAM
**Workstream K — Serve Mode — FULLY COMPLETE (K.1–K.6b, 2026-08-21).** Plan at `docs/workstream_serve_plan.md`.

All planned workstreams (A–K) are now COMPLETE. The project's primary acceptance criteria are fully satisfied: all 6 functions (INSERT, DELETE, MODIFY, exists, get-source, upsert) work via both the C++ CLI (`tsdev_cli`) and the Python wrapper (`TsDevClient`). Serve mode is enabled by default (`use_serve=True`). No further workstreams are currently planned.

### K.6a — E2E integration + backwards-compat verification (COMPLETE — 2026-08-21)
All 7 phases passed. Adversarial review APPROVED (1 CRITICAL, 1 MAJOR, 4 MINOR — all fixed). See "K.6a COMPLETE" section at top of this file for full details (bugs fixed, Frida findings, test counts).

### K.6b — Documentation + `use_serve` default flip (COMPLETE — 2026-08-21)
`use_serve` default flipped to `True`; `_ServeBroker.start()` FileNotFoundError guard added; serve-mode docs added to BUILD.md, E2E_TEST_SPEC.md (Part F), README.md; `test_serve_e2e.py` updated to use default. E2E: 2/2 serve E2E passed (default-flip path), 12/12 C++ regression suites / 135 cases passed. Adversarial review APPROVED (17 INFO findings).

## Workstream I progress
- **TVS-I.1: batch-upsert (C++ CLI — manifest file + stdin modes)** — COMPLETE (2026-08-20). Added `batch-upsert` subcommand to `src/tsdev_cli.cpp` (6 surgical edits): (1) `doUpsert()` gained `bool flushPerItem = true` default param — `doInsert()` call now passes `flushPerItem` through (not hardcoded `true`); single-item `cmdUpsert`/`runUpsert` unchanged (call `doUpsert` with 5 args → default `true`, NO behavioral change). (2) `runBatch()` gained `else if (op == "upsert")` branch calling `doUpsert(..., /*flushPerItem=*/false)` (batch "flush once" pattern — single `closeWarehouse()` at end, no per-item flush). (3) `cmdBatch()` `needsSource` now `(op == "insert" || op == "modify" || op == "upsert")`. (4) `main()` dispatch recognizes `batch-upsert` → `op = "upsert"` (both `<manifest_file>` and `--batch-stdin` paths reuse `cmdBatch()`). (5) `printUsage()` + (6) header comment list `batch-upsert` + manifest format updated. Exit codes match existing batch pattern (0/1/2), no new exit codes, per-item upsert never emits exit 5 by construction. Tests: `tests/test_batch_upsert.cpp` (library-level invariant — multi-item upsert on shared TsDevLib, ProgId preservation, single closeWarehouse) + `tests/test_batch_upsert.ps1` (5 E2E cases BU1-BU5: manifest file mixed insert+modify paths w/ ProgId preservation, --batch-stdin, compile-error exit 2, empty manifest exit 1, no-args exit 1). Wiring: CMakeLists.txt (test_batch_upsert exe + test_regression dep), run_unit_tests.ps1 ($TestDefinitions), run_regression.ps1 (Suite 9, `-SkipBatchUpsert`). Verification: TDD red→green (RED 0/5 unknown subcommand → GREEN 5/5), full regression 9/9 suites PASS (99 cases: 12 sigtool + 19 unit + 34 E2E + 5 exists + 8 batch-stdin + 5 upsert + 6 get-source + 5 exists-ci + 5 batch-upsert), adversarial review APPROVED (1 DISAPPROVE→fix→APPROVE cycle: 3 MAJOR doc gaps + 2 MINOR stale-comment findings all fixed), E2E CLI verification PASSED (manifest file + stdin + compile-error; ProgId preservation confirmed: 5930→5930, 5931→5931). Docs updated: BUILD.md (19 exes, 9 suites, 99 cases), E2E_TEST_SPEC.md (D.6), PROJECT_HISTORY.md (DONE entry). **Next: TVS-I.2 (directory batch mode).**
- **TVS-I.2: Directory batch mode (C++ CLI — all 4 ops)** — COMPLETE (2026-08-20). Added 4 directory batch subcommands to `src/tsdev_cli.cpp`: `batch-insert-dir`, `batch-modify-dir`, `batch-delete-dir`, `batch-upsert-dir`. Each takes `<type> <dir_path> [-t <ext>...]` args. `parseDirectory()` enumerates regular files (no recursion) via `std::filesystem::directory_iterator`, filters by extension (default `.el`+`.txt`; first `-t` overrides defaults, subsequent add; dot-prefix optional, case-insensitive), uses file stem as item name and file content as EL source (empty for delete). `runBatchAndEmit()` helper extracted from `cmdBatch()` for DRY reuse — shared by manifest and directory batch codepaths. `cmdBatchDir()` handler dispatches to `runBatchAndEmit()`. `#ifndef TSDEV_CLI_TEST` guard on `main()` allows unit test to `#include` the `.cpp` directly. Exit codes: 0=all success, 1=usage/empty-dir/non-existent-dir/missing-`-t`-arg, 2=one-or-more-failed (incl. unknown type). Tests: `tests/test_directory_batch.cpp` (6 unit cases — extension filtering, override, stem extraction, empty dir, subdir skipping, source reading) + `tests/test_directory_batch.ps1` (9 E2E cases DB1-DB9: insert-dir/modify-dir/delete-dir/upsert-dir with ProgId preservation, `-t` override, empty dir, unknown type, non-existent dir, missing `-t` arg). Wiring: CMakeLists.txt (test_directory_batch exe + test_regression dep), run_unit_tests.ps1 ($TestDefinitions), run_regression.ps1 (Suite 10, `-SkipDirectoryBatch`, skipped under `-NoTs`). Verification: TDD red→green, full regression 10/10 suites PASS (113 cases: 12 sigtool + 20 unit + 34 E2E + 5 exists + 8 batch-stdin + 9 upsert + 6 get-source + 5 exists-ci + 5 batch-upsert + 9 directory-batch), adversarial review APPROVED (7 MINOR findings, 4 fixed, 0 CRITICAL/MAJOR). Docs updated: BUILD.md (20 exes, 10 suites, 113 cases), E2E_TEST_SPEC.md (Part E), AGENTS.md (TVS-I.2 row, counts), PROJECT_HISTORY.md (DONE entry). **Next: TVS-I.3 (Python wrapper — batch_upsert + directory batch).**
- **TVS-I.3: Python wrapper — batch_upsert + directory batch + _cli.py subcommands** — COMPLETE (2026-08-20). Added Python wrapper support for `batch_upsert` (manifest mode) and 4 directory batch methods, plus corresponding `_cli.py` subcommands. `src/tsdev_automation/client.py`: `batch_upsert(items)` method (manifest-mode via `--batch-stdin`, reuses `_batch()` with `_BATCH_SUBCMD["upsert"]`), `_batch_dir()` helper (shared logic for all 4 directory batch methods — builds CLI args `<sub> <type> <dir_path> [-t <ext>...]`, runs CLI, parses JSON array), 4 `batch_*_dir()` methods (`batch_insert_dir`/`batch_modify_dir`/`batch_delete_dir`/`batch_upsert_dir`, each takes `type, dir_path, extensions=None`), `_BATCH_SUBCMD["upsert"]` entry, `needs_source` fix. `src/tsdev_automation/_cli.py`: `batch-upsert` subcommand (manifest mode), 4 `batch-*-dir` subcommands (with `-t` flag via `action="append"`), dispatch branch, docstring updates. Tests: `tests/test_tsdev_client.py` — 22 new test functions (3 TestBatchUpsert + 8 TestBatchDir + 5 TestCliNewSubcommands + 5 TestE2E + `_TEST_NAMES` update). Verification: TDD red→green, 83/83 Python tests pass (68 non-e2e + 15 e2e against real CLI + TradeStation warehouse), adversarial review APPROVED (1 MINOR non-blocking: README not yet updated — fixed in docs update), E2E CLI verification PASSED (all 5 new methods verified against real CLI with ProgId preservation). **Workstream I COMPLETE.** No next TVS currently planned.

## Workstream H (Username Scrubbing for Public Release) — COMPLETE (2026-08-20)
Scrubbed the two personal usernames (leaked via USERPROFILE- and USERNAME-derived absolute paths) from repo before public release.
- **TVS-H.1 (working-tree scrub):** 20 tracked files edited — absolute paths `C:\Users\<user>\projects\tsdev_automation\...` replaced with relative paths (code) / `$env:USERPROFILE` (home-dir-relative docs) / `$PSScriptRoot` (script outline). Adversarial review APPROVED. Zero personal-username hits in tracked files.
- **TVS-H.2 (history analysis):** 57 commits → 9 condensed per-workstream commits. Plan adversarially reviewed (DISAPPROVED first pass — 4 critical findings: hash invalidation, missing --force, wrong verification, read-tree stale files; all fixed).
- **TVS-H.3 (history rewrite):** Committed H.1 → condensed 57→9 via orphan branch + `git read-tree --reset -u` → `git filter-repo --replace-text` (regex:`<username>`==>user) for text blobs → `--invert-paths` stripped binary artifacts (build/, build_probe/, ghidra_project/, *.obj/exe/lib/pdb/pyc/gbf/grf/ps/bin, __pycache__) → first adversarial review found CRITICAL uppercase variants of the username in .tlog files + proprietary TradeStation files in history → FIX: `--invert-paths --path tmp/` (stripped ALL of tmp/ including ~10,000 proprietary .xml/.ell files + 4 .dlls + .tlog files containing the username) + case-insensitive scrub `regex:(?i)<username>==>user` → force-pushed to origin. Final adversarial review APPROVED (all 10 checks pass: zero username hits in any text or binary blob across all 9 commits, H.1 relative paths preserved in HEAD, 129 files, key files intact, monotonic trees, .gitignore covers future contamination).
- **Build + regression verification:** 8/8 C++ suites (93 cases) + 62/62 Python tests PASS. No regressions. (Historical — now 10/10 suites / 113 cases after TVS-I.1 + TVS-I.2.)
- **Backup:** `$env:USERPROFILE\tsdev_automation_backup_pre_rewrite.bundle` (2.04 MB) — full pre-rewrite history for rollback.
- **Key lesson:** filter-repo `--replace-text` regex is CASE-SENSITIVE by default — always use `(?i)` for username scrubs. MSVC .tlog files uppercase paths. Binary blobs need `--invert-paths` stripping, not just `--replace-text`.
- **Key lesson (2026-08-21): `git filter-repo` strips the upstream tracking config.** filter-repo deliberately removes the `origin` remote after rewriting history (safety mechanism to prevent accidental pushes of rewritten history). Re-adding the remote (`git remote add origin <url>`) restores only the `[remote "origin"]` config section — it does NOT restore the `[branch "main"]` tracking section (`branch.main.remote` + `branch.main.merge`). This went unnoticed for 3 commits (TVS-I.1/I.2/I.3) because agents don't push unless explicitly asked. The user discovered it when a bare `git push` failed with "no upstream branch." **FIX after any `git filter-repo` run:** always use `git push -u origin main` (the `-u` flag restores tracking) — NOT just `git push --force origin main` (which works without tracking but doesn't set it). Verify with `git config --get branch.main.remote` (should print `origin`) and `git status -sb` (should show `## main...origin/main`).

## Workstream J (Admin Privilege Enforcement) — COMPLETE (2026-08-21)
Force all TS-accessing executables (tsdev_cli + 18 TS-dependent test executables) to run elevated via `requireAdministrator` manifest, gated by CMake option `TSDEV_REQUIRE_ADMIN` (default ON). UX: run parent (terminal/Python/test runner) elevated once → all child processes inherit token silently (no per-run UAC prompt). No way to fully avoid UAC on Windows while preserving stdout-JSON piping (scheduled tasks/services run in separate sessions, can't pipe stdout back — dealbreaker). `-DTSDEV_REQUIRE_ADMIN=OFF` for dev builds. CI safe with default ON (no -NoTs suite or non-e2e Python test spawns tsdev_cli.exe or TS-dependent test exes). 9 non-TS unit tests (test_no_hardcoded_rvas, test_tradingapp_type, test_showme_type, test_orlib_resolver, test_elevation_guard, test_crash_handler, test_json_serialize, test_serve_dispatch, test_serve_framing) + sigtool excluded from manifest (run non-elevated in CI). Changes: CMakeLists.txt (option + `TSDEV_ADMIN_TARGETS` list + foreach loop applying `/MANIFESTUAC:level='requireAdministrator'` to all TS-accessing targets), docs/BUILD.md + README.md + docs/BUNDLE_README.md (admin UX docs). Verified: mt.exe manifest extraction (8/8 sample exes: TS targets→requireAdministrator, non-TS+sigtool→asInvoker), non-elevated CreateProcess→ERROR_ELEVATION_REQUIRED, -NoTs regression 2/2 PASS. Adversarial review APPROVED (2 MINOR non-blocking findings: stale PM_WORKING_MEMORY ref fixed, test_directory_batch conservatively included though pure unit test — harmless).

## Workstream K (Serve Mode) — K.1–K.6b COMPLETE
Plan: `docs/workstream_serve_plan.md`. Goal: `--serve` mode for tsdev_cli.exe — elevated named-pipe broker, one UAC per session, no per-call prompts, native speed, no third-party dependency.

### Completed TVSs
- **TVS-K.1 (elevation guard):** `isElevated()` helper, exit code 11, "not_elevated" error string, guard in main() before all subcommands including serve. Test: test_elevation_guard.cpp. Review APPROVED.
- **TVS-K.2 (cliResultToJson):** Extracted JSON serialization from emitOutput() into `cliResultToJson(const CliResult&)`. emitOutput() and emitBatchOutput() refactored to use it. Byte-identical output preserved. Test: test_json_serialize.cpp. Review APPROVED.
- **TVS-K.3 (serve dispatch):** `ServeRequest` struct, `parseServeRequest()` (validates op/type/name/source/case_insensitive/output_path), `dispatchServeRequest()` (routes to doInsert/doModify/doDelete/doUpsert/doExists/doGetSource/quit). Test: test_serve_dispatch.cpp. Review APPROVED.
- **TVS-K.4 (named pipe server):** Framing helpers (writeAll/readAll/writeFramed/readFramed with 64MB cap), createServePipe (SDDL: D:P(A;;GA;;;BA)(A;;GWGR;;;SY)(A;;GWGR;;;OW)S:(ML;;NW;;;ME)), runServe (64MB-stack worker thread, one TsDevLib for whole session), cmdServe (--serve-pipe-file atomic handshake), `serve` subcommand in main(). Test: test_serve_framing.cpp. Review APPROVED.
- **TVS-K.5 (Python serve-broker):** _pipe.py (framed-JSON pipe I/O via ctypes), _ServeBroker class (ShellExecuteExW runas, lazy start, reconnect-once, shutdown with quit+terminate), _detect_elevation(), use_serve=False param on TsDevClient, routing logic (elevated→subprocess, non-elevated+use_serve→broker, batch→NotImplementedError when serve active). 115 Python tests pass. Review APPROVED (after fixing 3 MAJOR findings: 64MB cap in recv_response, orphaned broker on start failure, reconnect process leak).

### Completed TVSs (continued)
- **TVS-K.6a (E2E integration): COMPLETE (2026-08-21)** — All 7 phases passed, adversarial review APPROVED. See "K.6a COMPLETE" section at top of this file for full details.
- **TVS-K.6b (documentation + default flip): COMPLETE (2026-08-21)** — `use_serve` default flipped to `True`; `_ServeBroker.start()` FileNotFoundError guard added (critical bug fix — Windows popup on non-existent CLI path); serve-mode docs added to BUILD.md, E2E_TEST_SPEC.md (Part F), README.md; `test_serve_e2e.py` updated to use default (no explicit `use_serve=True`). E2E: 2/2 serve E2E passed (default-flip path), 12/12 C++ regression suites / 135 cases passed. Adversarial review APPROVED (17 INFO findings).

## Crash Handling (VEH + TsDevCrashError) — COMPLETE (not in original plan)
Added alongside Workstream K to make serve mode safe — a non-elevated Python client cannot kill an elevated TSCrashReport.exe, so crashes must be prevented from spawning the dialog in the first place.

### C++ VEH (Vectored Exception Handler)
- `vehCrashHandler()` in anonymous namespace catches EXCEPTION_ACCESS_VIOLATION (0xC0000005), EXCEPTION_STACK_OVERFLOW (0xC00000FD), STATUS_STACK_BUFFER_OVERRUN (0xC0000409), 0xC0000374 (STATUS_HEAP_CORRUPTION — raw hex, no ntstatus.h include due to winnt.h conflicts, no warning suppression per hard rule 5).
- Calls `TerminateProcess(GetCurrentProcess(), code)` — prevents TSCrashReport.exe from spawning.
- Installed as FIRST thing in main() via `AddVectoredExceptionHandler(1, ...)` + `SetErrorMode(SEM_NOGPFAULTERRORBOX)`.
- Test: test_crash_handler.cpp (normal mode + --crash mode), test_crash_handler.ps1. Review APPROVED.

### Python TsDevCrashError
- New exception subclass of TsDevError with `exit_code` attribute.
- `_is_crash_exit()` helper checks NT status codes (handles signed/unsigned returncodes).
- Crash detection in `_run_cli()`, `_run_batch()`, and `_ServeBroker.call()`.
- `_ServeBroker._get_exit_code()` via GetExitCodeProcess.
- No taskkill/TSCrashReport killing (VEH prevents dialogs).
- 115 Python tests pass. Review APPROVED.

### Key findings about the crash handling approach
- **VEH runs before orlib20's unhandled exception filter** — `AddVectoredExceptionHandler(1, ...)` with priority 1 means our handler is called FIRST, before orlib20's own unhandled exception filter or the Windows default crash dialog.
- **TerminateProcess prevents TSCrashReport.exe** — by calling `TerminateProcess(GetCurrentProcess(), code)` from the VEH, the process exits immediately without triggering the Windows Error Reporting dialog or TSCrashReport.exe. This is critical for serve mode where the broker runs elevated and a non-elevated Python client cannot kill an elevated TSCrashReport.exe.
- **Works for elevated broker** — the VEH is installed in the broker process itself, so it works regardless of elevation level. The Python client never needs to interact with crash dialogs.
- **Hard rule 5 applied:** `ntstatus.h` could not be included (conflicts with `winnt.h` redefining STATUS_SXS_* macros). Instead of suppressing the warning with `#pragma warning(disable: 4005)`, the raw hex constant `0xC0000374` was used with a comment `// STATUS_HEAP_CORRUPTION`. This follows hard rule 5: use the most explicit/raw form rather than suppressing warnings.

## ShowMe Component Type (TVS-C.8) — COMPLETE (kept per user decision)
- ShowMe support added: whType=1282 (0x0502), category 2, sub-type 0x05.
- C++ CLI, Python wrapper, and tests all support ShowMe.
- Fixed `enumerateRecords` category-2 stack overflow (typeFilterBuf[3] → [16]).
- Files: src/tsdev_lib.cpp, src/tsdev_lib.h, src/tsdev_cli.cpp, src/tsdev_automation/_cli.py, src/tsdev_automation/client.py, tests/test_showme_type.cpp, tests/test_showme_e2e.ps1, docs/TS_COMPONENT_TYPES.md, docs/FEASIBILITY.md.

## Completed Work Summary

All workstreams A, B, C, D, E, F, G, I, J complete. Workstream K (Serve mode) K.1–K.6b complete. Crash handling complete. ShowMe (TVS-C.8) complete. Detailed per-TVS logs archived in `docs/PROJECT_HISTORY.md`.

| Workstream | TVSs | Result |
|------------|------|--------|
| A (Compilation errors) | A.2, A.4, A.6 | Real compiler error text with source location (Line/Col) |
| B (Robustness) | B.1–B.7, B.6.5, B.6.6 | OrlibResolver (signature-based RVA resolution), version detection, regression suite (113 test cases across 10 sub-suites), path discovery |
| C (TradingApp + ShowMe) | C.1–C.7, C.8 | TradingApp whType 5122 + ShowMe whType 1282, INSERT/DELETE/MODIFY via CLI |
| D (Python integration) | D.0–D.5 | TsDevClient wrapper (single-item + batch + inline stdin), --json output, batch mode, Python E2E + packaging |
| E (Architecture) | E.1–E.7 | JSON signature source of truth + build-time codegen, sigtool CLI, probe archival, test relocation, RVA dedup |
| F (New Operations & Inline Batch) | F.1–F.6 | exists, get-source, upsert, inline batch stdin, case-insensitive exists, Python wrapper for F ops |
| G (Distributable bundle) | G.1, G.2, G.3, G.5 | dist/ bundle (exe + Python package + README) via CMake POST_BUILD + timestamped zip in artifacts/ |
| I (Directory Batch Operations) | I.1–I.3 | batch-upsert (manifest+stdin), directory batch all 4 ops, Python wrapper for batch_upsert + directory batch |
| J (Admin Privilege Enforcement) | J.1 | requireAdministrator manifest on all TS-accessing executables |
| K (Serve Mode) | K.1–K.6b COMPLETE | Elevation guard, cliResultToJson, serve dispatch, named pipe server, Python serve-broker, E2E integration verified, documentation + default flip (use_serve=True). |
| Crash Handling | VEH + TsDevCrashError | C++ VEH crash handler (prevents TSCrashReport.exe), Python TsDevCrashError exception |

## Workstream F progress
- **TVS-F.6: Python wrapper for all new operations** — COMPLETE. Added Python wrapper support for the 4 new C++ CLI ops so all 6 functions work via BOTH the C++ CLI AND `TsDevClient` (satisfies the primary acceptance criteria). Changes: `src/tsdev_automation/results.py` — added `TsDevResult.exists: bool` + `TsDevResult.source: str` fields; `from_json()` parses `exists` (default False) + `source` (default ""); `prog_id` absent/None now coerces to `0` (int) instead of None (needed for exists not-found; verified no existing test asserts `prog_id is None` on a from_json-constructed result). `src/tsdev_automation/exceptions.py` — added `OutputWriteError(TsDevError)` (exit 3, error.code "output_write_error", mirrors SourceFileError style) + `ERROR_CODE_MAP["output_write_error"]` entry. `src/tsdev_automation/client.py` — added `exists(type, name)` (calls `_single("exists", type, name, None)`; does NOT raise on not-found since C++ returns success=True/exists=False; raises ValueError on bad type via `_validate_type`), `get_source(type, name, output_file=None)` (inline impl, NOT `_single` — builds `--json get-source <t> <name>` + optional `--output <file>`; raises NotFoundError on exit 5 [covers password-protected, indistinguishable] / OutputWriteError on exit 3; always uses --json, reads `result.source`), `upsert(type, name, source)` (calls `_single("upsert", type, name, source)`; reuses source materialization; raises CompileError/SourceFileError/ValueError, never NotFoundError/AlreadyExistsError); added "exists"/"get-source"/"upsert" to `_SINGLE_SUBCMD`; **inline batch via stdin**: `_batch()` now builds the manifest as an in-memory string via new `_build_manifest_string()` helper (replaces removed `_write_manifest()`; byte-identical line format + string-source temp `.el` materialization preserved) and pipes it via `input=` to `_run_batch()` with cmd `[sub, "--batch-stdin"]` (NO temp manifest file); `_run_batch()` gained `input: str` param passed to `subprocess.run`. Empty items → `input=""` → C++ exit 1 → `TsDevError` (contract PRESERVED — no empty-items guard added). `src/tsdev_automation/__init__.py` — exported `OutputWriteError`. `src/tsdev_automation/_cli.py` — added `exists`/`get-source`/`upsert` subparsers + dispatch (exists: prints `[OK] exists ... -> ProgId=N` / `-> not found`, returns 0 always [query]; get-source: prints raw `r.source` to stdout when no --output, else `Source written to <file>`; upsert: reuses `_print_result`). NO `case_insensitive` param (deferred to F.5 — C++ CLI has no `--case-insensitive` flag yet); NO `verbatim_name` field (deferred to F.5); NO `PasswordProtectedError` (C++ maps password-protected to exit 5 "not_found", indistinguishable — NotFoundError covers it, message carries detail). Tests: `tests/test_tsdev_client.py` — added `TestExists` (4), `TestGetSource` (5), `TestUpsert` (5), `TestBatchStdin` (5), `TestCliNewSubcommands` (5) classes; extended `test_unknown_type_raises_valueerror` to cover exists/upsert/get_source (6 ValueError blocks); updated `test_batch_string_sources_cleaned` to read manifest from `input` kwarg instead of manifest file path. Total Python suite: 53 passed, 0 failed. Verification: TDD red-green per feature (5 features × write-test/RED/implement/GREEN), adversarial review APPROVED (3 minor findings: #1 newline pinning was INVALID — `subprocess.run`/`Popen` rejects `newline=` kwarg on Py3.14.7, broke real-CLI E2E test, REVERTED; original CRLF concern mitigated by C++ `\r`-stripping; #2 extended unknown-type test coverage — applied; #3 prog_id annotation cosmetic — left as-is), C++ regression (non-TS: sigtool 12/12 + 3 unit tests) PASS, **E2E CLI 14/14 PASS** with REAL CLI + REAL TradeStation (upsert insert+modify paths ProgId 4946 preserved; exists found+non-raising-not-found; get_source round-trip reflects modified source + NotFoundError on missing; inline batch stdin 2 functions compiled+inserted; full cleanup no library pollution). NOTE: adversarial reviewer incorrectly claimed `subprocess.run` accepts `newline=` since Py3.7 — it does NOT (`Popen.__init__` has no `newline` param); the real-CLI E2E test caught this. LESSON: always run real-subprocess E2E, not just mocked unit tests, before accepting a subprocess-related change.
- **TVS-F.4: `get-source` subcommand (C++ CLI, RE-based)** — COMPLETE. Added `get-source` op to `src/tsdev_cli.cpp` (`doGetSource()`/`cmdGetSource()`/`runGetSource()`/`GetSourceParams`, dispatch branch, `CliResult.sourceText` field, `emitOutput()` emits `source` field in JSON mode / raw EL to stdout in human mode / "Source written to <file>" in --output mode, `writeFile()` helper, `--output <file>` flag parsing in main() dispatch [both space and equals forms], `errorCodeString()` exit 3 → `output_write_error` for get-source op, usage docs updated). Added `TsDevLib::getSource(type, name)` to `src/tsdev_lib.h`/`.cpp` (`LoadTextFileExFn` typedef, `m_loadTextFileEx` member, `callLoadTextFileEx` SEH helper [POD-only, mirrors `callFindExistPos`], export resolved in constructor, patch applied+restored). Calls `findExistPos` → `_LoadTextFileEX@16(nullptr, getCategory(type), progId, nullptr)` → `GlobalLock` → copy → `GlobalUnlock` → `GlobalFree`. Returns empty string on any failure (not found, SEH, NULL return, GlobalLock failure). Exit codes: 0=success, 5=not_found (also covers password-protected), 2=unknown_type/empty_name, 1=usage_error, 3=output_write_error. Works on 64MB-stack worker thread. RE: `_LoadTextFileEX@16` param order confirmed via Ghidra (thunk 0x101081A0 → FUN_101a0f90, authoritative mirror caller FUN_10108630; param order: unused/whType/progId/password; Frida NOT needed — doubly verified by adversarial review). New signature `FUN_101d0e80_thread_check_INT3` (13th signature, offset 57, CC→90, 74-byte pattern, unique match) + new export `_LoadTextFileEX@16` (8th export, RVA 0x1081A0) added to `signatures/v9.50.01.3344.json`; `EXPECTED_SIGNATURE_COUNT` 12→13, `EXPECTED_EXPORT_COUNT` 7→8 in `tools/sigcodegen.py`. Stale count fixes: `tests/test_sigtool.ps1` (12→13, 12/12→13/13 patterns), `tests/test_orlib_resolver.cpp` (hardcoded 12u→kSignatureCount), `src/orlib_resolver.h`/`.cpp` (comments 12→13 sigs, 7→8 exports, 4→5 patch sites). Tests: `tests/test_get_source.cpp` (unit: G1 strategy happy path, G2 nonexistent→empty, G3 function happy path, cleanup), `tests/test_get_source.ps1` (8-case E2E: GS1 stdout, GS2 JSON w/ source field, GS3 --output file, GS4 nonexistent exit 5, GS5 no-args exit 1, GS6 unknown-type exit 2), wired into `CMakeLists.txt`, `tests/run_unit_tests.ps1`, `tests/run_regression.ps1` (7th suite with `-SkipGetSource` flag, skipped under `-NoTs`). Verification: TDD red→green (RED: C2039 getSource not a member; C2712 SEH+std::string → fixed via callLoadTextFileEx helper; GREEN: all pass), adversarial review APPROVED (5 non-blocking findings: stale comments [fixed], stale docs [PM-owned], test coverage gap for new patch in test_orlib_resolver Cycle 5 [indirectly covered], --output= form not in E2E [minor], --output "" edge case [minor]), full regression 93/93 (12 sigtool + 17 unit + 34 E2E + 5 exists + 8 batch-stdin + 9 upsert + 8 get-source), E2E CLI 12/12 (insert ProgId 4756 → get-source stdout → get-source --json w/ source field → get-source --output file [both space+equals forms] → nonexistent exit 5 → no-args exit 1 → unknown-type exit 2 → delete → exists false). RE findings: `docs/RE/RE_F4_loadtextfileex_findings.md`, `docs/RE/RE_F4_cmd11_patch_findings.md`.
- **TVS-F.1: `exists` subcommand (exact-match, C++ CLI)** — COMPLETE. Added `exists` op to `src/tsdev_cli.cpp` (`doExists()`/`cmdExists()`/`runExists()`/`ExistsParams`, dispatch branch, `CliResult.exists` field, `emitOutput()` emits `exists` boolean + `prog_id` only when >0, usage docs updated). Calls `TsDevLib::findExistPos(name, type)`: >0 ProgId = found, -1 = not found (raw orlib20 result), 0 = SEH exception; `<= 0` = not found. Exit 0 for both found AND not-found (not-found is a valid query result, not an error); exit 2 = unknown type / empty name; NEVER emits exit 5 / `not_found`. Works on 64MB-stack worker thread (same pattern as insert/modify/delete). Tests: `tests/test_exists.cpp` (unit, guards `findExistPos`), `tests/test_exists.ps1` (5-case E2E: D1.1a/b, D1.2, D1.3a/b), wired into `CMakeLists.txt`, `tests/run_unit_tests.ps1`, `tests/run_regression.ps1` (4th suite with `-SkipExists` flag, skipped under `-NoTs`). Harness captures stdout/stderr SEPARATELY (matching `Invoke-CliRaw` pattern) because the library emits an intentional stderr diagnostic that must not be merged into the JSON stream. Verification: TDD red-green, adversarial review APPROVED, full regression 66/66 (12 sigtool + 15 unit incl. test_exists + 34 E2E + 5 exists), E2E CLI 24/24 across all 4 types + 4 edge cases. Non-blocking follow-up: empty-name validation reuses exit code 2 (maps to `error.code: "unknown_type"` in JSON) while `error.message` correctly says "component name must not be empty" — cosmetic mismatch in the code field only; exit code and message are correct.
- **TVS-F.3: `upsert` subcommand (C++ CLI)** — COMPLETE. Added `upsert` op to `src/tsdev_cli.cpp` (`doUpsert()`/`cmdUpsert()`/`runUpsert()`/`UpsertParams`, dispatch branch, usage docs updated). `doUpsert()` calls `findExistPos(name, type)` FIRST → if ProgId >0, delegates to `doModify()` (preserves ProgId); if ProgId <=0, delegates to `doInsert(flushPerItem=true)` (assigns new ProgId). Overrides `r.op = "upsert"` after delegation (assignment outside if/else → runs unconditionally on ALL paths). NEVER emits exit 5 / `not_found` (branches on existence by construction — not-found is impossible). Exit codes: 0=success, 8=compile_error, 2=unknown_type/empty_name, 1=usage_error, 3=source_file_read_error. `errorCodeString()` needed NO change (upsert never hits exit 5). Works on 64MB-stack worker thread (same pattern as insert/modify/exists). Source file read on main thread (matching cmdInsert/cmdModify). Tests: `tests/test_upsert.cpp` (unit, verifies KEY invariant: ProgId preserved after modify — findExistPos→createEntry→saveSource→verifyModule→findExistPos→modifyEntry→findExistPos same ProgId→deleteEntry), `tests/test_upsert.ps1` (9-case E2E: U1 insert-path, U1.verify exists, U2.prep insert, U2 modify-path with ProgId preservation assertion, U2.verify exists, U3 unknown-type exit 2, U4 compile-error exit 8, U5 function-type insert, U5.verify exists), wired into `CMakeLists.txt`, `tests/run_unit_tests.ps1`, `tests/run_regression.ps1` (6th suite with `-SkipUpsert` flag, skipped under `-NoTs`). Verification: TDD red→green (RED: "unknown subcommand upsert" exit 1; GREEN: all pass), adversarial review APPROVED (zero blocking findings, 2 minor cosmetic stale-RED-comment fixes applied), full regression 84/84 (12 sigtool + 16 unit incl. test_upsert + 34 E2E + 5 exists + 8 batch-stdin + 9 upsert), E2E CLI 11/11 (insert/delete/modify/exists regression check + upsert insert-path ProgId 4688 + upsert modify-path ProgId 4688 preserved + function-type ProgId 709 + all cleanup verified).
- **TVS-F.2: Inline batch via stdin (`--batch-stdin` flag, C++ CLI)** — COMPLETE. Satisfies the primary acceptance criterion: "C++ CLI must support batch operations both via a manifest file path AND via stdin pipe (no manifest file)." Changes in `src/tsdev_cli.cpp`: (1) extracted `parseManifestLines(const std::string& content, bool needsSource, vector<BatchItem>& outItems)` from `parseManifest()` (loop body byte-identical, only stream var `f`→`ss`); `parseManifest()` is now a thin file-reading wrapper that delegates to `parseManifestLines`. (2) `cmdBatch()` gained a `bool useStdin` param; stdin branch sets `_setmode(_fileno(stdin), _O_BINARY)` then reads via `std::fread` loop (C-stdio, NOT `std::cin` — `<iostream>` not included) and calls `parseManifestLines`; empty stdin → stderr `"manifest from stdin is empty"` + exit 1 (no JSON array). File branch unchanged (byte-identical error messages/exit codes). (3) `main()` batch dispatch detects `reducedArgvPtr[2]=="--batch-stdin"` → `cmdBatch(op,"",true,...)`; else file path. `--batch-stdin` is NOT in the global flag pre-scan (batch-specific, stays in reducedArgv). (4) `printUsage()` updated. Includes added: `<io.h>`, `<fcntl.h>`. Tests: `tests/test_batch_stdin.ps1` (8 E2E cases S1-S8: insert/delete/modify mixed via stdin, comments/blanks, empty stdin, backward-compat file-path, no-args rejected, stdin≡file equivalence). `Invoke-CliStdin` helper uses `Start-Process -RedirectStandardInput/Output/Error -Wait -NoNewWindow -PassThru` (encoding-safe binary stdin; NOT PowerShell `|` pipe which mangles encoding). Wired into `tests/run_regression.ps1` as Suite 5 with `-SkipBatchStdin` flag, skipped under `-NoTs`. Verification: TDD red→green (RED: `--batch-stdin` treated as filename → "failed to open manifest file"; GREEN: empty stdin → "manifest from stdin is empty"), adversarial review APPROVED (verified via `git diff` — loop body byte-identical, no scope creep, no hacks), full regression 74/74 (12 sigtool + 15 unit + 34 E2E + 5 exists + 8 batch-stdin), `-NoTs` correctly skips suites 3/4/5, E2E CLI PASS (insert/modify/delete via stdin against live TS, ProgId preserved across modify, backward-compat file-path + original single-item insert/exists/delete all operational). Two test-harness bugs found+fixed during GREEN: (a) `std::cin` used without `<iostream>` → switched to `std::fread` on C `stdin`; (b) `Invoke-CliStdin` async `BeginOutputReadLine` file-lock race → `Start-Process` redirection; (c) `-NoNewWindow` + `-WindowStyle Hidden` mutually exclusive → dropped `-WindowStyle Hidden`. Non-blocking: `docs/E2E_TEST_SPEC.md` Part D still a placeholder for `--batch-stdin` (test lives in `tests/test_batch_stdin.ps1`).

## Workstream G progress
- **TVS-G.1: CMake POST_BUILD bundle assembly + .gitignore** — COMPLETE. CMake POST_BUILD copies `tsdev_cli.exe` + 6 `.py` files (`__init__.py`, `client.py`, `exceptions.py`, `results.py`, `__main__.py`, `py.typed`) to `dist/` on every build via `copy_if_different`. `dist/.gitignore` removed; root `.gitignore` updated with `/dist/` and `/artifacts/` entries. Adversarial review APPROVED.
- **TVS-G.2: Python wrapper bundle-layout auto-discovery** — COMPLETE. New step 3 in `_resolve_cli_path()` (`src/tsdev_automation/client.py`): checks `<package_parent>/tsdev_cli.exe` before the CMakeLists.txt walk-up, so the wrapper auto-discovers the CLI when imported from the `dist/` bundle layout. 2 unit tests added (62 Python tests total). Adversarial review APPROVED.
- **TVS-G.3: Bundle README** — COMPLETE. `docs/BUNDLE_README.md` → `dist/README.md` via CMake POST_BUILD `copy_if_different`. Root `README.md` "Distribution / Bundle" section added. `docs/BUILD.md` "### 5. Bundle output" subsection added. Adversarial review APPROVED.
- **TVS-G.5: Automatic timestamped zip in artifacts/** — COMPLETE. `tools/create_bundle_zip.ps1` with `$ErrorActionPreference='Stop'` (fail-loudly). CMake POST_BUILD invokes it. Zip pattern: `tsdev_automation_<yymmdd><HHMM>.zip`. Selective contents: exe + `tsdev_automation/` + `README.md` only. Adversarial review APPROVED.
- **TVS-G.4 (optional): Remove stale capstone runtime dep** — COMPLETE. Removed `capstone>=5.0.9` from `pyproject.toml` `dependencies` (now `dependencies = []`). Fixed false capstone claim in `README.md` (now "stdlib-only"). Wrapper + sigcodegen.py verified stdlib-only (zero capstone imports). Adversarial review APPROVED.
- **Full E2E:** 7-step E2E passed (8/8 C++ regression suites + 62 Python tests). All adversarial reviews APPROVED.

## OBSERVATIONS / potential future TVS items (not blocking)
- **Shell privilege note (2026-08-22):** Default agent shell is NON-ELEVATED — tsdev_cli.exe and TS-dependent test exes carry requireAdministrator manifests (Workstream J) and cannot run from it. Safe here: pytest -m "not e2e", run_regression.ps1 -NoTs, builds, static checks. Full TS-side verification requires an elevated process (user may approve one UAC prompt on request).
- **RE_F5_bst_node_structure.md:104 label:** says FUN_101c36d0 is `__thiscall`; authoritative sources (signatures JSON, SIGNATURE_CATALOG, erratum banner) say `__fastcall` (binary-equivalent for single-ECX-arg). Cosmetic label fix.
- **SEH protection on error array reads:** `readErrorsFromArray` reads raw memory from orlib20.dll's data section without SEH guards. Low likelihood (array populated by compiler in normal flow). Defer.
- **3 unconfirmed warehouse types:** ActivityBar, PaintBar, ProbabilityMap. Require Ghidra/Frida RE work per type. Feature gap, not debt. (ShowMe was previously in this list but is now CONFIRMED — whType=1282, TVS-C.8.)
- **Multi-version signature selection untested:** Only one orlib20.dll version exists. Design is conservative (unknown → warning). Inherent limitation.
- **Empty-name `error.code` cosmetic mismatch (non-blocking, from TVS-F.1):** Empty-name validation reuses exit code 2, which maps to `error.code: "unknown_type"` in JSON output, while the `error.message` correctly says "component name must not be empty". Cosmetic mismatch in the code field only; exit code and message are correct. Consistent with existing subcommands' convention.

## CRITICAL REMINDER — Atomic subtask decomposition (VIOLATED ONCE, NEVER AGAIN)
- **One subagent → one narrowly scoped subtask.** NO multi-stage, multi-concern, or complex tasks for any individual subagent.
- TDD red-green is NOT one task. It is: (1) write failing test [edit], (2) build+run to confirm RED [verify], (3) implement fix [edit], (4) build+run to confirm GREEN [verify], (5) run regression [verify]. Each is a SEPARATE subagent.
- Build+run of ONE test = one task (acceptable granularity). Running 3 regression tests = one task. Editing one file = one task. Editing 2 files for one coherent logical change = one task (acceptable). But NEVER combine edit + build + run + regression in one subagent.
- If a subagent task description has "Step 1... Step 2... Step 3..." it is TOO BIG. Split it.

## COMPLETED TASK (2026-08-20): Documentation hygiene & consolidation — DONE, APPROVED
- **Goal was:** Clean up all docs — drop deprecated/outdated/untrue info, correct false claims, deduplicate, consolidate, optimize for progressive disclosure (AGENTS.md = root with always-on rules; structured reference chains with "when to read" guidance). Get docs consistent with codebase as source of truth. Create optimal repo state for future agent teams.
- **Result:** COMPLETE. Adversarial re-review APPROVED (all 20 findings from initial DISAPPROVE were fixed and verified).
- **What was done:**
  - **Archived 18 files:** 7 completed workstream plans/designs → docs/archive/ (HANDOFF_D, WORKSTREAM_F_PLAN, WORKSTREAM_F5_PLAN + 2 reviews, ARCHITECTURE_REDESIGN, ORLIB_RESOLVER_DESIGN); 11 RE process artifacts → docs/RE/archive/. Archive READMEs updated.
  - **Corrected 10 files:** AGENTS.md (removed false `-i` claim, fixed test counts 29/53→60, "A,B,C,E"→"A,B,C,D,E,F", restructured TODO→Project Status + Candidate Future Work, added Documentation Index section), PM_WORKING_MEMORY.md (deleted stale F.5 "BROKEN" note, deduplicated AGENTS.md rules, fixed workstream list + test count), BUILD.md (18 test executables, 9 CLI subcommands, 8 sub-suites/93 cases), E2E_TEST_SPEC.md (filled Part D, fixed date/count, removed `-i`), FEASIBILITY.md (_LoadTextFileEX@16 CONFIRMED, TradingApp out of unresolved), SIGNATURE_CATALOG.md (12→25 sigs, 7→10 exports, 13 new entries), TS_COMPONENT_TYPES.md (resolved open questions), INTEGRATION_APPROACH_EVALUATION.md (status→DECISION MADE), PROJECT_HISTORY.md (F.5 entry fixed to warehouse-based, `-i` removed, paths updated), src/tsdev_lib.h (Phase 2 comment FUN_101a42d0→FUN_101d05e0+FUN_101c36d0).
  - **Fixed 34 stale cross-references** across 12 files (docs + code + signatures JSON) to point to new archive locations.
- **Final doc structure (progressive disclosure):** AGENTS.md (root, always-on rules + doc index) → docs/{BUILD, E2E_TEST_SPEC, SIGNATURE_CATALOG, VERSION_DETECTION, TS_COMPONENT_TYPES, FEASIBILITY, INTEGRATION_APPROACH_EVALUATION, PROJECT_HISTORY, PM_WORKING_MEMORY}.md (operational reference) → docs/RE/ (10 durable RE findings) → docs/archive/ + docs/RE/archive/ (historical).

## CI (GitHub Actions) — COMPLETE (2026-08-21)
- **Workflow:** `.github/workflows/ci.yml` — single workflow, 2 jobs.
- **Triggers:** push (all branches + `v*` tags), pull_request (main only).
- **Build job** (windows-latest): checkout@v7 → setup-uv@v10.0.0 → `uv sync --frozen` → `cmake -S . -B build -A Win32` → `cmake --build build --config Release` → `run_regression.ps1 -NoTs` → `uv run pytest -m "not e2e"` → upload-artifact@v7 (`artifacts/*.zip`, `if-no-files-found: error`).
- **Release job** (ubuntu-latest, `needs: build`, `if: startsWith(github.ref, 'refs/tags/')`, `permissions: contents: write`): download-artifact@v8 → softprops/action-gh-release@v3 (`files: artifacts/*.zip`, `tag_name: ${{ github.ref_name }}`, `generate_release_notes: true`).
- **Manual release trigger:** push a tag (`git tag v0.1.0 && git push origin v0.1.0`). No workflow_dispatch.
- **Action versions:** checkout@v7, upload-artifact@v7, download-artifact@v8, setup-uv@v10.0.0 (pinned full semver — bare major tags don't exist for this action), action-gh-release@v3.
- **Side effect:** `uv.lock` updated to remove stale `capstone` entry (leftover from TVS-G.4 which removed capstone from pyproject.toml but never re-locked). Necessary for `uv sync --frozen` to succeed in CI.
- **Docs:** `docs/BUILD.md` — new `## CI (GitHub Actions)` section added at end.
- **Adversarial review:** APPROVED, zero findings.
- **Verification:** All CI-equivalent commands verified locally (build ~46s, regression -NoTs 2/2 suites PASS, pytest 68/68 non-e2e PASS, zip artifact created).
- **CI VERIFIED GREEN (2026-08-21):** Pushed commit `8c50793` to origin/main. GitHub Actions run `32449304024` PASSED in 1m37s — build job all 11 steps green, `tsdev-automation-bundle` artifact uploaded. Release job correctly skipped (branch push, not tag). Previous run `32448316226` (unauthorized subagent commit `e1d4fbe` with invalid `@v8`) failed in 6s — superseded by force-push of amended commit. Release job NOT yet verified end-to-end (requires tag push — user said no tags this session).
- **LESSON:** `astral-sh/setup-uv` does NOT publish bare major-version tags (`v8`, `v9`, `v10`). Only full semver tags exist (`v8.0.0`, `v10.0.0`, etc.). Always verify action tags exist via GitHub API before using them. GitHub first-party actions (`actions/*`) DO publish moving major tags; third-party actions may not.

## Reminders to self
- Re-read this file every turn / after context compaction.
- Do NOT accept sub-par quality from subagents. Production-grade only.
- Adversarial review is mandatory for every substantive result.
- Update AGENTS.md TODO section after each TVS completes.
- Use `subagent_general` for non-trivial work (NOT `subagent_explore`).
- **ATOMIC SUBTASKS ONLY.** One subagent = one narrowly focused task. No multi-stage/complex tasks.
- **Git commits on PowerShell — NO HEREDOC:** PowerShell does NOT support `$(cat <<'EOF' ... EOF)`. Write commit message to `tmp\commit_msg.txt` then `git commit -F tmp\commit_msg.txt`. Also: PowerShell uses `;` not `&&`; use `&` for executables with spaces in path. This is now a Standing Note in AGENTS.md so all agents are aware.

## CRITICAL LESSON � UAC prompt storm (2026-08-21)
**NEVER run test suites (run_unit_tests.ps1, run_regression.ps1 WITHOUT -NoTs) from a non-elevated process.** Workstream J added `requireAdministrator` manifests to all TS-accessing test executables. Spawning them from a non-elevated parent triggers a UAC prompt PER EXECUTABLE (dozens of prompts). The user had to confirm dozens of prompts manually � unacceptable.

**Correct approaches for running TS-dependent tests:**
1. **Non-TS tests only (NO UAC):** Run only the `asInvoker` executables individually (test_no_hardcoded_rvas, test_tradingapp_type, test_showme_type, test_orlib_resolver, sigtool). Use `run_regression.ps1 -NoTs`.
2. **TS-dependent tests (ONE UAC prompt):** Spawn ONE elevated PowerShell via `Start-Process powershell -Verb RunAs`. Child processes inherit the token silently.
3. **Best option:** Ask the user to open an elevated terminal themselves and run the test suite there. Zero UAC prompts.
4. **NEVER** run `run_unit_tests.ps1` without `--no-ts` from a non-elevated process. **NEVER** run `run_regression.ps1` without `-NoTs` from a non-elevated process.
