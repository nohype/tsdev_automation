# Adversarial Review — WORKSTREAM_F5_PLAN.md

**Reviewer:** Independent adversarial reviewer subagent
**Date:** 2026-08-20
**Verdict:** **DISAPPROVE** (2 critical findings, 3 non-blocking observations)

---

## Summary

The plan is well-structured, atomic, TDD-compliant, and faithful to the accepted RE
findings. The dependency graph is correct, risk mitigations are defensive and
appropriate, and scope is tightly limited to TVS-F.5. The RE-derived constants
(ordinal 568, RVA 0x1A5360, name offset 0x17, ProgId offset 0x00, subtype 0xE9,
filterMode=0, 3 patches with lengths 51/132/58) are all used correctly.

However, **2 critical findings** will cause test failures during execution and
must be fixed before approval.

---

## CRITICAL FINDINGS

### Finding 1 — `tests/test_sigtool.ps1` is not updated in S1 (sigtool regression will FAIL)

**What's wrong:** S1 adds 3 signatures to `signatures/v9.50.01.3344.json` and bumps
`EXPECTED_SIGNATURE_COUNT` 13→16 in `tools/sigcodegen.py`, but does NOT mention
updating `tests/test_sigtool.ps1`, which contains a **hardcoded** signature count
and stale regexes.

**Evidence (from the codebase):**

- `tests/test_sigtool.ps1` line 40: `$expectedSignatureCount = 13`
- `tests/test_sigtool.ps1` line 36 (comment): `"13 signatures, 4 patch_site"`
- `tests/test_sigtool.ps1` lines 172–174 (Test 1, non-verbose `verify`):
  ```powershell
  ($script:out -match '13\s*/\s*13') -or
  (($script:out -split "`r?`n" | Select-String -Pattern 'PASS' -CaseSensitive).Count -ge $expectedSignatureCount)
  ```
- `tests/test_sigtool.ps1` lines 271–272 (Test 8, same pattern).
- `src/sigtool.cpp` lines 501–505: non-verbose `verify` prints ONLY
  `"%d/%d signatures PASS\n"` (a single summary line — no per-signature `[PASS]`
  lines in non-verbose mode).

**Why it fails:** After adding 3 signatures, `sigtool verify` (non-verbose) outputs
`"16/16 signatures PASS\n"`. The regex `'13\s*/\s*13'` does NOT match `"16/16"`.
The fallback counts lines containing `PASS` in non-verbose output = **1** (just the
summary line), and `1 -ge 13` is FALSE. Both branches of the OR fail → Test 1
FAILS. Test 8 fails identically. This breaks:
- S1's own verification step ("run sigtool test suite to confirm all 16 signatures
  match").
- S17 (full regression suite — the sigtool sub-suite fails).

**What must be fixed:** S1 must additionally edit `tests/test_sigtool.ps1`:
1. Bump `$expectedSignatureCount` from `13` to `16` (line 40).
2. Update the regex from `'13\s*/\s*13'` to `'16\s*/\s*16'` (lines 173, 271).
3. Update comments: line 36 (`"16 signatures, 7 patch_site"`), line 163
   (`"all 16 signatures"`), line 195 (`"All 7 patch sites"`).

Note: `tests/test_orlib_resolver.cpp` and `tests/test_no_hardcoded_rvas.cpp` use
`tsdev::kSignatureCount` from the generated header — these auto-update and need no
manual change (verified: `test_no_hardcoded_rvas.cpp` line 55 explicitly excludes
exports from the forbidden set, and `test_orlib_resolver.cpp` uses
`kSignatureCount`/`kExports_v9_50_01_3344` from `signatures.gen.h`).

---

### Finding 2 — S9 flag-parsing location is contradictory; the `reducedArgc != 4` guard in `main()` is not addressed (E2E GREEN will FAIL)

**What's wrong:** S9 assigns `-i`/`--case-insensitive` flag parsing to `cmdExists()`,
but `cmdExists()` has no access to `argv`/`reducedArgc` (it receives already-parsed
`typeStr`/`name` strings). Simultaneously, S9 says `main()` passes the parsed flag
to `cmdExists` — a contradiction. Critically, the `reducedArgc != 4` arg-count guard
in `main()` (which runs BEFORE `cmdExists` is called) is never addressed, so passing
`-i` triggers a usage error before `cmdExists` ever runs.

**Evidence (from the codebase):**

- `src/tsdev_cli.cpp` lines 1682–1696 (the exists dispatch in `main()`):
  ```cpp
  if (iequals(subcommand, "exists")) {
      if (reducedArgc != 4) {   // ← rejects any extra arg (e.g. -i)
          ...
          r.errorMessage = "exists requires exactly 2 arguments: <type> <name>";
          ...
          return 1;
      }
      return cmdExists(reducedArgvPtr[2], reducedArgvPtr[3],
                       tsPathWidePtr, jsonMode);
  }
  ```
  With `-i` present, `reducedArgc == 5` → the guard fires → usage error exit 1.
  `cmdExists` is never called.

- `src/tsdev_cli.cpp` line 879: `cmdExists` signature is
  `int cmdExists(const std::string& typeStr, const std::string& name,
                const wchar_t* tsPathOverride, bool jsonMode)` — no argv access.

- The correct pattern already exists in the codebase for `get-source` (an optional
  flag on a 2-positional-arg subcommand): `src/tsdev_cli.cpp` lines 1716–1778.
  `get-source` scans `reducedArgv` for `--output`, extracts it into a local, builds
  a `gsArgs` vector of remaining positional args, checks `gsArgs.size() != 2`, then
  calls `cmdGetSource(gsArgs[0], gsArgs[1], outputPath, ...)`. The `-i` flag is
  simpler (boolean toggle, no value), but the scan-and-extract-in-`main()` pattern
  is the exact template to mirror.

**Why it fails:** S7's E2E test D2.1 invokes
`exists strategy E2E_<RUNID>_CIPROBE -i --json`. With the plan as written,
`reducedArgc == 5` → `"exists requires exactly 2 arguments"` → exit 1 → D2.1 FAILS.
S10 (confirm GREEN) cannot pass.

**What must be fixed:** S9 must be rewritten to:
1. Move ALL `-i`/`--case-insensitive` flag parsing into the `exists` dispatch in
   `main()` (lines 1682–1697), mirroring the `get-source` scan-and-extract pattern
   (lines 1716–1778): scan `reducedArgv` for `-i` or `--case-insensitive`, remove
   it, build a vector of remaining positional args, check `size() == 2`.
2. Change `cmdExists` signature to accept `bool caseInsensitive`:
   `int cmdExists(const std::string& typeStr, const std::string& name,
                 const wchar_t* tsPathOverride, bool jsonMode, bool caseInsensitive)`.
3. Remove the instruction that says `cmdExists` parses the flag from the arg list
   (it cannot — it has no argv).
4. Pass `caseInsensitive` through `ExistsParams` → `runExists` → `doExists` (this
   part of the plan is correct, just the parsing location is wrong).

---

## NON-BLOCKING OBSERVATIONS

### Observation 1 — Destructor comment not updated (cosmetic)

`src/tsdev_lib.cpp` line 588: the destructor comment says
`"Restore all 5 patch sites"` (persistence + 4 thread-check). After S4 grows the
thread-check arrays to [6], this becomes 7 patch sites (persistence + 6
thread-check). S4 does not mention updating this comment. Non-blocking (comment
only), but should be fixed for accuracy.

### Observation 2 — `applyThreadCheckPatches()` comment line reference is wrong

S4 says: "Update the comment block at the top of `applyThreadCheckPatches()`
(around line 85-88) to list the 2 new patch sites." The actual
`applyThreadCheckPatches()` comment block is at lines 1087–1096 in
`src/tsdev_lib.cpp` (line 85-88 is in the middle of unrelated path-discovery code).
Non-blocking — the implementer will find the correct location — but the line
reference is incorrect.

### Observation 3 — `getMyWorkBinPath()` as the `_LoadMasterRecsEX@16` path parameter

S4 passes `getMyWorkBinPath()` as the `path` parameter to `_LoadMasterRecsEX@16`.
The RE (§3.1) says this path is "Passed to `FUN_101a9650` (GetWarehouse)" and the
authoritative caller `FUN_1015b760` prepares it via `FUN_10009eb0(param_3, 3)`.
The exact path format expected by `FUN_101a9650` is not 100% confirmed by the RE.
However, `getMyWorkBinPath()` returns the discovered `MyWork\Bin` path, which
matches the RE's example, and the call is SEH-protected (defensive failure on
exception). This is an acceptable residual risk, not a blocker.

---

## CRITERIA CHECKLIST (for reference)

| Criterion | Status | Notes |
|-----------|--------|-------|
| Atomicity (one subagent → one narrow subtask) | **PASS** | Each subtask is either a single coherent edit (1–3 files, one logical change) or a single build+run+verify. No subtask combines edit+build+run+regression. |
| Completeness (all TVS-F.5 requirements) | **PASS** | Case-insensitive flag, verbatim stored name, ProgId, human+JSON output, C++ CLI + Python wrapper, stored_name in JSON, tests for CI matching AND verbatim name — all covered. |
| Correctness vs RE findings | **PASS** | `_LoadMasterRecsEX@16` (ordinal 568, RVA 0x1A5360, params category/HGLOBAL*/path/filterMode), name offset 0x17 (not 0x02), ProgId offset 0x00, TradingApp filter rec[0xE9]==0x14, 3 patches (0x08/0x09/0x07) with lengths 51/132/58, filterMode=0 — all correct. |
| TDD adherence (RED→GREEN, separate subtasks) | **PASS** | S2→S3→S4→S5, S7→S8→S9→S10, S12→S13→S14→S15 — each RED and GREEN is a separate subtask. |
| Dependency graph | **PASS** | Linear order S1→S20, no circular/missing dependencies. S2 depends on S1 (signatures must compile), S4 depends on S3 (RED confirmed), etc. |
| Risk mitigations | **PASS** | cmd 0x09 applied defensively, warehouse mode handled, empty warehouse (count<=0 → false), NULL buffer (GlobalLock fail → GlobalFree), SEH protection, 0x07 not applied (filterMode=0). |
| No scope creep | **PASS** | Only `exists` subcommand, `enumerateRecords` method, and Python `exists` method touched. No unrelated functionality. |
| Verification steps | **FAIL** | S1 verification claims sigtool test suite passes, but test_sigtool.ps1 is not updated (Finding 1). S10 GREEN cannot pass due to Finding 2. |
| Adversarial review placement | **PASS** | S6 (reviews S4+S1), S11 (reviews S9), S16 (reviews S14), S19 (reviews S18) — all substantive implementation/docs subtasks reviewed. |

---

## VERDICT: DISAPPROVE

The plan must be revised to:
1. **Add `tests/test_sigtool.ps1` to S1's scope** (bump count 13→16, update regexes
   and comments).
2. **Rewrite S9's flag-parsing instructions** to move `-i`/`--case-insensitive`
   parsing into `main()`'s exists dispatch (mirroring the `get-source`
   scan-and-extract pattern at lines 1716–1778), add `bool caseInsensitive` to the
   `cmdExists` signature, and remove the contradictory cmdExists-parses-argv
   instruction.

After these fixes, the plan should be re-reviewed before execution.
