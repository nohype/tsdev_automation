# Workstream I — Directory Batch Operations: TVS Decomposition Plan

**Status:** Planning document (read-only investigation; no code modified)
**Date:** 2026-08-20
**Author:** PM (planning synthesis from 3 parallel research subagent reports)
**Purpose:** Atomic TVS decomposition for Workstream I (Directory Batch Operations), ready for execution.

---

## 1. Project State Summary

### 1.1 Current Batch Architecture (C++ CLI)

The C++ CLI (`src/tsdev_cli.cpp`) supports **3 batch subcommands**: `batch-insert`, `batch-modify`, `batch-delete`. There is **no `batch-upsert`**.

**Shared batch pipeline (all 3 ops):**
1. `parseManifest()` (file mode) or stdin reading → `parseManifestLines()` → `std::vector<BatchItem>`
2. `cmdBatch()` sets up `BatchParams`, launches `runBatch()` on a 64 MB-stack worker thread
3. `runBatch()` loops over `BatchItem`s, calling `doInsert()`/`doModify()`/`doDelete()` per item (with `flushPerItem=false`; single `closeWarehouse()` at end)
4. `emitBatchOutput()` emits a JSON array on stdout

**Key structures:**
- `BatchItem` struct (`tsdev_cli.cpp:1234-1242`): `type`, `typeStr`, `name`, `sourceFile`, `source` (pre-read text), `preValid`, `result`
- `parseManifestLines()` (`tsdev_cli.cpp:1255-1359`): shared line-based parser; format `<type> <name> [<source_file>]`; source is a FILE PATH read via `readFile()` during parsing
- `--batch-stdin` mode (`tsdev_cli.cpp:1505-1520`): reads stdin via `fread` loop, feeds into same `parseManifestLines()`
- `doUpsert()` (`tsdev_cli.cpp:538-550`): single-item upsert; calls `findExistPos()` → `doModify()` (if exists) or `doInsert(flushPerItem=true)` (if not); overrides `r.op = "upsert"`

**Flush concern for batch-upsert:** `doUpsert()` internally calls `doInsert()` with `flushPerItem=true`. In batch mode, the canonical pattern is `flushPerItem=false` + single `closeWarehouse()` at end (`tsdev_cli.cpp:1448`). Adding batch-upsert requires passing `flushPerItem=false` through `doUpsert()` to `doInsert()`.

### 1.2 Current Batch Architecture (Python Wrapper)

`TsDevClient` (`src/tsdev_automation/client.py`) exposes **3 batch methods**: `batch_insert`, `batch_modify`, `batch_delete`. There is **no `batch_upsert`**.

- `_batch(op, items)` (`client.py:262-272`): builds manifest string via `_build_manifest_string()`, invokes C++ CLI via `--batch-stdin`, pipes manifest via `input=`
- `_build_manifest_string()` (`client.py:297-325`): line format `<type> <name> <src_path>`; source strings → temp `.el` files via `_materialize_source()`; existing file paths → pass through directly
- `_run_batch(cmd, input)` (`client.py:354-390`): runs C++ CLI, parses JSON array → `list[TsDevResult]`; exit 1 → `TsDevError`; per-item failures returned not raised
- `_BATCH_SUBCMD` dict (`client.py:37`): `{"insert": "batch-insert", "modify": "batch-modify", "delete": "batch-delete"}` — **no `"upsert"` entry**
- `needs_source = op in ("insert", "modify")` (`client.py:263`) — **does not include `"upsert"`**

### 1.3 Test Infrastructure

- **C++ unit tests:** `tests/test_*.cpp`, wired via `CMakeLists.txt` (`add_executable` + `test_regression` target) + `tests/run_unit_tests.ps1` (`$TestDefinitions` array with `RequiresTs` flag)
- **PowerShell E2E tests:** `tests/test_*.ps1`, wired via `tests/run_regression.ps1` (sub-script path var, `-Skip<Name>` param, skip-logic block, `Invoke-Suite` invocation block)
- **8 sub-suites / 93 cases** currently in `run_regression.ps1`
- **Helpers:** `Invoke-CliRaw` (stdout via pipeline, stderr to temp file), `Invoke-CliStdin` (Start-Process with -RedirectStandardInput), `Run-TestCase` + `Assert-True`, `Write-SourceFile` (ASCII encoding — UTF-8 BOM breaks EL compiler)
- **Case naming:** `<PREFIX><number>-<descriptive-slug>` (e.g., `S1-batch-stdin-insert-mixed`); unique names `<Feature>Test_${runId}_<case>`
- **Fixtures:** `tests/fixtures/orlib20_baseline.dll` (only fixture; no new fixtures needed for this workstream)

### 1.4 Key Findings for Workstream I

1. **Directory batch is highly amenable:** The entire batch execution path (`runBatch` → `doInsert`/`doModify`/`doDelete`/`doUpsert`, `emitBatchOutput`, exit codes) is **op-agnostic and source-agnostic** — it only consumes `std::vector<BatchItem>`. A directory mode need only synthesize `BatchItem`s from directory enumeration, then hand off to the existing machinery.

2. **batch-upsert is a prerequisite:** `batch-upsert-dir` requires the `"upsert"` branch in `runBatch()`, which doesn't exist yet. Must add `batch-upsert` (manifest + stdin modes) first.

3. **DRY opportunity:** `cmdBatch()` and a new `cmdBatchDir()` share steps 2-5 (BatchParams setup, worker thread launch, `runBatch`, `emitBatchOutput`, return exitCode). Extract a shared `runBatchAndEmit()` helper to avoid duplication.

4. **Python wrapper should call C++ CLI directory mode directly** (not Python-side synthesis): This ensures directory enumeration logic lives in ONE place (C++ CLI), provides parity, and the Python wrapper is a thin client. `_run_batch(cmd, input="")` is reusable for result parsing (empty stdin is harmless since the C++ directory mode doesn't read stdin).

5. **No RE work needed:** This workstream is pure CLI/Python implementation on top of existing orlib20 primitives. No Ghidra/Frida required.

---

## 2. TVS Decomposition

### Dependency graph

```
TVS-I.1 (batch-upsert: manifest + stdin)  ─────────────┐
                                                        ├──► TVS-I.3 (Python wrapper:
TVS-I.2 (directory batch: all 4 ops)  ── depends on I.1 ┘     batch_upsert + dir batch)
```

### TVS-I.1: batch-upsert (C++ CLI — manifest file + stdin modes)

**Goal:** Add `batch-upsert` subcommand to `tsdev_cli`, supporting both manifest file and `--batch-stdin` modes. Reuses existing `doUpsert()` core logic with a `flushPerItem` parameter fix for batch context.

**Scope:**
- IN: New `batch-upsert` subcommand in `main()` dispatch; `op = "upsert"` mapping; `else if (op == "upsert")` branch in `runBatch()`; `flushPerItem` parameter added to `doUpsert()` (default `true` for single-item backward compat; `false` from `runBatch()`); `needsSource` in `cmdBatch()` includes `"upsert"`; usage text + header comment updates; C++ unit test; E2E test cases; BUILD.md update.
- OUT: Directory batch mode (deferred to TVS-I.2). Python wrapper (deferred to TVS-I.3).

**Dependencies:** None (uses existing `doUpsert()` primitive from TVS-F.3).

**Atomic subtasks:**
1. **Write failing tests** — Add `tests/test_batch_upsert.cpp` (C++ unit test: verify doUpsert with flushPerItem=false preserves ProgId across insert-then-modify path) and `tests/test_batch_upsert.ps1` (E2E: batch-upsert via manifest file with mixed insert-path/modify-path items; batch-upsert via --batch-stdin; verify ProgId preservation on modify-path).
2. **Build + run to confirm RED** — Build new test executable + tsdev_cli; run tests; confirm failure ("unknown subcommand batch-upsert" / doUpsert flushPerItem param doesn't exist).
3. **Implement batch-upsert** — In `src/tsdev_cli.cpp`: (a) add `bool flushPerItem = true` param to `doUpsert()`, pass through to `doInsert()` call; (b) add `"batch-upsert"` to dispatch `if`-chain at ~line 1831, map to `op = "upsert"`; (c) add `else if (op == "upsert")` branch in `runBatch()` calling `doUpsert(..., /*flushPerItem=*/false)`; (d) update `needsSource = (op == "insert" || op == "modify" || op == "upsert")` in `cmdBatch()`; (e) update `printUsage()` + header comments.
4. **Build + run to confirm GREEN** — Rebuild; run unit test + E2E; confirm all pass.
5. **Run regression suite** — `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (all existing suites + new batch-upsert suite).
6. **Adversarial review** — Independent reviewer checks: complete execution, no scope creep, flushPerItem correctness, DRY reuse, exit code consistency.
7. **E2E CLI verification** — Run real `tsdev_cli batch-upsert` via manifest file and --batch-stdin; verify ProgId preservation on modify-path; verify insert-path creates new entries.

**Files that will be touched:**
- `src/tsdev_cli.cpp` — add `flushPerItem` param to `doUpsert()`; add `batch-upsert` dispatch; add `runBatch()` upsert branch; update `needsSource`; update usage
- `tests/test_batch_upsert.cpp` — new C++ unit test
- `tests/test_batch_upsert.ps1` — new E2E test suite
- `CMakeLists.txt` — add `test_batch_upsert` target + `test_regression` dependency
- `tests/run_unit_tests.ps1` — add `test_batch_upsert` to `$TestDefinitions`
- `tests/run_regression.ps1` — add batch-upsert suite (sub-script path, `-SkipBatchUpsert` param, skip-logic, invocation block)
- `docs/BUILD.md` — add `batch-upsert` to subcommand list; add test suite entry; update counts
- `docs/E2E_TEST_SPEC.md` — add batch-upsert test cases (Part D)

**Risks / unknowns:**
- **flushPerItem reconciliation:** `doUpsert()` currently hardcodes `doInsert(..., /*flushPerItem=*/true)`. In batch mode, this would call `closeWarehouse()` per upsert-insert item, deviating from the batch "flush once" pattern. **Decision:** Add `bool flushPerItem = true` parameter to `doUpsert()`; pass `false` from `runBatch()`. The modify branch calls `doModify()` which doesn't flush, so no change needed there. Single-item `cmdUpsert()` continues to call `doUpsert()` with default `flushPerItem=true` — no behavioral change for existing single-item upsert.
- **Exit codes:** batch-upsert exit codes match existing batch pattern: 0 = all succeeded, 1 = manifest/usage error, 2 = ≥1 item failed. Per-item upsert never emits exit 5 (not_found/already_exists) by construction. **No new exit codes needed.**

---

### TVS-I.2: Directory batch mode (C++ CLI — all 4 ops)

**Goal:** Add `batch-insert-dir`, `batch-modify-dir`, `batch-delete-dir`, `batch-upsert-dir` subcommands to `tsdev_cli`. Args: `<type> <dir_path> [-t <ext>...]`. Enumerates files in `dir_path` matching accepted extensions (default: `.el`, `.txt`; overridable via repeatable `-t` flag). File stem (sans last extension) = item name; file content = EL source (for insert/modify/upsert; ignored for delete). Reuses `runBatch()`/`emitBatchOutput()` machinery via a shared `runBatchAndEmit()` helper.

**Scope:**
- IN:
  - New `parseDirectory()` function: enumerates regular files in `dir_path`, filters by accepted extensions, builds `std::vector<BatchItem>` (stem = name, `readFile()` = source for insert/modify/upsert; source empty for delete)
  - Shared `runBatchAndEmit()` helper extracted from `cmdBatch()` (steps: BatchParams setup, worker thread launch, `runBatch`, `emitBatchOutput`, return exitCode) — used by both `cmdBatch()` and `cmdBatchDir()`
  - New `cmdBatchDir()` handler: validates type, parses `-t` flags, calls `parseDirectory()`, calls `runBatchAndEmit()`
  - `-t <ext>` flag parsing in directory batch dispatch: repeatable; first occurrence overrides defaults (`.el`, `.txt`); subsequent occurrences add to the override set; case-insensitive extension matching
  - 4 new subcommand dispatch entries: `batch-insert-dir`, `batch-modify-dir`, `batch-delete-dir`, `batch-upsert-dir` (each maps to canonical `op` string)
  - Usage text + header comment updates
  - C++ unit test (`test_directory_batch.cpp` — tests `parseDirectory()` logic: extension filtering, stem extraction, source reading, empty-dir handling)
  - E2E test suite (`test_directory_batch.ps1`)
  - BUILD.md + E2E_TEST_SPEC.md updates
- OUT: Python wrapper (deferred to TVS-I.3).

**Dependencies:** TVS-I.1 (for `batch-upsert-dir` which requires the `"upsert"` branch in `runBatch()`).

**Atomic subtasks:**
1. **Write failing tests** — Add `tests/test_directory_batch.cpp` (C++ unit test: `parseDirectory()` with mixed extensions, default vs override, stem extraction, empty dir, subdirectory skipping) and `tests/test_directory_batch.ps1` (E2E: batch-insert-dir with .el files; batch-modify-dir with ProgId preservation; batch-delete-dir; batch-upsert-dir mixed insert/modify; `-t` flag override; empty dir → exit 1; unknown type → exit 2; non-existent dir → exit 1).
2. **Build + run to confirm RED** — Build new test executable + tsdev_cli; run tests; confirm failure ("unknown subcommand batch-insert-dir" / `parseDirectory` doesn't exist).
3. **Implement directory batch** — In `src/tsdev_cli.cpp`: (a) add `#include <filesystem>`; (b) implement `parseDirectory(type, typeStr, dirPath, extensions, needsSource, outItems)` using `std::filesystem::directory_iterator`; (c) extract `runBatchAndEmit(op, items, tsPath)` from `cmdBatch()`; (d) implement `cmdBatchDir(op, type, dirPath, extensions, tsPath)` calling `parseDirectory()` + `runBatchAndEmit()`; (e) add `-t` flag parsing in `main()` directory batch dispatch (scan reducedArgv for `-t <ext>` pairs, collect extensions, remove from arg list, check remaining argc == 4); (f) add 4 dispatch entries; (g) update `printUsage()` + header comments.
4. **Build + run to confirm GREEN** — Rebuild; run unit test + E2E; confirm all pass.
5. **Run regression suite** — `powershell -ExecutionPolicy Bypass -File tests\run_regression.ps1` (all existing suites + new directory-batch suite).
6. **Adversarial review** — Independent reviewer checks: complete execution, no scope creep, DRY (runBatchAndEmit shared), extension filtering correctness, `-t` flag semantics (first overrides defaults), stem extraction, empty-dir handling, exit code consistency.
7. **E2E CLI verification** — Run real `tsdev_cli batch-insert-dir` / `batch-modify-dir` / `batch-delete-dir` / `batch-upsert-dir` with a temp directory of .el files; verify `-t` flag override; verify empty dir → exit 1.

**Files that will be touched:**
- `src/tsdev_cli.cpp` — add `#include <filesystem>`; `parseDirectory()`; `runBatchAndEmit()` (extracted); `cmdBatchDir()`; `-t` flag parsing; 4 dispatch entries; usage
- `tests/test_directory_batch.cpp` — new C++ unit test
- `tests/test_directory_batch.ps1` — new E2E test suite
- `CMakeLists.txt` — add `test_directory_batch` target + `test_regression` dependency; verify C++ standard ≥ C++17 for `<filesystem>`
- `tests/run_unit_tests.ps1` — add `test_directory_batch`
- `tests/run_regression.ps1` — add directory-batch suite (sub-script path, `-SkipDirectoryBatch` param, skip-logic, invocation block)
- `docs/BUILD.md` — add 4 subcommands + test suite; update counts
- `docs/E2E_TEST_SPEC.md` — add directory batch test cases (new Part or section)

**Risks / unknowns:**
- **`std::filesystem` availability:** The CLI does not currently include `<filesystem>`. VS Build Tools support C++17 `<filesystem>`. **Decision:** Add `#include <filesystem>`. Verify CMake `CMAKE_CXX_STANDARD` is ≥ 17 (if not, set it). Link `std::filesystem` if needed (MSVC typically doesn't require explicit linking for `<filesystem>` since VS 2017 15.7+).
- **`-t` flag naming:** Uses single-dash `-t` (per user spec), inconsistent with existing double-dash flags (`--ts-path`, `--json`, `--batch-stdin`). **Decision:** Follow user spec exactly (`-t`). Document the inconsistency in usage text. Support space-separated form only (`-t el`, not `-t=el`). If `-t` is the last token with no following argument, emit stderr `"tsdev_cli: -t requires an extension argument"` and return exit 1.
- **Extension matching:** Case-insensitive (`.EL` matches `-t el`). **Decision:** The `-t` flag accepts extensions with or without a leading dot (e.g., `-t el` and `-t .el` are equivalent). The implementation normalizes by prepending a `.` if missing, then lowercases for case-insensitive comparison. Same normalization applied in the Python wrapper (`extensions=["el"]` and `extensions=[".el"]` are equivalent).
- **`-t` flag semantics:** First occurrence of `-t` overrides defaults (`.el`, `.txt`); subsequent `-t` flags add to the override set. No `-t` → use defaults. **Decision:** If the extensions vector is empty after parsing (no `-t` flags), use defaults `{".el", ".txt"}`. If non-empty, use only the specified extensions.
- **Empty directory / no matching files:** If `parseDirectory()` produces zero items. **Decision:** Exit 1 with stderr `"no files matching <extensions> found in <dir_path>"` (analogous to empty manifest → exit 1). This is a usage-level error, not a per-item failure. No JSON array emitted.
- **Non-existent directory:** `std::filesystem::directory_iterator` throws `filesystem_error` for a non-existent path. **Decision:** Wrap in try/catch, emit stderr `"tsdev_cli: failed to open directory \"<path>\""`, return exit 1 (same as non-existent manifest file). No JSON array emitted.
- **Subdirectory handling:** **Decision:** Skip subdirectories (only enumerate regular files via `std::filesystem::is_regular_file()`). Do not recurse.
- **File stem with multiple dots:** `my.strategy.el` → stem = `my.strategy` (`std::filesystem::path::stem()` strips only the last extension). **Decision:** Use `std::filesystem::path::stem()` behavior verbatim.
- **Files with no extension:** `README` → stem = `README` (no extension to strip). Will be picked up only if the empty-string extension matches an accepted extension — it won't (accepted extensions are `.el`, `.txt`, etc.), so files with no extension are excluded by default. With `-t ""` (empty extension), they would be included — but this is an edge case the user is unlikely to hit. **Decision:** Files with no extension are excluded unless the user explicitly passes `-t ""` (empty string extension). Document this.
- **Shared helper extraction:** `cmdBatch()` and `cmdBatchDir()` share steps 2-5. **Decision:** Extract `runBatchAndEmit(const std::string& op, std::vector<BatchItem>& items, const wchar_t* tsPath)` from `cmdBatch()`. Both `cmdBatch()` and `cmdBatchDir()` call it. `cmdBatch()` retains its manifest-reading logic (file/stdin → items); `cmdBatchDir()` retains its directory-reading logic (parseDirectory → items). This satisfies the DRY requirement.
- **Arg parsing for `-t`:** The `-t` flag is directory-batch-specific. It survives the global flag pre-scan (which only strips `--ts-path` and `--json`) and is filtered in the directory batch dispatch block, similar to how `--case-insensitive` is filtered in the exists dispatch. The dispatch scans `reducedArgv` for `-t` tokens, extracts the next token as the extension, removes both from the arg list, then checks remaining argc == 4 (program + subcommand + type + dir).

---

### TVS-I.3: Python wrapper — batch_upsert + directory batch + _cli.py subcommands

**Goal:** Add `batch_upsert()` method and `batch_insert_dir()`, `batch_modify_dir()`, `batch_delete_dir()`, `batch_upsert_dir()` methods to `TsDevClient`. Directory batch methods call the C++ CLI's `batch-<op>-dir` subcommands directly (passing `<type> <dir_path>` and optional `-t` flags). Reuses `_run_batch()` for JSON array result parsing.

**Scope:**
- IN:
  - `batch_upsert(self, items)` method — adds `"upsert": "batch-upsert"` to `_BATCH_SUBCMD`; fixes `needs_source` to include `"upsert"`
  - `batch_insert_dir(self, type, dir_path, extensions=None)` method
  - `batch_modify_dir(self, type, dir_path, extensions=None)` method
  - `batch_delete_dir(self, type, dir_path, extensions=None)` method
  - `batch_upsert_dir(self, type, dir_path, extensions=None)` method
  - New `_batch_dir(self, op, type, dir_path, extensions)` helper — validates type, builds cmd `[cli_path, --ts-path ..., batch-<op>-dir, type, dir_path]` + optional `-t <ext>` flags, calls `_run_batch(cmd, input="")`
  - `batch-upsert` subcommand in `_cli.py` (mirrors existing `batch-insert` pattern: takes manifest file, reads via `_read_manifest`, calls `client.batch_upsert(items)`)
  - `batch-insert-dir`, `batch-modify-dir`, `batch-delete-dir`, `batch-upsert-dir` subcommands in `_cli.py` (take `<type> <dir_path>` + optional repeatable `-t <ext>` flag via `action="append"`, call `client.batch_*_dir(type, dir_path, extensions)`)
  - Update `_cli.py` module docstring usage examples
  - Python E2E tests for all new methods
  - Docstring/README updates
- OUT: None (final TVS).

**Dependencies:** TVS-I.1 (for `batch_upsert`) and TVS-I.2 (for directory batch subcommands).

**Atomic subtasks:**
1. **Write failing Python tests** — Add mocked-subprocess unit tests (verify cmd construction, `-t` flag serialization, `extensions=None` → no `-t` flags, `extensions=[]` → defaults, error handling for exit 1 → TsDevError) AND `@pytest.mark.e2e` tests (drive real CLI with temp .el directory) for `batch_upsert` and all 4 `batch_*_dir` methods AND the new `_cli.py` subcommands.
2. **Run to confirm RED** — Run new tests; confirm failure (`AttributeError: 'TsDevClient' has no attribute 'batch_upsert'` / `batch_insert_dir`).
3. **Implement Python wrapper** — In `src/tsdev_automation/client.py`: (a) add `"upsert": "batch-upsert"` to `_BATCH_SUBCMD`; (b) fix `needs_source = op in ("insert", "modify", "upsert")` in `_batch()`; (c) add `batch_upsert()` method; (d) add `_batch_dir()` helper; (e) add 4 `batch_*_dir()` methods; (f) update docstrings.
4. **Run to confirm GREEN** — Run new tests; confirm all pass.
5. **Run full Python test suite** — All 62+ existing tests + new tests.
6. **Adversarial review** — Independent reviewer checks: complete execution, no scope creep, DRY reuse of `_run_batch()`, extension parameter API, error handling consistency.
7. **E2E CLI verification** — Run Python wrapper driving C++ CLI for all new methods; verify directory batch with real .el files; verify `-t` override.

**Files that will be touched:**
- `src/tsdev_automation/client.py` — add `_BATCH_SUBCMD["upsert"]`, `needs_source` fix, `batch_upsert()`, `_batch_dir()`, 4 `batch_*_dir()` methods, docstrings
- `src/tsdev_automation/_cli.py` — add batch-upsert + 4 batch-*-dir subcommands, -t flag parsing (action="append"), docstring updates
- `tests/test_tsdev_client.py` (or equivalent) — add Python tests for new methods
- `README.md` — document new methods (if applicable)

**Risks / unknowns:**
- **`extensions` parameter API:** `None` means C++ defaults (`.el`, `.txt`). A non-empty list like `["el"]` means only `.el`. Each extension is passed as `-t <ext>` to the C++ CLI. **Decision:** `extensions: Sequence[str] | None = None`. `None` → no `-t` flags (C++ uses defaults). Non-empty list → `-t <ext>` for each element. Empty list `[]` → treated same as `None` (use defaults) to avoid accidental "match nothing" behavior. Validate each extension is a non-empty string.
- **`_run_batch()` with `input=""`:** The C++ CLI directory mode doesn't read stdin. Passing `input=""` to `subprocess.run` pipes an empty string, which is harmless. **Decision:** Reuse `_run_batch(cmd, input="")` as-is. No new helper needed for result parsing.
- **Type validation:** `_validate_type()` called once up-front in `_batch_dir()` before building the cmd. Consistent with existing single-item methods.
- **Error handling:** C++ exit 1 (empty dir / non-existent dir / no matching files) → `TsDevError` raised (same as manifest-level failure in existing `_run_batch()`). Per-item failures returned as `TsDevResult` elements with `success=False` (same as existing batch).

---

## 3. Recommended First TVS

**TVS-I.1 (batch-upsert: manifest + stdin)** is the recommended first TVS.

Rationale:
- No dependencies (uses existing `doUpsert()` primitive)
- Smallest scope (one new subcommand, one `flushPerItem` param fix)
- Unblocks TVS-I.2 (directory-upsert requires the `runBatch()` upsert branch)
- Validates the flushPerItem reconciliation before it's needed in directory mode
- Follows TDD red-green method

## 4. Open Questions

None. All design decisions are resolved:
- Subcommand naming: `batch-<op>-dir` (user-confirmed)
- File filtering: default `.el` + `.txt`; repeatable `-t <ext>` flag; first `-t` overrides defaults (user-confirmed)
- Python wrapper: calls C++ CLI directory mode directly (PM decision — single source of truth for directory enumeration)
- DRY: shared `runBatchAndEmit()` helper for `cmdBatch()` + `cmdBatchDir()`; Python reuses `_run_batch()`
- No RE work required

---

## 5. Documentation Updates (per TVS)

Each TVS updates documentation as part of its atomic subtasks (following the WORKSTREAM_F pattern):

| TVS | AGENTS.md | BUILD.md | E2E_TEST_SPEC.md | PROJECT_HISTORY.md | PM_WORKING_MEMORY.md |
|-----|-----------|----------|------------------|--------------------|-----------------------|
| I.1 | Update TVS-I.1 row from PLANNED to COMPLETE (on completion) | Add `batch-upsert` subcommand + test suite | Add batch-upsert test cases (Part D, e.g. D.6 — tests/test_batch_upsert.ps1) | Add `### DONE — TVS-I.1` entry | Update NEXT WORKSTREAM |
| I.2 | Update TVS-I.2 row from PLANNED to COMPLETE | Add 4 `batch-*-dir` subcommands + test suite | Add directory batch test cases | Add `### DONE — TVS-I.2` entry | Update progress |
| I.3 | Update TVS-I.3 row from PLANNED to COMPLETE | — | — | Add `### DONE — TVS-I.3` entry | Update progress; mark Workstream I COMPLETE |

**Note:** Upon completion of TVS-I.3, also update the AGENTS.md 'Project Status' and 'Completed Work Summary' intro statements (currently 'All workstreams A, B, C, D, E, F, G are COMPLETE') to include Workstream I.

**No dedicated operational spec doc** is warranted for this workstream (the feature is small enough to document via BUILD.md subcommand entries + E2E_TEST_SPEC.md test cases, following progressive disclosure). The plan document (`docs/archive/WORKSTREAM_I_PLAN.md`) serves as the historical reference.

---

## 6. TVS Summary

| TVS | Title | Dependencies | Est. subtasks |
|-----|-------|--------------|---------------|
| TVS-I.1 | batch-upsert (C++ CLI — manifest + stdin) | None | 7 |
| TVS-I.2 | Directory batch mode (C++ CLI — all 4 ops) | TVS-I.1 | 7 |
| TVS-I.3 | Python wrapper — batch_upsert + directory batch + _cli.py subcommands | TVS-I.1, TVS-I.2 | 7 |
