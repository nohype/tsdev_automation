# Re-Review — WORKSTREAM_F5_PLAN.md (post-fix)

**Reviewer:** Independent adversarial reviewer subagent
**Date:** 2026-08-20
**Verdict:** **APPROVE** (all 2 critical findings + 3 non-blocking observations correctly fixed)

---

## Summary

The revised plan correctly addresses all 5 items from the original review. Each
fix was independently verified against the actual codebase. No new issues were
introduced. The plan remains internally consistent, the atomicity HARD RULE
holds for all subtasks, and the dependency graph is valid.

---

## Critical Finding 1 — `tests/test_sigtool.ps1` now in S1 scope (FIXED)

**Requirement:** S1 must include `tests/test_sigtool.ps1` with explicit edits
(count bump 13→16, regex updates, comment updates) and a verification step that
invokes the sigtool test.

**Evidence from the revised plan (lines 87–117):**

- Line 90: S1 scope now says "Edit 3 files" — explicitly lists
  `tests/test_sigtool.ps1` as the third file.
- Lines 107–113: Prescribes the exact edits:
  - `$expectedSignatureCount` 13→16 (line 40).
  - `'13\s*/\s*13'`→`'16\s*/\s*16'` regexes at lines 173 and 271.
  - Comment updates at lines 36 (`"16 signatures, 7 patch_site"`), 163
    (`"all 16 signatures"`), 195 (`"All 7 patch sites"`).
- Line 116: Verification step now invokes
  `powershell -ExecutionPolicy Bypass -File tests\test_sigtool.ps1`.

**Independent codebase verification:**

1. `tests/test_sigtool.ps1` line 40: `$expectedSignatureCount = 13` — the plan's
   edit to 16 is correct.
2. `tests/test_sigtool.ps1` line 173: `($script:out -match '13\s*/\s*13')` —
   changing to `'16\s*/\s*16'` is correct.
3. `tests/test_sigtool.ps1` line 271: same regex — same fix applies.
4. `tests/test_sigtool.ps1` line 36: `"13 signatures, 4 patch_site"` — the plan
   changes to `"16 signatures, 7 patch_site"`. Verified: 4 existing thread-check
   INT3 patch sites + 3 new = 7. Correct.
5. `tests/test_sigtool.ps1` line 163: `"all 13 signatures"` → `"all 16
   signatures"`. Correct.
6. `tests/test_sigtool.ps1` line 195: `"All 4 patch sites"` → `"All 7 patch
   sites"`. Correct.
7. `src/sigtool.cpp` lines 501–505: non-verbose `verify` prints
   `"%d/%d signatures PASS\n"` — a single summary line. With 16 signatures,
   this produces `"16/16 signatures PASS\n"`. The regex `'16\s*/\s*16'` matches
   this. The fallback branch (count PASS lines ≥ `$expectedSignatureCount`)
   would yield 1 ≥ 16 = FALSE, but the first OR branch succeeds. **The test
   would PASS.**

**Conclusion:** Critical 1 is correctly fixed. The prescribed edits would make
`tests/test_sigtool.ps1` pass when sigtool reports 16/16 signatures.

---

## Critical Finding 2 — S9 flag parsing moved to main()-level (FIXED)

**Requirement:** S9 must describe main()-level flag parsing (NOT cmdExists-level),
mirroring the get-source pattern at lines 1716–1778. The old `reducedArgc != 4`
guard must be replaced by a positional-count check. `cmdExists` must accept
`bool caseInsensitive`.

**Evidence from the revised plan (lines 386–475):**

- Line 414: Explicitly states "the flag is parsed in `main()`, NOT here —
  `cmdExists` receives only already-parsed `typeStr`/`name` strings and has no
  access to `argv`/`reducedArgc`".
- Line 420: "Set `params.caseInsensitive = caseInsensitive` ... Do NOT scan
  `argv` here."
- Lines 416–418: `cmdExists` signature changed to add `bool caseInsensitive` as
  the 5th parameter.
- Lines 421–456: Full rewrite of the `exists` dispatch in `main()`:
  - Scans `reducedArgv` from index 2 for `-i`/`--case-insensitive` (boolean
    toggle, no value — simpler than get-source's `--output`).
  - Builds `exArgs` vector of remaining positional args.
  - Checks `exArgs.size() != 2` (replaces the old `reducedArgc != 4` guard).
  - Calls `cmdExists(exArgs[0], exArgs[1], tsPathWidePtr, jsonMode,
    caseInsensitive)`.
- Line 457: Explicitly states "The guard now accepts 2 positional args + the
  optional flag ... NOT a fixed `reducedArgc != 4`."

**Independent codebase verification:**

1. `src/tsdev_cli.cpp` lines 1682–1697: The current `exists` dispatch has
   `if (reducedArgc != 4)` which rejects any extra arg. With `-i` present,
   `reducedArgc == 5` → usage error. The plan's rewrite replaces this with the
   scan-and-extract approach. **Correct.**
2. `src/tsdev_cli.cpp` line 879–880: Current `cmdExists` signature is
   `int cmdExists(const std::string& typeStr, const std::string& name, const
   wchar_t* tsPathOverride, bool jsonMode)` — no `caseInsensitive`. The plan
   adds it as the 5th param. **Correct.**
3. `src/tsdev_cli.cpp` lines 1716–1778: The `get-source` dispatch uses the exact
   scan-and-extract pattern (scan for `--output`, build `gsArgs` vector, check
   `gsArgs.size() != 2`). The plan's `exists` rewrite mirrors this pattern.
   **Correct.**
4. `src/tsdev_cli.cpp` lines 1534–1596: Global flag pre-scan removes `--json`
   and `--ts-path` from `reducedArgv` BEFORE the subcommand dispatch. So for
   `exists strategy NAME -i --json`, `reducedArgv` =
   `["tsdev_cli", "exists", "strategy", "NAME", "-i"]` (`--json` stripped). The
   scan loop would set `caseInsensitive = true` and build `exArgs = ["strategy",
   "NAME"]`, `exArgs.size() == 2` → calls `cmdExists("strategy", "NAME", ...,
   true)`. **The command reaches `cmdExists` with `caseInsensitive=true`.**
5. The scan loop handles flags in any position (e.g., `exists -i strategy NAME`
   also works) since it iterates all args from index 2. **Robust.**

**Conclusion:** Critical 2 is correctly fixed. The prescribed edits would allow
`exists strategy NAME -i --json` to reach `cmdExists` with
`caseInsensitive=true`.

---

## Observation 1 — Destructor comment + loop bound (FIXED)

**Requirement:** S4 must update the destructor comment "Restore all 5 patch
sites" → "7" (line 588) and the loop bound 4→6.

**Evidence from the revised plan (lines 202–205):**

- Line 202: "update the comment `"Restore all 5 patch sites"` to `"Restore all 7
  patch sites"` (S4 grows the thread-check sites from 4 to 6; 1 persistence + 6
  thread-check = 7 total)."
- Lines 203–205: "change the loop bound from `4` to `6`: `for (int i = 0; i < 6;
  i++) {`"

**Independent codebase verification:**

1. `src/tsdev_lib.cpp` line 588: `// Restore all 5 patch sites to their original
   bytes ...` — matches the plan's "before" state.
2. `src/tsdev_lib.cpp` line 611: `for (int i = 0; i < 4; i++) {` — matches the
   plan's "before" state. Changing to `i < 6` is correct (arrays grow from [4]
   to [6]).
3. `src/tsdev_lib.h` lines 180–184: `m_threadCheckRvas[4]` and
   `m_threadCheckOriginalBytes[4]` — the plan (line 194–196) grows these to [6].
   With [6] arrays and `i < 6` loop, all 6 thread-check sites are restored.
   **Consistent.**

**Conclusion:** Obs 1 is correctly fixed.

---

## Observation 2 — `applyThreadCheckPatches()` line reference corrected (FIXED)

**Requirement:** S4's line reference must be corrected from "around line 85-88"
to "lines 1087–1096".

**Evidence from the revised plan (line 318):**

- "Update the comment block at the top of `applyThreadCheckPatches()` (lines
  1087–1096 in `src/tsdev_lib.cpp`) ... (Note: an earlier draft referenced
  "around line 85-88" — that is incorrect; line 85-88 is in the middle of
  unrelated path-discovery code. The actual `applyThreadCheckPatches()` comment
  block is at lines 1087–1096.)"

**Independent codebase verification:**

1. `src/tsdev_lib.cpp` line 1087: `void TsDevLib::applyThreadCheckPatches() {`
2. Lines 1087–1096: The comment block at the top of the function (describing the
   resolve→cache→applyPatch flow). **Matches the plan's corrected reference.**

**Conclusion:** Obs 2 is correctly fixed.

---

## Observation 3 — `getMyWorkBinPath()` path arg + SEH protection noted (FIXED)

**Requirement:** S4 must include a note that `getMyWorkBinPath()` is the path arg
and is SEH-protected.

**Evidence from the revised plan (lines 256–272):**

- Line 256: `const char* path = getMyWorkBinPath();`
- Lines 258–263: Explicit note: "getMyWorkBinPath() returns the discovered
  MyWork\Bin path, which matches the RE's example path for
  _LoadMasterRecsEX@16. The exact path format expected by FUN_101a9650
  (GetWarehouse) is not 100% confirmed by the RE, but the call is SEH-protected
  via callLoadMasterRecsEx below, so a wrong/invalid path fails gracefully
  (returns {exists: false}) rather than crashing."
- Lines 225–242: The `callLoadMasterRecsEx` SEH-protected helper is defined.
- Lines 268–272: The actual call uses `callLoadMasterRecsEx(...)` which wraps the
  function call in `__try`/`__except(EXCEPTION_EXECUTE_HANDLER)`.

**Conclusion:** Obs 3 is correctly fixed.

---

## Additional Checks

### No new issues introduced

The fixes are surgical and targeted:
- S1's scope grew from 2 files to 3 files (adding `test_sigtool.ps1`), which is
  justified as "all 3 edits are part of a single logical count-bump" (line 90).
- S9's rewrite replaces the contradictory cmdExists-parses-argv instruction with
  a clear main()-level scan-and-extract pattern.
- S4's fixes are comment/line-reference corrections that don't change the
  implementation logic.

No new scope creep, no new contradictions, no new atomicity violations.

### Internal consistency

- S1 lists 3 files (line 90): `signatures/v9.50.01.3344.json`,
  `tools/sigcodegen.py`, `tests/test_sigtool.ps1`. **Consistent.**
- S9 describes main()-level parsing (lines 421–456) with the scan-and-extract
  pattern, `cmdExists` accepts `bool caseInsensitive`, `doExists` branches on
  it. **Consistent.**
- S4 has the destructor comment/loop fix (lines 202–205), the corrected line
  reference (line 318), and the SEH-protection note (lines 258–263).
  **Consistent.**

### Atomicity HARD RULE

All subtasks remain atomic:
- S1: edit 3 files (one logical count-bump) + verification (codegen + sigtool
  test — both are pre-existing tools, not a C++ build + full regression). The
  sigtool test is a targeted verification of S1's own change, not the full
  regression suite (S17 does that). **PASS.**
- S2: write test (edit only). S3: build + run (confirm RED). S4: implement
  (edit only). S5: build + run (confirm GREEN). **No edit+build+run
  combination. PASS.**
- S7: write test (edit only). S8: build + run (confirm RED). S9: implement
  (edit only). S10: build + run (confirm GREEN). **PASS.**
- S12: write tests (edit only). S13: run tests (confirm RED). S14: implement
  (edit only). S15: run tests (confirm GREEN). **PASS.**
- S17: run full regression (run only). S18: update docs (edit only). S20: E2E
  verification (run only). **PASS.**

### Dependency graph

The dependency graph (lines 33–73) and execution order (line 80) remain valid:
linear S1→S20 with no circular or missing dependencies. S2 depends on S1
(signatures must compile), S4 depends on S3 (RED confirmed), S9 depends on S8
(RED confirmed), S14 depends on S13 (RED confirmed). **PASS.**

---

## Non-blocking observations (not disapproval reasons)

1. **S1 verification may require a rebuild of sigtool.exe.** The plan says "Run
   `python tools/sigcodegen.py ...` (or just rebuild the project)." If the
   implementer runs only codegen without rebuilding, `signatures.gen.h` is
   regenerated but `sigtool.exe` still has the old header compiled in. However,
   sigtool loads signatures from the JSON file at runtime (not from the compiled
   header), so the test would still pass (sigtool reads 16 signatures from the
   JSON and prints "16/16"). This is a minor clarification issue, not a blocker.

2. **S9's `cmdExists` signature change has no default value for
   `caseInsensitive`.** The plan shows `bool caseInsensitive` without a default.
   This is safe because `cmdExists` has only one call site (the exists dispatch
   in `main()`), which the plan rewrites to pass the 5th arg explicitly. No
   other call sites exist. Non-blocking.

---

## VERDICT: APPROVE

All 2 critical findings and 3 non-blocking observations from the original review
have been correctly fixed in the revised plan. Each fix was independently
verified against the actual codebase. No new issues were introduced. The plan is
internally consistent, the atomicity HARD RULE holds, and the dependency graph
is valid. The plan is ready for execution.
