# E2E CLI Test Specification — All Component Types

**Status:** Production-grade test specification
**Applies to:** `tsdev_cli.exe` (the production CLI for the TSDev Library)

> **Current status:** The authoritative Workstream L/M status (L closure,
> M.1–M.5 state, M.4 blocker) is in `AGENTS.md` §"Current state and TODO".
>
> The historical L.1–L.17 claims and fault records in this
> specification are retained for provenance only.

**Historical verification snapshot (2026-08-23; not current acceptance
evidence):** The retained record reports core runner `tests/run_e2e_tests.ps1`
PASSED 34/34 Part A-C cases, plus Workstream F tests and Workstream I directory
batch tests. ShowMe is covered by the separate suite
`tests/test_showme_e2e.ps1` (regression Suite 12, cases SM1-SM9). Serve mode E2E
has a retained historical/transient K.6a result of 2/2 pytest cases in
regression Suite 11 (`tests/run_serve_e2e.ps1` driving
`tests/test_serve_e2e.py`); this result is not current inventory or acceptance
evidence. The current serve inventory is three default tests plus one opt-in
stress test (`TS_SERVE_STRESS=1`). TVS-L.5 added Suite 13 (249/249 assertions)
and Suite 14 (600/600 stress cycles) and recorded the Desktop runtime gate
(Part H.10). These figures and the historical Desktop observation are retained
for provenance only; they are not current acceptance evidence.

**Evidence convention:** The expected-result tables and expected-output comments below define what a future run must verify; they are not recorded-run evidence. Any retained run result is labeled historical/transient and is not current acceptance evidence.

---

## 0. Workstream L Status — Desktop Runtime Gate (historical record)

> **Current status:** The authoritative Workstream L/M status (L closure,
> M.1–M.5 state, M.4 blocker) is in `AGENTS.md` §"Current state and TODO".
>
> The Desktop gate and fault-path records in this section are
> historical provenance, not a current production-acceptance claim.

**This E2E suite verifies CLI/library-level behavior** (exit codes,
ProgId preservation, `.ell` file creation/removal, compiler error text,
warehouse cleanup). It also documents the Desktop runtime-execution gate in
Part H.10, which was executed and passed for all 5 supported component types
as a historical observation.

The following Workstream L chronology is retained as historical provenance.
TVS-L.1–L.3 were recorded as COMPLETE (RED harness, STA COM lifecycle guard,
persistence-bypass removal). The TVS-L.4/L.5 completion claim was later
falsified by independent persistence and deletion fault injection and recorded
as superseded by the L.6–L.15 recovery. The historical Desktop runtime gate in
Part H.10 observed no "Unknown link error occurred" for CLI-inserted items
across all 5 supported component types before and after Desktop restart; that
observation remains below and in Part H.10. It did not exercise the stale-ELL
modify (Fault A) or orphan-ELL delete (Fault B) paths. These historical facts
do not change the current Workstream L closure or the current Workstream M
status stated at the top of this section.

The Desktop runtime-execution gate is a procedure on the
`+TP:GC_1M_010115_081426_CLEAN*` chart; the procedure and historical results
are documented in Part H.10. The CLI/library tests in this specification
(Parts A–G) confirm correct CLI/library behavior and are a necessary
precondition for the Desktop gate; they are not, by themselves, a sufficient
runtime acceptance verdict, and the Desktop gate in H.10 is likewise not
sufficient on its own — it did not exercise the persistence/delete fault
paths.

### 0.1 Implemented tests vs. the Desktop runtime-execution gate

The suites listed below are implemented. Their recorded PASS results and the
Desktop runtime-execution result are historical/transient records, not current
acceptance evidence; see Part H.10.

- `tests/run_e2e_tests.ps1` (Parts A–C, 34 cases) — exit codes, ProgId
  preservation, `.ell` create/remove, compiler error text, cleanup.
- `tests/test_showme_e2e.ps1` (Suite 12, SM1–SM9) — ShowMe lifecycle.
- `tests/run_serve_e2e.ps1` / `tests/test_serve_e2e.py` (Suite 11) — three
  default serve tests covering broker lifecycle, compile-failure recovery, and
  batch-over-serve; plus one opt-in long-lived stress test gated by
  `TS_SERVE_STRESS=1`.
- Workstream F / I suites (exists, get-source, upsert, batch, directory
  batch) — CLI/library behavior.
- `tests/test_unknown_link_regression.ps1` (Suite 13) — artifact invariants,
  persistence/failure-path checks, and semantic errors across all 5 component types
  (249/249 assertions: 40 L1 + 195 L4 + 14 L5.SEM; Part H).
- `tests/test_unknown_link_stress.ps1` (Suite 14, opt-in via
  `-ExtendedStress` in `tests/run_regression.ps1`) — 600 insert/delete cycles
  (600/600 PASSED; 120 cycles × 5 types).

**The Desktop runtime-execution test was executed and passed (historical,
TVS-L.5).** It compiles components via the CLI, loads them in TradeStation
Desktop on the `+TP:GC_1M_010115_081426_CLEAN*` chart, and asserts a
successful runtime link. This historical result is retained as evidence of
runtime linkability for freshly inserted items; it is **not** a sufficient
final gate for Workstream L completion or full runtime acceptance, because it
did not exercise the persistence (stale-ELL modify) or delete (orphan-ELL
delete) fault paths whose `exit 0` / `success=true` results falsified the
L.4/L.5 completion claim (see the historical L.4/L.5 record in
`docs/workstream_unknown_link_fix_plan.md`). The procedure and results are
documented in Part H.10.

### 0.2 Workstream L chronology (historical record)

**Workstream L** addressed the unknown-link runtime defect and added the
Desktop runtime-execution regression gate. The L.1–L.3 completion records,
the later falsification of the L.4/L.5 completion claim by persistence and
delete fault injection, and the subsequent recovery records are retained as
historical chronology. The historical Desktop gate result (Part H.10) is also
retained as a factual observation; it did not prove the persistence/delete
fault paths. Current Workstream L/M status is in `AGENTS.md` §"Current state
and TODO".

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
- The 3 unsupported types — these are not yet wired into the CLI (see `docs/TS_COMPONENT_TYPES.md` for the unconfirmed type list)
- Performance or stress testing
- **Serve mode E2E** — implemented; a historical/transient Suite 11 run was recorded as passing, not as current acceptance evidence. The detailed operator procedure is documented in Part F below (TVS-K.6b).
- **Crash handling E2E** — implemented; the retained test procedure and results are historical/transient, not current acceptance evidence (see Part G below).
- **Desktop runtime-execution / runtime-linkability** — executed as the
  Desktop runtime gate in Part H.10 (TVS-L.5), not by the standard automated
  CLI runners in Parts A–G. The gate passed for all 5 supported component types
  on the `+TP:GC_1M_010115_081426_CLEAN*` chart (historical result, retained).
  That gate did not exercise the persistence (stale-ELL modify) or delete
  (orphan-ELL delete) fault paths and does not, by itself, prove Workstream L
  completion or full runtime acceptance at that time. This historical observation
  does not establish current Workstream L/M status; see `AGENTS.md` §"Current
  state and TODO".

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

### 2.3 TradeStation Desktop/TSDev running state

There is no general requirement for TradeStation Desktop or TSDev.exe to be running or closed for the CLI/library tests. Desktop state is relevant only when an individual test explicitly exercises it; Part H.10 is the chart/runtime procedure and specifies its own live Desktop steps.

### 2.4 Direct `tsdev_cli` elevation

Every procedure in this specification that invokes a `tsdev_cli` subcommand directly (Parts A–E and their cleanup commands) must run from an elevated administrator process. The full elevation contract (direct-CLI exit 11 / `not_elevated`, `use_serve` broker, `ElevationRequiredError`, `ShellExecuteExW`/`runas`) is in `AGENTS.md` (rule) and `docs/BUILD.md` (operational). See Part F for the serve-mode procedure.

### 2.5 PowerShell environment

This specification uses PowerShell syntax:
- Use `;` to chain commands (NOT `&&`)
- Use `&` to invoke executables (e.g., `& $cli insert strategy $name $srcFile`)
- Use `$LASTEXITCODE` to read the exit code of the last native command

### 2.6 Unique test names

To avoid collisions with real components or prior test runs, prefix all test component names with `E2E_` plus a run identifier (timestamp or run ID). Example:

```
$runId = "20250122_153000"
$strategyName = "E2E_${runId}_Strat"
$indicatorName = "E2E_${runId}_Ind"
$functionName  = "E2E_${runId}_Func"
$tradingappName = "E2E_${runId}_TA"
```

### 2.7 Source files — ASCII encoding

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

All `tsdev_cli` commands shown in the procedures below are direct CLI
invocations and must be run from an elevated administrator process. The
elevation contract (exit 11 / `not_elevated`, `ERROR_ELEVATION_REQUIRED` /
740, `asInvoker` vs `requireAdministrator`) is in `AGENTS.md` (rule) and
`docs/BUILD.md` (operational); see §2.4.

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

The type→`whType` mapping SSOT is `docs/TS_COMPONENT_TYPES.md` (code SSOT:
`src/tsdev_lib.h` enum). The table below is the authoritative E2E-specific
view for this spec (type string + `.ell` file pattern columns).

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
| **9** | Missing/inconsistent persistence artifact (`artifact_missing`) | Compilation/verification may succeed, but a required `.ell`/`.xml` artifact is missing or post-verify integrity detects an inconsistent state, including Fault A's stale `.ell` |
| **10** | Thread creation failure | CreateThread failed (system-level error) |
| **11** | Not elevated (`not_elevated`) | The `asInvoker` runtime guard found no administrator token. JSON form: `{"success":false,"exit_code":11,"error":{"code":"not_elevated","message":"..."}}` |

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
| **Function** | `<FuncName> = 1;` | `<FuncName> = 42;` | **Function name in source MUST match the `<name>` argument** passed to the CLI for EasyLanguage compiler syntax; this is not a metadata lookup convention. Replace `<FuncName>` with the actual test name (e.g., `E2E_20250122_Func = 1;`). No comment wrapper or `value =` syntax needed; stored names/types/ProgIds remain warehouse/API-derived. |
| **TradingApp** | `using elsystem;` | `using elsystem;` + newline + `using elsystem.windows.forms;` | Minimal source `using elsystem;` compiles successfully |
| **ShowMe** | `Inputs: int x(0);` + newline + `Plot1(Close, "MyPlot");` | `Inputs: int x(1);` + newline + `Plot1(Close * 2, "MyPlot2");` | Plot statements are allowed in ShowMe (like Indicators) |

> **Function note (critical):** EasyLanguage functions require the function body to assign a value to the function name. The name used in the source assignment MUST exactly match the `<name>` argument passed to the CLI for compiler syntax; this is not a metadata lookup convention. For example, if the CLI command is `tsdev_cli insert function E2E_20250122_Func source.txt`, then `source.txt` must contain `E2E_20250122_Func = 1;`. A mismatch will cause a secondary compiler error: `"A value was never assigned to user function."` Stored names/types/ProgIds remain warehouse/API-derived.

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

The EasyLanguage compiler produces type-specific error messages. The retained reference run observed and verified the messages below; they are historical/transient observations, not current acceptance evidence:

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

Workstream F added 4 new operations and inline batch via stdin. All are implemented; the dedicated script results retained here are historical/transient, not current acceptance evidence.

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

Workstream I (TVS-I.2) added 4 directory batch subcommands. These enumerate regular files in a directory (default extensions `.el`+`.txt`, overridable via repeatable `-t <ext>` flag), use the file stem as the item name and the file content as the EasyLanguage source, and reuse the existing batch pipeline via a shared `runBatchAndEmit()` helper. The retained test records for `tests/test_directory_batch.ps1` (9 E2E cases DB1–DB9) and `tests/test_directory_batch.cpp` (6 non-TS pure parser/filesystem unit cases for `parseDirectory()` logic) are historical/transient, not current acceptance evidence.

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

**Part E total: 9 E2E test cases + 6 non-TS C++ parser unit test cases.**

### E.2: C++ non-TS parser unit test cases — tests/test_directory_batch.cpp (6 cases)

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

The overall test run verdict is **PASS** only if ALL of the following are true. These are acceptance requirements for a future run, not a claim that the retained historical/transient results represent a current PASS:

| Criterion | Requirement |
|-----------|-------------|
| **Part A — Success cases** | All 16 main-runner test cases pass (A1.1–A4.4; exit codes match expected for every insert/modify/delete/delete-neg step) |
| **Part A — ProgId preservation** | ProgId is preserved across modify for all 4 main-runner types (+ ShowMe via Suite 12, SM5) |
| **Part A — `.ell` files** | `.ell` files are created on insert and removed on delete for all 4 main-runner types (+ ShowMe via Suite 12) |
| **Part B — Semantic errors** | All 8 main-runner test cases pass (B1–B8): negative tests return exit 8 with correct error text, positive controls return exit 0 |
| **Part C — General errors** | All 10 main-runner test cases pass (C1, C2.1–C2.4, C3.1–C3.4, C4; exit codes match expected) |
| **ShowMe suite (Suite 12)** | All 9 `tests/test_showme_e2e.ps1` cases pass (SM1–SM9) |
| **Serve E2E (Suite 11)** | Three default tests plus one opt-in stress test (`TS_SERVE_STRESS=1`) are in the current inventory; a default run must pass the three default tests, and an opt-in run must also pass the stress test (driven by `tests/run_serve_e2e.ps1`) |
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

The following table documents the EasyLanguage construct constraints encoded by
this test suite. The supporting reference runs (2026-08-20, 34/34 Part A-C, and
TVS-C.8 ShowMe coverage) are historical/transient and not current acceptance
evidence.

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

The `.ell` file naming pattern (`elmodule_<whType>_<ProgId>.ell`) per type is
the authoritative 5-row table in §3 "Valid type strings" above. The
type→`whType` mapping SSOT is `docs/TS_COMPONENT_TYPES.md` (code SSOT:
`src/tsdev_lib.h` enum).

---

## Part F: Serve Mode E2E (Workstream K — TVS-K.6b)

**Status:** Implemented. A retained historical/transient run was recorded as passing for regression **Suite 11**: `tests/run_serve_e2e.ps1` drives `tests/test_serve_e2e.py`. The retained K.6a record was a historical/transient 2/2 result; it is not current acceptance evidence or a current test inventory. The current inventory is three default tests plus one opt-in stress test gated by `TS_SERVE_STRESS=1`. TVS-K.6a recorded the end-to-end path (commit 027e7fe); TVS-K.6b documented this procedure and flipped the `use_serve` default to `True`.

### F.1: Prerequisites

- A built `tsdev_cli.exe` in `build\Release\` (the standard build output).
- TradeStation 9.5 installed (the broker loads the real `orlib20.dll`).
- No general TradeStation Desktop/TSDev running-state prerequisite applies; this
  broker test uses the installed TradeStation library rather than the Desktop UI.
- A direct `tsdev_cli serve` invocation requires an elevated administrator
  process; the elevation contract (exit 11 / `not_elevated`,
  `ERROR_ELEVATION_REQUIRED` / 740, `use_serve` broker via
  `ShellExecuteExW`/`runas`) is in `AGENTS.md` (rule) and `docs/BUILD.md`
  (operational). This procedure instead exercises Python's separate
  `TsDevClient(use_serve=True)` broker path: the non-elevated Python client
  starts an elevated broker.
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

1. Launches `uv run pytest tests/test_serve_e2e.py` **unelevated** via a
   Task Scheduler task with `-RunLevel Limited` — this is the key step:
   the pytest process runs as a non-elevated (Medium-IL) process, exactly
   like a real non-elevated Python session.
2. Captures stdout+stderr to a run-specific temp file. The child records
   pytest's integer process exit code in a separate run-specific file from a
   `finally` path; output text is display-only and is never a success oracle.
   A missing or unreadable capture is an infrastructure failure, even when the
   child exit-code file looks valid.
3. Waits for confirmed Task Scheduler completion (a run is observed and the
   task reaches `Ready`), then requires the child exit-code file and
   `LastTaskResult` to be valid and mutually consistent before accepting any
   pytest result. A timeout, task launch failure, missing/invalid capture or
   exit-code file, or task-result mismatch fails closed; a validated nonzero
   pytest result is returned unchanged. The broker's `ShellExecuteExW`/`runas`
   request is handled through ordinary Windows UAC consent.
4. In a `finally` block, explicitly stops this run's task when it is still
   active. If the task remains `Running`/`Queued`, or its state cannot be
   queried, the runner reports failure and leaves the task and all run
   artifacts for manual recovery. It unregisters the task only after a
   confirmed terminal state and successful deletion of the temporary files;
   task-unregister and file-deletion failures are surfaced and prevent a
   successful verdict. Cleanup is attempted on timeout and error paths as well
   as normal completion.

Runner-only infrastructure statuses are distinct from pytest's exit codes:
`20` = missing/invalid child exit code, `21` = timeout, `22` = scheduled-task
launch failure, `23` = task completion/result failure, `24` = cleanup could not
be fully confirmed on an otherwise successful run, `25` = unexpected runner
failure, and `26` = missing/unreadable stdout/stderr capture. A validated
nonzero pytest result is returned unchanged.

### F.3: What it verifies

The retained K.6a run covered the two historical serve cases below. The
current `tests/test_serve_e2e.py` inventory is three default tests plus one
opt-in stress test gated by `TS_SERVE_STRESS=1`: `test_serve_full_lifecycle`,
`test_serve_compile_failure_recovery`, and `test_serve_batch_over_serve_l14b`
are the default tests; `test_serve_long_lived_stress_l15` is opt-in. The
retained `2 passed` result is historical/transient and is not a current test
inventory or acceptance result.

**`test_serve_full_lifecycle`** — verifies the full serve-mode lifecycle from
a non-elevated Python process:

1. A non-elevated Python process constructs `TsDevClient()` with the default
   `use_serve=True`.
2. The first operation auto-starts the elevated broker via **one**
   `ShellExecuteExW` `"runas"` call (one ordinary UAC consent prompt).
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

**`test_serve_batch_over_serve_l14b`** — current default test for Python batch
operations over the serve broker:

1. A non-elevated Python process routes `batch_insert`, `batch_modify`,
   `batch_upsert`, `exists`, `batch_delete`, and post-delete `exists` requests
   through one elevated broker.
2. The test asserts stable broker identity across the batch, per-item results
   matching CLI JSON semantics, ProgId preservation on modify paths, and
   successful production-path cleanup.

**`test_serve_long_lived_stress_l15`** — current opt-in stress test. It runs
only when `TS_SERVE_STRESS=1` is set (and is not part of the default suite),
drives tens-to-low-hundreds of operations through one elevated broker, verifies
broker-handle stability, injects a compile failure followed by recovery, and
checks that disposable names are cleaned up.

### F.4: Retained K.6a expected result (historical expectation, not current-run evidence)

The retained K.6a baseline recorded the following historical/transient 2/2
result for its two cases:

```
2 passed, 0 failed
```

The `2 passed, 0 failed` output recorded for the retained K.6a run is a
historical/transient result, not current inventory or acceptance evidence. For
that baseline, `test_serve_full_lifecycle` and
`test_serve_compile_failure_recovery` must both pass. The historical runner
implementation used for that retained run parsed the pytest output for
`(\d+) passed` / `(\d+) failed`; the current runner instead validates the
child's recorded exit code and completed task result, and never treats output
text as success evidence. The current module contains three default tests plus
one opt-in stress test gated by `TS_SERVE_STRESS=1`; their results are not
established by this retained count.

### F.5: Backwards-compat verification (part of TVS-K.6a)

The retained TVS-K.6a record says the full existing E2E suite (Parts A–E
above) was re-run **without** `use_serve` (elevated session, direct
`subprocess.run`) and recorded byte-identical JSON output vs. the pre-serve
baseline. This byte-identical result is historical/transient, not current
acceptance evidence. It records that serve mode was intended to be purely
additive and not change the behavior of the direct-subprocess path when the
session was already elevated.

---

## Part G: Crash Handling E2E (VEH + TsDevCrashError)

**Status:** C++ VEH crash handler and Python TsDevCrashError are implemented; the retained completion, test, and review records are historical/transient, not current acceptance evidence. Tests: test_crash_handler.cpp (normal mode + --crash mode), test_crash_handler.ps1. Review APPROVED.

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

## Part H: Unknown Link Validation Harness (TVS-L.1–L.5)

> **Current status:** The authoritative Workstream L/M status (L closure,
> M.1–M.5 state, M.4 blocker) is in `AGENTS.md` §"Current state and TODO".
>
> This part retains the L.1–L.17 harness, fault, and Desktop-gate chronology as
> historical provenance, not as a current production-acceptance claim.

**Historical status record:** TVS-L.1, TVS-L.2, and TVS-L.3 were recorded as
**COMPLETE** (RED harness, STA COM lifecycle guard, and removal of the
`FUN_10011230` persistence bypass). The harness is registered as **regression
Suite 13** in `tests/run_regression.ps1` and is mandatory (skipped only under
`-NoTs`). It covers **all 5 supported component types** with artifact
invariants, mutation/failure-path checks, and semantic-error cases. The opt-in
stress test is **regression Suite 14** (see [H.9](#h9-opt-in-extended-stress-test)).

**Historical fault-path record:** The TVS-L.4/L.5 completion claim was
**falsified by independent persistence and deletion fault injection** (Fault A:
stale-ELL modify; Fault B: orphan-ELL delete; both returned `exit 0` /
`success=true`) and was recorded as superseded by the L.6–L.15 recovery. The
retained historical harness/gate figures are 249 Suite 13 assertions
(40 L1 + 195 L4 + 14 L5.SEM) and 600/600 Suite 14 stress cycles. The Desktop
runtime gate in [H.10](#h10-desktop-runtime-acceptance-tvs-l5) was recorded as
passed for all 5 types as a historical observation, but did not exercise the
persistence/delete fault paths. These figures are unverified
historical/transient; they are not current acceptance evidence.

### H.1: Purpose and the XML-vs-runtime distinction

`tests/test_unknown_link_regression.ps1` is a focused desired-state E2E harness
that proves the production CLI completes the original finalization path and
produces consistent artifacts, while explicitly distinguishing two different
kinds of evidence. The L.1 baseline uses a Strategy smoke case; the full
per-type matrix is documented in [H.8](#h8-tvs-l5-expansion-to-all-5-component-types):

- **XML evidence (a completion marker):** presence of
  `elmodule_<whType>_<ProgId>.xml` with the expected
  `<symbols file="elmodule_<whType>_<ProgId>" version="{GUID}">` header proves
  the original finalization path (`SymWriter::Save`) ran. The XML is a debugger
  source-map artifact, not a runtime artifact.
- **Runtime linkability:** what TradeStation Desktop actually links at
  runtime. This harness does NOT assert runtime linkability and does NOT
  substitute a fake runtime proxy. The Desktop runtime-execution acceptance
  gate is documented in Part H.10; its retained TVS-L.5 result is
  historical/transient, not current acceptance evidence. The durable root-cause
  record is in `docs/RE/RE_UNKNOWN_LINK_ROOT_CAUSE.md`.

**Passing the XML/GUID assertions in this harness proves the original
finalization path ran; it does NOT prove Desktop linkability.** A PASS verdict
from this harness must be read as "the artifact-completion path ran" — not as
"the component is runtime-linkable in Desktop."

### H.2: How to run the harness

The harness runs against the production CLI and requires an elevated
(administrator) session. The elevation contract (exit 11 / `not_elevated`,
`ERROR_ELEVATION_REQUIRED` / 740, `asInvoker` vs `requireAdministrator`) is
in `AGENTS.md` (rule) and `docs/BUILD.md` (operational).

It is now part of the standard regression suite:

```powershell
# Run Suite 13 through the master regression runner:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1

# Or run the harness directly:
powershell -ExecutionPolicy Bypass -File tests\test_unknown_link_regression.ps1

# Optional overrides:
powershell -ExecutionPolicy Bypass -File tests\test_unknown_link_regression.ps1 `
    -CliPath "build\Release\tsdev_cli.exe" `
    -TsPath  "C:\Program Files (x86)\TradeStation 9.5" `
    -Verbose
```

Parameters:
- `-CliPath` — path to `tsdev_cli.exe` (default:
  `$PSScriptRoot\..\build\Release\tsdev_cli.exe`).
- `-TsPath` — TradeStation install directory, passed through to the CLI as
  `--ts-path <dir>` and used to derive the `MyWork\Bin` artifact directory.
- `-BinPath` — explicit override for the `MyWork\Bin` directory used for
  `.ell`/`.xml` artifact checks (use only if `-TsPath` is insufficient).
- `-Verbose` — print full CLI output for every step.
- `-IncludeRedFaultInjection` — opt in to the L7 read-only-ELL fault cases.
  Routine runs without this switch do not run those cases.
- `-UpdateTrackedEvidence` — explicitly write the L7 evidence summary to the
  tracked `docs/RE/RE_L7_RED_FAULT_INJECTION_EVIDENCE.md`; routine L7 runs
  write to gitignored `tmp\` instead.

Exit codes returned by the harness:
- `0` — all desired-state assertions passed (GREEN).
- `1` — one or more desired-state assertions failed.
- `2` — environment/prerequisite error (not elevated, CLI missing, etc.).

### H.3: Test flow and desired-state assertions

1. Generate a unique Strategy name `TS_AUTO_L1_RED_<timestamp>`.
2. Insert a minimal valid Strategy through the production CLI with `--json`;
   parse the `prog_id` from the JSON output.
3. Desired-state assertions after the CLI reports success (all expected to
   pass on post-L.3 code):
   - **L1.2a — ELL exists and is fresh:** `elmodule_0_<ProgId>.ell` exists in
     `MyWork\Bin` and its modification time is at or after the insert
     operation start.
   - **L1.2b — XML exists and is fresh:** `elmodule_0_<ProgId>.xml` exists in
     `MyWork\Bin` and is fresh; this proves `SymWriter::Save` ran.
   - **L1.2c — XML header parses:** the XML contains a
     `<symbols file="elmodule_0_<ProgId>" version="{GUID}">` opening tag and
     the GUID string is extracted.
   - **L1.2d — GUID byte match (complete ELL search):** the GUID string is
     converted to its 16 Windows mixed-endian layout bytes
     (Data1 little-endian DWORD, Data2 little-endian WORD, Data3
     little-endian WORD, Data4 big-endian as-is) and the COMPLETE ELL binary
     is searched for those 16 bytes. They must occur exactly once. The
     harness does NOT use a "last 16 bytes" assumption; the authoritative
     byte-layout analysis is `tmp/com_nopatch_guid_analysis.json`.
   - **L1.2e — Source round-trips:** `tsdev_cli get-source strategy <name>
     --json` returns source matching the inserted source unchanged.
4. Cleanup (MANDATORY, `try/finally`): delete the warehouse entry via
   `tsdev_cli delete strategy <name>`; verify via `exists` that the entry is
   gone; verify both `.ell` and `.xml` artifacts are absent afterward. Cleanup
   runs even after an assertion failure and its status is reported in the
   summary.

### H.4: Why this harness IS part of the always-green regression runner

`tests/run_regression.ps1` is the always-green master regression runner. The
TVS-L.1 harness was intentionally RED on pre-L.3 production code (the XML/GUID
assertions failed because the persistence bypass skipped `SymWriter::Save`).
That bypass was removed in TVS-L.3, the original finalization path was
restored, and the harness turned GREEN. It is now **regression Suite 13** in
`tests/run_regression.ps1` and runs by default; it is skipped only when
`-NoTs` is set.

### H.5: Historical RED result on pre-L.3 code

Before TVS-L.3, the harness was expected to:
- PASS L1.1 (insert succeeds, `prog_id` assigned).
- PASS L1.2a (ELL exists and is fresh; the bypass still wrote the ELL).
- **FAIL L1.2b** (XML absent; the bypass skipped `SymWriter::Save`).
- **FAIL L1.2c** (cannot parse the absent XML).
- **FAIL L1.2d** (no GUID to match against).
- PASS L1.2e (source round-trips; `getSource` reads the warehouse, not the
  XML).
- Exit nonzero (RED baseline).

The exact nonzero run against the pre-L.3 production binary is the recorded
RED baseline for Workstream L. If the harness unexpectedly passed on pre-L.3
code, the plan's decision gate required stopping and re-examining the causal
boundary before proceeding (see `docs/workstream_unknown_link_fix_plan.md`).

### H.6: L.3 turned this GREEN and registered it

The retained historical record says that after TVS-L.2 (STA COM) and TVS-L.3
(remove the bypass, restore the original finalization path), the production CLI
ran `SymWriter::Save`, the XML was written with the expected
`<symbols ... version="{GUID}">` header, and the GUID's 16 Windows mixed-endian
bytes occurred exactly once in the complete ELL. The recorded harness result
was exit 0 (GREEN), and it is registered in `tests/run_regression.ps1` as
**regression Suite 13**. This result is historical/transient, not current
acceptance evidence.

### H.7: L.4 persistence-validation and failure-path cases (TVS-L.4)

TVS-L.4 extended `tests/test_unknown_link_regression.ps1` with the L4.1–L4.5
persistence/failure-path scenarios (39 new assertions) on top of the L.1
artifact-completion baseline. These L4 scenarios are now run for every
component type as part of the full L.5 matrix documented in
[H.8](#h8-tvs-l5-expansion-to-all-5-component-types).
These cases validate persistence and partial-artifact cleanup behavior around
insert/modify/upsert, on top of the L1.* artifact-completion assertions. They
are not evidence of a general rollback, retry, or restore mechanism. They run
as part of the same harness and share its helper functions
(`Get-ProgIdFromJson`, `Get-ExistsFromJson`, `Get-SourceFromJson`,
`Extract-XmlGuid`, `Test-GuidBytesInEll`, `Invoke-CleanupForName`,
`Remove-PreExistingArtifacts`, `Get-ValidStrategySource`,
`Get-InvalidPlotStrategySource`).

| Test ID | Scenario | Key assertions |
|---------|----------|-----------------|
| **L4.1** (Strategy sub-IDs: `L4.1a`, `L4.1b`, `L4.1c`, `L4.1d`, `L4.1e`, `L4.1.CLEANUP`) | Failed insert (compile error) | No `.ell`, no `.xml`, no warehouse entry left behind (no orphan artifacts; TD-001 cleanup verified) |
| **L4.2** (Strategy sub-IDs: `L4.2a`, `L4.2b`, `L4.2c`, `L4.2d`, `L4.2e`, `L4.2f`, `L4.2g`, `L4.2h`, `L4.2i`, `L4.2.CLEANUP`) | Failed modify (compile error) | Old `.ell`/`.xml` intact (SHA-256 unchanged from pre-modify baseline); warehouse entry still exists (observed compile-failure outcome; no rollback claim) |
| **L4.3** (Strategy sub-IDs: `L4.3a`–`L4.3g`, `L4.3.CLEANUP`) | Successful modify | Both `.ell` and `.xml` exist and are fresh; XML GUID matches the GUID bytes embedded in the ELL; source round-trips via `get-source` |
| **L4.4** (Strategy sub-IDs: `L4.4a`–`L4.4f`, `L4.4.CLEANUP`) | Successful upsert-new | Both `.ell` and `.xml` exist; XML GUID matches the ELL-embedded GUID; source round-trips via `get-source` |
| **L4.5** (Strategy sub-IDs: `L4.5a`–`L4.5g`, `L4.5.CLEANUP`) | Successful upsert-existing | Both `.ell` and `.xml` exist and are fresh; XML GUID matches the ELL-embedded GUID; source round-trips via `get-source` |

**Exact Strategy sub-ID meanings (from `tests/test_unknown_link_regression.ps1`):**
- **L4.1** — `L4.1a` (pre-existence: entry absent before failed insert), `L4.1b` (insert FAILED with compile error, exit 8), `L4.1c` (no `.ell` after failed insert), `L4.1d` (no `.xml` after failed insert), `L4.1e` (warehouse entry absent after failed insert), `L4.1.CLEANUP` (cleanup removed entry + artifacts).
- **L4.2** — `L4.2a` (pre-existence: entry absent before L4.2 insert), `L4.2b` (insert valid strategy succeeded, exit 0, prog_id>0), `L4.2c` (pre-modify artifacts exist for hashing), `L4.2d` (modify FAILED with compile error, exit 8), `L4.2e` (`.ell` still exists after failed modify), `L4.2f` (`.xml` still exists after failed modify), `L4.2g` (`.ell` SHA-256 unchanged after failed modify), `L4.2h` (`.xml` SHA-256 unchanged after failed modify), `L4.2i` (warehouse entry still exists after failed modify), `L4.2.CLEANUP` (cleanup removed entry + artifacts).

**Why the L4.2 compile-error result holds:** For the tested compile-stage
failure, the compiler reports the error before finalization and its artifact
writes, so the pre-existing `.ell` and `.xml` remain unchanged. A distinct
post-compile finalization-failure path can also leave `.ell` unchanged when
`FUN_10011230` returns false and `MDVault::SaveModule` is skipped; on that path
`SymWriter::Save` may still rewrite `.xml`. These are stage-specific outcomes,
not a guarantee for every modify failure.

**Fault A distinction:** In the post-compile stale-write Fault A described by
`docs/RE/RE_L8_STALE_WRITE_FAULT_A.md`, finalization succeeds and
`MDVault::SaveModule` attempts the `.ell` write, but an OS-level write failure is
suppressed. The `.ell` can remain stale while XML and warehouse source change,
so existence checks alone do not catch it. The current post-verify
persistence-integrity validator detects and reports this inconsistent state
(after orlib reports success); it performs detect-and-fail only, with no
rollback, retry, or restore.

**Production-code hardening recorded as verified by these historical/transient cases (not current acceptance evidence):**
- `doInsert` now checks `.xml` existence after the existing `.ell` check
  (exit 9 / `artifact_missing` if either required artifact is absent).
- `doModify` now checks both `.ell` and `.xml` existence after `modifyEntry`
  success (exit 9 / `artifact_missing` if either required artifact is absent).
- The shared post-verify persistence-integrity check also reuses exit 9 /
  `artifact_missing` for detected inconsistencies, including Fault A's stale
  `.ell` state.
- The TD-001 cleanup path in `verifyModule` and the `deleteEntry`/`closeWarehouse`
  paths log failures to stderr instead of swallowing them in empty catches.

### H.8: TVS-L.5 expansion to all 5 component types

TVS-L.5 expanded the Unknown Link regression harness from the L.1/L.4
Strategy baseline to a full matrix across all 5 supported component types:
**Strategy, Indicator, Function, TradingApp, and ShowMe**. The retained
historical/transient inventory lists approximately **249** assertions; this
count is not current acceptance evidence.

#### H.8.1: L1 artifact invariants (per type)

For each component type, the harness asserts the same desired-state
artifact-completion properties established by L.1:

- **L1.1 — insert succeeds:** the CLI returns exit 0 and assigns a `prog_id`.
- **L1.2a — ELL exists and is fresh:** `elmodule_<whType>_<ProgId>.ell`
  exists in `MyWork\Bin` and its modification time is at or after the insert
  operation start.
- **L1.2b — XML exists and is fresh:** `elmodule_<whType>_<ProgId>.xml` exists
  in `MyWork\Bin` and is fresh, proving `SymWriter::Save` ran.
- **L1.2c — XML header parses:** the XML contains a
  `<symbols file="elmodule_<whType>_<ProgId>" version="{GUID}">` opening tag
  and the GUID string is extracted.
- **L1.2d — GUID byte match (complete ELL search):** the GUID string is
  converted to its 16 Windows mixed-endian layout bytes and the complete ELL
  binary is searched for those 16 bytes. They must occur exactly once.
- **L1.2e — Source round-trips:** `tsdev_cli get-source <type> <name> --json`
  returns source matching the inserted source unchanged.
- **L1.0_PREEXIST — pre-existing cleanup:** any prior test artifacts for the
  generated name are removed before the run.
- **L1.CLEANUP — mandatory cleanup:** `delete`, `exists`, and artifact
  absence checks run in a `try/finally` even after an assertion failure.

The test IDs use a per-type prefix for the four non-Strategy extensions:
**L1.IND**, **L1.FUNC**, **L1.TA**, **L1.SHOWME**. The original Strategy
baseline cases (from TVS-L.1) use unprefixed IDs (`L1.1`, `L1.0_PREEXIST`,
`L1.2a` ... `L1.2e`, `L1.CLEANUP`). The four non-Strategy extensions use the
prefixed convention with `.1`, `.0_PREEXIST`, `.2a`, `.2b`, `.2c`, `.2d`,
`.2e`, and `.CLEANUP` sub-cases (e.g., `L1.IND.1`, `L1.IND.2a`,
`L1.IND.CLEANUP`).

#### H.8.2: L4 mutation cases (per type)

For each of the 5 component types, the L4 scenarios from H.7 are executed:

| Test ID (per type) | Scenario | Key assertions |
|--------------------|----------|----------------|
| **L4.1** | Failed insert (compile error) | No `.ell`, no `.xml`, no warehouse entry left behind |
| **L4.2** | Failed modify (compile error) | Old `.ell`/`.xml` intact; warehouse entry still exists (observed compile-failure outcome; no rollback claim) |
| **L4.3** | Successful modify | `.ell` and `.xml` exist and fresh; XML GUID matches ELL bytes; source round-trips |
| **L4.4** | Successful upsert-new | `.ell` and `.xml` exist; XML GUID matches ELL bytes; source round-trips |
| **L4.5** | Successful upsert-existing | `.ell` and `.xml` exist and fresh; XML GUID matches ELL bytes; source round-trips |

The test IDs use a per-type prefix for the four non-Strategy extensions:
**L4.IND**, **L4.FUNC**, **L4.TA**, **L4.SHOWME**. The original Strategy
baseline cases (from TVS-L.4) use unprefixed IDs: `L4.1a`–`L4.1e` +
`L4.1.CLEANUP`, `L4.2a`–`L4.2i` + `L4.2.CLEANUP`, `L4.3a`–`L4.3g` +
`L4.3.CLEANUP`, `L4.4a`–`L4.4f` + `L4.4.CLEANUP`, `L4.5a`–`L4.5g` +
`L4.5.CLEANUP` (exact sub-IDs verified in `tests/test_unknown_link_regression.ps1`).
The four non-Strategy extensions use the same sub-letter scheme under the
prefixed convention, with `Run-L4ForType` passing `IdBase = "<prefix>1"` ..
`"<prefix>5"` to the shared `Run-L4-1` .. `Run-L4-5` helpers (e.g.,
`L4.IND.1a`, `L4.IND.1b`, `L4.IND.1c`, `L4.IND.1d`, `L4.IND.1e`,
`L4.IND.1.CLEANUP`, `L4.IND.2a`–`L4.IND.2i`, `L4.IND.2.CLEANUP`, …;
likewise for `L4.FUNC.*`, `L4.TA.*`, `L4.SHOWME.*`).

#### H.8.3: L5 type-specific semantic error cases

TVS-L.5 adds 7 semantic-error cases that exercise constructs which are valid in
one component type but rejected by the compiler in another. Each case is
expected to produce CLI exit code 8 and to leave no orphan `.ell`/`.xml` or
warehouse entry.

| Test ID | Type | Invalid source | Expected exit code | Expected error substring |
|---|---|---|---|---|
| **L5.SEM.STRAT_PLOT** | Strategy | `Plot` statement | 8 | `This word not allowed in a Strategy.` |
| **L5.SEM.IND_BUY** | Indicator | `Buy`/`Sell` order | 8 | `This word not allowed in a study.` |
| **L5.SEM.FUNC_PLOT** | Function | `Plot` statement | 8 | `This word not allowed in a function.` |
| **L5.SEM.FUNC_BUY** | Function | `Buy`/`Sell` order | 8 | `This word not allowed in a function.` |
| **L5.SEM.TA_BUY** | TradingApp | `Buy`/`Sell` order | 8 | `This word not allowed in a study.` |
| **L5.SEM.TA_PLOT** | TradingApp | `Plot` statement | 8 | `This word only allowed in an Indicator, ShowMe or PaintBar study.` |
| **L5.SEM.SHOWME_BUY** | ShowMe | `Buy`/`Sell` order | 8 | `This word not allowed in a study.` |

### H.9: Opt-in extended stress test

**File:** `tests/test_unknown_link_stress.ps1`

This is an opt-in, long-duration stress suite registered as **regression
Suite 14** in `tests/run_regression.ps1`. It runs only when the
`-ExtendedStress` switch is set; by default it is skipped without failing the
overall regression.

#### H.9.1: What it exercises

The stress suite performs **600 insert/delete cycles** (120 per component type
× 5 component types) against a live TradeStation warehouse. Each cycle:

1. Generates a unique name.
2. Inserts the component through `tsdev_cli`.
3. Verifies the `.ell` and `.xml` artifacts exist.
4. Deletes the component.
5. Verifies the `.ell` and `.xml` artifacts are gone.
6. Verifies no TradeStation crash-reporter processes (`TSCrashReport.exe` or
   similar) are running.

#### H.9.2: How to run it

```powershell
# As part of the full regression suite:
powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1 -ExtendedStress

# Directly:
powershell -ExecutionPolicy Bypass -File tests\test_unknown_link_stress.ps1
```

#### H.9.3: Required cadence

Run the stress suite **once at the end of every TVS development cycle** as a
long-horizon gate. It is not part of the default quick regression because it
takes significantly longer than the standard 14 suites.

#### H.9.4: Key assertions

- Insert succeeds (exit 0, `prog_id` assigned).
- `elmodule_<whType>_<ProgId>.ell` and `.xml` exist immediately after insert.
- Delete succeeds (exit 0).
- The `.ell` and `.xml` artifacts are absent immediately after delete.
- No TradeStation crash reporter processes are observed during or after the run.

### H.10: Desktop runtime acceptance (TVS-L.5 historical record)

> **Current status:** The authoritative Workstream L/M status (L closure,
> M.1–M.5 state, M.4 blocker) is in `AGENTS.md` §"Current state and TODO".
>
> The procedure and result below are retained as historical provenance.

The final unknown-link acceptance gate is a **Desktop runtime-execution test**:
compile a component with `tsdev_cli`, apply it to a specific TradeStation
Desktop chart, and assert that the component links and runs without an
"Unknown link error occurred" failure.

This procedure is performed on a live chart and is **not** a pure CLI/library
test. The CLI-inserted components are loaded through the standard TradeStation
Desktop Insert Study dialog onto the `+TP:GC_1M_010115_081426_CLEAN*` chart.
The Events Log is observed for each component; any "Unknown link error occurred"
entry is a failure. The retained TVS-L.5 record says the gate was executed and
passed; that result is historical/transient, not current acceptance evidence.

#### H.10.1: Procedure

1. Build or reuse the existing Win32 Release `tsdev_cli.exe`.
2. Use `tsdev_cli insert <type> <name> <source_file>` to insert a test
   component for each of the 5 supported types.
3. In TradeStation Desktop, open the `+TP:GC_1M_010115_081426_CLEAN*` chart.
4. For each inserted component, open the chart's Insert Study dialog, select the
   CLI-inserted item, and apply it to the chart.
5. Observe the Events Log for errors.
6. For Strategies, use `Inputs: int x(0);` and an `if x > 0 then Buy` condition
   so the strategy *calculates* but does not place live market orders.
7. Restart TradeStation Desktop and verify that all inserted items remain on the
   chart without error.

#### H.10.2: Automated vs. manual observation

- **Automated checks (via CLI, no Desktop interaction; retained historical/transient record, not current acceptance evidence):**
  - `tests/test_unknown_link_regression.ps1` (Suite 13): 249/249 assertions.
  - `tests/test_unknown_link_stress.ps1` (Suite 14): 600/600 insert/delete
    cycles.

- **Manual checks (on the `+TP:GC_1M_010115_081426_CLEAN*` chart only; retained historical observations, not current acceptance evidence):**
  - **Strategy (`x=0`, no orders):** No Events Log error, no trades generated —
    PASS. The gate observed successful strategy *calculation* (`if x > 0 then
    Buy` condition evaluated), not order placement.
  - **Function caller strategy (calls `TS_AUTO_L5_RT_FUNC`, no orders):** No
    Events Log error — PASS. Function linked and executed.
  - **Indicator (`Plot1(x, "MyPlot")`):** No Events Log error, plots a
    horizontal straight line — PASS.
  - **ShowMe (`Plot1(High, "Up bar")` on `Close > Open`):** No Events Log
    error, plots high markers on up-bars — PASS.
  - **TradingApp (using `elsystem`):** No Events Log error (black window, no UI
    — expected for minimal fixture) — PASS.
  - **After Desktop restart:** All items still on chart, no errors — PASS.

#### H.10.3: Key finding (historical)

No "Unknown link error occurred" was observed for any CLI-inserted item, before
or after Desktop restart. This historical observation is retained. It is
**not** a sufficient defect-closure statement: the gate did not exercise the
persistence (stale-ELL modify) or delete (orphan-ELL delete) fault paths whose
`exit 0` / `success=true` results later falsified the L.4/L.5 completion claim.
This historical observation is not a current acceptance result and does not
establish current Workstream L/M status; see `AGENTS.md` §"Current state and
TODO".

---

## Appendix: Quick-Start Test Script Outline

The following is a high-level outline for a PowerShell script that implements this test specification. It is not a complete script — it shows the structure and key commands.

```powershell
# Run this direct-tsdev_cli outline from an elevated administrator process.
# Elevation contract: see AGENTS.md (rule) and docs/BUILD.md (operational).
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
