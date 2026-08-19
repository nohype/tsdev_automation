# TVS-F.5 enumerateRecords Implementation — Adversarial Review

**Reviewer:** Independent adversarial reviewer (subagent with Ghidra MCP access)
**Date:** 2026-09-02
**Implementation under review:** TVS-F.5 `enumerateRecords` + `refreshCache` rewrite
**Files reviewed:**
- `src/tsdev_lib.h` — header changes (new members, array expansions)
- `src/tsdev_lib.cpp` — `refreshCache()` and `enumerateRecords()` rewrite
- `signatures/v9.50.01.3344.json` — new `FUN_101cfce0` signature
- `tools/sigcodegen.py` — count updates
- `tests/test_sigtool.ps1` — count updates
- `tests/test_enumerate.cpp` — test changes

**Binary:** orlib20.dll v9.50.01.3344 (Ghidra program `/orlib20.dll`, image base `0x10000000`)

---

## Verdict: APPROVE

The implementation is correct, well-structured, and production-ready. The two-phase
approach is sound, the FindExistPos case-insensitivity claim is independently verified
via Ghidra decompilation, and all orlib20 calls are SEH-protected with proper resource
balancing. Two MINOR issues and one non-blocking observation are noted below; none
warrant disapproval.

---

## Findings

### Finding 1 (MINOR): Stale comment in test_sigtool.ps1 — "18 signatures"

**Location:** `tests/test_sigtool.ps1:163`

The comment says "report all 18 signatures as PASS" but the expected signature count
was updated to 19 (line 40: `$expectedSignatureCount = 19`, and the ExtraCheck on
line 174 correctly uses `$expectedSignatureCount`). The actual test logic is correct;
only the comment is stale.

**Fix:** Change "18 signatures" to "19 signatures" on line 163.

---

### Finding 2 (MINOR): Potential memory leak on SEH exception in _LoadMasterRecsEX

**Location:** `src/tsdev_lib.cpp:856-896` (enumerateRecords Phase 1)

If `callLoadMasterRecsEx` returns `false` (SEH exception during the orlib20 call),
the `HGLOBAL buf` may have been allocated by orlib20 before the exception occurred.
In that case, `buf` is non-NULL but is never freed — the `if (buf) GlobalFree(buf)`
is inside the `if (callLoadMasterRecsEx(...))` success block.

The same pattern exists in `refreshCache()` (line 1065-1068) and is a pre-existing
pattern in the codebase. In practice, SEH exceptions in orlib20 are rare, so this
is a low-risk leak.

**Fix (optional):** Move `if (buf) GlobalFree(buf)` outside the success check, or
initialize `buf = NULL` and always free it at the end of the block.

---

### Finding 3 (NON-BLOCKING): refreshCache() cmd 0x0D with type=0, param=0 is unverified

**Location:** `src/tsdev_lib.cpp:1120-1132`

The `refreshCache()` function calls `FUN_101cfce0` (cmd 0x0D dispatcher) with
`type=0, param=0` to obtain a fresh COM interface. The RE findings
(`RE_F5_cache_fix_findings.md` §1.5) show that `FUN_101cfce0` takes `param_2` (type)
and `param_3` (value) from `[module+0x15]` (ProgId) and `[module+0x18]` in the
`FUN_101c09f0` context. Passing `type=0, param=0` is not directly verified to return
a valid COM interface.

Additionally, `RE_F5_warehouse_com_interface.md` §6 reports that `[warehouse+0x5E]`
remains NULL after all our operations (createEntry, saveSource, verifyModule) in
server mode, which contradicts the earlier `RE_F5_cache_refresh_feasibility.md`
claim that `[warehouse+0x5E]` is set after INSERT/MODIFY/DELETE.

**Impact:** If `refreshCache()` fails to obtain a valid COM interface, Phase 1
(`_LoadMasterRecsEX@16`) will use a stale cache and may not find newly created
records. However, the implementation correctly falls back to Phase 2
(`FindExistPos` + `getSource`), which does NOT depend on `[warehouse+0x5E]` or
the COM interface. The two-phase design provides defense in depth.

**No fix required** — the fallback to Phase 2 handles this case. The defensive
code in `refreshCache()` (restoring the old interface if cmd 0x0D fails, returning
early if `[warehouse+0x5E]` is NULL after cmd 0x0F) prevents crashes.

---

## Verified Claims

The following claims were independently verified:

### 1. FindExistPos IS case-insensitive (VERIFIED via Ghidra)

This is the most critical claim. Verified by decompiling the BST lookup chain:

**FUN_101e3db0** (BST lookup, RVA 0x1E3DB0) — called by FindExistPos in server mode:
1. Copies the search name into a local string (`FUN_10009990`)
2. **Normalizes the search name to lowercase** via `FUN_1002b700`
3. Compares the lowercase search key against BST node names

**FUN_1002b700** (RVA 0x1002B700) — the normalization function:
- Sets `local_14 = tolower_exref` (a function pointer to `tolower`)
- Calls `FUN_1002b680(puVar1, &local_14)` which applies `tolower` to each character

**FUN_1002b680** (RVA 0x1002B680) — the character transform:
```c
do {
    uVar1 = (*(code *)*param_4)((int)(char)param_3[iVar3]);  // call tolower(char)
    *param_3 = uVar1;                                         // store back
    ...
} while (iVar4 != iVar2);
```

**BST insertion also normalizes to lowercase:** In the BST miss fallback path
within `FUN_101e3db0`, after cmd 0x10 finds the item:
```c
FUN_101c4990();           // get ProgId
FUN_101e30f0();           // BST lookup (check if already in BST)
uVar5 = FUN_101c36d0();   // get name
FUN_1002c560(uVar5);      // copy name to string
FUN_1002b700(local_64);   // NORMALIZE TO LOWERCASE (same function!)
FUN_1002bdb0();           // ...
FUN_101e35a0();           // insert into BST
```

Both the search key AND the BST node names are normalized to lowercase via
`tolower`, making the comparison case-insensitive for ALL names (not just
all-lowercase names).

### 2. Two-phase approach is correct

- **Phase 1** (`_LoadMasterRecsEX@16` + `_stricmp`): Correctly enumerates cached
  records case-insensitively. The `_stricmp` comparison is inherently
  case-insensitive. The TradingApp subtype filter (checking `rec[0xE9]`) is
  correct for distinguishing Indicators from TradingApps within category 2.

- **Phase 2** (`FindExistPos` + `getSource`): Correctly handles newly created
  records. `FindExistPos` uses the in-memory BST (updated immediately by
  `createEntry`), which is case-insensitive (verified above). `getSource`
  parses the verbatim name from the EasyLanguage source comment `{ name }`.

- **Fallback**: Phase 2 triggers when Phase 1 doesn't find a match
  (`result.exists` is false after Phase 1). This is correct — Phase 1 may
  fail to find newly created records if the cache is stale, and Phase 2
  covers this gap.

### 3. Record layout offsets are correct

Verified against `RE_F5_enumeration_findings.md` §4 (independently reviewed in
`RE_F5_enumeration_rereview.md`):
- `kRecordSize = 0x10A` (266 bytes) ✓
- `kNameOffset = 0x17` (inline long name) ✓
- `kProgIdOffset = 0x00` (short) ✓
- `kSubTypeOffset = 0xE9` (category/subtype byte) ✓

### 4. FUN_101cfce0 signature is needed and correct

- **Used in implementation:** `refreshCache()` calls `callCmd0DDispatcher`
  which invokes `FUN_101cfce0` to obtain a fresh COM interface via cmd 0x0D.
- **Pattern uniqueness:** The pattern ends with `C7 46 04 0D 00 00 00`
  (cmd 0x0D), distinguishing it from `FUN_101d1fa0` (cmd 0x17). Verified
  in `RE_F5_cache_fix_findings.md` §1.5.
- **INT3 patch at offset 40:** Confirmed by disassembly in the RE findings.
- **sigcodegen count:** `EXPECTED_SIGNATURE_COUNT = 19` ✓
- **test_sigtool count:** `$expectedSignatureCount = 19` ✓
- **Generated header:** `kSignatures_v9_50_01_3344, 19` ✓

### 5. SEH protection is comprehensive

All orlib20 calls in `enumerateRecords` and `refreshCache` are SEH-protected:
- `callLoadMasterRecsEx` — SEH wrapper ✓
- `callFindExistPos` — SEH wrapper ✓
- `callLoadTextFileEx` — SEH wrapper (in `getSource`) ✓
- `callGetWarehouse` — SEH wrapper ✓
- `callBuildTypeArray` — SEH wrapper ✓
- `callCelProgramWriterCtor` — SEH wrapper ✓
- `callCmd0FDispatcher` — SEH wrapper ✓
- `callCmd0DDispatcher` — SEH wrapper ✓
- `callCmdDispatcher` — SEH wrapper ✓

### 6. Resource balancing is maintained

- `GlobalLock` / `GlobalUnlock` / `GlobalFree` in correct order in both
  `enumerateRecords` and `refreshCache` ✓
- `GlobalFree(buf)` is called in all success paths ✓
- (See Finding 2 for the SEH exception edge case)

### 7. No debug logging or temporary diagnostics

- `enumerateRecords` (lines 843-941): No debug logging ✓
- `refreshCache` (lines 1031-1139): No debug logging ✓
- The `TSDEV_DBG` macro is only used in `deleteEntry` (pre-existing) ✓

### 8. m_threadCheckRvas array size is correct

- Header: `uintptr_t m_threadCheckRvas[8]` and `unsigned char m_threadCheckOriginalBytes[8]` ✓
- `applyThreadCheckPatches`: applies 8 patches (tc1 through tc8) ✓
- Destructor: `for (int i = 0; i < 8; i++)` ✓

### 9. Source text parsing is robust

- Finds first `{` and first `}` after it ✓
- Trims leading/trailing whitespace ✓
- Handles empty comment blocks (`{}`) gracefully (storedName remains empty) ✓
- Handles missing comment blocks gracefully (storedName remains empty) ✓
- If parsing fails, `exists=true` and `progId>0` are still set (degraded but
  not incorrect) ✓

### 10. All 4 component types are handled

- Strategy (category 0): Phase 1 uses `_LoadMasterRecsEX@16(0, ...)`, Phase 2
  uses `findExistPos(name, Strategy)` → category 0 ✓
- Indicator (category 2, subtype 0): Phase 1 uses `_LoadMasterRecsEX@16(2, ...)`
  with subtype filter (skip 0x14), Phase 2 uses `findExistPos(name, Indicator)`
  → category 2 ✓
- Function (category 4): Phase 1 uses `_LoadMasterRecsEX@16(4, ...)`, Phase 2
  uses `findExistPos(name, Function)` → category 4 ✓
- TradingApp (category 2, subtype 0x14): Phase 1 uses `_LoadMasterRecsEX@16(2, ...)`
  with subtype filter (only 0x14), Phase 2 uses `findExistPos(name, TradingApp)`
  → category 2 ✓

### 11. TradingApp subtype filter is correct

```cpp
if (category == 2) {
    unsigned char recSubType = (unsigned char)rec[kSubTypeOffset];
    if (subType == 0x14 && recSubType != 0x14) continue;  // TradingApp: skip non-TradingApp
    if (subType == 0x00 && recSubType == 0x14) continue;  // Indicator: skip TradingApp
}
```

This correctly filters records within category 2 (which includes both Indicators
and TradingApps) based on the subtype byte at offset 0xE9. Verified against
`RE_F5_enumeration_findings.md` §3.2 (category 2 includes 0x1402 = TradingApp).

---

## Test Quality Assessment

### What the test covers (test_enumerate.cpp)
- ✓ Creates a new Strategy entry (createEntry + saveSource + verifyModule)
- ✓ Queries with UPPERCASE name, asserts case-insensitive match (exists=true,
  storedName=verbatim lowercase name, progId>0)
- ✓ Queries nonexistent name, asserts not found (exists=false, storedName="", progId=0)
- ✓ Cleans up after itself (deleteEntry)
- ✓ Runs on a 64MB stack worker thread (correct for EL compiler + thread-affine EDIT control)

### Gaps in test coverage (non-blocking)
1. **Only Strategy type tested:** No tests for Indicator, Function, or TradingApp.
   The TradingApp subtype filter (Phase 1) is not exercised.
2. **Only all-lowercase name tested:** The test uses "zzz_test_ci_probe" (all lowercase).
   A mixed-case name (e.g., "MyStrategy") would more thoroughly verify case-insensitivity.
   However, the Ghidra verification confirms that `tolower` normalization applies to
   ALL characters, so mixed-case names ARE handled correctly.
3. **No test for pre-existing records:** The test only tests newly created records.
   Pre-existing records (from a previous process) would exercise Phase 1 without
   Phase 2 fallback.

These gaps are acceptable for the current TVS scope. The Ghidra verification of
FindExistPos case-insensitivity provides confidence that the implementation works
for all name casings and all component types.

---

## Required Fixes

None (APPROVE).

---

## Recommendations (optional improvements)

1. **Fix the stale comment** in `tests/test_sigtool.ps1:163` — change "18 signatures"
   to "19 signatures".

2. **Add a mixed-case name test** to `test_enumerate.cpp` — e.g., create a strategy
   named "Zzz_Test_Mixed_Case" and query with "ZZZ_TEST_MIXED_CASE". This would
   more thoroughly verify the `tolower` normalization in FindExistPos.

3. **Add a TradingApp test** to `test_enumerate.cpp` — create a TradingApp and verify
   that the subtype filter in Phase 1 correctly distinguishes it from Indicators.

4. **Consider adding a test for pre-existing records** — query an item that existed
   before the process started (e.g., a built-in strategy). This would exercise Phase 1
   without Phase 2 fallback.

5. **Move `GlobalFree(buf)` outside the success check** in `enumerateRecords` Phase 1
   to handle the SEH exception edge case (Finding 2).
