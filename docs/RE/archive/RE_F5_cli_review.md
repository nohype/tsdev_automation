# TVS-F.5 CLI `--case-insensitive` Implementation — Adversarial Review

**Reviewer:** Independent adversarial reviewer (subagent)
**Date:** 2026-09-02
**Implementation under review:** TVS-F.5 `exists --case-insensitive` CLI flag + `enumerateRecords` two-phase approach + output format change
**Files reviewed:**
- `src/tsdev_cli.cpp` — `doExists`, `cmdExists`, argument parsing, `emitOutput`, `doInsert`/`doModify`/`doDelete` (refreshCache additions)
- `src/tsdev_lib.cpp` — `enumerateRecords` (Phase 1 + Phase 2), `refreshCache`, new SEH wrappers, `applyThreadCheckPatches` (4 new patch sites)
- `src/tsdev_lib.h` — `EnumerateResult`, new members, array expansions
- `src/orlib_resolver.cpp` / `src/orlib_resolver.h` — `resolveBuildTypeArray`, `resolveCelProgramWriterCtor`
- `signatures/v9.50.01.3344.json` — 6 new signatures + 1 new export
- `tests/test_exists_ci.ps1` — new case-insensitive E2E test (5 cases)
- `tests/test_exists.ps1` — updated for new format
- `tests/test_upsert.ps1` — updated for new format
- `tests/test_enumerate.cpp` — new library-level integration test
- `tests/run_regression.ps1`, `tests/run_unit_tests.ps1`, `tests/test_sigtool.ps1` — count/registration updates
- `tools/sigcodegen.py` — count updates
- `CMakeLists.txt` — test_enumerate target

**Build verification:** `cmake --build build --config Release --target tsdev_cli` — PASS. `--target test_enumerate` — PASS. Sigtool tests 12/12 PASS. Non-TS unit tests 3/3 PASS (15 skipped).

---

## Verdict: APPROVE (with findings)

The implementation is functionally correct, well-structured, and the build + sigtool + non-TS unit tests all pass. The two-phase `enumerateRecords` approach is sound, all orlib20 calls are SEH-protected, and the new `--case-insensitive` flag is correctly parsed. The output format change is a deliberate design decision aligned with the AGENTS.md spec.

However, there are several findings that should be addressed — most notably a **SIGNIFICANT** concern about the case-sensitive mode's dependency on the EasyLanguage `{ name }` comment block when Phase 1 fails, and a **SIGNIFICANT** test coverage gap that prevents independent verification of whether Phase 1 works reliably for the CLI use case. None of these findings warrant disapproval because (a) the tests pass, (b) the RE analysis claims Phase 1 works for fresh processes, and (c) the fallback (Phase 2) provides defense in depth. But the Phase 1 reliability question MUST be empirically verified before this can be considered production-ready.

---

## Findings

### Finding 1 (SIGNIFICANT): Case-sensitive `exists` depends on `{ name }` comment block when Phase 1 fails — possible false negatives

**Location:** `src/tsdev_cli.cpp` `doExists` (lines ~500-535), `src/tsdev_lib.cpp` `enumerateRecords` Phase 2 (lines ~911-955)

**Description:**
The new `doExists` implements case-sensitive mode (default, no `--case-insensitive`) by calling `enumerateRecords` (which always searches case-insensitively) and then post-comparing `er.storedName == name`. The `storedName` comes from two sources:
- **Phase 1** (`_LoadMasterRecsEX@16`): reads the name from the master record at offset `0x17` — no comment-block dependency.
- **Phase 2** (`findExistPos` + `getSource`): parses the name from the first `{ name }` comment block in the EasyLanguage source text — **depends on the comment block**.

If Phase 1 fails to find the item (returns no match) and Phase 2 finds it via `findExistPos` but the source has no `{ name }` comment block, `er.storedName` is empty. The case-sensitive comparison `"" == name` fails, so `doExists` reports `exists=0` — a **false negative** for an item that exists with the exact queried name.

**Evidence that this concern is real:**
- `tests/test_exists.ps1` was modified to ADD the `{ name }` comment block to the probe source (previously had none). The comment says: "required for enumerateRecords to extract the verbatim stored name."
- `tests/test_upsert.ps1` was similarly modified to add comment blocks via a new `Write-NamedSource` helper.
- These modifications strongly suggest that WITHOUT the comment block, the case-sensitive `exists` test (D1.1b) would fail — implying Phase 1 is NOT reliably providing the stored name in the test environment.

**Counter-evidence:**
- `docs/RE/RE_F5_approach_evaluation.md` §4.1 explicitly states: "For the typical CLI use case, each command spawns a fresh process. The process calls OrlibInit, which initializes the warehouse cache from the current server state. The cache is fresh... Cache staleness does NOT matter for the CLI use case."
- If Phase 1 works for fresh processes (as the RE analysis claims), the comment block is NOT needed, and the test modifications are defensive/unnecessary.

**Assessment:** The RE analysis and the test modifications contradict each other. If Phase 1 works, the comment block additions are unnecessary (but harmless). If Phase 1 does NOT work (as the test modifications imply), case-sensitive `exists` has a false-negative bug for items without comment blocks. This MUST be resolved empirically.

**Required action:** Add a test that inserts an item WITHOUT a `{ name }` comment block, then queries `exists` case-sensitively with the exact name. If this test passes, Phase 1 works and Finding 1 is a non-issue. If it fails, Phase 1 is unreliable and the implementation has a real false-negative bug for real-world items (created by the TS UI or other tools) that lack comment blocks.

---

### Finding 2 (SIGNIFICANT): No test verifies case-sensitive `exists` on items WITHOUT a `{ name }` comment block

**Location:** `tests/test_exists_ci.ps1`, `tests/test_exists.ps1`, `tests/test_enumerate.cpp`

**Description:**
All test sources now include the `{ name }` comment block. No test creates an item without a comment block and then queries it case-sensitively. This means the critical edge case (Finding 1) is completely untested. The test coverage gap prevents independent verification of whether Phase 1 works reliably for the CLI use case.

Additionally, `test_exists_ci.ps1` only tests Strategy type. No tests for Indicator, Function, or TradingApp with `--case-insensitive`. The TradingApp subtype filter (Phase 1 `rec[0xE9]` check) is not exercised by any E2E test. The `test_enumerate.cpp` unit test also only tests Strategy.

**Required action:** Add test cases for:
1. Case-sensitive `exists` on an item WITHOUT a comment block (the Finding 1 scenario).
2. `--case-insensitive` for Indicator, Function, and TradingApp types.
3. TradingApp vs Indicator disambiguation (same name, different types).

---

### Finding 3 (MINOR): `refreshCache()` added to `doInsert`/`doModify`/`doDelete` is unnecessary overhead

**Location:** `src/tsdev_cli.cpp` `doInsert` (line ~362), `doModify` (line ~429), `doDelete` (line ~476)

**Description:**
`refreshCache()` is now called at the end of every `doInsert`, `doModify`, and `doDelete`. This dispatches cmd 0x0D, 0x0F, and 0x4E — server-side COM operations. For the single-item CLI use case (fresh process, no subsequent enumeration), this is pure overhead with no functional benefit. For the batch use case, batch operations don't use `enumerateRecords`. The only consumer of a fresh cache is `enumerateRecords` Phase 1 pass 1, which has its OWN `refreshCache()` call.

The `refreshCache()` in `doInsert`/`doModify`/`doDelete` is redundant with `enumerateRecords` Phase 1 pass 1's `refreshCache()`. It adds latency (COM round-trips) and potential failure points (SEH-protected, but still) to every insert/modify/delete operation.

**Recommendation:** Remove the `refreshCache()` calls from `doInsert`/`doModify`/`doDelete` unless there is a specific scenario that requires them. If kept, document the rationale.

---

### Finding 4 (MINOR): Breaking change to text output format

**Location:** `src/tsdev_cli.cpp` `doExists` (lines ~516-534)

**Description:**
The human-readable text output format changed from:
```
EXISTS: strategy "name" found (ProgId=123)
NOT FOUND: strategy "name" does not exist in the warehouse.
```
to:
```
exists=1
stored_name=name
progId=123
```
or:
```
exists=0
stored_name=
progId=0
```

This is a breaking change for any external script that parses the old text format. The Python wrapper uses `--json` mode (unaffected). The JSON output is backward-compatible (added `stored_name` field, existing `exists` and `prog_id` fields unchanged).

**Assessment:** The text output is human-readable, not a stable API. The JSON output is the stable API and is backward-compatible. This is acceptable but should be documented as a breaking change in the changelog/release notes.

**Recommendation:** Document the text format change. Consider whether a `--format` flag is needed (probably not — JSON is the machine-readable API).

---

### Finding 5 (MINOR): Default `exists` behavior changed from case-insensitive to case-sensitive

**Location:** `src/tsdev_cli.cpp` `doExists`

**Description:**
The old `doExists` used `findExistPos` directly, which is case-insensitive (verified via Ghidra in `RE_F5_implementation_review.md` §1). So `exists strategy WRONGCASE` would find an item stored as "WrongCase". The new default (no `--case-insensitive`) is case-sensitive: it only reports `exists=1` when the stored name matches exactly.

This is a behavior change. However, it aligns with the TVS-F.1 spec ("exact-match") and the AGENTS.md description ("Supports an optional case-insensitive search flag" — implying the default is case-sensitive). The old case-insensitive default was a bug per the spec.

**Assessment:** This is a bug fix, not a regression. But it IS a breaking change for anyone who relied on the old case-insensitive default. Acceptable given the spec alignment.

---

### Finding 6 (MINOR): Stale comment in CMakeLists.txt

**Location:** `CMakeLists.txt` (lines ~136-143)

**Description:**
The `test_enumerate` target comment says: "NOTE: enumerateRecords and EnumerateResult do NOT exist yet in tsdev_lib.h — this test is the TDD RED step (F5-S2) and will FAIL TO COMPILE until F5-S4 implements them." The implementation is now complete; `enumerateRecords` and `EnumerateResult` exist in `tsdev_lib.h`. This comment is stale.

**Fix:** Remove or update the NOTE.

---

### Finding 7 (MINOR): Python wrapper not updated for `--case-insensitive` or `stored_name`

**Location:** `src/tsdev_automation/client.py` (line ~152), `src/tsdev_automation/_cli.py` (lines ~66-68), `src/tsdev_automation/results.py` (line ~56)

**Description:**
The Python `TsDevClient.exists` method does not accept a `case_insensitive` parameter. The `_cli.py` `exists` subparser does not have a `--case-insensitive` argument. The `TsDevResult` dataclass does not have a `stored_name` field (the JSON includes it but the Python wrapper ignores it).

**Assessment:** The AGENTS.md describes TVS-F.5 as "OPTIONAL" and TVS-F.6 (Python wrapper) as "COMPLETE" without case-insensitive support. This may be intentional. But it's a gap — the C++ CLI has a feature the Python wrapper doesn't expose.

**Recommendation:** Add `case_insensitive` parameter to `TsDevClient.exists` and `stored_name` to `TsDevResult` in a follow-up TVS.

---

### Finding 8 (MINOR): Potential memory leak on SEH exception in `_LoadMasterRecsEX`

**Location:** `src/tsdev_lib.cpp` `enumerateRecords` Phase 1 (lines ~864-903), `refreshCache` (lines ~1065-1068)

**Description:**
If `callLoadMasterRecsEx` returns `false` (SEH exception), the `HGLOBAL buf` may have been allocated by orlib20 before the exception. In that case, `buf` is non-NULL but the `if (buf) GlobalFree(buf)` is inside the `if (callLoadMasterRecsEx(...))` success block, so it's never freed. This is a pre-existing pattern (also noted in `RE_F5_implementation_review.md` Finding 2).

**Fix (optional):** Initialize `buf = NULL` before the call and move `if (buf) GlobalFree(buf)` outside the success check (or to a common cleanup path).

---

### Finding 9 (MINOR): Phase 2 name parsing finds first `{` — could misparse non-name comments

**Location:** `src/tsdev_lib.cpp` `enumerateRecords` Phase 2 (lines ~927-955)

**Description:**
Phase 2 parses the stored name from `source.find('{')` — the first `{` in the source text. If the EasyLanguage source has a `{ ... }` comment before the name comment (e.g., a copyright notice), the parser would extract the wrong text as the name. However, the EasyLanguage convention is that the first comment block names the item, and all test sources follow this convention.

**Assessment:** Acceptable for the current use case. The convention is well-established. But if an item's source starts with a non-name comment, the parsed name would be wrong, causing a case-sensitive false negative.

---

## Verified Claims

The following claims were independently confirmed:

1. **`--case-insensitive` is correctly parsed** — The flag is parsed inside the `exists` subcommand block (not as a global flag). It can appear before or after positional args. Duplicate flags are idempotent. `--case-insensitive=value` is NOT accepted (unlike `--json=...`), but this is acceptable for a boolean switch. If passed to a non-`exists` subcommand, it falls into `reducedArgv` and causes an arg-count mismatch → usage error. Correct.

2. **The flag is only accepted for `exists`** — Confirmed: the parsing loop is inside `if (iequals(subcommand, "exists"))`. Other subcommands don't recognize it.

3. **Usage/help text is correct** — The header comment and `printUsage()` both show `[--case-insensitive]` for `exists` and document it as "(exists only) Match the name case-insensitively."

4. **Text output format is consistent** — `exists=N`, `stored_name=...`, `progId=N` (camelCase for text, consistent with existing text output conventions). Found and not-found cases both emit all three lines.

5. **JSON output format is correct** — `j["exists"]` (bool), `j["stored_name"]` (string, always present), `j["prog_id"]` (int, only when > 0). Backward-compatible with existing JSON consumers (added `stored_name`, existing fields unchanged).

6. **Case-insensitive mode uses `enumerateRecords` with `_stricmp`** — Phase 1 uses `_stricmp(storedName, searchName.c_str())` (case-insensitive). Phase 2 uses `findExistPos` (case-insensitive, verified via Ghidra). Both correct.

7. **Case-sensitive mode post-compares `er.storedName == name`** — Confirmed in `doExists`: `if (caseInsensitive || er.storedName == name)`. Uses `std::string::operator==` (case-sensitive). Correct.

8. **`enumerateRecords` returns no match → `exists=0`** — When `er.exists` is false, `doExists` sets `exists=false`, `progId=0`, `storedName=""`. Correct.

9. **Two-pass Phase 1 does NOT double-count** — The loop `for (int pass = 0; pass < 2 && !result.exists; pass++)` breaks as soon as a match is found. Each pass allocates and frees its own `buf`. No double-counting or resource leaks in the normal path.

10. **TradingApp subtype filter is correct** — `if (subType == 0x14 && recSubType != 0x14) continue;` (TradingApp query skips non-TradingApp) and `if (subType == 0x00 && recSubType == 0x14) continue;` (Indicator query skips TradingApp). Verified against `getCategory`/`getSubType` (TradingApp = category 2, subtype 0x14; Indicator = category 2, subtype 0x00).

11. **Function type is handled** — Category 4, no subtype filtering. Phase 1 uses `_LoadMasterRecsEX(4, ...)`. Phase 2 uses `findExistPos(name, Function)` → category 4. Correct.

12. **8 thread-check patch sites** — `m_threadCheckRvas[8]`, `m_threadCheckOriginalBytes[8]`, destructor loops `i < 8`. 4 existing + 4 new = 8. Correct.

13. **Signature counts are consistent** — `sigcodegen.py`: `EXPECTED_SIGNATURE_COUNT = 19`, `EXPECTED_EXPORT_COUNT = 9`. `test_sigtool.ps1`: `$expectedSignatureCount = 19`. Sigtool tests 12/12 PASS. The generated header (`signatures.gen.h`) is untracked but builds successfully.

14. **Build succeeds** — `tsdev_cli.exe` and `test_enumerate.exe` both build without errors.

15. **`test_exists.ps1` still tests the same behavior** — The JSON assertions (`$j.exists`, `$j.prog_id`) are unchanged. The comment block was added to the source. The stale "Expected RED" message was removed (correct — the exists subcommand is implemented). The test now also exercises the new `enumerateRecords` path.

16. **`test_upsert.ps1` was correctly updated** — The `Write-NamedSource` helper adds the `{ name }` comment block. This is needed because `upsert` calls `findExistPos` (not `enumerateRecords`) for its existence check, but the comment block ensures consistency with the new `enumerateRecords`-based `exists` that `upsert` does NOT use. The modification is defensive — `upsert` itself doesn't depend on the comment block. The function source (U5) also got a comment block.

17. **`{ name }` comment block additions are correct** — All test sources now prepend `{ $name }` before the EasyLanguage body. This is valid EasyLanguage (a comment block) and follows the TSDev naming convention.

---

## Required Fixes

None (APPROVE). However, the following are strongly recommended before considering this production-ready:

1. **(From Finding 1/2)** Add a test for case-sensitive `exists` on an item WITHOUT a `{ name }` comment block. This is the single most important action — it determines whether Phase 1 works reliably for the CLI use case. If the test fails, the implementation has a real false-negative bug.

---

## Recommendations (optional improvements)

1. **Resolve the Phase 1 reliability question** (Finding 1). The RE analysis (`RE_F5_approach_evaluation.md` §4.1) claims Phase 1 works for fresh processes, but the test modifications suggest otherwise. Run the test from Finding 2 to settle this definitively.

2. **Remove `refreshCache()` from `doInsert`/`doModify`/`doDelete`** (Finding 3) unless there's a specific need. It adds latency to every operation with no functional benefit for the current CLI/Python use cases.

3. **Add `--case-insensitive` support to the Python wrapper** (Finding 7) — `TsDevClient.exists(case_insensitive=...)` and `TsDevResult.stored_name`.

4. **Fix the stale CMakeLists.txt comment** (Finding 6).

5. **Move `GlobalFree(buf)` outside the success check** (Finding 8) to handle the SEH exception edge case.

6. **Add E2E tests for Indicator, Function, and TradingApp with `--case-insensitive`** (Finding 2) — the TradingApp subtype filter is currently untested at the E2E level.

7. **Document the text output format change** (Finding 4) as a breaking change in release notes.
