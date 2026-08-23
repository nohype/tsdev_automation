# E2E CLI Test Specification — All Component Types

**Status:** Production-grade test specification
**Applies to:** `tsdev_cli.exe` (the production CLI for the TSDev Library)
**Last verified:** 2026-08-20 — core runner `tests/run_e2e_tests.ps1` PASSED 34/34 Part A-C cases, plus Workstream F tests and Workstream I directory batch tests. ShowMe is NOT executed by that runner; it is covered by the separate suite `tests/test_showme_e2e.ps1` (regression Suite 12, cases SM1-SM9). Serve mode E2E is implemented and passing as regression Suite 11 (`tests/run_serve_e2e.ps1` driving `tests/test_serve_e2e.py`, 2 pytest cases; TVS-K.6a complete).

---

## 0. Known Critical Runtime Defect — Scope Limitation of This Suite

**This E2E suite verifies CLI/library-level behavior only** (exit codes,
ProgId preservation, `.ell` file creation/removal, compiler error text,
warehouse cleanup). **It does NOT verify that components produced by the CLI
are runtime-linkable in TradeStation Desktop.**

A known critical defect exists: Strategies inserted/modified/upserted by the
production `tsdev_cli` appear **VERIFIED in TSDev** but fail at runtime in
TradeStation Desktop with "Unknown link error occurred." A full Desktop restart
does not repair a broken CLI Strategy. Other supported component types use the
common compile/finalization path and therefore belong in Workstream L's
validation matrix, but equivalent runtime failure is not asserted without
type-specific evidence. The user workaround (open the Strategy in TSDev, make
a harmless edit, re-Verify) re-runs the original finalization path and is a
workaround only — it is **not an acceptable fix** for an automated tool.

**Passing the existing CLI/library tests in this specification (Parts A–G)
does NOT establish runtime linkability.** Those tests confirm the CLI and
Python wrapper behave correctly at the library/warehouse level; they do not
exercise the TradeStation Desktop runtime linker, which is where this defect
manifests. A PASS verdict must be read as "CLI/library behavior is correct" —
not as "components are runtime-linkable in Desktop."

### 0.1 Implemented tests vs. the missing Desktop runtime-execution test

The currently implemented and passing suites are CLI/library-level only:
- `tests/run_e2e_tests.ps1` (Parts A–C, 34 cases) — exit codes, ProgId
  preservation, `.ell` create/remove, compiler error text, cleanup.
- `tests/test_showme_e2e.ps1` (Suite 12, SM1–SM9) — ShowMe lifecycle.
- `tests/run_serve_e2e.ps1` / `tests/test_serve_e2e.py` (Suite 11) — serve
  broker lifecycle and compile-failure recovery.
- Workstream F / I suites (exists, get-source, upsert, batch, directory
  batch) — CLI/library behavior.

**No Desktop runtime-execution test exists in the current suite.** Such a
test — compile via CLI, then load and execute the component in TradeStation
Desktop and assert a successful runtime link — is required to gate the
runtime-linkability property. It is part of the future Workstream L
requirement below, not a test that exists today.

### 0.2 Workstream L (planned — not implemented)

**Workstream L** is a future, planned workstream to fix the runtime-link
root cause and add the missing Desktop runtime-execution regression test. It
is **not yet started**; no production implementation has been made, and no
Desktop runtime-execution test exists today. This is a requirement for a
future TVS — not a claim that such a test currently exists.

- Workstream L plan: [`workstream_unknown_link_fix_plan.md`](workstream_unknown_link_fix_plan.md)
- Durable root-cause record: [`RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`](RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md)

---

## 1. Purpose & Scope

This document is the **definitive end-to-end (E2E) CLI test specification** for the `tsdev_cli` production CLI. It defines a comprehensive, repeatable test procedure that any agent or subagent can follow to verify the CLI is fully operational.

### When to use this specification

- **After every TVS** that touches the CLI, `tsdev_lib`, or component type handling — as part of the mandatory E2E CLI verification described in `AGENTS.md`.
- **As a regression gate** whenever the orlib20 integration, compiler error handling, or warehouse persistence logic changes.
- **On demand** when a full health check of all 5 supported component types is needed.

### What it covers

- All 5 supported component types: **Strategy, Indicator, Function, TradingApp, ShowMe**
- **Success cases:** insert → modify → delete lifecycle for each type
- **Type-specific semantic error cases:** constructs that are valid in some types but rejected by the compiler in others (e.g., Plot in a Strategy, Buy in an Indicator)
- **General error cases:** invalid syntax, modify/delete nonexistent, unknown type strings
- **Cleanup verification:** no orphaned `.ell`/`.xml` files or warehouse entries left behind

### What it does NOT cover

- Unit tests or library-level tests (those have their own test suites)
- The 3 unsupported types (ActivityBar, PaintBar, ProbabilityMap) — these are not yet wired into the CLI
- Performance or stress testing
- **Serve mode E2E** — implemented and passing as regression Suite 11; the detailed operator procedure is documented in Part F below (TVS-K.6b)
- **Crash handling E2E** — VEH crash handler + TsDevCrashError (see Part G below for the planned test procedure)
- **Desktop runtime-execution / runtime-linkability** — NOT covered. The known critical runtime defect and the missing Desktop runtime-execution test are documented in §0 above (Workstream L, planned). Passing this suite does not establish runtime linkability.

---

## 2. Prerequisites

### 2.1 Build the CLI

```powershell
cmake --build <build_dir> --config Release
```

The CLI executable is produced at:

```
<build_dir>\Release\tsdev_cli.exe
```

The default build directory for this project is `build`.

### 2.2 TradeStation must be installed

The CLI loads `orlib20.dll` and related DLLs from the TradeStation installation. The DLL must be loadable. The default install path is:

```
C:\Program Files (x86)\TradeStation 9.5
```

If TradeStation is installed elsewhere, use the `--ts-path <directory>` CLI flag to override.

### 2.3 No TS processes or services running

The CLI operates headlessly (no TSDev.exe, no TS Desktop). Ensure no TradeStation processes or services are running to avoid conflicts with the warehouse files.

### 2.4 PowerShell environment

This specification uses PowerShell syntax:
- Use `;` to chain commands (NOT `&&`)
- Use `&` to invoke executables (e.g., `& $cli insert strategy $name $srcFile`)
- Use `$LASTEXITCODE` to read the exit code of the last native command

### 2.5 Unique test names

To avoid collisions with real components or prior test runs, prefix all test component names with `E2E_` plus a run identifier (timestamp or run ID). Example:

```
$runId = "20250122_153000"
$strategyName = "E2E_${runId}_Strat"
$indicatorName = "E2E_${runId}_Ind"
$functionName  = "E2E_${runId}_Func"
$tradingappName = "E2E_${runId}_TA"
```

### 2.6 Source files — ASCII encoding

Write all EasyLanguage source to temp files using **ASCII encoding** (NOT UTF-8). UTF-8 files may include a BOM that the EasyLanguage compiler does not handle correctly. In PowerShell:

```powershell
$src = "Inputs: int x(0); if x > 0 then Buy next bar at market;"
$srcFile = "$tmpDir\strategy_insert.txt"
[System.IO.File]::WriteAllText($srcFile, $src, [System.Text.Encoding]::ASCII)
```

Create a temp directory for test artifacts:

```powershell
$tmpDir = "tmp\e2e_run_$runId"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
```

---

## 3. CLI Argument Format

Confirmed from `src/tsdev_cli.cpp`:

```
# Single-item operations (Parts A-C)
tsdev_cli [--ts-path <directory>] insert  <type> <name> <source_file>
tsdev_cli [--ts-path <directory>] modify  <type> <name> <source_file>
tsdev_cli [--ts-path <directory>] delete  <type> <name>

# New operations (Part D — Workstream F)
tsdev_cli [--ts-path <directory>] exists  <type> <name> [--case-insensitive]
tsdev_cli [--ts-path <directory>] get-source  <type> <name> [--output <file>]
tsdev_cli [--ts-path <directory>] upsert <type> <name> <source_file>

# Batch operations (Part D — Workstream F)
tsdev_cli [--ts-path <directory>] batch-insert  <manifest_file> | --batch-stdin
tsdev_cli [--ts-path <directory>] batch-modify  <manifest_file> | --batch-stdin
tsdev_cli [--ts-path <directory>] batch-delete  <manifest_file> | --batch-stdin
tsdev_cli [--ts-path <directory>] batch-upsert  <manifest_file> | --batch-stdin

# Directory batch operations (Part E — Workstream I)
tsdev_cli [--ts-path <directory>] batch-insert-dir  <type> <dir_path> [-t <ext>...]
tsdev_cli [--ts-path <directory>] batch-modify-dir  <type> <dir_path> [-t <ext>...]
tsdev_cli [--ts-path <directory>] batch-delete-dir  <type> <dir_path> [-t <ext>...]
tsdev_cli [--ts-path <directory>] batch-upsert-dir  <type> <dir_path> [-t <ext>...]
```

### Global flags

| Flag | Description |
|------|-------------|
| `--ts-path <directory>` | Optional. Overrides the TradeStation install directory (default: auto-discovered from registry). Can appear as `--ts-path <dir>` or `--ts-path=<dir>`. If specified multiple times, the last occurrence wins. |
| `--json` | Optional. Switches single-item subcommand output from human-friendly text to structured JSON. Reused by `exists`, `get-source`, `upsert`, `insert`, `modify`, `delete`. |

### Subcommands

| Subcommand | Description |
|------------|-------------|
| `insert` / `modify` / `delete` | Single-item CRUD (case-insensitive subcommand). |
| `exists` | Query item existence by `<type>` + `<name>`. Returns `exists=N`, `stored_name=...`, `progId=N`. `--case-insensitive` enables case-insensitive matching. |
| `get-source` | Retrieve EasyLanguage source for `<type>` + `<name>` to stdout, or to a file via `--output <file>`. Internally encrypted storage is decrypted automatically; library items have no user password-protection feature. |
| `upsert` | Insert-or-modify: if the item exists, perform a modify (ProgId preserved); otherwise perform an insert. |
| `batch-insert` / `batch-modify` / `batch-delete` | Batch operations driven by a line-based manifest file, or by manifest content piped via stdin with `--batch-stdin`. |
| `batch-upsert` | Batch insert-or-modify driven by a line-based manifest file, or by manifest content piped via stdin with `--batch-stdin`. Each line: `<type> <name> <source_file>`. If the item exists → modify (ProgId preserved); otherwise → insert. |
| `batch-insert-dir` / `batch-modify-dir` / `batch-delete-dir` | Directory batch operations: enumerate regular files in `<dir_path>` matching accepted extensions (default `.el`+`.txt`; overridable via repeatable `-t <ext>`), use file stem as item name and file content as EL source (source ignored for delete). Reuse the same batch pipeline as manifest batch ops via shared `runBatchAndEmit()` helper. |
| `batch-upsert-dir` | Directory batch insert-or-modify: same enumeration as other `batch-*-dir` subcommands. If the item exists → modify (ProgId preserved); otherwise → insert. |
| `serve` | Named-pipe server mode: the CLI runs as an elevated broker serving requests dispatched over the pipe (used by the Python serve-broker; exercised by regression Suite 11, see Part F). Fully documented in TVS-K.6b (Part F below). |

### Arguments

| Argument | Description |
|----------|-------------|
| `<type>` | Component type string: `strategy`, `indicator`, `function`, `tradingapp`, or `showme` (case-insensitive). |
| `<name>` | Component name. Must not be empty. |
| `<source_file>` | Path to a file containing EasyLanguage source. Required for `insert`, `modify`, and `upsert`. Not used for `delete` or `exists`. The file is read in binary mode; must be non-empty. |
| `<manifest_file>` | Path to a line-based manifest file for batch operations. Mutually exclusive with `--batch-stdin`. |
| `--batch-stdin` | Read the manifest from stdin instead of a file (enables inline batch without temp files). |
| `--output <file>` | (`get-source` only) Write retrieved EL source to the given file instead of stdout. |
| `--case-insensitive` | (`exists` only) Enable case-insensitive name matching. |
| `<dir_path>` | (`batch-*-dir` only) Path to a directory whose regular files are enumerated as batch items. File stem (sans last extension) = item name; file content = EL source (for insert/modify/upsert; ignored for delete). |
| `-t <ext>` | (`batch-*-dir` only) Repeatable extension filter. Default extensions: `.el` + `.txt`. The first `-t` overrides the defaults; subsequent `-t` flags add additional extensions. Dot-prefix is optional (`-t el` ≡ `-t .el`). Case-insensitive. |

### Valid type strings

| Type string | ComponentType enum | Warehouse type ID | `.ell` file pattern |
|-------------|-------------------|-------------------|---------------------|
| `strategy` | Strategy | 0 | `elmodule_0_<ProgId>.ell` |
| `indicator` | Indicator | 2 | `elmodule_2_<ProgId>.ell` |
| `function` | Function | 4 | `elmodule_4_<ProgId>.ell` |
| `tradingapp` | TradingApp | 5122 | `elmodule_5122_<ProgId>.ell` |
| `showme` | ShowMe | 1282 | `elmodule_1282_<ProgId>.ell` |

### Exit codes

| Exit code | Meaning | When produced |
|-----------|---------|---------------|
| **0** | Success | insert/modify/delete/upsert/get-source completed successfully; **`exists` returns 0 for BOTH found and not-found** (not-found is a valid query result, not an error) |
| **1** | Usage error (no subcommand or wrong arg count) | No subcommand given, or wrong number of positional args |
| **2** | Unknown type string | `<type>` is not one of the 5 valid strings |
| **3** | Source file error / **output_write_error** | File not found, unreadable, or empty (insert/modify/upsert); **or `get-source` output file write failure** (`--output <file>` could not be written) |
| **4** | Init failure | TsDevLib construction failed (DLL load, patch, or OrlibInit) |
| **5** | Not found | Component does not exist in the warehouse (modify/delete/get-source), or already exists (insert). **`upsert` never returns 5** — it branches on existence by construction (modify if present, insert if absent). **`exists` never returns 5** — not-found is reported via exit 0 with `exists=false`. |
| **6** | Entry operation failure | createEntry or deleteEntry returned failure |
| **7** | saveSource failure | Source could not be saved to the entry |
| **8** | Compile error | Compilation/verification failed; error text printed to stderr |
| **9** | .ell file missing | Compilation succeeded but the `.ell` file was not found on disk |
| **10** | Thread creation failure | CreateThread failed (system-level error) |
| **11** | Not elevated | Process is not running with administrator privileges (elevation guard, TVS-K.1). JSON form: `{"success":false,"exit_code":11,"error":{"code":"not_elevated","message":"..."}}` |

> **Batch subcommands** (`batch-insert`/`batch-modify`/`batch-delete`/`batch-upsert`/`batch-insert-dir`/`batch-modify-dir`/`batch-delete-dir`/`batch-upsert-dir`) use a **separate exit code scheme**:
>
>| Exit code | Meaning |
>|-----------|---------|
>| **0** | All items succeeded |
>| **1** | Usage/manifest error — the batch did not run (bad args, unreadable manifest, malformed manifest, empty directory with no matching files, non-existent directory, missing `-t` argument) |
>| **2** | One or more items failed (per-item results are reported in the JSON output; the batch ran but was not fully successful; includes unknown type) |

The exit codes most relevant to this test specification are **0** (success), **5** (not found), **8** (compile error), and **2** (unknown type).

---

## 4. Test Procedure — Part A: Success Cases (insert → modify → delete cycle)

For each of the 5 component types, run the full insert → modify → delete lifecycle. Each step must produce the expected exit code.

### 4.1 Test source table

| Type | Insert source | Modify source | Notes |
|------|--------------|---------------|-------|
| **Strategy** | `Inputs: int x(0);` + newline + `if x > 0 then Buy next bar at market;` | `Inputs: int x(1);` + newline + `if x > 0 then Sell next bar at market;` | Order statements (Buy/Sell) are allowed in Strategies |
| **Indicator** | `Inputs: int x(0);` + newline + `Plot1(x, "MyPlot");` | `Inputs: int x(1);` + newline + `Plot1(x * 2, "MyPlot2");` | Plot statements are allowed in Indicators |
| **Function** | `<FuncName> = 1;` | `<FuncName> = 42;` | **Function name in source MUST match the `<name>` argument** passed to the CLI. Replace `<FuncName>` with the actual test name (e.g., `E2E_20250122_Func = 1;`). No comment wrapper or `value =` syntax needed. |
| **TradingApp** | `using elsystem;` | `using elsystem;` + newline + `using elsystem.windows.forms;` | Minimal source `using elsystem;` compiles successfully |
| **ShowMe** | `Inputs: int x(0);` + newline + `Plot1(Close, "MyPlot");` | `Inputs: int x(1);` + newline + `Plot1(Close * 2, "MyPlot2");` | Plot statements are allowed in ShowMe (like Indicators) |

> **Function note (critical):** EasyLanguage functions require the function body to assign a value to the function name. The name used in the source assignment MUST exactly match the `<name>` argument passed to the CLI. For example, if the CLI command is `tsdev_cli insert function E2E_20250122_Func source.txt`, then `source.txt` must contain `E2E_20250122_Func = 1;`. A mismatch will cause a secondary compiler error: `"A value was never assigned to user function."`

### 4.2 Procedure per type

For each type (Strategy, Indicator, Function, TradingApp, ShowMe), using the appropriate test name and source from the table above:

**Step 1 — Insert:**
```powershell
& $cli insert $type $name $insertSrcFile
# Expected: exit 0, stdout contains "SUCCESS" and "ProgId=<N>"
# Record the ProgId for later verification
```

**Step 2 — Modify:**
```powershell
& $cli modify $type $name $modifySrcFile
# Expected: exit 0, stdout contains "SUCCESS" and "ProgId=<N>"
# Verify the ProgId is the SAME as the one from insert (ProgId preserved)
```

**Step 3 — Delete:**
```powershell
& $cli delete $type $name
# Expected: exit 0, stdout contains "SUCCESS" and "deleted"
```

**Step 4 — Delete-negative (delete again, should fail):**
```powershell
& $cli delete $type $name
# Expected: exit 5, stderr contains "not found in the warehouse"
```

### 4.3 What to verify

| Check | How |
|-------|-----|
| Insert returns exit 0 | Check `$LASTEXITCODE -eq 0` |
| Insert creates a `.ell` file | Parse ProgId from stdout; verify file matching the type's pattern exists in the warehouse Bin directory |
| Modify returns exit 0 | Check `$LASTEXITCODE -eq 0` |
| **ProgId is preserved** across modify | Compare ProgId from insert stdout with ProgId from modify stdout — they must be identical |
| Delete returns exit 0 | Check `$LASTEXITCODE -eq 0` |
| Delete removes the `.ell` file | Verify the `.ell` file no longer exists |
| Delete-negative returns exit 5 | Check `$LASTEXITCODE -eq 5` |

### 4.4 Expected results table

| Test ID | Type | Step | Expected exit | Key verification |
|---------|------|------|---------------|------------------|
| A1.1 | strategy | insert | 0 | ProgId assigned, `.ell` created |
| A1.2 | strategy | modify | 0 | ProgId preserved |
| A1.3 | strategy | delete | 0 | `.ell` removed |
| A1.4 | strategy | delete-neg | 5 | "not found in the warehouse" |
| A2.1 | indicator | insert | 0 | ProgId assigned, `.ell` created |
| A2.2 | indicator | modify | 0 | ProgId preserved |
| A2.3 | indicator | delete | 0 | `.ell` removed |
| A2.4 | indicator | delete-neg | 5 | "not found in the warehouse" |
| A3.1 | function | insert | 0 | ProgId assigned, `.ell` created |
| A3.2 | function | modify | 0 | ProgId preserved |
| A3.3 | function | delete | 0 | `.ell` removed |
| A3.4 | function | delete-neg | 5 | "not found in the warehouse" |
| A4.1 | tradingapp | insert | 0 | ProgId assigned, `.ell` created |
| A4.2 | tradingapp | modify | 0 | ProgId preserved |
| A4.3 | tradingapp | delete | 0 | `.ell` removed |
| A4.4 | tradingapp | delete-neg | 5 | "not found in the warehouse" |
| A5.1 | showme | insert | 0 | ProgId assigned, `.ell` created |
| A5.2 | showme | modify | 0 | ProgId preserved |
| A5.3 | showme | delete | 0 | `.ell` removed |
| A5.4 | showme | delete-neg | 5 | "not found in the warehouse" |

> **ShowMe cases — executed by `tests/test_showme_e2e.ps1` (regression Suite 12), NOT by `tests/run_e2e_tests.ps1`.** The main runner implements exactly the 16 cases A1.1–A4.4 (4 types × 4 steps) and contains zero ShowMe calls. ShowMe lifecycle coverage (insert/exists/get-source/modify/delete, incl. ProgId preservation and delete-negativity) is provided by the separate ShowMe suite (cases SM1–SM9).

**Part A total (main runner, `run_e2e_tests.ps1`): 16 test cases** (A1.1–A4.4). The 4 ShowMe rows above (A5.1–A5.4) are covered by the ShowMe suite (Suite 12).

---

## 5. Test Procedure — Part B: Type-Specific Semantic Error Cases

This is the **key section** that verifies the EasyLanguage compiler enforces type-specific constraints. For each test, an EasyLanguage construct that is valid in some component types is inserted into a type where it should be rejected. Each negative test is paired with a positive control that confirms the construct works where it IS allowed.

### 5.1 Test case table

| Test ID | Type | Source construct | Expected exit | Rationale | Expected error text (if failing) |
|---------|------|-----------------|---------------|-----------|----------------------------------|
| **B1** | strategy | `Plot1(Close, "Test");` | **8** | Plot statements not allowed in Strategies | `"This word not allowed in a Strategy."` |
| **B2** | indicator | `Buy next bar at market;` | **8** | Order statements not allowed in Indicators | `"This word not allowed in a study."` |
| **B3** | function | `Plot1(Close, "Test");` | **8** | Plot statements not allowed in Functions | `"This word not allowed in a function."` (plus secondary: `"A value was never assigned to user function."`) |
| **B4** | function | `Buy next bar at market;` | **8** | Order statements not allowed in Functions | `"This word not allowed in a function."` (plus secondary: `"A value was never assigned to user function."`) |
| **B5** | tradingapp | `Buy next bar at market;` | **8** | Order statements not allowed in TradingApps | `"This word not allowed in a study."` |
| **B6** | tradingapp | `Plot1(Close, "Test");` | **8** | Plot statements not allowed in TradingApps | `"This word only allowed in an Indicator, ShowMe or PaintBar study."` |
| **B7** (control) | indicator | `Plot1(Close, "Test");` | **0** | Plot IS allowed in Indicators (positive control) | — (success, ProgId assigned, `.ell` created) |
| **B8** (control) | strategy | `Buy next bar at market;` | **0** | Buy IS allowed in Strategies (positive control) | — (success, ProgId assigned, `.ell` created) |
| **B9** | showme | `Buy next bar at market;` | **8** | Order statements not allowed in ShowMe | `"This word not allowed in a study."` |
| **B10** (control) | showme | `Plot1(Close, "Test");` | **0** | Plot IS allowed in ShowMe (positive control, like Indicators) | — (success, ProgId assigned, `.ell` created) |

> **ShowMe cases — executed by `tests/test_showme_e2e.ps1` (regression Suite 12), NOT by `tests/run_e2e_tests.ps1`.** The main runner implements exactly the 8 cases B1–B8 and contains zero ShowMe calls. Equivalent ShowMe semantic coverage is provided by the ShowMe suite: SM7 = Buy-in-ShowMe rejected with exit 8 / "not allowed in a study" (cf. B9); SM1 = valid Plot1 source inserts successfully (cf. B10).

**Part B total (main runner, `run_e2e_tests.ps1`): 8 test cases** (B1–B8). B9/B10 are covered by the ShowMe suite (Suite 12).

### 5.2 Procedure per test case

For each test case in the table above, using a **unique test name** per case (e.g., `E2E_<runId>_B1`, `E2E_<runId>_B2`, etc.):

1. Write the source construct to a temp file (ASCII encoding).
2. Run the insert command:
   ```powershell
   & $cli insert $type $testName $srcFile
   ```
3. Check `$LASTEXITCODE` matches the expected exit code from the table.
4. For failing tests (exit 8): verify the stderr output contains the expected error text (substring match is sufficient — the full message includes line/column prefixes).
5. For succeeding tests (exit 0, the positive controls): record the ProgId and proceed to cleanup (delete).

### 5.3 Error message details

The EasyLanguage compiler produces type-specific error messages. These are the exact messages observed and verified in the reference test run:

| Violation | Error message | Notes |
|-----------|--------------|-------|
| Plot in Strategy | `This word not allowed in a Strategy.` | Strategy-specific message |
| Buy/Sell in Indicator | `This word not allowed in a study.` | Indicators are treated as "studies" |
| Plot in Function | `This word not allowed in a function.` | Plus secondary: `A value was never assigned to user function.` |
| Buy/Sell in Function | `This word not allowed in a function.` | Plus secondary: `A value was never assigned to user function.` |
| Buy/Sell in TradingApp | `This word not allowed in a study.` | TradingApps also treated as "studies" for order rejection |
| Plot in TradingApp | `This word only allowed in an Indicator, ShowMe or PaintBar study.` | More specific message listing allowed types |

> **Note:** Error messages are prefixed with `"Line X, Col Y: "` source location information by the CLI. When matching, search for the error text as a substring of the full stderr output.

### 5.4 Cleanup for Part B

After each test case:
- For **failing tests** (exit 8): the compilation failed, so no `.ell` file is written. However, a warehouse entry may have been created via `createEntry` before compilation. Attempt a cleanup delete:
  ```powershell
  & $cli delete $type $testName
  # May return exit 0 (entry existed and was deleted) or exit 5 (entry was not persisted)
  # Either is acceptable for cleanup purposes
  ```
- For **succeeding tests** (exit 0, positive controls B7/B8): the component was fully inserted. Delete it:
  ```powershell
  & $cli delete $type $testName
  # Expected: exit 0
  ```

### 5.5 Key verification: no orphaned `.ell` files from failed compiles

When compilation fails (exit 8), the CLI creates a warehouse entry via `createEntry` but does NOT write an `.ell` file (compilation failed, so no `.ell` is produced). Verify during cleanup that no `.ell` files exist for the ProgIds of failed compile tests. This confirms the CLI does not leave orphaned artifacts on failure.

---

## 6. Test Procedure — Part C: General Error Cases

These tests verify that the CLI handles general error conditions correctly across all 5 types.

### 6.1 Test case table

| Test ID | Type | Test | Expected exit | Expected output |
|---------|------|------|---------------|-----------------|
| **C1** | strategy | Invalid EasyLanguage syntax | **8** | Compiler error with line/column info (e.g., `"This word cannot start a statement."` + `"Semicolon (;) expected here."`) |
| **C2.1** | strategy | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.2** | indicator | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.3** | function | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.4** | tradingapp | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.5** | showme | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C3.1** | strategy | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.2** | indicator | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.3** | function | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.4** | tradingapp | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.5** | showme | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C4** | (any) | Unknown type string (e.g., `paintbar`) | **2** | `"unknown type"` message |

> **ShowMe cases — executed by `tests/test_showme_e2e.ps1` (regression Suite 12), NOT by `tests/run_e2e_tests.ps1`.** The main runner implements exactly the 10 cases C1, C2.1–C2.4, C3.1–C3.4, C4 and contains zero ShowMe calls. ShowMe not-found behavior is exercised by the ShowMe suite (SM9 verifies non-existence after delete).

**Part C total (main runner, `run_e2e_tests.ps1`): 10 test cases** (C1, C2.1–C2.4, C3.1–C3.4, C4). C2.5/C3.5 are covered by the ShowMe suite (Suite 12).

### 6.2 Procedure

**C1 — Invalid syntax:**
```powershell
# Write invalid EasyLanguage to a temp file
$src = "this is not valid easylanguage;"
[System.IO.File]::WriteAllText("$tmpDir\C1_invalid.txt", $src, [System.Text.Encoding]::ASCII)
& $cli insert strategy "E2E_${runId}_C1" "$tmpDir\C1_invalid.txt"
# Expected: exit 8, stderr contains compiler error text
# Cleanup: attempt delete (may return 5 or 0)
& $cli delete strategy "E2E_${runId}_C1"
```

**C2.x — Modify nonexistent (for each of the 5 types):**
```powershell
# Use a name that definitely does not exist
& $cli modify $type "E2E_${runId}_NonExistent" "$tmpDir\any_valid_source.txt"
# Expected: exit 5, stderr contains "not found in the warehouse. Use insert to create it."
```

**C3.x — Delete nonexistent (for each of the 5 types):**
```powershell
& $cli delete $type "E2E_${runId}_NonExistent"
# Expected: exit 5, stderr contains "not found in the warehouse."
```

**C4 — Unknown type string:**
```powershell
& $cli insert paintbar "E2E_${runId}_C4" "$tmpDir\any_valid_source.txt"
# Expected: exit 2, stderr contains "unknown type"
```

---

## 7. Cleanup Procedure

After ALL tests (Parts A, B, and C) are complete, perform a comprehensive cleanup verification:

### 7.1 Verify no leftover warehouse entries

Re-attempt `delete` on every test component name used in the run. All should return exit 5 (not found), confirming no entries were left behind:

```powershell
$allTestNames = @(
    "E2E_${runId}_Strat",
    "E2E_${runId}_Ind",
    "E2E_${runId}_Func",
    "E2E_${runId}_TA",
    "E2E_${runId}_B1", "E2E_${runId}_B2", "E2E_${runId}_B3", "E2E_${runId}_B4",
    "E2E_${runId}_B5", "E2E_${runId}_B6", "E2E_${runId}_B7", "E2E_${runId}_B8",
    "E2E_${runId}_B9", "E2E_${runId}_B10",
    "E2E_${runId}_C1"
)
foreach ($name in $allTestNames) {
    foreach ($type in @("strategy","indicator","function","tradingapp","showme")) {
        & $cli delete $type $name 2>$null
        # Expected: exit 5 (not found). If any returns 0, that entry was not cleaned up.
    }
}
```

> **Note:** It is normal for some of these delete attempts to target the wrong type for a given name. The key check is that no delete returns exit 0 (which would mean an entry was still present). All should return exit 5.

### 7.2 Verify no orphaned `.ell` files

Check the warehouse Bin directory for any `.ell` files matching the test ProgIds:

```powershell
$binDir = "C:\Program Files (x86)\TradeStation 9.5\MyWork\Bin"
Get-ChildItem -Path $binDir -Filter "elmodule_*_*.ell" | Where-Object {
    $_.Name -match "E2E" -or $_.Name -match $runId
}
# Expected: no results (all test .ell files should have been deleted)
```

### 7.3 Remove temp source files

```powershell
Remove-Item -Path $tmpDir -Recurse -Force
```

---

## Part D: New Operations & Inline Batch (Workstream F — COMPLETE)

Workstream F added 4 new operations and inline batch via stdin. All are implemented and tested by dedicated PowerShell test scripts.

### D.1: exists (exact-match) — tests/test_exists.ps1 (5 cases)
- D1.1a: exists on a known item → exists=true, progId>0
- D1.1b: exists on a non-existent item → exists=false, progId=0
- D1.2: exists with unknown type → exit 2 (unknown_type)
- D1.3a/b: exists with empty name / bad args → exit 1/2

### D.2: exists (case-insensitive) — tests/test_exists_ci.ps1 (9 invocations across CI.1-CI.5)
- CI.1-CI.5: case-insensitive matching returns verbatim stored name + ProgId. Each CI case that requires a probe item pairs an `insert` setup invocation with the `exists --case-insensitive` query invocation, plus a standalone nonexistent-item query (CI.3) — 9 CLI invocations total across CI.1-CI.5.

### D.3: get-source — tests/test_get_source.ps1 (7 invocations across GS1-GS6)
- GS1-GS6: retrieve EL source as raw stdout, JSON, and `--output` file; verify not-found, usage-error, and unknown-type handling. GS1 includes a paired `insert` setup invocation for the probe strategy; GS2-GS6 are direct `get-source` invocations (the GS3 output-file content check is a file assertion, not a CLI invocation) — 7 CLI invocations total across GS1-GS6. Internal-encryption retrieval is not part of this PowerShell sub-suite; add it to this inventory only when an actual registered invocation exists.

### D.4: upsert — tests/test_upsert.ps1 (9 invocations across U1-U5)
- U1: insert-path (new item)
- U2: modify-path (existing item, ProgId preserved)
- U3-U5: unknown type, compile error, function-type insert
- U1/U2/U5 each pair the `upsert` invocation with a follow-up `exists` verification invocation; U2 additionally includes a paired `insert` setup invocation for the existing item — 9 CLI invocations total across U1-U5.

### D.5: Inline batch via stdin — tests/test_batch_stdin.ps1 (8 cases)
- S1-S8: insert/delete/modify mixed via stdin, comments/blanks, empty stdin, backward-compat file-path, no-args rejected, stdin≡file equivalence

### D.6: batch-upsert — tests/test_batch_upsert.ps1 (5 cases)
- BU1: batch-upsert via manifest file, mixed insert+modify paths, ProgId preserved on modify-path
- BU2: batch-upsert via --batch-stdin, same invariants
- BU3: compile-error item → exit 2, per-item failure with compile_error
- BU4: empty manifest → exit 1, stderr "empty"
- BU5: no args → exit 1, stderr "requires exactly 1 argument"

---

## Part E: Directory Batch Operations (Workstream I — TVS-I.2 COMPLETE)

Workstream I (TVS-I.2) added 4 directory batch subcommands. These enumerate regular files in a directory (default extensions `.el`+`.txt`, overridable via repeatable `-t <ext>` flag), use the file stem as the item name and the file content as the EasyLanguage source, and reuse the existing batch pipeline via a shared `runBatchAndEmit()` helper. Tested by `tests/test_directory_batch.ps1` (9 E2E cases DB1–DB9) and `tests/test_directory_batch.cpp` (6 unit cases for `parseDirectory()` logic).

### E.1: Directory batch test cases — tests/test_directory_batch.ps1 (9 cases)

| Test ID | Subcommand | Scenario | Expected exit | Key assertions |
|---------|------------|----------|---------------|-----------------|
| **DB1** | `batch-insert-dir` | Create temp dir with 3 `.el` files (valid EL strategy source), run batch-insert-dir | **0** | JSON array of 3 items, all `success=true`, `op=insert`, `prog_id>0`; each verified via `exists --json` |
| **DB2** | `batch-modify-dir` | Insert items first (from DB1), then modify via batch-modify-dir with updated source files | **0** | JSON array of 2 items, all `success=true`, `op=modify`; ProgIds preserved (verified via `exists --json` before and after) |
| **DB3** | `batch-delete-dir` | Insert items (reuse DB1 item + 2 new pre-inserted), then delete via batch-delete-dir | **0** | JSON array of 3 items, all `success=true`, `op=delete`, `prog_id>0`; all verified deleted via `exists --json` (exists=false) |
| **DB4** | `batch-upsert-dir` | Mixed insert/modify: one file already exists (modify-path, ProgId preserved), one file is new (insert-path) | **0** | JSON array of 2 items; modify-path: `success=true`, `op=upsert`, `prog_id` preserved; insert-path: `success=true`, `op=upsert`, `prog_id>0` (new); both verified via `exists --json` |
| **DB5** | `batch-insert-dir` | `-t` flag override: dir with `.el`, `.abc`, `.txt` files; run with `-t el -t abc` | **0** | JSON array of 2 items (`.el` + `.abc` only); `.txt` item excluded; `.txt` item verified NOT inserted via `exists --json` |
| **DB6** | `batch-insert-dir` | Empty directory (no matching files) | **1** | No JSON array on stdout; stderr contains error message |
| **DB7** | `batch-insert-dir` | Unknown type string (`bogusType`) | **2** | No JSON array on stdout; stderr contains "unknown type" |
| **DB8** | `batch-insert-dir` | Non-existent directory path | **1** | No JSON array on stdout; stderr contains error message |
| **DB9** | `batch-insert-dir` | `-t` flag with missing argument (trailing `-t` with no extension) | **1** | No JSON array on stdout; stderr contains "requires an extension argument" |

**Part E total: 9 E2E test cases + 6 C++ unit test cases (parseDirectory logic).**

### E.2: C++ unit test cases — tests/test_directory_batch.cpp (6 cases)

| Test | Scenario | Key assertions |
|------|----------|-----------------|
| 1. Extension filtering (default) | Dir with `alpha.el`, `beta.txt`, `gamma.md` | 2 items (alpha, beta); gamma excluded; sources read |
| 2. Extension override (`.el` only) | Same dir, extensions `{".el"}` | 1 item (alpha); beta.txt excluded |
| 3. Stem extraction | `my.strategy.el` → stem = "my.strategy" | `path::stem()` strips only last extension |
| 4. Empty directory | Freshly-created empty dir | 0 items |
| 5. Subdirectory skipping | Top-level `.el` + nested subdir with `.el` | 1 item (top only); no recursion |
| 6. Source reading | `needsSource=true` vs `false` | true: source = file content; false (delete): source empty |

### E.3: Directory batch exit codes

Directory batch subcommands use the same exit code scheme as manifest batch subcommands (see §3 Exit codes):

| Exit code | Meaning |
|-----------|---------|
| **0** | All items succeeded |
| **1** | Usage error / empty dir (no matching files) / non-existent directory / missing `-t` argument (batch did not run) |
| **2** | Batch ran but one or more items failed (incl. unknown type) |

---

## 8. Pass/Fail Criteria

The overall test run verdict is **PASS** only if ALL of the following are true:

| Criterion | Requirement |
|-----------|-------------|
| **Part A — Success cases** | All 16 main-runner test cases pass (A1.1–A4.4; exit codes match expected for every insert/modify/delete/delete-neg step) |
| **Part A — ProgId preservation** | ProgId is preserved across modify for all 4 main-runner types (+ ShowMe via Suite 12, SM5) |
| **Part A — `.ell` files** | `.ell` files are created on insert and removed on delete for all 4 main-runner types (+ ShowMe via Suite 12) |
| **Part B — Semantic errors** | All 8 main-runner test cases pass (B1–B8): negative tests return exit 8 with correct error text, positive controls return exit 0 |
| **Part C — General errors** | All 10 main-runner test cases pass (C1, C2.1–C2.4, C3.1–C3.4, C4; exit codes match expected) |
| **ShowMe suite (Suite 12)** | All 9 `tests/test_showme_e2e.ps1` cases pass (SM1–SM9) |
| **Serve E2E (Suite 11)** | Both `tests/test_serve_e2e.py` pytest cases pass (driven by `tests/run_serve_e2e.ps1`) |
| **Cleanup — Warehouse** | No leftover warehouse entries (all cleanup deletes return exit 5) |
| **Cleanup — `.ell` files** | No orphaned `.ell` files for any test ProgId |
| **Cleanup — Temp files** | Temp source files removed |

If ANY criterion fails, the verdict is **FAIL** and the specific failing test case(s) must be documented with the actual vs. expected results.

---

## 9. Reporting

### 9.1 Report format

Document the results in a markdown file with the following structure:

1. **Header:** Date, CLI path, test artifacts directory, run ID
2. **Build result:** Build command and success/failure
3. **Part A results table:** Test ID, type, step, exit code, expected, PASS/FAIL, key output
4. **Part B results table:** Test ID, type, construct, expected, actual exit, PASS/FAIL, error text
5. **Part C results table:** Test ID, type, test, exit code, expected, PASS/FAIL, key output
6. **Summary table:** Part, tests, PASS, FAIL counts
7. **Overall verdict:** PASS or FAIL
8. **Key findings:** Notable observations (ProgId preservation, error messages, etc.)
9. **Cleanup confirmation:** Warehouse entries verified deleted, no orphaned `.ell` files, temp files removed
10. **Appendix:** Test source code used for each test case

### 9.2 Report location

Write the full report to:

```
tmp/re_artifacts/e2e_all_types_<timestamp>.md
```

Where `<timestamp>` is in the format `YYYYMMDD_HHMMSS`.

### 9.3 Report table template

```
| Test ID | Type | Step/Construct | Exit Code | Expected | Result | Key Output |
|---------|------|----------------|-----------|----------|--------|------------|
| A1.1 | strategy | insert | 0 | 0 | PASS | ProgId=2719, .ell created |
| ... | ... | ... | ... | ... | ... | ... |
```

---

## 10. Reference: Known Type-Specific Constraints

The following table documents the EasyLanguage construct constraints that are **verified by this test suite**. These are the known constraints confirmed by the reference test run (2026-08-20, 34/34 Part A-C PASSED via `run_e2e_tests.ps1`); the ShowMe column was confirmed via the ShowMe E2E suite (`test_showme_e2e.ps1`, TVS-C.8).

| Construct | Strategy | Indicator | Function | TradingApp | ShowMe |
|-----------|----------|-----------|----------|------------|--------|
| **Plot statements** (Plot1, Plot2, etc.) | **NO** | **YES** | **NO** | **NO** | **YES** |
| **Buy/Sell order statements** | **YES** | **NO** | **NO** | **NO** | **NO** |
| **`using elsystem;`** | YES | YES | YES | YES | YES |
| **Function name assignment** (e.g., `MyFunc = 1;`) | N/A | N/A | **Required** | N/A | N/A |

### Error messages by violation

| Violation | Error message |
|-----------|--------------|
| Plot in Strategy | `This word not allowed in a Strategy.` |
| Buy/Sell in Indicator | `This word not allowed in a study.` |
| Plot in Function | `This word not allowed in a function.` |
| Buy/Sell in Function | `This word not allowed in a function.` |
| Buy/Sell in TradingApp | `This word not allowed in a study.` |
| Plot in TradingApp | `This word only allowed in an Indicator, ShowMe or PaintBar study.` |
| Buy/Sell in ShowMe | `This word not allowed in a study.` |
| Function without return assignment | `A value was never assigned to user function.` (secondary error) |

> **NOTE:** This table documents the KNOWN constraints verified by this test suite. There may be other type-specific constraints in the EasyLanguage compiler that are not yet tested by this specification. If new constraints are discovered, they should be added as additional Part B test cases and documented here.

### Additional reference: `.ell` file naming by type

| Type | Warehouse type ID | `.ell` file pattern |
|------|-------------------|---------------------|
| Strategy | 0 | `elmodule_0_<ProgId>.ell` |
| Indicator | 2 | `elmodule_2_<ProgId>.ell` |
| Function | 4 | `elmodule_4_<ProgId>.ell` |
| TradingApp | 5122 | `elmodule_5122_<ProgId>.ell` |
| ShowMe | 1282 | `elmodule_1282_<ProgId>.ell` |

---

## Part F: Serve Mode E2E (Workstream K — TVS-K.6b)

**Status:** DONE — implemented and passing as regression **Suite 11**: `tests/run_serve_e2e.ps1` drives `tests/test_serve_e2e.py` (2 pytest cases). TVS-K.6a verified the end-to-end path (commit 027e7fe); TVS-K.6b documented this procedure and flipped the `use_serve` default to `True`.

### F.1: Prerequisites

- A built `tsdev_cli.exe` in `build\Release\` (the standard build output).
- TradeStation 9.5 installed (the broker loads the real `orlib20.dll`).
- No TradeStation processes or services running.
- An **elevated** PowerShell session to launch the runner (see F.2 — the
  runner itself launches pytest unelevated via Task Scheduler so the test
  exercises the real non-elevated → broker auto-start path).

### F.2: Procedure — run `tests/run_serve_e2e.ps1`

The runner script handles the elevation mechanics automatically. From an
**elevated** PowerShell session at the project root:

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_serve_e2e.ps1
```

What the script does:

1. Saves the current `ConsentPromptBehaviorAdmin` registry value.
2. Sets `ConsentPromptBehaviorAdmin=0` (auto-elevate without a UAC prompt)
   so the broker auto-start inside the test is silent.
3. Launches `uv run pytest tests/test_serve_e2e.py` **unelevated** via a
   Task Scheduler task with `-RunLevel Limited` — this is the key step:
   the pytest process runs as a non-elevated (Medium-IL) process, exactly
   like a real non-elevated Python session.
4. Captures stdout+stderr to a temp file and waits for the task to finish.
5. Restores `ConsentPromptBehaviorAdmin` to its original value in a
   `finally` block (always, even on error).
6. Prints the pytest output and returns the pytest exit code.

> **Note:** the temporary `ConsentPromptBehaviorAdmin=0` setting means any
> elevation request during the test session auto-approves silently. This is
> restored in the `finally` block even on error. The script also cleans up
> the scheduled task and temp files in the same block.

### F.3: What it verifies

The pytest module `tests/test_serve_e2e.py` contains 2 test cases:

**`test_serve_full_lifecycle`** — verifies the full serve-mode lifecycle from
a non-elevated Python process:

1. A non-elevated Python process constructs `TsDevClient()` with the default
   `use_serve=True`.
2. The first operation auto-starts the elevated broker via **one**
   `ShellExecuteExW` `"runas"` call (one UAC prompt — silenced by the
   `ConsentPromptBehaviorAdmin=0` setting).
3. All subsequent operations reuse the **same** broker process over the
   named pipe (the test asserts the broker is NOT restarted between
   operations).
4. The full lifecycle is exercised against the real TSDev Library:
   `insert` → `exists` → `get-source` → `modify` → `get-source` → `delete`
   → `exists` (confirming deletion).
5. Each step's result is asserted to match the expected `TsDevResult`
   fields (success, ProgId, source text, etc.).

**`test_serve_compile_failure_recovery`** — verifies broker resilience:

1. The broker (started by the lifecycle test or reused) receives an
   `insert` request whose EasyLanguage source fails to compile.
2. The broker **survives** the compile error (returns a `CompileError`
   result, does not crash) and the partial warehouse entry is
   auto-cleaned.
3. The broker continues serving subsequent requests normally.

### F.4: Expected result

```
2 passed, 0 failed
```

The two test cases `test_serve_full_lifecycle` and
`test_serve_compile_failure_recovery` must both pass. The runner script
parses the pytest output for `(\d+) passed` / `(\d+) failed` and returns
exit `0` on success, `1` on failure.

### F.5: Backwards-compat verification (part of TVS-K.6a)

As part of TVS-K.6a, the full existing E2E suite (Parts A–E above) was
re-run **without** `use_serve` (elevated session, direct `subprocess.run`)
and confirmed byte-identical JSON output vs. the pre-serve baseline. This
verifies that serve mode is purely additive and does not change the
behavior of the direct-subprocess path when the session is already
elevated.

---

## Part G: Crash Handling E2E (VEH + TsDevCrashError)

**Status:** C++ VEH crash handler and Python TsDevCrashError are COMPLETE. Tests: test_crash_handler.cpp (normal mode + --crash mode), test_crash_handler.ps1. Review APPROVED.

### G.1: C++ VEH crash handler test — tests/test_crash_handler.cpp (COMPLETE)

- **Normal mode:** Run test_crash_handler.exe without `--crash` flag. Asserts the VEH is installed and the process exits normally (exit 0).
- **Crash mode:** Run test_crash_handler.exe with `--crash` flag. The test deliberately triggers an access violation. Asserts the VEH catches it and calls `TerminateProcess` with the correct NT status code (0xC0000005). No TSCrashReport.exe dialog should appear.

### G.2: Python TsDevCrashError test (COMPLETE)

- Python tests verify that `TsDevCrashError` is raised when crash NT status codes are detected in subprocess return codes.
- `_is_crash_exit()` helper handles both signed and unsigned returncode representations.
- Crash detection is tested in `_run_cli()`, `_run_batch()`, and `_ServeBroker.call()` code paths (mocked).

### G.3: E2E crash verification (recommended when running Suite 11)

When running serve mode E2E (Part F / regression Suite 11), verify that if the broker process crashes (e.g., due to an orlib20 access violation), the Python client receives a `TsDevCrashError` with the correct `exit_code` attribute, and no TSCrashReport.exe dialog appears (the VEH prevents it). This is a recommended additional check whenever the serve E2E suite is run.

---

## Appendix: Quick-Start Test Script Outline

The following is a high-level outline for a PowerShell script that implements this test specification. It is not a complete script — it shows the structure and key commands.

```powershell
# --- Setup ---
$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$projectRoot = $PSScriptRoot
$cli = "$projectRoot\build\Release\tsdev_cli.exe"
$tmpDir = "$projectRoot\tmp\e2e_run_$runId"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

# Helper: write ASCII source file
function Write-Src($path, $content) {
    [System.IO.File]::WriteAllText($path, $content, [System.Text.Encoding]::ASCII)
}

# Helper: run CLI and capture result
function Run-Cli($args) {
    & $cli @args 2>&1 | ForEach-Object { Write-Output $_ }
    $code = $LASTEXITCODE
    return @{ exitCode = $code; output = $args }
}

# --- Part A: Success cases (repeat for each type) ---
#   1. Write insert source to file
#   2. Run insert, verify exit 0, capture ProgId
#   3. Write modify source to file
#   4. Run modify, verify exit 0, verify ProgId preserved
#   5. Run delete, verify exit 0
#   6. Run delete again, verify exit 5

# --- Part B: Semantic errors (per test case table) ---
#   1. Write construct source to file
#   2. Run insert, verify exit code (8 for negative, 0 for control)
#   3. Verify error text for negative cases
#   4. Cleanup: delete (expect 0 or 5)

# --- Part C: General errors (per test case table) ---
#   C1: invalid syntax -> exit 8
#   C2.x: modify nonexistent -> exit 5 (all 5 types)
#   C3.x: delete nonexistent -> exit 5 (all 5 types)
#   C4: unknown type -> exit 2

# --- Cleanup ---
#   Re-delete all test names (all should return 5)
#   Verify no orphaned .ell files
#   Remove temp directory

# --- Report ---
#   Write results to tmp/re_artifacts/e2e_all_types_$runId.md
```
