# Workstream F — TVS Decomposition Plan

**Status:** Planning document (read-only investigation; no code modified)
**Date:** 2026-08-18
**Author:** Planning analyst subagent
**Purpose:** Atomic TVS decomposition for Workstream F (New Operations & Inline Batch), ready for PM execution.

---

## 1. Project State Summary

### 1.1 C++ CLI (`tsdev_cli`) — Current Architecture

**Source files:**
- `src/tsdev_cli.cpp` (1259 lines) — the production CLI entry point
- `src/tsdev_lib.h` (267 lines) / `src/tsdev_lib.cpp` (1120 lines) — shared orlib20 wrapper library
- `src/orlib_resolver.h` (982 lines) / `src/orlib_resolver.cpp` (1486 lines) — signature-based RVA resolution

**Subcommand structure:**
The CLI dispatches on the subcommand string in `main()` (line 1074). Current subcommands:
- `insert` (line 1172) → `cmdInsert()` (line 481) → `runInsert()` worker thread (line 455) → `doInsert()` (line 264)
- `modify` (line 1189) → `cmdModify()` (line 594) → `runModify()` worker thread (line 570) → `doModify()` (line 358)
- `delete` (line 1206) → `cmdDelete()` (line 706) → `runDelete()` worker thread (line 682) → `doDelete()` (line 402)
- `batch-insert`/`batch-modify`/`batch-delete` (line 1227) → `cmdBatch()` (line 1028) → `runBatch()` worker thread (line 922)

**Key reusable functions for Workstream F:**
- `doInsert()` (line 264) — core insert logic on an already-constructed `TsDevLib`; takes `flushPerItem` flag. Calls `findExistPos` → `createEntry` → `saveSource` → `verifyModule` → `closeWarehouse` → `ellFileExists`.
- `doModify()` (line 358) — core modify logic; calls `lib.modifyEntry()` which internally does `findExistPos` → `saveSource` → `verifyModule`.
- `doDelete()` (line 402) — core delete logic; calls `findExistPos` → `deleteEntry`.
- `parseManifest()` (line 800) — parses a manifest FILE into `vector<BatchItem>`. Takes a file path, `needsSource` flag. Returns false if file can't be opened.
- `emitOutput()` (line 205) — renders a `CliResult` in human-readable OR JSON mode.
- `emitBatchOutput()` (line 991) — renders a `vector<CliResult>` as a JSON array.
- `parseType()` (line 88) — maps type string → `ComponentType` enum.
- `readFile()` (line 97) — reads a file into a string (binary mode).

**`--json` output mode:**
- Global flag pre-scan in `main()` (line 1086-1134) removes `--json` and `--ts-path` from argv before subcommand dispatch.
- `CliResult` struct (line 159) captures all result fields. `emitOutput()` renders either human-readable (`humanStdout`/`humanStderr`) or a JSON object with fields: `op`, `type`, `name`, `success`, `exit_code`, `prog_id`, `ell_path`, `error{code, message, ...}`.
- `errorCodeString()` (line 177) maps exit codes to machine-readable error codes.
- Batch subcommands ALWAYS emit a JSON array (line 991), regardless of `--json`.

**Batch mode (current):**
- `cmdBatch()` (line 1028) takes a manifest FILE PATH as the single positional arg.
- `parseManifest()` (line 800) reads the file line-by-line, skipping `#` comments and blank lines.
- `runBatch()` (line 922) constructs ONE `TsDevLib` on a 64MB-stack worker thread, loops over items calling `doInsert`/`doModify`/`doDelete`, calls `closeWarehouse()` once at end.
- Exit codes: 0 = all succeeded, 1 = usage/manifest error, 2 = batch ran but some failed.

**Worker thread pattern:**
All orlib operations run on a 64MB-stack worker thread (the EasyLanguage compiler needs a large stack; the hidden EDIT control is thread-affine). Each single-item subcommand creates a params struct, launches `CreateThread` with 64MB stack, waits, then emits output. Batch creates one thread for all items.

**`TsDevLib` public API (tsdev_lib.h line 66-157):**
- `findExistPos(name, type)` → ProgId (>0 if found, 0 if not) [line 78, impl line 604]
- `createEntry(type, name, errCode)` → ProgId [line 82, impl line 614]
- `saveSource(type, progId, source)` → bool [line 87, impl line 637]
- `verifyModule(type, progId)` → error string (empty = success) [line 97, impl line 651]
- `closeWarehouse()` [line 105]
- `deleteEntry(type, name)` → bool [line 117, impl line 775]
- `modifyEntry(type, name, newSource)` → Result [line 135, impl line 907]
- `ellFileExists(type, progId)` → bool [line 139]
- `getEllPath(type, progId)` → string [line 142]
- `getMyWorkBinPath()` → const char* [line 148]

**Constructor (line 442):** Loads orlib20.dll via `OrlibResolver`, resolves 7 exports + 8 internal functions via signature scan, resolves 3 data-global RVAs, applies persistence patch + 3 thread-check patches, calls `OrlibInit(NULL, 1)`, creates hidden EDIT control.

**Export resolution mechanism:**
- `OrlibResolver::resolveExport(const char* decoratedName)` (orlib_resolver.cpp line 749) — uses `GetProcAddress` in production mode, PE export table parsing in test mode.
- 7 exports currently resolved: `_OrlibInit@8`, `_OrlibExit@0`, `FindExistPos`, `_CreateEntryEX@24`, `_SaveModule_New@20`, `_VerifyModule_New@20`, `_CloseWarehouseForCurrentThreadAndPath@4`.
- `_LoadTextFileEX@16` is NOT yet resolved — it is a named export that `resolveExport()` can resolve with a single call (no signature scan needed for the function itself).

**Signature/patch system:**
- `signatures/v9.50.01.3344.json` — authoritative signature data (12 signatures, 3 data globals, 7 exports).
- `tools/sigcodegen.py` — generates `signatures.gen.h` at build time. Has hardcoded expected counts: `EXPECTED_SIGNATURE_COUNT = 12`, `EXPECTED_EXPORT_COUNT = 7` (line 37-39). **Adding a new signature or export requires updating these counts.**
- `OrlibResolver::applyPatch(name)` (orlib_resolver.h line 784) — resolves a patch site by signature, validates original bytes, applies replacement bytes via `VirtualProtect`.
- 4 patch sites currently: persistence patch (FUN_10011230) + 3 thread-check INT3 NOPs (commands 0x0F, general dispatch, 0x17).
- A NEW thread-check INT3 patch for warehouse command 0x11 will need a new signature entry in the JSON + updated `EXPECTED_SIGNATURE_COUNT`.

### 1.2 Python Wrapper (`TsDevClient`) — Current Architecture

**Source files:**
- `src/tsdev_automation/client.py` (309 lines) — `TsDevClient` class
- `src/tsdev_automation/results.py` (108 lines) — `TsDevResult`, `TsDevErrorInfo`, `TsDevBatchResult`
- `src/tsdev_automation/exceptions.py` (111 lines) — typed exception hierarchy
- `src/tsdev_automation/_cli.py` (116 lines) — argparse CLI (`tsdev-automation` console script)
- `src/tsdev_automation/__init__.py` (64 lines) — public API exports

**`TsDevClient` API (client.py):**
- `insert(type, name, source)` → `TsDevResult` (raises on failure) [line 124]
- `modify(type, name, source)` → `TsDevResult` [line 133]
- `delete(type, name)` → `TsDevResult` [line 137]
- `batch_insert(items)` → `list[TsDevResult]` [line 141]
- `batch_modify(items)` → `list[TsDevResult]` [line 149]
- `batch_delete(items)` → `list[TsDevResult]` [line 153]

**Internals:**
- `_single()` (line 165) — builds `--json` command, materializes source to temp file if string, runs subprocess, parses JSON, raises typed exception on failure.
- `_batch()` (line 182) — writes manifest to temp file (`_write_manifest`), runs batch subcommand, parses JSON array.
- `_materialize_source()` (line 194) — if source is an existing file path, uses directly; otherwise writes string to temp `.el` file.
- `_write_manifest()` (line 217) — writes `BatchItem` list to a temp manifest file. **This is the temp file that `--batch-stdin` will eliminate.**
- `_run_cli()` (line 239) — runs subprocess with `capture_output=True`, parses JSON from stdout.
- `_run_batch()` (line 266) — same but parses JSON array; exit 1 raises `TsDevError`.

**Python CLI (`_cli.py`):**
- argparse-based with subcommands: `insert`, `modify`, `delete`, `batch-insert`, `batch-modify`, `batch-delete`.
- `_read_manifest()` (line 99) reads a manifest file into `list[BatchItem]`.
- `_print_result()` (line 31) prints human-readable result.

**Python tests (`tests/test_tsdev_client.py`, 554 lines):**
- `TestSingleItem` (line 78) — 12 unit tests (mocked subprocess): insert/modify/delete success + error cases, source materialization, timeout, JSON parse failure.
- `TestBatch` (line 206) — 5 unit tests: batch insert mixed, manifest error, batch delete no source, batch modify mixed, string source cleanup.
- `TestCliMain` (line 272) — CLI subcommand tests.
- `TestExceptionHierarchy` (line 296) — exception mapping tests.
- `TestE2E` (line 378, `@pytest.mark.e2e`) — 9 E2E tests driving the real CLI.
- **Missing for Workstream F:** No tests for `exists`, `get-source`, `upsert`, or `--batch-stdin`.

### 1.3 Test Infrastructure

**Regression suite:** `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1`
- 3 sub-suites (run in child PowerShell processes):
  1. **sigtool verify gate** (`tests/test_sigtool.ps1`) — 12 tests against baseline DLL. No live TS needed.
  2. **C++ unit tests** (`tests/run_unit_tests.ps1`) — 14 test executables. 3 don't need TS (`test_no_hardcoded_rvas`, `test_tradingapp_type`, `test_orlib_resolver`).
  3. **E2E CLI tests** (`tests/run_e2e_tests.ps1`) — 34 tests per `docs/E2E_TEST_SPEC.md`. Requires live TS.

**TDD pattern used:**
- Tests are written FIRST (red), then implementation makes them pass (green).
- C++ tests: individual `.cpp` files in `tests/`, each linked against `tsdev_lib`, run on a 64MB-stack worker thread.
- Python tests: `pytest` with mocked `subprocess.run` for unit tests; `@pytest.mark.e2e` for real CLI tests.
- PowerShell batch/JSON tests: `tests/test_batch_mode.ps1`, `tests/test_json_output.ps1` — drive the real CLI.

**E2E test spec:** `docs/E2E_TEST_SPEC.md` — 34 tests covering insert/modify/delete for all 4 types + semantic errors + general errors. Part D (line 405) is a PENDING SPECIFICATION placeholder for Workstream F's new operations.

**Build:** `cmake --build build --config Release` (or `--target test_regression` for all test executables). 32-bit (Win32) only.

### 1.4 Key Findings for Workstream F

1. **`exists` (exact-match) is trivial:** `TsDevLib::findExistPos()` already exists (line 604) and returns ProgId >0 if found, 0 if not. The CLI just needs a new `exists` subcommand that calls it and formats the result. No new library code needed.

2. **`exists` (case-insensitive) is BLOCKED:** `findExistPos` is exact-match only and returns a ProgId, not the stored name. Case-insensitive search requires enumerating items of a given type and comparing names. An orlib20 enumeration API is NOT yet confirmed (needs RE). See §4 below.

3. **`upsert` is trivial:** It branches on `findExistPos` — if found, call `doModify`; if not found, call `doInsert`. Both already exist. No new library code.

4. **Inline batch via stdin is low-risk:** `parseManifest()` (line 800) currently reads from a file. Adding a `--batch-stdin` flag means reading the manifest from stdin instead. The parsing logic (line 806-906) is identical; only the input source changes. The `cmdBatch()` function (line 1028) needs to be modified to accept either a file path or `--batch-stdin`.

5. **`get-source` requires MODERATE RE:** Needs resolving `_LoadTextFileEX@16` (a named export — `resolveExport` can handle this), a NEW thread-check INT3 patch for warehouse command 0x11 (new signature + updated `EXPECTED_SIGNATURE_COUNT`), and live RE (Frida trace) to confirm parameter order. See §4 below.

6. **Signature count coupling:** `tools/sigcodegen.py` has `EXPECTED_SIGNATURE_COUNT = 12` and `EXPECTED_EXPORT_COUNT = 7` (line 37-39). Adding a new signature (for the command 0x11 patch) or a new export (`_LoadTextFileEX@16`) requires updating these constants AND the JSON, or the build fails.

7. **`test_no_hardcoded_rvas.cpp`** derives forbidden RVA constants from the generated header. Any new signature/export RVA will be automatically included in the forbidden set — no manual update needed to that test.

---

## 2. TVS Decomposition

Workstream F decomposes into **6 TVSs**, ordered by ROI and dependency. Each TVS is a minimal vertical slice that delivers value end-to-end (C++ CLI + Python wrapper + tests + E2E verification).

### Dependency graph:
```
TVS-F.1 (exists exact-match)     ────────────────┐
TVS-F.2 (inline batch via stdin)  ───────────────┤
TVS-F.3 (upsert)                  ─── depends on F.1 ──┤
                                                 ├──► TVS-F.6 (Python wrapper for all)
TVS-F.4 (get-source RE + impl)    ─── independent ────┤
                                                 │
TVS-F.5 (exists case-insensitive) ─── depends on RE ──┘
```

**Rationale for ordering:** F.1, F.2, F.3 require NO new RE and compose existing primitives. F.4 requires moderate RE. F.5 requires RE for an enumeration API. F.6 wraps everything in Python. The highest-ROI slices come first.

---

### TVS-F.1: `exists` subcommand (exact-match, C++ CLI)

**Goal:** Add an `exists` subcommand to `tsdev_cli` that queries item existence by name + component type (exact-match), returning the ProgId and existence flag, with `--json` output support.

**Scope:**
- IN: New `exists` subcommand in `tsdev_cli.cpp`. Calls `TsDevLib::findExistPos()`. Returns ProgId (>0 = exists, 0 = not found). Human-readable + JSON output. Exit code 0 (exists or not-found, distinguished by the `exists` flag in output) / 2 (unknown type). Works on a 64MB-stack worker thread (same pattern as insert/modify/delete). Returns the verbatim name as passed (exact-match = the query name IS the stored name when found).
- OUT: Case-insensitive search (deferred to TVS-F.5). Python wrapper (deferred to TVS-F.6). `get-source`, `upsert`, inline batch.

**Dependencies:** None (uses existing `findExistPos` primitive).

**Atomic subtasks:**
1. **Write failing test for `exists` subcommand** — Add a new C++ test file `tests/test_exists.cpp` that constructs `TsDevLib`, inserts a test strategy, calls `findExistPos` for the existing name (expect ProgId >0), calls `findExistPos` for a nonexistent name (expect 0). Also add a PowerShell E2E test in `tests/test_exists.ps1` (or extend `run_e2e_tests.ps1`) that runs `tsdev_cli exists strategy <name>` for an existing item (expect exit 0, JSON with `exists: true`, `prog_id: N`) and a nonexistent item (expect exit 0, JSON with `exists: false`). *(edit: create test files)*
2. **Build + run to confirm RED** — Build the new test executable, run it, confirm it fails (the `exists` subcommand doesn't exist yet). *(verify: build + run)*
3. **Implement `exists` subcommand in `tsdev_cli.cpp`** — Add `doExists()` function (calls `lib.findExistPos(name, type)`, populates `CliResult` with `op="exists"`, `exists` flag, `progId`). Add `cmdExists()` (validates type/name, launches worker thread). Add `runExists()` worker. Add subcommand dispatch in `main()`. Note: the `exists` op never emits the `not_found` error code (not-found is exit 0 with `exists: false`, not an error); do not add an `exit 5 → not_found` mapping for the `exists` op. Update `printUsage()`. Update the usage comment at the top of the file. *(edit: tsdev_cli.cpp)*
4. **Build + run to confirm GREEN** — Build `tsdev_cli` + the test executable, run the test, confirm it passes. *(verify: build + run)*
5. **Run regression suite** — `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (or at minimum the sigtool + unit test suites). Confirm no regressions. *(verify: run regression)*
6. **Adversarial review** — Independent reviewer checks: complete execution of the subtask instructions, no scope creep, no drift, no undue shortcuts. *(review)*
7. **E2E CLI verification** — Run E2E test per `docs/E2E_TEST_SPEC.md` Part A (insert → exists → delete cycle for at least Strategy). Confirm `exists` returns correct results. *(verify: E2E)*

**Files that will be touched:**
- `src/tsdev_cli.cpp` — add `exists` subcommand (doExists, cmdExists, runExists, dispatch, usage, error codes)
- `tests/test_exists.cpp` — new C++ integration test
- `tests/test_exists.ps1` or `tests/run_e2e_tests.ps1` — new E2E test cases
- `CMakeLists.txt` — add `test_exists` executable target
- `tests/run_unit_tests.ps1` — add `test_exists` to the test inventory

**Risks / unknowns:**
- Exit code for "not found" in `exists` — should it be 5 (same as modify/delete not-found) or 0 (it's a query, not an error)? **Decision:** `exists` emits exit 0 with `exists: false` for not-found (not-found is a valid query result, not an error). This differs from modify/delete which use exit 5 for not-found. The `errorCodeString()` mapping does not need a `not_found` case for the `exists` op (the `exists` op never emits the `not_found` error code). The E2E test spec (§Part D, line 409) says "expected: verbatim stored name, exists flag, ProgId, exit code" — doesn't specify the exit code for not-found. This plan uses exit 0 for both exists and not-exists (query success), with the `exists` boolean distinguishing.

---

### TVS-F.2: Inline batch via stdin (`--batch-stdin` flag, C++ CLI)

**Goal:** Add a `--batch-stdin` flag to `batch-insert`/`batch-modify`/`batch-delete` that reads the manifest from stdin instead of a file path, maintaining full backward compatibility with the existing file-path mode.

**Scope:**
- IN: Modify `cmdBatch()` to accept either a manifest file path (existing) OR `--batch-stdin` (new). When `--batch-stdin` is present, read manifest content from stdin (cin) instead of opening a file. Refactor `parseManifest()` to parse from a string/stream (not just a file path). Update subcommand dispatch in `main()` to handle the `--batch-stdin` flag. Update `printUsage()`. The Python wrapper change (using `--batch-stdin` instead of temp manifest files) is deferred to TVS-F.6.
- OUT: Python wrapper changes. Other new subcommands.

**Dependencies:** None (modifies existing batch mode only).

**Atomic subtasks:**
1. **Write failing test for `--batch-stdin`** — Add a PowerShell test in `tests/test_batch_stdin.ps1` that pipes a manifest via stdin to `tsdev_cli batch-insert --batch-stdin` (using PowerShell's piping or `echo | tsdev_cli`), verifies the JSON array output matches the file-path mode. Also test that the file-path mode still works (backward compat). *(edit: create test file)*
2. **Build + run to confirm RED** — Build `tsdev_cli`, run the stdin test, confirm it fails (`--batch-stdin` not recognized yet). *(verify: build + run)*
3. **Refactor `parseManifest()` to accept a string** — Extract the line-parsing logic from `parseManifest()` (line 800-906) into a new `parseManifestLines(const std::string& content, bool needsSource, vector<BatchItem>& outItems)` that parses from a string. Keep `parseManifest(path, ...)` as a thin wrapper that reads the file then calls `parseManifestLines()`. *(edit: tsdev_cli.cpp)*
4. **Implement `--batch-stdin` in `cmdBatch()` and `main()`** — Modify `cmdBatch()` to accept a `bool useStdin` parameter. When true, read all of stdin into a string and call `parseManifestLines()`. Modify the subcommand dispatch in `main()` (line 1227) to detect `--batch-stdin` in the reduced argv and set a `useStdin` flag. When `--batch-stdin` is present, no manifest file path arg is expected (reducedArgc == 3, with `--batch-stdin` as the third arg (reducedArgvPtr[2]) in place of a manifest file path). The dispatch logic should detect `--batch-stdin` in reducedArgv, set the `useStdin` flag, and in that branch skip the file-path-open path (no `parseManifest(file)` call). Update `printUsage()`. *(edit: tsdev_cli.cpp)*
5. **Build + run to confirm GREEN** — Build `tsdev_cli`, run the stdin test, confirm it passes. Run the existing `tests/test_batch_mode.ps1` to confirm backward compat. *(verify: build + run)*
6. **Run regression suite** — `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1`. Confirm no regressions. *(verify: run regression)*
7. **Adversarial review** — Independent reviewer. *(review)*
8. **E2E CLI verification** — Run E2E test per `docs/E2E_TEST_SPEC.md` Part A (at least one insert/modify/delete cycle). Confirm CLI is operational. *(verify: E2E)*

**Files that will be touched:**
- `src/tsdev_cli.cpp` — refactor `parseManifest()`, add `parseManifestLines()`, modify `cmdBatch()`, modify `main()` dispatch, update usage
- `tests/test_batch_stdin.ps1` — new PowerShell test

**Risks / unknowns:**
- Reading stdin in the main thread before launching the worker thread is safe (no orlib dependency). The manifest content is read fully into a string, then parsed, then items are passed to the worker thread (same as file-path mode).
- PowerShell piping may add encoding issues. The test should use `[System.IO.File]::WriteAllText` + input redirection or `$content | & $cli batch-insert --batch-stdin` carefully. May need to read stdin as binary to avoid newline translation.
- The `--batch-stdin` flag must be detected in the global flag pre-scan OR in the batch subcommand dispatch. Since it's batch-specific, it should be handled in the batch dispatch section (line 1227), not the global pre-scan (which handles `--ts-path` and `--json`).

---

### TVS-F.3: `upsert` subcommand (C++ CLI)

**Goal:** Add an `upsert` subcommand that inserts if the item doesn't exist, or modifies if it does, by name + component type + source file.

**Scope:**
- IN: New `upsert` subcommand in `tsdev_cli.cpp`. Calls `findExistPos` — if found, calls `doModify`; if not found, calls `doInsert`. Human-readable + JSON output. Exit codes match insert/modify (0 = success, 8 = compile error, etc.). Works on a 64MB-stack worker thread.
- OUT: Python wrapper (deferred to TVS-F.6). `exists`, `get-source`, inline batch.

**Dependencies:** TVS-F.1 (conceptually — `upsert` branches on existence, same primitive as `exists`; but technically `upsert` can be implemented independently since `doInsert` already checks `findExistPos` internally and returns exit 5 if exists — the upsert logic needs to catch that and redirect to `doModify`). **No hard dependency** — can be done in parallel with F.1.

**Atomic subtasks:**
1. **Write failing test for `upsert` subcommand** — Add `tests/test_upsert.cpp` (C++ integration test: insert a new item via upsert, verify it exists; upsert the same item with modified source, verify ProgId preserved and source changed). Add PowerShell E2E test cases for upsert-when-absent and upsert-when-present. *(edit: create test files)*
2. **Build + run to confirm RED** — Build, run, confirm failure. *(verify: build + run)*
3. **Implement `upsert` subcommand in `tsdev_cli.cpp`** — Add `doUpsert()` function: calls `lib.findExistPos(name, type)`; if progId >0, calls `doModify()`; if progId == 0, calls `doInsert()` with `flushPerItem=true`. Add `cmdUpsert()` (same arg structure as insert/modify: type, name, source_file). Add `runUpsert()` worker. Add dispatch in `main()`. Update `printUsage()`. Add `"upsert"` to `errorCodeString()` if needed (or reuse insert/modify codes). *(edit: tsdev_cli.cpp)*
4. **Build + run to confirm GREEN** — Build, run test, confirm pass. *(verify: build + run)*
5. **Run regression suite** — Full regression. *(verify: run regression)*
6. **Adversarial review** — Independent reviewer. *(review)*
7. **E2E CLI verification** — E2E test: upsert a new strategy (expect exit 0, ProgId assigned), upsert same strategy with modified source (expect exit 0, same ProgId), delete, verify clean. *(verify: E2E)*

**Files that will be touched:**
- `src/tsdev_cli.cpp` — add `upsert` subcommand
- `tests/test_upsert.cpp` — new C++ integration test
- `tests/test_upsert.ps1` or extension to `run_e2e_tests.ps1` — new E2E test cases
- `CMakeLists.txt` — add `test_upsert` target
- `tests/run_unit_tests.ps1` — add `test_upsert` to inventory

**Risks / unknowns:**
- The `doInsert` function (line 264) already checks `findExistPos` and returns exit 5 if the item exists. For `upsert`, we need to check `findExistPos` FIRST, then branch — NOT call `doInsert` and catch exit 5. The `doUpsert()` function should call `findExistPos` directly, then call `doModify()` or `doInsert()` based on the result. This avoids the overhead of a failed insert attempt.
- ProgId preservation: when upserting an existing item, `doModify` preserves the ProgId (confirmed in FEASIBILITY.md §5.8). When upserting a new item, `doInsert` assigns a new ProgId. Both are correct behaviors.

---

### TVS-F.4: `get-source` subcommand (C++ CLI) — REQUIRES RE

**Goal:** Add a `get-source` subcommand that retrieves EasyLanguage source by name + component type, outputting raw EL to stdout or writing to a file via `--output <file_path>`.

**Scope:**
- IN: RE work to resolve `_LoadTextFileEX@16` parameter order (Frida trace). New signature for warehouse command 0x11 thread-check INT3 patch site. New `getSource()` method on `TsDevLib`. New `get-source` subcommand in `tsdev_cli.cpp`. `--output <file_path>` flag. Error handling for password-protected items (return error). Human-readable + JSON output.
- OUT: Python wrapper (deferred to TVS-F.6). Case-insensitive exists.

**Dependencies:** None (independent of F.1/F.2/F.3, but requires RE work first).

**Prerequisite RE subtasks (must be completed before implementation):**
1. **RE: Confirm `_LoadTextFileEX@16` parameter order** — Use Frida to trace TSDev.exe: hook `_LoadTextFileEX@16` (addr 0x101081A0), trigger a "view source" action in the TSDev GUI, capture `args[0..3]` in `onEnter`. Confirm: param_1 unused, param_2=whType, param_3=progId, param_4=password/NULL. Document findings. *(RE: Frida trace)*
2. **RE: Identify the warehouse command 0x11 thread-check INT3 patch site** — Use Ghidra to find the `swi(3)` (INT3) instruction in the command-0x11 dispatch path (`FUN_101d0e80` or its callee). Determine the containing function, the byte offset from the function entry to the INT3, and the surrounding byte pattern for a signature. Document the RVA, offset, and signature bytes. *(RE: Ghidra static analysis)*
3. **RE: Confirm the return value semantics of `_LoadTextFileEX@16`** — Confirm it returns an `HGLOBAL` (global memory handle) containing the source text. Confirm `GlobalLock` retrieves the text pointer and `GlobalFree` releases it. Confirm NULL return = failure. *(RE: Frida trace or Ghidra decompilation)*

**Implementation atomic subtasks:**
4. **Add new signature for command 0x11 thread-check patch** — Add a new signature entry to `signatures/v9.50.01.3344.json` (13th signature, `targetType: "patch_site"`). Update `EXPECTED_SIGNATURE_COUNT` in `tools/sigcodegen.py` from 12 to 13. Run `sigtool` to verify the new signature matches uniquely against the baseline DLL. *(edit: signatures JSON + sigcodegen.py)*
5. **Write failing test for `get-source`** — Add `tests/test_get_source.cpp` that inserts a test strategy with known source, then calls `getSource()` and verifies the returned text matches. Add negative test for nonexistent item. *(edit: create test file)*
6. **Build + run to confirm RED** — Build, run, confirm failure. *(verify: build + run)*
7. **Implement `getSource()` in `TsDevLib`** — Resolve `_LoadTextFileEX@16` via `m_resolver->resolveExport("_LoadTextFileEX@16")`. Apply the new command-0x11 thread-check patch in the constructor (alongside the existing 3 patches). Implement `getSource()`: call `findExistPos` to get progId, call `_LoadTextFileEX(nullptr, getCategory(type), progId, nullptr)`, `GlobalLock` the result, copy to string, `GlobalUnlock` + `GlobalFree`. Return empty string on failure. Add the new export to `signatures/v9.50.01.3344.json` exports array and update `EXPECTED_EXPORT_COUNT` from 7 to 8. *(edit: tsdev_lib.h, tsdev_lib.cpp, signatures JSON, sigcodegen.py)*
8. **Build + run to confirm GREEN** — Build, run test, confirm pass. *(verify: build + run)*
9. **Implement `get-source` subcommand in `tsdev_cli.cpp`** — Add `doGetSource()` (calls `lib.getSource()`, outputs raw EL to stdout or writes to `--output` file). Add `cmdGetSource()` (validates type/name, handles `--output` flag, launches worker). Add `runGetSource()` worker. Add dispatch in `main()`. Update `printUsage()`. Handle password-protected items (if `_LoadTextFileEX` returns NULL with a nonzero progId, return an error). *(edit: tsdev_cli.cpp)*
10. **Build + run to confirm GREEN (full)** — Build `tsdev_cli`, run the get-source E2E test. *(verify: build + run)*
11. **Run regression suite** — Full regression. *(verify: run regression)*
12. **Adversarial review** — Independent reviewer. *(review)*
13. **E2E CLI verification** — E2E: insert a strategy with known source, `get-source` to stdout (verify text matches), `get-source --output <file>` (verify file content matches), `get-source` nonexistent (expect error), cleanup delete. *(verify: E2E)*

**Files that will be touched:**
- `src/tsdev_lib.h` — add `getSource()` declaration, new function pointer typedef, new patch RVA member
- `src/tsdev_lib.cpp` — implement `getSource()`, resolve new export, apply new patch in constructor, restore patch in destructor
- `src/tsdev_cli.cpp` — add `get-source` subcommand
- `signatures/v9.50.01.3344.json` — add 13th signature (command 0x11 patch) + 8th export (`_LoadTextFileEX@16`)
- `tools/sigcodegen.py` — update `EXPECTED_SIGNATURE_COUNT` to 13, `EXPECTED_EXPORT_COUNT` to 8
- `tests/test_get_source.cpp` — new C++ integration test
- `tests/test_get_source.ps1` or extension to `run_e2e_tests.ps1` — new E2E test cases
- `CMakeLists.txt` — add `test_get_source` target
- `tests/run_unit_tests.ps1` — add `test_get_source` to inventory

**Risks / unknowns:**
- **Parameter order of `_LoadTextFileEX@16`** is PROVISIONAL (derived from static decompilation). If Frida trace reveals a different order, the implementation must be adjusted. This is the primary RE risk.
- **Command 0x11 thread-check INT3** — the exact containing function and byte offset must be identified via Ghidra. If the INT3 is in a different function than expected, a new signature must be crafted. The existing 3 thread-check patches (commands 0x0F/0x1E/0x17) follow the same pattern (NOP the INT3 byte), so the technique is proven.
- **Password-protected items** — the requirement says "password-protected items return an error." Passing NULL for the password parameter should retrieve standard items. If the item is password-protected, `_LoadTextFileEX` may return NULL or may prompt for a password (which would hang in headless mode). Need to verify behavior via RE. If it hangs, a timeout or pre-check may be needed.
- **`GlobalLock`/`GlobalFree`** — the returned `HGLOBAL` must be properly managed to avoid memory leaks. SEH protection may be needed around the call.
- **`EXPECTED_SIGNATURE_COUNT` and `EXPECTED_EXPORT_COUNT`** changes will cause `test_no_hardcoded_rvas` and `test_orlib_resolver` to need verification — the generated header changes, and these tests derive from it. The tests should automatically adapt (they derive forbidden constants from the generated header), but this must be verified.

---

### TVS-F.5: `exists` case-insensitive flag (C++ CLI) — REQUIRES RE

**Goal:** Add a `--case-insensitive` (or `-i`) flag to the `exists` subcommand that performs case-insensitive name matching and returns the verbatim (correctly-cased) stored name.

**Scope:**
- IN: RE work to find an orlib20 enumeration API. New `enumerateItems()` method on `TsDevLib` (if an enumeration API is found) OR an alternative approach (e.g., scanning the warehouse BST). Modify the `exists` subcommand to support the `--case-insensitive` flag. Return the verbatim stored name in the result.
- OUT: Python wrapper (deferred to TVS-F.6). `get-source`, `upsert`, inline batch.

**Dependencies:** TVS-F.1 (the `exists` subcommand must exist first). RE investigation (must find an enumeration API first).

**Prerequisite RE subtasks (must be completed before implementation):**
1. **RE: Find an orlib20 item enumeration API** — Use Ghidra to search for functions that iterate over warehouse entries by type. Candidates: functions that call `GetWarehouse` + `TypeConvert` + iterate a BST or linked list. Look for export names containing "Enum", "Iter", "Next", "First", "Count", "GetCount", "GetItem", "GetByName". Check if `_GetMasterRecByName` can be used with a wildcard. Check if the warehouse object has a vtable method for enumeration. Document findings. *(RE: Ghidra static analysis)*
2. **RE: Confirm the enumeration API parameter order and return semantics** — If an enumeration API is found, use Frida to trace TSDev.exe calling it. Confirm: how to start enumeration for a given whType, how to get the next item, how to retrieve the name from each item, how to detect end-of-list. Document findings. *(RE: Frida trace)*
3. **RE: Alternative approach if no enumeration API exists** — If no enumeration API is found, investigate whether the warehouse BST can be traversed directly (the `GetWarehouse` singleton returns a pointer to the per-thread warehouse object; its internal BST may be accessible). Alternatively, investigate whether `_GetMasterRecByName` can be called with a partial/wildcard name. Document findings and feasibility. *(RE: Ghidra static analysis)*

**Implementation atomic subtasks ( contingent on RE findings):**
4. **Write failing test for case-insensitive `exists`** — Add test cases to `tests/test_exists.cpp` (or a new `test_exists_ci.cpp`): insert "MyStrategy", query `exists --case-insensitive strategy mystrategy` (expect exists=true, verbatim_name="MyStrategy", prog_id matches). Query `exists --case-insensitive strategy MYSTRATEGY` (same result). *(edit: test files)*
5. **Build + run to confirm RED** — Build, run, confirm failure. *(verify: build + run)*
6. **Implement case-insensitive enumeration in `TsDevLib`** — Based on RE findings, add an `enumerateItems(type)` or `findExistPosCaseInsensitive(name, type)` method to `TsDevLib`. If using an enumeration API: resolve the new export(s) via `resolveExport()`, call the enumeration API, iterate items, compare names case-insensitively, return the matching ProgId + verbatim name. If no enumeration API: implement the alternative approach. Add new exports to `signatures/v9.50.01.3344.json` and update `EXPECTED_EXPORT_COUNT` if new exports are resolved. *(edit: tsdev_lib.h, tsdev_lib.cpp, signatures JSON, sigcodegen.py)*
7. **Modify `exists` subcommand for `--case-insensitive`** — Add `--case-insensitive` flag parsing to `cmdExists()`. When set, call the new case-insensitive method instead of `findExistPos`. Include `verbatim_name` in the JSON output. *(edit: tsdev_cli.cpp)*
8. **Build + run to confirm GREEN** — Build, run test, confirm pass. *(verify: build + run)*
9. **Run regression suite** — Full regression. *(verify: run regression)*
10. **Adversarial review** — Independent reviewer. *(review)*
11. **E2E CLI verification** — E2E: insert "E2E_CI_Test", query `exists --case-insensitive strategy e2e_ci_test` (expect exists=true, verbatim_name="E2E_CI_Test"), cleanup. *(verify: E2E)*

**Files that will be touched:**
- `src/tsdev_lib.h` — add new enumeration/case-insensitive method declaration
- `src/tsdev_lib.cpp` — implement the new method
- `src/tsdev_cli.cpp` — add `--case-insensitive` flag to `exists` subcommand
- `signatures/v9.50.01.3344.json` — add new export(s) if enumeration API is found
- `tools/sigcodegen.py` — update `EXPECTED_EXPORT_COUNT` if new exports are added
- `tests/test_exists.cpp` or `tests/test_exists_ci.cpp` — new test cases
- `tests/run_e2e_tests.ps1` — new E2E test cases

**Risks / unknowns:**
- **No confirmed enumeration API** — this is the primary risk. If no enumeration API exists in orlib20, the case-insensitive search may require traversing internal warehouse data structures (BST), which is fragile and version-dependent. The RE investigation (subtask 1-3) will determine feasibility.
- **Performance** — enumerating all items of a type to find a case-insensitive match may be slow for large libraries (3,413 items). This is acceptable for a CLI query but should be noted.
- **Verbatim stored name retrieval** — even if an enumeration API is found, the API must return the stored name (not just the ProgId). If the API only returns ProgIds, an additional call (e.g., `_GetMasterRecByName` with the exact name from the enumeration) may be needed to retrieve the verbatim name.
- **This TVS may be DEFERRED** if the RE investigation concludes that no enumeration API exists and the BST traversal approach is too risky. In that case, case-insensitive `exists` would be marked as a known limitation.

---

### TVS-F.6: Python wrapper for all new operations

**Goal:** Add `TsDevClient` methods and Python CLI subcommands for `exists`, `get-source`, `upsert`, and update batch methods to use `--batch-stdin` (eliminating temp manifest files).

**Scope:**
- IN: New `TsDevClient.exists()`, `TsDevClient.get_source()`, `TsDevClient.upsert()` methods. Update `TsDevClient._batch()` to use `--batch-stdin` (pipe manifest via stdin) instead of writing a temp manifest file. New Python CLI subcommands in `_cli.py`. New `TsDevResult` fields for `exists` (verbatim_name, exists flag) and `get-source` (source text). New exception types if needed. Python unit tests (mocked subprocess) + E2E tests.
- OUT: C++ CLI changes (all done in F.1-F.5).

**Dependencies:** TVS-F.1 (exists), TVS-F.2 (inline batch), TVS-F.3 (upsert). TVS-F.4 (get-source) if `get_source()` method is included. TVS-F.5 (case-insensitive exists) if the `case_insensitive` parameter is included.

**Atomic subtasks:**
1. **Write failing Python tests for `exists`** — Add `TestExists` class to `tests/test_tsdev_client.py` with mocked subprocess tests: `exists` returns `TsDevResult` with `exists=True/False`, `prog_id`, `verbatim_name`. Test `case_insensitive` parameter. *(edit: test file)*
2. **Run Python tests to confirm RED** — `pytest tests/test_tsdev_client.py -k "exists"` — confirm failures. *(verify: run pytest)*
3. **Implement `TsDevClient.exists()`** — Add `exists(type, name, case_insensitive=False)` method to `client.py`. Builds `--json exists <type> <name>` command (add `--case-insensitive` if requested). Parses JSON result. Returns `TsDevResult` (does NOT raise on not-found — it's a query). Add `ExistsResult` or extend `TsDevResult` with `exists: bool`, `verbatim_name: str|None` fields. Update `results.py`. *(edit: client.py, results.py)*
4. **Run Python tests to confirm GREEN for exists** — `pytest tests/test_tsdev_client.py -k "exists"` — confirm pass. *(verify: run pytest)*
5. **Write failing Python tests for `upsert`** — Add `TestUpsert` class with mocked subprocess tests. *(edit: test file)*
6. **Run Python tests to confirm RED for upsert** — `pytest tests/test_tsdev_client.py -k "upsert"` — confirm failures. *(verify: run pytest)*
7. **Implement `TsDevClient.upsert()`** — Add `upsert(type, name, source)` method. Builds `--json upsert <type> <name> <source_file>` command. Raises typed exceptions on failure (same as insert/modify). *(edit: client.py)*
8. **Run Python tests to confirm GREEN for upsert** — `pytest tests/test_tsdev_client.py -k "upsert"` — confirm pass. *(verify: run pytest)*
9. **Write failing Python tests for `get_source`** — Add `TestGetSource` class with mocked subprocess tests: `get_source` returns source text. Test `output_file` parameter. *(edit: test file)*
10. **Run Python tests to confirm RED for get_source** — `pytest tests/test_tsdev_client.py -k "get_source"` — confirm failures. *(verify: run pytest)*
11. **Implement `TsDevClient.get_source()`** — Add `get_source(type, name, output_file=None)` method. Builds `--json get-source <type> <name>` command (add `--output <file>` if requested). For stdout mode, the CLI outputs raw EL to stdout (NOT JSON) — the Python wrapper needs to handle this differently (read stdout as raw text, not JSON). Add `GetSourceResult` or extend `TsDevResult` with `source: str` field. *(edit: client.py, results.py)*
12. **Run Python tests to confirm GREEN for get_source** — `pytest tests/test_tsdev_client.py -k "get_source"` — confirm pass. *(verify: run pytest)*
13. **Write failing Python tests for `--batch-stdin`** — Add tests verifying that `TsDevClient.batch_*` should pass the manifest via stdin instead of writing a temp manifest file (assert `_batch()` passes `input` to `subprocess.run` and does NOT create temp manifest files). *(edit: test file)*
14. **Run Python tests to confirm RED for `--batch-stdin`** — `pytest tests/test_tsdev_client.py -k "batch"` — confirm failures (tests fail because `_batch()` still writes temp manifest files). *(verify: run pytest)*
15. **Update `_batch()` to use `--batch-stdin`** — Modify `TsDevClient._batch()` (line 182) to build the manifest string in-memory and pipe it via stdin instead of writing a temp manifest file. Change the command to `[sub, "--batch-stdin"]` and pass `input=manifest_string` to `subprocess.run()`. Remove `_write_manifest()` or keep it as dead code for backward compat. Update `_run_batch()` to pass `input` parameter. *(edit: client.py)*
16. **Run Python tests to confirm GREEN for batch-stdin** — `pytest tests/test_tsdev_client.py -k "batch"` — confirm pass. *(verify: run pytest)*
17. **Add Python CLI subcommands** — Add `exists`, `get-source`, `upsert` subcommands to `_cli.py`'s argparse parser. *(edit: _cli.py)*
18. **Run full Python test suite** — `pytest tests/test_tsdev_client.py` — all tests pass. *(verify: run pytest)*
19. **Run regression suite** — `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` — confirm no C++ regressions. *(verify: run regression)*
20. **Adversarial review** — Independent reviewer. *(review)*
21. **E2E CLI verification** — Run Python E2E tests (`pytest tests/test_tsdev_client.py -m e2e`) + C++ E2E tests. Confirm all pass. *(verify: E2E)*

**Files that will be touched:**
- `src/tsdev_automation/client.py` — add `exists()`, `get_source()`, `upsert()` methods; modify `_batch()` to use stdin
- `src/tsdev_automation/results.py` — extend `TsDevResult` with `exists`, `verbatim_name`, `source` fields
- `src/tsdev_automation/exceptions.py` — add `NotFoundError` for `exists` (if needed), `PasswordProtectedError` for `get-source`
- `src/tsdev_automation/_cli.py` — add `exists`, `get-source`, `upsert` subcommands
- `src/tsdev_automation/__init__.py` — export new result/exception classes
- `tests/test_tsdev_client.py` — add `TestExists`, `TestUpsert`, `TestGetSource` classes + batch-stdin tests

**Risks / unknowns:**
- **`get-source` output mode:** When `get-source` outputs raw EL to stdout (default, no `--output`), the Python wrapper cannot use `--json` mode (the CLI outputs raw text, not JSON). The wrapper needs to either: (a) always use `--output <temp_file>` and read the file, or (b) have the CLI support `--json` for `get-source` where the JSON includes the source text as a string field. Option (b) is cleaner for the Python wrapper but requires the C++ CLI to support `--json` for `get-source` (which should be designed in TVS-F.4). **Recommendation:** In TVS-F.4, design `get-source` to support `--json` mode where the JSON output includes `"source": "..."` field, AND raw mode (no `--json`) where raw EL goes to stdout. The Python wrapper always uses `--json`.
- **`exists` not raising on not-found:** Unlike insert/modify/delete which raise exceptions on failure, `exists` is a query — "not found" is a valid result, not an error. The Python wrapper should return a `TsDevResult` with `exists=False` and NOT raise an exception. This is a different pattern from the existing methods.
- **Batch stdin encoding:** When piping manifest content via stdin, encoding must be consistent. The CLI should read stdin as binary (matching the file-path mode's binary read). The Python wrapper should pass `input` as bytes or use `text=True` with appropriate encoding.

---

## 3. Recommended First TVS

### **TVS-F.1: `exists` subcommand (exact-match, C++ CLI)**

**Justification:**
1. **Lowest risk:** Requires NO new reverse engineering. The `findExistPos` primitive already exists (`TsDevLib::findExistPos()`, tsdev_lib.cpp line 604) and is confirmed working. The implementation is a thin CLI wrapper — similar in complexity to the existing `delete` subcommand (which also just calls `findExistPos` + one operation).
2. **Unblocks the most:** `exists` is a prerequisite for `upsert` (TVS-F.3, which branches on existence) and for E2E testing of `upsert` and `get-source` (verifying items exist before/after operations). It also establishes the pattern for adding new subcommands, which F.2/F.3/F.4 will follow.
3. **Highest ROI:** Delivers a user-facing capability immediately. The `exists` query is one of the 6 functions in the acceptance criteria (AGENTS.md line 17-18). It's the simplest of the 4 new capabilities to implement and test.
4. **No dependencies:** Can be started immediately with zero prerequisite work.
5. **Establishes the exit-code convention:** The `exists` subcommand introduces a new pattern — a query that returns "not found" as a successful result (exit 0, `exists: false`) rather than an error (exit 5). Resolving this design decision early prevents rework in F.3 (upsert) and F.5 (case-insensitive exists).

**Estimated effort:** 7 subtasks (write test, confirm RED, implement, confirm GREEN, regression, review, E2E). Each is a single narrowly-scoped subagent job. Total: ~1-2 hours of subagent work.

---

## 4. Open RE Questions

These must be resolved before TVS-F.4 (`get-source`) and TVS-F.5 (case-insensitive `exists`) can be implemented. The PM should schedule RE subtasks (using `subagent_general` profile with Ghidra/Frida MCP tools) before kicking off those TVSs.

### 4.1 For `get-source` (TVS-F.4)

| # | Question | Method | Status | Notes |
|---|----------|--------|--------|-------|
| RE-GS-1 | **`_LoadTextFileEX@16` parameter order** — Confirm: param_1 unused, param_2=whType, param_3=progId, param_4=password/NULL. | Frida trace of TSDev.exe: hook `_LoadTextFileEX@16` at orlib20_base+0x1081A0, trigger "view source" in TSDev GUI, capture `args[0..3]` in `onEnter`. | PROVISIONAL (static decompilation only) | FEASIBILITY.md §5.1 line 125-127. The named export is confirmed via Ghidra (addr 0x101081A0). Parameter order derived from static decompilation of inner logic (`FUN_101a0f90`). |
| RE-GS-2 | **Warehouse command 0x11 thread-check INT3 patch site** — Find the `swi(3)` instruction in the command-0x11 dispatch path. Identify the containing function, byte offset from entry, and surrounding byte pattern for a signature. | Ghidra: decompile `FUN_101d0e80` (command 0x11 dispatcher) and its callees. Search for `INT3` (0xCC) instructions. Document the RVA, offset, and signature bytes. | NOT STARTED | FEASIBILITY.md §5.1 line 127. Same NOP technique as existing 3 patches (commands 0x0F/0x1E/0x17, signatures/v9.50.01.3344.json lines 178-246). |
| RE-GS-3 | **`_LoadTextFileEX@16` return value semantics** — Confirm it returns an `HGLOBAL` (global memory handle). Confirm `GlobalLock` retrieves the text pointer and `GlobalFree` releases it. Confirm NULL = failure. | Frida trace or Ghidra decompilation of `_LoadTextFileEX@16` and its inner logic (`FUN_101a0f90`). | PROVISIONAL | FEASIBILITY.md §5.1 line 125: "returns a global memory handle containing the source text (caller must GlobalLock/GlobalFree); returns NULL on failure." |
| RE-GS-4 | **Password-protected item behavior** — What happens when `_LoadTextFileEX` is called with NULL password on a password-protected item? Does it return NULL, prompt for password (hang in headless mode), or crash? | Frida trace: create a password-protected item in TSDev, call `_LoadTextFileEX` with NULL password, observe behavior. | NOT STARTED | Critical for the "password-protected items return an error" requirement. If it hangs, a timeout or pre-check is needed. |
| RE-GS-5 | **Source text encoding** — Is the returned source text ANSI, UTF-8, or wide? The existing `saveSource` uses `SetWindowTextW` (wide) → `SaveModule_New` converts wide→UTF-8. Does `_LoadTextFileEX` return UTF-8 or ANSI? | Frida trace: examine the returned buffer's byte content for non-ASCII characters. | NOT STARTED | Affects how the CLI outputs the source (stdout as binary vs text). |

### 4.2 For case-insensitive `exists` (TVS-F.5)

| # | Question | Method | Status | Notes |
|---|----------|--------|--------|-------|
| RE-CI-1 | **Does orlib20 export an item enumeration API?** — Search for exports containing "Enum", "Iter", "Next", "First", "Count", "GetItem", "GetCount". Check if the warehouse object has a vtable method for enumeration. | Ghidra: search orlib20.dll export table (699 exports) for enumeration-related names. Decompile candidate functions. | NOT STARTED | FEASIBILITY.md §5.1 line 127 and AGENTS.md line 150: "An orlib20 enumeration API is NOT yet confirmed — needs RE investigation during implementation." |
| RE-CI-2 | **Can the warehouse BST be traversed directly?** — The `GetWarehouse` singleton (ordinal 10, RVA 0x1A9650) returns a pointer to the per-thread warehouse object. Does it have an accessible BST or array of items per type? | Ghidra: decompile `GetWarehouse` return type, examine the warehouse object's vtable and data members. Look for BST root pointers indexed by whType. | NOT STARTED | Fallback approach if no enumeration API exists. Higher risk (version-dependent internal structure). |
| RE-CI-3 | **Can `_GetMasterRecByName` be used with a partial/wildcard name?** — Does it support prefix matching or wildcard patterns? | Ghidra: decompile `_GetMasterRecByName@12` (RVA from FEASIBILITY.md §5.1). Check if the name comparison supports wildcards. | NOT STARTED | FEASIBILITY.md §5.1 line 105: `_GetMasterRecByName@12(const char* name, int WHType, void* outRecord)` — copies 268-byte master record. If it supports wildcards, case-insensitive search could iterate matches. |
| RE-CI-4 | **How does TSDev.exe implement case-insensitive search?** — When the TSDev GUI user searches for a component, does it use an orlib20 API or does it scan XML metadata files? | Frida trace: perform a search in the TSDev GUI, hook orlib20 functions to see which are called. | NOT STARTED | If TSDev.exe scans XML files out-of-band, the CLI could do the same (scan `MyWork\Bin\*.xml` files for name matches). This would bypass the need for an orlib20 enumeration API entirely. |
| RE-CI-5 | **Master record name field offset** — If an enumeration API is found, where is the item name stored in the 268-byte master record? | Ghidra: decompile `FindExistPos` and `_GetMasterRecByName` to find the name field offset in the master record. | NOT STARTED | FEASIBILITY.md §5.1 line 103: "ID = record[+0x18]". The name field offset is not documented. Needed to extract the verbatim stored name from enumerated records. |

### 4.3 RE scheduling recommendation

- **RE-GS-1 through RE-GS-5** should be scheduled as a batch BEFORE TVS-F.4 implementation. RE-GS-1 and RE-GS-2 are the critical path (parameter order + patch site). RE-GS-4 (password behavior) is a high-risk unknown that could affect the design.
- **RE-CI-1 through RE-CI-5** should be scheduled BEFORE TVS-F.5 implementation. RE-CI-1 (enumeration API search) is the critical path — if no API exists, RE-CI-2/CI-3/CI-4 are fallback investigations. RE-CI-5 is needed regardless of which approach is taken.
- All RE tasks should use the `subagent_general` profile with Ghidra MCP tools (for static analysis) and Frida CLI (for live tracing), per AGENTS.md rules.
- RE tasks are investigative only — they produce findings documents, not code changes. Each RE finding must be adversarially reviewed before being accepted as a basis for implementation.

---

## 5. Summary

| TVS | Title | RE needed? | Dependencies | Est. subtasks |
|-----|-------|-----------|--------------|---------------|
| **F.1** | `exists` subcommand (exact-match) | No | None | 7 |
| **F.2** | Inline batch via stdin | No | None | 8 |
| **F.3** | `upsert` subcommand | No | None (conceptually F.1) | 7 |
| **F.4** | `get-source` subcommand | Yes (RE-GS-1..5) | None | 13 (3 RE + 10 impl) |
| **F.5** | `exists` case-insensitive flag | Yes (RE-CI-1..5) | F.1 | 11 (3 RE + 8 impl) |
| **F.6** | Python wrapper for all new ops | No | F.1, F.2, F.3, (F.4, F.5) | 21 |

**Recommended execution order:** F.1 → F.2 → F.3 → (RE for F.4 in parallel) → F.4 → (RE for F.5) → F.5 → F.6

**Total estimated subtasks:** ~67 (including RE investigations)

**Key design decisions for PM:**
1. **`exists` exit code for not-found:** Exit 0 with `exists: false` (query success) vs exit 5 (error). This plan recommends exit 0 (it's a query, not an error).
2. **`get-source` JSON mode:** Should `--json` for `get-source` include the source text as a JSON string field? This plan recommends yes (enables clean Python wrapper integration).
3. **`--case-insensitive` flag name:** `--case-insensitive` (verbose) or `-i` (short) or both? This plan recommends both.
4. **Batch stdin encoding:** Read stdin as binary or text? This plan recommends binary (matching the file-path mode's binary read).
