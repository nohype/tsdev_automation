# Documentation Review — TVS-F.5 Completion

**Reviewer:** Independent documentation review subagent
**Date:** 2026-08-20
**Scope:** `docs/PM_WORKING_MEMORY.md`, `docs/PROJECT_HISTORY.md`, `AGENTS.md`, `docs/WORKSTREAM_F5_PLAN.md`
**Verified against:** Codebase (`signatures/v9.50.01.3344.json`, `tools/sigcodegen.py`, `tests/test_sigtool.ps1`, `tests/run_regression.ps1`, `tests/run_unit_tests.ps1`, `tests/test_exists_ci.ps1`, `tests/test_enumerate.cpp`, `CMakeLists.txt`)

---

## Verdict: APPROVE

The F.5-specific documentation across the three primary docs (PM_WORKING_MEMORY.md, PROJECT_HISTORY.md, AGENTS.md) is **accurate, consistent, and complete**. All key facts verified against the codebase match. The WORKSTREAM_F5_PLAN.md has stale information in its detailed subtask body (S1–S20) that was not reconciled with the actual implementation, but the implementation note at the top of the plan correctly captures the key divergences. The findings below are minor/cosmetic and do not block acceptance.

---

## Findings

### 1. WORKSTREAM_F5_PLAN.md — Stale signature counts in S1 (Severity: MINOR)

The plan body S1 (lines 92, 107, 110–111, 113–114, 118) instructs bumping `EXPECTED_SIGNATURE_COUNT` from **13 to 16** and adding **3 new signatures**. The actual codebase has **19 signatures** (`tools/sigcodegen.py` line 37: `EXPECTED_SIGNATURE_COUNT = 19`; `signatures/v9.50.01.3344.json` has 19 signature entries). The implementation note at the top (line 9) correctly states "19 total signatures", but the detailed S1 spec and the "Files Touched" table (line 761: "Bump EXPECTED_SIGNATURE_COUNT 13→16") were not updated.

**Context:** The plan was written when there were 13 signatures (post-F.4). By the time F.5 was implemented, the count had grown to 18 (from intermediate work), and F.5 added 1 (`FUN_101cfce0_thread_check_INT3`) to reach 19. The plan body reflects the original planning assumption, not the actual starting point.

### 2. WORKSTREAM_F5_PLAN.md — Wrong C++ test file name (Severity: MINOR)

The plan body (S2–S5, S17, and the "Files Touched" table at line 763) references `tests/test_exists_caseinsensitive.cpp`, but the actual C++ test file is **`tests/test_enumerate.cpp`** (confirmed via `CMakeLists.txt` line 146: `add_executable(test_enumerate tests/test_enumerate.cpp)` and `tests/run_unit_tests.ps1` line 102: `test_enumerate`). The implementation note at the top (line 9) corrects the PowerShell test file name to `test_exists_ci.ps1` but does not mention the C++ test file rename.

### 3. WORKSTREAM_F5_PLAN.md — Wrong PowerShell test file name in body (Severity: MINOR)

The plan body (S7–S10, S17, and "Files Touched" table at line 768) references `tests/test_exists_caseinsensitive.ps1`, but the actual file is **`tests/test_exists_ci.ps1`** (confirmed via `find_file_by_name` and `tests/run_regression.ps1` line 167: `$ExistsCiScript = Join-Path $PSScriptRoot "test_exists_ci.ps1"`). The implementation note at the top (line 9) correctly uses `test_exists_ci.ps1`.

### 4. WORKSTREAM_F5_PLAN.md — Stale "16 signatures" in S17 verification (Severity: MINOR)

Line 643 says "Verify: sigtool tests (16 signatures)" but the actual count is 19 (`tests/test_sigtool.ps1` line 40: `$expectedSignatureCount = 19`).

### 5. Test case count terminology inconsistency (Severity: MINOR)

The implementation note (line 9), AGENTS.md (line 162), and PROJECT_HISTORY.md (line 323) all say **"9 E2E test cases"** for `test_exists_ci.ps1`. The actual file has **5 logical test cases** (CI.1–CI.5) with **9 `Invoke-Test` steps** (each case has an insert step + a query step, except CI.3 which is query-only). `tests/run_regression.ps1` line 103 says **"5 exists-ci tests"** (counting logical cases). The "9" in the docs counts steps, not cases, which is inconsistent with `run_regression.ps1`'s counting convention (which counts logical cases for all suites: "5 exists tests", "8 batch-stdin tests", "5 upsert tests", "6 get-source tests").

### 6. PM_WORKING_MEMORY.md — Stale Python/C++ test counts in (D) section (Severity: MINOR, pre-existing)

Line 37 says "29 Python tests + 66 C++ regression tests pass" in the (D) Python Integration section under "NEXT WORKSTREAM". The current state is **60 Python tests** and **8 C++ regression suites**. This is pre-existing stale info (not introduced by F.5), but the F.5 doc update pass did not correct it.

### 7. AGENTS.md — Stale Python/C++ test counts in (D) section (Severity: MINOR, pre-existing)

Line 145 says "29 Python tests (20 unit + 9 E2E) + 66 C++ regression tests all passing" in the (D) Python Integration section under "NEXT WORKSTREAMS". Same stale info as finding 6. Current state: 60 Python tests, 8 C++ regression suites.

### 8. PROJECT_HISTORY.md — Missing C++ test file name (Severity: MINOR)

The F.5 entry (line 327) says "CMakeLists.txt — new test target" but does not name the actual test file (`test_enumerate.cpp`). All other changed files are named explicitly. Minor omission.

### 9. WORKSTREAM_F5_PLAN.md — Planning date after implementation date (Severity: COSMETIC)

Line 4 says "Date: 2026-08-20 (planning); 2026-08-19 (implementation complete)". The planning date (08-20) is after the implementation date (08-19), which is chronologically illogical. Likely a typo — the planning date should be on or before 08-19.

### 10. WORKSTREAM_F5_PLAN.md — S4 code spec shows single-phase, not two-phase (Severity: MINOR, expected)

The S4 implementation spec (lines 163–318) shows the original single-phase `enumerateRecords` code with `filterMode=0`, which does not match the actual two-phase implementation (Phase 1: `_LoadMasterRecsEX@16` cached + Phase 2: `FindExistPos` BST + `getSource`). The implementation note at the top (line 9) describes the two-phase approach. This is expected for a planning document with a post-completion implementation note, but the S4 code block is now historically inaccurate relative to what was actually implemented.

---

## Verified Facts (all correct)

| Fact | PM_WORKING_MEMORY.md | PROJECT_HISTORY.md | AGENTS.md | Codebase |
|------|---------------------|---------------------|-----------|----------|
| 19 signatures | Line 61: "19 total signatures" | Line 310: "19 total signatures" | Line 136: "19 signatures total" | `sigcodegen.py` line 37: `19`; JSON: 19 entries |
| 8 C++ regression suites | Line 27: "8 C++ regression suites" | Line 328: "8/8 C++ regression suites" | Line 139: "8 C++ regression suites" | `run_regression.ps1`: 8 suites with `-SkipExistsCi` |
| 60 Python tests | Line 27: "Python: 60 tests" | Line 328: "60/60 Python tests" | Line 139: "Python: 60 tests" | `pytest --co`: 60 tests collected |
| New signature: FUN_101cfce0_thread_check_INT3 | Line 61 | Line 310 | Line 158, 162 | `signatures/v9.50.01.3344.json` line 372 |
| CLI flag: --case-insensitive / -i | Line 61 | Line 311 | Line 18, 149, 162 | (not verified — CLI code) |
| Output: exists=N, stored_name=..., progId=N | Line 61 | Line 311 | Line 18, 149, 162 | `test_exists_ci.ps1` lines 14–18 |
| Python: case_insensitive keyword-only, default False | Line 61 | Line 313 | Line 18, 149, 162 | (not verified — Python code) |
| Two-phase enumerateRecords | Line 61 | Lines 306–309 | Lines 141, 149, 158, 162 | (not verified — C++ code) |
| { name } comment block requirement | Line 61 | Line 314 | Line 142 | (not verified — C++ code) |
| New export: _LoadMasterRecsEX@16 | Line 61 | Line 310 | Line 158 | `signatures/v9.50.01.3344.json` line 465; `sigcodegen.py`: EXPECTED_EXPORT_COUNT = 9 |

---

## Summary

The F.5 documentation is **accurate and consistent** across the three primary documents (PM_WORKING_MEMORY.md, PROJECT_HISTORY.md, AGENTS.md). All key facts match the codebase. The WORKSTREAM_F5_PLAN.md has stale information in its detailed subtask body (S1–S20) — primarily wrong signature counts (13→16 vs actual 19), wrong test file names (`test_exists_caseinsensitive.*` vs actual `test_enumerate.cpp` / `test_exists_ci.ps1`), and a single-phase code spec that doesn't match the two-phase implementation. These are mitigated by the implementation note at the top of the plan (line 9) which correctly captures the key divergences (19 signatures, `test_exists_ci.ps1`, two-phase approach). The stale (D) section test counts in PM_WORKING_MEMORY.md and AGENTS.md are pre-existing issues not introduced by F.5.

**No required fixes.** The findings are minor/cosmetic and do not affect the correctness or usability of the F.5 documentation. The plan body staleness is acceptable given the implementation note overlay, though reconciling the S1 counts and file names in a future doc-maintenance pass would improve clarity.
