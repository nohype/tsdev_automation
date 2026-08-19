# Adversarial Review: RE_F5_cache_fix_findings.md

**Reviewer**: Independent adversarial reviewer (subagent)
**Date**: 2026-08-26
**Document under review**: `docs/RE/RE_F5_cache_fix_findings.md`
**Verdict**: **DISAPPROVE** — Two CRITICAL correctness bugs that would cause the enumeration to miss user-created items.

---

## Summary

The document recommends switching from `_LoadMasterRecsEX@16` (cached, command 0x08) to
`_GetMasterRecByType@16` (live, command 0x0D) for the case-insensitive `exists` feature.
The core approach (live lookup via an exported function + one new INT3 patch) is sound and
well-researched. However, the document contains **two CRITICAL bugs** in the implementation
guidance that would cause the enumeration to silently miss user-created items:

1. **Wrong subtype mapping** — The proposed subtypes (Strategy=2, Indicator=5) do not match
   the subtypes used by `createEntry` (Strategy=0, Indicator=0). This would look up the wrong
   expanded warehouse types and miss all user-created Strategy and Indicator items.
2. **Unreliable count mechanism** — The "index" parameter is likely the ProgId (not a
   sequential 0-based index), but the document assumes dense sequential indexing. The
   10-miss gap handler is insufficient for large ProgId gaps caused by deletions.

Additionally, there is a **SIGNIFICANT** issue: `_LoadMasterRecsEX` enumerates multiple type
codes per category, but the proposed approach uses a single subtype, missing items with other
subtypes of the same category.

---

## Detailed Findings

### Finding 1 (CRITICAL): Subtype Mapping is WRONG

**What the document claims** (§2.6, §5.5):
- Strategy: subtype = **2** → `FUN_101d4280(0, 2)` = 0x200
- Indicator: subtype = **5** → `FUN_101d4280(2, 5)` = 0x502
- Function: subtype = 0 → `FUN_101d4280(4, 0)` = 4
- TradingApp: subtype = 0x14 → `FUN_101d4280(2, 0x14)` = 0x1402

The document explicitly states: *"The existing `getSubType()` returns 0 for Strategy and
Indicator, which would NOT work with `_GetMasterRecByType@16`."*

**What is actually correct**:
The existing `getSubType()` values (0, 0, 0, 0x14) ARE correct for `_GetMasterRecByType@16`.
The document's proposed values (2, 5, 0, 0x14) are WRONG for Strategy and Indicator.

**Ghidra evidence**:

1. **`createEntry` passes `getSubType(type)` to `_CreateEntryEX@24`** (`src/tsdev_lib.cpp:673`):
   ```cpp
   progId = m_pCreateEntry(getCategory(type), getSubType(type), name.c_str(), 0, 0, &errCode);
   ```
   - `getSubType(ComponentType::Strategy)` = 0 >> 8 = **0**
   - `getSubType(ComponentType::Indicator)` = 2 >> 8 = **0**

2. **`_CreateEntryEX@24` calls `FUN_101d4280(category, subtype)` to get the expanded type**
   (decompilation of `0x10159610`):
   ```c
   uVar2 = FUN_101d4280(param_1, param_2);  // (category, subtype) → expanded type
   puVar3 = (undefined4 *)FUN_101cf990(uVar2, param_3);  // create with expanded type
   ```

3. **`FUN_101d4280` (0x1D4280) decompilation** confirms:
   - `FUN_101d4280(0, 0)`: `ConvertToPowerType(0, 0)` = 0 (no case matches) → returns **0**
   - `FUN_101d4280(0, 2)`: `ConvertToPowerType(0, 2)` = 2 → returns **0x200**
   - `FUN_101d4280(2, 0)`: `ConvertToPowerType(2, 0)` = 0 → case 0 returns **2**
   - `FUN_101d4280(2, 5)`: `ConvertToPowerType(2, 5)` = 5 → returns **0x502**

4. **`ConvertToPowerType` (0x1584F0) decompilation** confirms:
   ```c
   case 0:  // Strategy
       switch(param_2) {
       case 2: case 3: return 2;  // only subtypes 2,3 map to power type 2
       }
       break;  // subtype 0 → falls through → returns 0
   case 2:  // Indicator
       switch(param_2) {
       case 3: case 5: return 5;  // only subtypes 3,5 map to power type 5
       }
       break;  // subtype 0 → falls through → returns 0
   ```

5. **`_GetMasterRecByType@16` (0x1A0460) decompilation** confirms it calls the SAME
   `FUN_101d4280` conversion:
   ```c
   uVar2 = FUN_101d4280(param_1, param_2);  // same conversion as _CreateEntryEX
   puVar3 = (undefined4 *)FUN_101cfe00(uVar2, param_3, 0);
   ```

**Impact**: User-created Strategy items have expanded type **0** (not 0x200). User-created
Indicator items have expanded type **2** (not 0x502). The document's proposed subtypes would
look up types 0x200 and 0x502, **missing all user-created Strategy and Indicator items**.
This defeats the entire purpose of the live-lookup enumeration (finding newly created items).

**What must be fixed**: Use `getSubType(type)` (the existing function) instead of the
document's proposed `getMasterRecSubType()`. The correct subtypes are:
- Strategy: 0 → type 0
- Indicator: 0 → type 2
- Function: 0 → type 4
- TradingApp: 0x14 → type 0x1402

The document's claim that "getSubType() would NOT work" is incorrect. Both `_CreateEntryEX`
and `_GetMasterRecByType` call the same `FUN_101d4280(category, subtype)` conversion, so they
require the same subtype values.

---

### Finding 2 (CRITICAL): Count Mechanism is Unreliable — Index is Likely ProgId, Not Sequential

**What the document claims** (§2.7):
- The index parameter is "0-based"
- "The warehouse likely uses dense indexing (items are stored in an array)"
- Gap handling: continue past 10 consecutive misses

**Ghidra evidence that the index is the ProgId, not a sequential position**:

1. **The only code caller of `_GetMasterRecByType@16` passes the ProgId as the index**
   (xref from `CompileText` at `0x1001e19b`):
   ```c
   _GetMasterRecByType_16(uVar7, pIVar8[0x16], param_3, local_1a0);
   ```
   Where:
   - `uVar7` = `param_2 & 0xff` = category (low byte of whType)
   - `pIVar8[0x16]` = subtype from the record
   - `param_3` = **ProgId** (CompileText's third parameter, confirmed by the earlier call
     `FUN_101d04f0(param_2, param_3, &local_20)` which uses param_3 as ProgId)
   - `local_1a0` = output buffer

2. **Server-mode path (`FUN_101e3990`, 0x1E3990) uses a BST keyed by ProgId**:
   ```c
   while (cVar1 == '\0') {
       if ((int)puVar3[4] < param_3) {   // compare BST key with index
           puVar7 = (undefined4 *)puVar3[2];  // right child
       } else {
           puVar7 = (undefined4 *)*puVar3;    // left child
       }
   }
   ```
   `puVar3[4]` (offset 0x10 in BST node) is the key, compared with `param_3` (the index).
   This is a key-based lookup (by ProgId), not a sequential position lookup.

3. **`FUN_101e10d0` (used by `_LoadMasterRecsEX`) uses a DIFFERENT indexing scheme**:
   ```c
   if ((-1 < param_2) && (param_2 < *(int *)(param_1 + 0xc))) {
       param_2 = **(int **)(param_1 + 8);   // get BST root
       FUN_101e5210(&param_2, iVar1, local_8);  // in-order traversal by `iVar1` steps
   }
   ```
   This navigates the BST in-order by `param_2` steps — a **sequential index** (0 to
   count-1). This is fundamentally different from the ProgId-based lookup in
   `_GetMasterRecByType`.

4. **Command 0x0D worker thread handler** (in `FUN_101d2150`) calls a COM method:
   ```c
   iVar8 = (**(code **)(*piVar9 + 0x20))
                     (piVar9,
                      *(undefined2 *)((int)&param_1[1].AddRef + 2),    // type (short at [wh+0x12])
                      *(undefined4 *)((int)&param_1[1].Release + 2));  // index (int at [wh+0x16])
   ```
   The COM method at vtable offset 0x20 takes (type, index). The CompileText caller passes
   the ProgId as this index, strongly suggesting it's a ProgId-based lookup.

**Impact**: If the index is the ProgId:
- Iterating from 0 iterates through ProgId space, not sequential positions
- Deleted items leave ProgId gaps (TradeStation does not renumber remaining items)
- The 10-miss gap handler stops after 10 consecutive missing ProgIds
- If a user deletes >10 consecutive items (or items with ProgIds spanning a gap >10),
  the iteration stops prematurely, missing items with higher ProgIds
- This is a **silent data loss** bug — `exists` would return false for existing items

**What must be fixed**: The count mechanism needs a fundamentally different approach. Options:
1. **Find a live count API**: Investigate whether command 0x19 (FUN_101d0300, "get next
   ProgId") can be used to determine the ProgId range. The next ProgId gives an upper bound
   on the ProgId space.
2. **Use a much larger gap threshold** (e.g., 1000+ consecutive misses) — still not
   guaranteed correct but practically safer.
3. **Fall back to `_LoadMasterRecsEX` for built-in items and use `findExistPos` for
   case-insensitive matching** — `findExistPos` is live but exact-match only; enumerate
   cached records for name candidates, then verify each candidate with `findExistPos`.
4. **Investigate the WHServer COM method at vtable offset 0x20** to determine definitively
   whether the index is a ProgId or a sequential position. This requires decompiling
   WHServer.exe (available in the Ghidra project).

**Note**: The document's claim that "the warehouse likely uses dense indexing" is
**unsubstantiated**. No evidence is provided. The CompileText caller evidence contradicts it.

---

### Finding 3 (SIGNIFICANT): Single Subtype Misses Items with Other Subtypes

**What the document proposes**: Use a single subtype per component type (§5.6).

**Ghidra evidence**: `FUN_101d43a0` (0x1D43A0, used by `_LoadMasterRecsEX`) returns MULTIPLE
type codes per category:
```c
case 0:  // Strategy
    *param_2 = 0;        // type 0
    param_2[1] = 0x200;  // type 0x200
    param_2[2] = 0xffffffff;  // terminator
case 2:  // Indicator
    *param_2 = 2;        // type 2
    param_2[1] = 0x502;  // type 0x502
    param_2[2] = 0x402;  // type 0x402
    param_2[3] = 0x602;  // ... etc.
    param_2[10] = 0x1402;  // type 0x1402 (TradingApp)
    param_2[0xb] = 0xffffffff;
case 4:  // Function
    *param_2 = 4;        // type 4 only
    param_2[1] = 0xffffffff;
```

`_LoadMasterRecsEX` enumerates ALL type codes for a category. For Strategy, it enumerates
both type 0 and type 0x200. For Indicator, it enumerates 11 different type codes.

`_GetMasterRecByType` with a single subtype only looks up ONE type code. Even with the
correct subtype (0), it would only get type 0 for Strategy and type 2 for Indicator,
missing items with other subtypes (e.g., type 0x200 strategies, type 0x502 indicators).

**Impact**: For the `exists` use case, this means items with non-zero subtypes of the same
category would be missed. For example, if a user has a strategy with subtype 2 (type 0x200),
`_GetMasterRecByType(0, 0, index, buf)` would not find it.

**What must be fixed**: To match `_LoadMasterRecsEX` behavior, call `_GetMasterRecByType`
with each type code from `FUN_101d43a0` and merge results. Alternatively, confirm that
user-created items always have subtype 0 (which is true for the current `createEntry` code)
and document that only subtype-0 items are enumerated.

---

## Confirmed Claims (with Ghidra evidence)

### Approach A is NOT FEASIBLE ✓

| Claim | Evidence |
|-------|----------|
| `FUN_101cf5b0` is `__thiscall` with warehouse as `this` | Decompilation of `0x101cf5b0`: `void __thiscall FUN_101cf5b0(int param_1, int param_2)` — param_1 = warehouse, stores command at `[param_1+4]`, signals worker via `[param_1+0x6E]` ✓ |
| `FUN_101c09f0` requires module object as `this` | Decompilation of `0x101c09f0`: `void __thiscall FUN_101c09f0(int param_1, int param_2)` — param_1 has cache fields at 0x8F, 0x1B8, etc. and sync at 0x218; warehouse at `[param_1+0xC]` ✓ |
| `FUN_101c09f0` sets `[warehouse+0x5E]` and `[warehouse+0x12]` before 0x4E | Decompilation confirms: `*(undefined4 *)(*(int *)(param_1 + 0xc) + 0x5e) = *(undefined4 *)(param_1 + 0x10)` and `*(int *)(*(int *)(param_1 + 0xc) + 0x12) = param_2` ✓ |
| `FUN_101cf710` calls `QueryInterface` on `[warehouse+0x5E]` | Decompilation of `0x101cf710`: `HVar1 = (*param_1->lpVtbl[6].QueryInterface)(param_1, param_2, unaff_EDI)` — param_1 = IUnknown* from `[warehouse+0x5E]` ✓ |
| Module object only accessible via program-loading path (0x0F) | No export or simple internal function returns the module object. `FUN_101a8360` (delete orchestrator) obtains it via `FUN_101d0950` (command 0x0F), which requires a ProgId. Chicken-and-egg confirmed. ✓ |
| `FUN_101cfce0` has unpatched INT3 at offset 40 | Decompilation of `0x101cfce0`: INT3 at `swi(3)` in thread check. Disassembly confirms INT3 at offset 0x28 (40). Not in existing signatures. ✓ |

### Approach B — Export and Patch ✓

| Claim | Evidence |
|-------|----------|
| `_GetMasterRecByType@16` is exported (ordinal 510, RVA 0x1A0460) | `search_functions` returns `_GetMasterRecByType@16 @ 101a0460`. Decompilation comment: `/* 0x1a0460  510  _GetMasterRecByType@16 */` ✓ |
| `__stdcall` with 4 params: (category, subtype, index, outBuf) | Decompilation: `undefined4 _GetMasterRecByType_16(undefined4 param_1, undefined4 param_2, undefined4 param_3, undefined4 *param_4)`. Caller `CompileText` confirms: `_GetMasterRecByType_16(uVar7, pIVar8[0x16], param_3, local_1a0)` where uVar7=category, pIVar8[0x16]=subtype, param_3=index, local_1a0=outBuf ✓ |
| Uses command 0x0D via `FUN_101d15c0` | `_GetMasterRecByType` → `FUN_101cfe00` → `FUN_101d15c0` (local mode). `FUN_101d15c0` sets `[warehouse+4] = 0xd` ✓ |
| Returns 1 on success, 0 on failure | Decompilation: `return 1` after successful copy, `return 0` on warehouse unavailable or record not found ✓ |
| Same 0x10A-byte record format | Decompilation: copies `0x42` dwords + 1 word = 0x10A bytes. Name at 0x17, ProgId at 0x00, subtype at 0xE9 (same as `_LoadMasterRecsEX`) ✓ |
| INT3 patch at `FUN_101d15c0` offset 84 | Disassembly: INT3 at address `0x101d1614`, function start `0x101d15c0`, offset = 0x54 = 84 ✓ |
| Byte pattern is unique | `search_byte_patterns` with the 97-byte pattern returns exactly 1 match at `0x101d15c0` ✓ |

### Empty Names at 0x17 ✓

| Claim | Evidence |
|-------|----------|
| `[rec+0x73] < 6` → inline name at offset 0x02 | Decompilation of `FUN_101c3660` (0x1C3660): `if (*(byte *)((int)param_1 + 0x73) < 6) { return param_1 + 1; }` ✓ |
| `[rec+0x73] >= 6` → warehouse-stored name via command 0x0D | Decompilation: `FUN_101d04f0(*(undefined4 *)(param_1 + 1), (int)*param_1, &local_8)` — looks up name via command 0x0D/0x09 ✓ |
| Skip records with empty names is acceptable | User-created items always have `[rec+0x73] < 6` (inline names at 0x17, written by `FUN_10158fb0`). The existing `enumerateRecords` already skips non-printable names. ✓ |

**Note on §3.1**: The document says `FUN_101d04f0` "dispatches command 0x09". My decompilation
of `0x101d04f0` shows it calls `FUN_101d15c0` (command 0x0D), not command 0x09. This is a
minor inaccuracy in the document but does not affect the approach — the empty-name behavior
is the same regardless of which command is used for the name lookup.

---

## Non-Blocking Observations

1. **`FUN_101cfce0` byte pattern**: The document claims the pattern for `FUN_101cfce0` is
   unique (1 match). I did not independently verify this (it's only relevant for Approach A,
   which is correctly assessed as not feasible). The pattern for `FUN_101d15c0` (Approach B)
   was verified as unique.

2. **Performance estimate**: The document estimates ~150ms for ~1477 records. This is
   reasonable for the mutex/event overhead but doesn't account for the multiple-subtype
   calls needed (Finding 3), which would multiply the iteration count.

3. **SEH wrapper**: The proposed `callGetMasterRecByType` SEH wrapper (§5.7) is correct and
   follows the existing pattern. The `#pragma warning(disable: 4731)` is unnecessary since
   the wrapper doesn't use inline asm (unlike `callDeleteOrchestrator`), but it's harmless.

4. **Patch restoration**: The document doesn't mention adding the new patch to the
   destructor's restoration loop. The existing code restores 6 thread-check patches
   (`m_threadCheckRvas[6]`). Adding a 7th would require increasing the array size to 7
   and updating the loop bound. This is an implementation detail not covered in the document.

---

## Required Fixes Before Approval

1. **(CRITICAL) Fix subtype mapping**: Replace the proposed `getMasterRecSubType()` with
   `getSubType()`. The correct subtypes are 0, 0, 0, 0x14 (not 2, 5, 0, 0x14). Remove the
   incorrect claim that "getSubType() would NOT work with _GetMasterRecByType@16".

2. **(CRITICAL) Fix or validate the count mechanism**: Either:
   - Decompile WHServer.exe's COM method at vtable offset 0x20 to definitively determine
     whether the index is a ProgId or a sequential position, OR
   - Use command 0x19 (`FUN_101d0300`) to get the next ProgId as an upper bound for
     iteration, OR
   - Use a fundamentally different enumeration strategy (e.g., cached enumeration +
     `findExistPos` verification for case-insensitive matching)

3. **(SIGNIFICANT) Address multi-subtype enumeration**: Either:
   - Call `_GetMasterRecByType` with each type code from `FUN_101d43a0` and merge results, OR
   - Document that only subtype-0 items are enumerated (sufficient for user-created items
     but may miss built-in items with other subtypes)

4. **(MINOR) Fix §3.1 inaccuracy**: `FUN_101d04f0` dispatches command 0x0D (via
   `FUN_101d15c0`), not command 0x09.

5. **(MINOR) Add patch restoration guidance**: Note that `m_threadCheckRvas` array size
   and the destructor's restoration loop must be updated for the 7th patch.
