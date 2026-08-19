# E2E CLI Test Specification — All Component Types

**Status:** Production-grade test specification
**Applies to:** `tsdev_cli.exe` (the production CLI for the TSDev Library)
**Last verified:** 2025-01-22 (33/33 tests PASSED)

---

## 1. Purpose & Scope

This document is the **definitive end-to-end (E2E) CLI test specification** for the `tsdev_cli` production CLI. It defines a comprehensive, repeatable test procedure that any agent or subagent can follow to verify the CLI is fully operational.

### When to use this specification

- **After every TVS** that touches the CLI, `tsdev_lib`, or component type handling — as part of the mandatory E2E CLI verification described in `AGENTS.md`.
- **As a regression gate** whenever the orlib20 integration, compiler error handling, or warehouse persistence logic changes.
- **On demand** when a full health check of all 4 supported component types is needed.

### What it covers

- All 4 supported component types: **Strategy, Indicator, Function, TradingApp**
- **Success cases:** insert → modify → delete lifecycle for each type
- **Type-specific semantic error cases:** constructs that are valid in some types but rejected by the compiler in others (e.g., Plot in a Strategy, Buy in an Indicator)
- **General error cases:** invalid syntax, modify/delete nonexistent, unknown type strings
- **Cleanup verification:** no orphaned `.ell`/`.xml` files or warehouse entries left behind

### What it does NOT cover

- Unit tests or library-level tests (those have their own test suites)
- The 4 unsupported types (ActivityBar, PaintBar, ProbabilityMap, ShowMe) — these are not yet wired into the CLI
- Performance or stress testing

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

The default build directory for this project is `C:\Users\user\projects\tsdev_automation\build`.

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
$tmpDir = "C:\Users\user\projects\tsdev_automation\tmp\e2e_run_$runId"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
```

---

## 3. CLI Argument Format

Confirmed from `src/tsdev_cli.cpp`:

```
tsdev_cli [--ts-path <directory>] insert  <type> <name> <source_file>
tsdev_cli [--ts-path <directory>] modify  <type> <name> <source_file>
tsdev_cli [--ts-path <directory>] delete  <type> <name>
```

### Arguments

| Argument | Description |
|----------|-------------|
| `--ts-path <directory>` | Optional. Overrides the TradeStation install directory (default: auto-discovered from registry). Can appear as `--ts-path <dir>` or `--ts-path=<dir>`. If specified multiple times, the last occurrence wins. |
| `insert` / `modify` / `delete` | Subcommand (case-insensitive). |
| `<type>` | Component type string: `strategy`, `indicator`, `function`, or `tradingapp` (case-insensitive). |
| `<name>` | Component name. Must not be empty. |
| `<source_file>` | Path to a file containing EasyLanguage source. Required for `insert` and `modify`. Not used for `delete`. The file is read in binary mode; must be non-empty. |

### Valid type strings

| Type string | ComponentType enum | Warehouse type ID | `.ell` file pattern |
|-------------|-------------------|-------------------|---------------------|
| `strategy` | Strategy | 0 | `elmodule_0_<ProgId>.ell` |
| `indicator` | Indicator | 2 | `elmodule_2_<ProgId>.ell` |
| `function` | Function | 4 | `elmodule_4_<ProgId>.ell` |
| `tradingapp` | TradingApp | 5122 | `elmodule_5122_<ProgId>.ell` |

### Exit codes

| Exit code | Meaning | When produced |
|-----------|---------|---------------|
| **0** | Success | insert/modify/delete completed successfully |
| **1** | Usage error (no subcommand or wrong arg count) | No subcommand given, or wrong number of positional args |
| **2** | Unknown type string | `<type>` is not one of the 4 valid strings |
| **3** | Source file error | File not found, unreadable, or empty |
| **4** | Init failure | TsDevLib construction failed (DLL load, patch, or OrlibInit) |
| **5** | Not found | Component does not exist in the warehouse (modify/delete), or already exists (insert) |
| **6** | Entry operation failure | createEntry or deleteEntry returned failure |
| **7** | saveSource failure | Source could not be saved to the entry |
| **8** | Compile error | Compilation/verification failed; error text printed to stderr |
| **9** | .ell file missing | Compilation succeeded but the `.ell` file was not found on disk |
| **10** | Thread creation failure | CreateThread failed (system-level error) |

The exit codes most relevant to this test specification are **0** (success), **5** (not found), **8** (compile error), and **2** (unknown type).

---

## 4. Test Procedure — Part A: Success Cases (insert → modify → delete cycle)

For each of the 4 component types, run the full insert → modify → delete lifecycle. Each step must produce the expected exit code.

### 4.1 Test source table

| Type | Insert source | Modify source | Notes |
|------|--------------|---------------|-------|
| **Strategy** | `Inputs: int x(0);` + newline + `if x > 0 then Buy next bar at market;` | `Inputs: int x(1);` + newline + `if x > 0 then Sell next bar at market;` | Order statements (Buy/Sell) are allowed in Strategies |
| **Indicator** | `Inputs: int x(0);` + newline + `Plot1(x, "MyPlot");` | `Inputs: int x(1);` + newline + `Plot1(x * 2, "MyPlot2");` | Plot statements are allowed in Indicators |
| **Function** | `<FuncName> = 1;` | `<FuncName> = 42;` | **Function name in source MUST match the `<name>` argument** passed to the CLI. Replace `<FuncName>` with the actual test name (e.g., `E2E_20250122_Func = 1;`). No comment wrapper or `value =` syntax needed. |
| **TradingApp** | `using elsystem;` | `using elsystem;` + newline + `using elsystem.windows.forms;` | Minimal source `using elsystem;` compiles successfully |

> **Function note (critical):** EasyLanguage functions require the function body to assign a value to the function name. The name used in the source assignment MUST exactly match the `<name>` argument passed to the CLI. For example, if the CLI command is `tsdev_cli insert function E2E_20250122_Func source.txt`, then `source.txt` must contain `E2E_20250122_Func = 1;`. A mismatch will cause a secondary compiler error: `"A value was never assigned to user function."`

### 4.2 Procedure per type

For each type (Strategy, Indicator, Function, TradingApp), using the appropriate test name and source from the table above:

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

**Part A total: 16 test cases.**

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

**Part B total: 8 test cases.**

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

These tests verify that the CLI handles general error conditions correctly across all 4 types.

### 6.1 Test case table

| Test ID | Type | Test | Expected exit | Expected output |
|---------|------|------|---------------|-----------------|
| **C1** | strategy | Invalid EasyLanguage syntax | **8** | Compiler error with line/column info (e.g., `"This word cannot start a statement."` + `"Semicolon (;) expected here."`) |
| **C2.1** | strategy | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.2** | indicator | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.3** | function | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C2.4** | tradingapp | Modify nonexistent component | **5** | `"not found in the warehouse. Use insert to create it."` |
| **C3.1** | strategy | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.2** | indicator | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.3** | function | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C3.4** | tradingapp | Delete nonexistent component | **5** | `"not found in the warehouse."` |
| **C4** | (any) | Unknown type string (e.g., `paintbar`) | **2** | `"unknown type"` message |

**Part C total: 10 test cases.**

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

**C2.x — Modify nonexistent (for each of the 4 types):**
```powershell
# Use a name that definitely does not exist
& $cli modify $type "E2E_${runId}_NonExistent" "$tmpDir\any_valid_source.txt"
# Expected: exit 5, stderr contains "not found in the warehouse. Use insert to create it."
```

**C3.x — Delete nonexistent (for each of the 4 types):**
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
    "E2E_${runId}_C1"
)
foreach ($name in $allTestNames) {
    foreach ($type in @("strategy","indicator","function","tradingapp")) {
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

## Part D: New Operations & Inline Batch (PENDING SPECIFICATION)

E2E test cases for the following new subcommands will be specified when implementation begins (see AGENTS.md `### NEXT WORKSTREAMS` item 2, Workstream F). They are NOT yet defined here:

- `exists` — query item existence by name + type (case-sensitive and case-insensitive variants); expected: verbatim stored name, exists flag, ProgId, exit code.
- `get-source` — retrieve EL source to stdout and via `--output <file_path>`; expected: source text matches a known-inserted item; error case for nonexistent item; error case for password-protected item.
- `upsert` — insert-when-absent and modify-when-present variants for all 4 component types; expected: correct branch taken (verify via `exists` + `get-source`).
- Inline batch via stdin (`--batch-stdin`) — pipe manifest content via stdin for batch-insert/modify/delete; expected: identical results to the manifest-file path; verify backward compatibility of the manifest-file path still works.

Test cases must follow the existing Part A/B/C structure (test ID, type, name, expected exit code, procedure, cleanup).

---

## 8. Pass/Fail Criteria

The overall test run verdict is **PASS** only if ALL of the following are true:

| Criterion | Requirement |
|-----------|-------------|
| **Part A — Success cases** | All 16 test cases pass (exit codes match expected for every insert/modify/delete/delete-neg step) |
| **Part A — ProgId preservation** | ProgId is preserved across modify for all 4 types |
| **Part A — `.ell` files** | `.ell` files are created on insert and removed on delete for all 4 types |
| **Part B — Semantic errors** | All 8 test cases pass: negative tests return exit 8 with correct error text, positive controls return exit 0 |
| **Part C — General errors** | All 10 test cases pass (exit codes match expected) |
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

The following table documents the EasyLanguage construct constraints that are **verified by this test suite**. These are the known constraints confirmed by the reference test run (2025-01-22, 33/33 PASSED).

| Construct | Strategy | Indicator | Function | TradingApp |
|-----------|----------|-----------|----------|------------|
| **Plot statements** (Plot1, Plot2, etc.) | **NO** | **YES** | **NO** | **NO** |
| **Buy/Sell order statements** | **YES** | **NO** | **NO** | **NO** |
| **`using elsystem;`** | YES | YES | YES | YES |
| **Function name assignment** (e.g., `MyFunc = 1;`) | N/A | N/A | **Required** | N/A |

### Error messages by violation

| Violation | Error message |
|-----------|--------------|
| Plot in Strategy | `This word not allowed in a Strategy.` |
| Buy/Sell in Indicator | `This word not allowed in a study.` |
| Plot in Function | `This word not allowed in a function.` |
| Buy/Sell in Function | `This word not allowed in a function.` |
| Buy/Sell in TradingApp | `This word not allowed in a study.` |
| Plot in TradingApp | `This word only allowed in an Indicator, ShowMe or PaintBar study.` |
| Function without return assignment | `A value was never assigned to user function.` (secondary error) |

> **NOTE:** This table documents the KNOWN constraints verified by this test suite. There may be other type-specific constraints in the EasyLanguage compiler that are not yet tested by this specification. If new constraints are discovered, they should be added as additional Part B test cases and documented here.

### Additional reference: `.ell` file naming by type

| Type | Warehouse type ID | `.ell` file pattern |
|------|-------------------|---------------------|
| Strategy | 0 | `elmodule_0_<ProgId>.ell` |
| Indicator | 2 | `elmodule_2_<ProgId>.ell` |
| Function | 4 | `elmodule_4_<ProgId>.ell` |
| TradingApp | 5122 | `elmodule_5122_<ProgId>.ell` |

---

## Appendix: Quick-Start Test Script Outline

The following is a high-level outline for a PowerShell script that implements this test specification. It is not a complete script — it shows the structure and key commands.

```powershell
# --- Setup ---
$runId = Get-Date -Format "yyyyMMdd_HHmmss"
$projectRoot = "C:\Users\user\projects\tsdev_automation"
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
#   C2.x: modify nonexistent -> exit 5 (all 4 types)
#   C3.x: delete nonexistent -> exit 5 (all 4 types)
#   C4: unknown type -> exit 2

# --- Cleanup ---
#   Re-delete all test names (all should return 5)
#   Verify no orphaned .ell files
#   Remove temp directory

# --- Report ---
#   Write results to tmp/re_artifacts/e2e_all_types_$runId.md
```
